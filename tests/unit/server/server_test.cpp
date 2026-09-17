// In-process server tests: a real event loop on a loopback socket, a real log
// thread, and SimFs underneath so that "durable" can be frozen, crashed and
// inspected. The wire contract being tested is docs/protocol.md.

#include "server/server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.h"
#include "common/logging.h"
#include "log/format.h"
#include "support/resp_client.h"
#include "testing/sim_fs.h"

namespace baton {
namespace {

::testing::AssertionResult StartsWith(const std::string& value, std::string_view prefix) {
  if (value.starts_with(prefix)) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
         << "\"" << value << "\" does not start with \"" << prefix << "\"";
}

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    set_log_level(LogLevel::kError);
    start(fs_);
  }
  void TearDown() override {
    fs_.release_syncs();
    stop();
    set_log_level(LogLevel::kInfo);
  }

  virtual ServerConfig config() const {
    ServerConfig c;
    c.dir = "data";
    c.port = 0;  // any free port
    return c;
  }

  void start(FileSystem& fs) {
    auto server = Server::create(config(), fs, clock_);
    ASSERT_TRUE(server.ok()) << server.error().to_string();
    server_ = std::move(*server);
    thread_ = std::thread([this] {
      const Status served = server_->run();
      EXPECT_TRUE(served.ok()) << served.error().to_string();
    });
  }

  void stop() {
    if (!server_) return;
    server_->stop();
    thread_.join();
    server_.reset();
  }

  RespClient connect() const { return RespClient(server_->port()); }

  SimFs fs_;
  std::unique_ptr<SimFs> image_;  // a crash image some tests restart on; outlives server_
  SystemClock clock_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

// --- basics ------------------------------------------------------------------------------

TEST_F(ServerTest, PingEchoAndMalformedCommands) {
  RespClient c = connect();
  EXPECT_EQ(c.command({"PING"}), "+PONG");
  EXPECT_EQ(c.command({"ping", "hi"}), "$hi") << "command names are case-insensitive";
  EXPECT_EQ(c.command({"ECHO", std::string("bin\0ary", 7)}), "$" + std::string("bin\0ary", 7));
  EXPECT_EQ(c.command({"NOSUCH"}), "-ERR unknown command 'NOSUCH'");
  EXPECT_EQ(c.command({"ECHO"}), "-ERR wrong number of arguments for 'ECHO'");
  EXPECT_EQ(c.command({"ACK", "one", "2"}), "-ERR usage: ACK <job_id> <token>");
  EXPECT_EQ(c.command({"PING"}), "+PONG") << "errors do not cost the connection";
}

TEST_F(ServerTest, JobLifecycleOverTheWire) {
  RespClient producer = connect();
  RespClient worker = connect();
  EXPECT_EQ(producer.command({"ENQUEUE", "emails", "hello", "PRIORITY", "5", "KEY", "k1"}), ":1");
  EXPECT_EQ(producer.command({"ENQUEUE", "emails", "again", "KEY", "k1"}), ":1")
      << "same idempotency key, same job";
  EXPECT_EQ(producer.command({"ENQUEUE", "emails", "world"}), ":2");

  const std::string lease = worker.command({"RESERVE", "0", "30000", "emails"});
  EXPECT_TRUE(StartsWith(lease, "[:1, :1, $emails, $hello, :1, :10, :17")) << lease;

  const std::string status = worker.command({"STATUS", "1"});
  EXPECT_NE(status.find("$state, $leased"), std::string::npos) << status;
  EXPECT_NE(status.find("$key, $k1"), std::string::npos) << status;
  EXPECT_NE(status.find("$payload_size, :5"), std::string::npos) << status;
  EXPECT_EQ(status.find("$payload,"), std::string::npos) << "payload only on request";
  EXPECT_NE(worker.command({"STATUS", "1", "PAYLOAD"}).find("$payload, $hello"), std::string::npos);

  EXPECT_TRUE(StartsWith(worker.command({"HEARTBEAT", "1", "1", "60000"}), ":17"));
  EXPECT_EQ(worker.command({"ACK", "1", "1"}), "+OK");
  EXPECT_TRUE(StartsWith(worker.command({"ACK", "1", "1"}), "-STALE"));
  EXPECT_EQ(worker.command({"ACK", "99", "1"}), "-NOTFOUND no such job: 99");

  const std::string stats = producer.command({"STATS", "emails"});
  EXPECT_NE(stats.find("$ready, :1"), std::string::npos) << stats;
  EXPECT_NE(stats.find("$succeeded, :1"), std::string::npos) << stats;
  EXPECT_NE(stats.find("$total_enqueued, :2"), std::string::npos) << stats;
  EXPECT_TRUE(StartsWith(producer.command({"STATS"}), "[[$queue, $emails,"));
}

TEST_F(ServerTest, FailCancelAndTheirErrors) {
  RespClient c = connect();
  ASSERT_EQ(c.command({"ENQUEUE", "q", "a", "MAXATTEMPTS", "2"}), ":1");
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "1000", "q"}), "[:1, :1,"));
  EXPECT_TRUE(StartsWith(c.command({"FAIL", "1", "1", "boom", "RETRYIN", "0"}), "[$retry, "));
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "1000", "1000", "q"}), "[:1, :2,"));
  EXPECT_TRUE(StartsWith(c.command({"FAIL", "1", "1", "zombie"}), "-STALE"));
  EXPECT_EQ(c.command({"FAIL", "1", "2", "boom again"}), "[$dead, :0]");
  EXPECT_NE(c.command({"STATUS", "1"}).find("$last_error, $boom again"), std::string::npos);

  ASSERT_EQ(c.command({"ENQUEUE", "q", "b"}), ":2");
  EXPECT_TRUE(StartsWith(c.command({"FAIL", "2", "1", "x", "SOMETIME"}), "-ERR usage:"));
  EXPECT_EQ(c.command({"CANCEL", "2"}), "+OK");
  EXPECT_EQ(c.command({"CANCEL", "2"}), "-STATE job 2 is already cancelled");
  EXPECT_EQ(c.command({"CANCEL", "77"}), "-NOTFOUND no such job: 77");
}

TEST_F(ServerTest, EnqueueOptionsAreValidated) {
  RespClient c = connect();
  EXPECT_TRUE(StartsWith(c.command({"ENQUEUE", "bad queue", "x"}), "-ERR queue name"));
  EXPECT_TRUE(StartsWith(c.command({"ENQUEUE", "q", "x", "PRIORITY", "high"}), "-ERR"));
  EXPECT_TRUE(StartsWith(c.command({"ENQUEUE", "q", "x", "DELAY", "5", "AT", "9"}), "-ERR"));
  EXPECT_TRUE(StartsWith(c.command({"ENQUEUE", "q", "x", "BACKOFF", "9", "1"}), "-ERR"));
  EXPECT_TRUE(StartsWith(c.command({"ENQUEUE", "q", "x", "KEY"}), "-ERR"));
  EXPECT_EQ(c.command({"ENQUEUE", "q", "x", "SHINY", "1"}), "-ERR syntax error near 'SHINY'");
  EXPECT_EQ(c.command({"ENQUEUE", "q", "x", "priority", "-3", "delay", "60000", "maxattempts", "3",
                       "backoff", "10", "20", "key", "k"}),
            ":1");
  const std::string status = c.command({"STATUS", "1"});
  EXPECT_NE(status.find("$state, $scheduled"), std::string::npos) << status;
  EXPECT_NE(status.find("$priority, :-3"), std::string::npos) << status;
  EXPECT_NE(status.find("$max_attempts, :3"), std::string::npos) << status;
}

TEST_F(ServerTest, ClientLibraryHandshakeCommandsWork) {
  RespClient c = connect();
  EXPECT_EQ(c.command({"HELLO", "3"}), "-NOPROTO baton speaks RESP2 only");
  EXPECT_TRUE(StartsWith(c.command({"HELLO", "2", "SETNAME", "worker-7"}), "[$server, $baton,"));
  EXPECT_EQ(c.command({"CLIENT", "GETNAME"}), "$worker-7");
  EXPECT_EQ(c.command({"CLIENT", "SETINFO", "LIB-NAME", "redis-py"}), "+OK");
  EXPECT_EQ(c.command({"CLIENT", "SETNAME", "x"}), "+OK");
  EXPECT_EQ(c.command({"COMMAND", "DOCS"}), "[]");
  EXPECT_EQ(c.command({"SELECT", "0"}), "+OK");
  EXPECT_TRUE(StartsWith(c.command({"SELECT", "1"}), "-ERR"));
  const std::string info = c.command({"INFO"});
  EXPECT_NE(info.find("baton_version:"), std::string::npos);
  EXPECT_NE(info.find("# Persistence"), std::string::npos);
  EXPECT_NE(info.find("durable_lsn:"), std::string::npos);
  EXPECT_EQ(c.command({"INFO", "jobs"}).find("# Server"), std::string::npos);
  EXPECT_EQ(c.command({"QUIT"}), "+OK");
  EXPECT_TRUE(c.wait_for_close());
}

// --- the core invariant ------------------------------------------------------------------

// No reply that confirms or reveals a state change leaves the server before the
// log records it depends on are durable. With fsync frozen, clients must see
// nothing at all; when it thaws, everything arrives, in order.
TEST_F(ServerTest, NoReplyBeforeItsRecordsAreDurable) {
  RespClient setup = connect();
  ASSERT_EQ(setup.command({"ENQUEUE", "jobs", "already durable"}), ":1");

  fs_.hold_syncs();
  RespClient producer = connect();
  RespClient worker = connect();
  RespClient reader = connect();

  // A pipeline of enqueues: acknowledged only once durable.
  producer.send_raw(RespClient::encode({"ENQUEUE", "jobs", "a"}) +
                    RespClient::encode({"ENQUEUE", "jobs", "b"}) +
                    RespClient::encode({"ENQUEUE", "jobs", "c"}));
  EXPECT_TRUE(producer.stays_silent(200)) << "ENQUEUE was acknowledged before fsync";

  // RESERVE of a job that is itself durable: the lease record is not, so the
  // worker must not hear about the job yet.
  worker.send({"RESERVE", "0", "30000", "jobs"});
  EXPECT_TRUE(worker.stays_silent(200)) << "RESERVE replied before its lease was durable";

  // A read must not reveal state that a crash could still take back.
  reader.send({"STATUS", "2"});
  EXPECT_TRUE(reader.stays_silent(200)) << "STATUS revealed an undurable job";

  // A power failure right now loses exactly the things nobody was told about.
  {
    const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
    auto after_crash = Server::create(config(), *image, clock_);
    ASSERT_TRUE(after_crash.ok()) << after_crash.error().to_string();
    EXPECT_EQ((*after_crash)->recovery().records_replayed, 1U);
  }

  fs_.release_syncs();
  EXPECT_EQ(producer.read_reply(), ":2");
  EXPECT_EQ(producer.read_reply(), ":3");
  EXPECT_EQ(producer.read_reply(), ":4");
  EXPECT_TRUE(StartsWith(worker.read_reply(), "[:1, :1,"));
  EXPECT_NE(reader.read_reply().find("$state, $ready"), std::string::npos);
}

// The other half: once a client HAS the reply, a power failure cannot undo it.
TEST_F(ServerTest, EverythingAcknowledgedSurvivesPowerLoss) {
  RespClient c = connect();
  for (int i = 1; i <= 50; ++i) {
    ASSERT_EQ(c.command({"ENQUEUE", "q", "job-" + std::to_string(i)}), ":" + std::to_string(i));
  }
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:1, :1,"));
  ASSERT_EQ(c.command({"ACK", "1", "1"}), "+OK");
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:2, :2,"));

  // Power fails now. Nothing unsynced survives.
  image_ = fs_.crash_image(CrashMode::kLoseUnsynced);
  stop();
  start(*image_);

  RespClient after = connect();
  EXPECT_NE(after.command({"STATUS", "1"}).find("$state, $succeeded"), std::string::npos);
  EXPECT_NE(after.command({"STATUS", "2"}).find("$state, $leased"), std::string::npos);
  EXPECT_NE(after.command({"STATUS", "50", "PAYLOAD"}).find("$payload, $job-50"),
            std::string::npos);
  EXPECT_EQ(after.command({"ENQUEUE", "q", "next"}), ":51") << "ids continue, never reused";
  // The lease survived the restart: its holder can still finish the job.
  EXPECT_EQ(after.command({"ACK", "2", "2"}), "+OK");
}

TEST_F(ServerTest, PipelinedRepliesKeepTheirOrderAndShareCommits) {
  RespClient c = connect();
  constexpr int kJobs = 2'000;
  std::string pipeline;
  for (int i = 0; i < kJobs; ++i) pipeline += RespClient::encode({"ENQUEUE", "bulk", "x"});
  pipeline += RespClient::encode({"PING", "end"});
  c.send_raw(pipeline);
  for (int i = 1; i <= kJobs; ++i) ASSERT_EQ(c.read_reply(), ":" + std::to_string(i));
  EXPECT_EQ(c.read_reply(), "$end");

  const std::string info = c.command({"INFO", "persistence"});
  const size_t at = info.find("log_batches:");
  ASSERT_NE(at, std::string::npos);
  EXPECT_LT(std::stoi(info.substr(at + 12)), kJobs) << "group commit: far fewer fsyncs than jobs";
}

// --- blocking RESERVE ----------------------------------------------------------------------

TEST_F(ServerTest, BlockingReserveTimesOutWithNil) {
  RespClient c = connect();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(c.command({"RESERVE", "150", "1000", "empty"}), "nil");
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_GE(waited, std::chrono::milliseconds(140));
  EXPECT_LT(waited, std::chrono::milliseconds(3000));
  EXPECT_EQ(c.command({"RESERVE", "0", "1000", "empty"}), "nil") << "timeout 0 never waits";
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "-1", "1000", "q"}), "-ERR"));
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "0", "5", "q"}), "-ERR")) << "lease too short";
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "100", "1000", "bad queue"}), "-ERR"));
}

TEST_F(ServerTest, BlockingReserveWakesWhenAJobArrives) {
  RespClient worker = connect();
  RespClient producer = connect();
  worker.send({"RESERVE", "10000", "1000", "a", "b"});
  EXPECT_TRUE(worker.stays_silent(100));
  ASSERT_EQ(producer.command({"ENQUEUE", "b", "wake up"}), ":1");
  EXPECT_TRUE(StartsWith(worker.read_reply(), "[:1, :1, $b, $wake up, :1"));
}

TEST_F(ServerTest, ParkedWorkersAreServedFirstComeFirstServed) {
  RespClient first = connect();
  RespClient second = connect();
  RespClient producer = connect();
  first.send({"RESERVE", "10000", "1000", "q"});
  ASSERT_TRUE(first.stays_silent(100));  // make sure `first` is parked before `second`
  second.send({"RESERVE", "10000", "1000", "q"});
  ASSERT_TRUE(second.stays_silent(100));

  ASSERT_EQ(producer.command({"ENQUEUE", "q", "one"}), ":1");
  EXPECT_TRUE(StartsWith(first.read_reply(), "[:1,"));
  EXPECT_TRUE(second.stays_silent(100));
  ASSERT_EQ(producer.command({"ENQUEUE", "q", "two"}), ":2");
  EXPECT_TRUE(StartsWith(second.read_reply(), "[:2,"));
}

TEST_F(ServerTest, BlockingReserveWakesForDelayedJobsAndRetries) {
  RespClient worker = connect();
  RespClient producer = connect();
  ASSERT_EQ(producer.command({"ENQUEUE", "q", "later", "DELAY", "150"}), ":1");
  EXPECT_TRUE(StartsWith(worker.command({"RESERVE", "5000", "1000", "q"}), "[:1, :1,"));
  ASSERT_TRUE(
      StartsWith(worker.command({"FAIL", "1", "1", "again", "RETRYIN", "150"}), "[$retry,"));
  EXPECT_TRUE(StartsWith(worker.command({"RESERVE", "5000", "1000", "q"}), "[:1, :2,"));
}

TEST_F(ServerTest, RequestsPipelinedBehindAParkedReserveRunAfterIt) {
  RespClient worker = connect();
  RespClient producer = connect();
  worker.send_raw(RespClient::encode({"RESERVE", "10000", "1000", "q"}) +
                  RespClient::encode({"PING", "after"}));
  EXPECT_TRUE(worker.stays_silent(150)) << "PING must wait its turn behind the RESERVE";
  ASSERT_EQ(producer.command({"ENQUEUE", "q", "x"}), ":1");
  EXPECT_TRUE(StartsWith(worker.read_reply(), "[:1,"));
  EXPECT_EQ(worker.read_reply(), "$after");
}

TEST_F(ServerTest, DisconnectedWaiterDoesNotSwallowAJob) {
  RespClient producer = connect();
  {
    RespClient quitter = connect();
    quitter.send({"RESERVE", "10000", "1000", "q"});
    ASSERT_TRUE(quitter.stays_silent(100));
  }  // the parked client disconnects
  EXPECT_EQ(producer.command({"PING"}), "+PONG");  // gives the loop a turn to notice
  ASSERT_EQ(producer.command({"ENQUEUE", "q", "still here"}), ":1");
  EXPECT_NE(producer.command({"STATUS", "1"}).find("$state, $ready"), std::string::npos)
      << "the job must not be leased to a connection that is gone";
  RespClient worker = connect();
  EXPECT_TRUE(StartsWith(worker.command({"RESERVE", "0", "1000", "q"}), "[:1,"));
}

TEST_F(ServerTest, ShutdownAnswersParkedReservesAndFlushesAcknowledgements) {
  RespClient worker = connect();
  RespClient producer = connect();
  worker.send({"RESERVE", "60000", "1000", "q"});
  ASSERT_TRUE(worker.stays_silent(100));
  ASSERT_EQ(producer.command({"ENQUEUE", "other", "x"}), ":1");
  stop();
  EXPECT_EQ(worker.read_reply(), "nil");
  EXPECT_TRUE(worker.wait_for_close());
}

// --- protocol errors and limits -------------------------------------------------------------

TEST_F(ServerTest, ProtocolErrorsGetAnErrorReplyAndAClosedConnection) {
  RespClient inline_command = connect();
  inline_command.send_raw("PING\r\n");
  EXPECT_TRUE(StartsWith(inline_command.read_reply(), "-ERR Protocol error:"));
  EXPECT_TRUE(inline_command.wait_for_close());

  RespClient browser = connect();
  browser.send_raw("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_TRUE(StartsWith(browser.read_reply(), "-ERR Protocol error:"));
  EXPECT_TRUE(browser.wait_for_close());

  RespClient liar = connect();
  liar.send_raw("*1\r\n$99999999999\r\n");
  EXPECT_EQ(liar.read_reply(), "-ERR Protocol error: invalid bulk length");
  EXPECT_TRUE(liar.wait_for_close());

  RespClient fine = connect();
  EXPECT_EQ(fine.command({"PING"}), "+PONG") << "other connections are unaffected";
}

class ServerLimitsTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.engine.max_payload_bytes = 1'000;
    c.engine.max_memory_bytes = 200'000;
    c.max_connections = 3;
    c.max_output_buffer_bytes = 256 * 1024;
    return c;
  }
};

TEST_F(ServerLimitsTest, OversizedPayloadIsRefusedAndTheConnectionSurvives) {
  RespClient c = connect();
  EXPECT_EQ(c.command({"ENQUEUE", "q", std::string(1'000, 'x')}), ":1") << "exactly the limit";
  EXPECT_EQ(c.command({"ENQUEUE", "q", std::string(1'001, 'x')}),
            "-LIMIT argument exceeds max-payload of 1000 bytes");
  EXPECT_EQ(c.command({"ENQUEUE", "q", std::string(5'000'000, 'x'), "KEY", "k"}),
            "-LIMIT argument exceeds max-payload of 1000 bytes");
  EXPECT_EQ(c.command({"PING"}), "+PONG") << "the stream is still in sync";
  EXPECT_EQ(c.command({"ENQUEUE", "q", "small"}), ":2");
}

TEST_F(ServerLimitsTest, MemoryLimitRefusesEnqueueButNothingElse) {
  RespClient c = connect();
  std::string last;
  int accepted = 0;
  for (int i = 0; i < 1'000; ++i) {
    last = c.command({"ENQUEUE", "q", std::string(900, 'm')});
    if (last[0] != ':') break;
    ++accepted;
  }
  EXPECT_GT(accepted, 50);
  EXPECT_TRUE(StartsWith(last, "-LIMIT max-memory")) << last;
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "0", "1000", "q"}), "[:1,"))
      << "draining still works";
  EXPECT_EQ(c.command({"ACK", "1", "1"}), "+OK");
}

TEST_F(ServerLimitsTest, ConnectionLimitRefusesPolitely) {
  RespClient a = connect();
  RespClient b = connect();
  RespClient c = connect();
  ASSERT_EQ(a.command({"PING"}), "+PONG");
  ASSERT_EQ(b.command({"PING"}), "+PONG");
  ASSERT_EQ(c.command({"PING"}), "+PONG");
  RespClient extra = connect();
  EXPECT_EQ(extra.read_reply(), "-LIMIT max-connections reached, try again later");
  EXPECT_TRUE(extra.wait_for_close());

  // A freed slot is usable again. The server learns about the disconnect
  // asynchronously, so allow it a few tries.
  b.close();
  std::string reply;
  for (int attempt = 0; attempt < 100 && reply != "+PONG"; ++attempt) {
    try {
      RespClient replacement = connect();
      reply = replacement.command({"PING"}, 200);
    } catch (const std::runtime_error&) {
      reply.clear();  // refused and already closed by the server: try again
    }
  }
  EXPECT_EQ(reply, "+PONG");
}

TEST_F(ServerLimitsTest, ClientThatNeverReadsIsDisconnected) {
  RespClient producer = connect();
  ASSERT_EQ(producer.command({"ENQUEUE", "q", std::string(1'000, 'p')}), ":1");
  RespClient deaf = connect();
  // ~1 KB per reply; far more than socket buffers plus the 256 KiB limit can hold.
  std::string flood;
  for (int i = 0; i < 2'000; ++i) flood += RespClient::encode({"STATUS", "1", "PAYLOAD"});
  for (int round = 0; round < 40; ++round) {
    try {
      deaf.send_raw(flood);
    } catch (const std::runtime_error&) {
      break;  // the server hung up on us: exactly the point
    }
  }
  EXPECT_TRUE(deaf.wait_for_close(20'000));
  EXPECT_EQ(producer.command({"PING"}), "+PONG") << "everyone else is unaffected";
}

// --- authentication --------------------------------------------------------------------------

class ServerAuthTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.requirepass = "s3cret";
    return c;
  }
};

TEST_F(ServerAuthTest, EverythingNeedsAuthUntilThePasswordIsGiven) {
  RespClient c = connect();
  EXPECT_EQ(c.command({"PING"}), "-NOAUTH Authentication required.");
  EXPECT_EQ(c.command({"ENQUEUE", "q", "x"}), "-NOAUTH Authentication required.");
  EXPECT_EQ(c.command({"INFO"}), "-NOAUTH Authentication required.");
  EXPECT_EQ(c.command({"AUTH", "wrong"}), "-WRONGPASS invalid password");
  EXPECT_EQ(c.command({"AUTH", "s3cre"}), "-WRONGPASS invalid password");
  EXPECT_EQ(c.command({"AUTH", "s3cret!"}), "-WRONGPASS invalid password");
  EXPECT_EQ(c.command({"PING"}), "-NOAUTH Authentication required.");
  EXPECT_EQ(c.command({"AUTH", "s3cret"}), "+OK");
  EXPECT_EQ(c.command({"ENQUEUE", "q", "x"}), ":1");

  RespClient with_user = connect();
  EXPECT_EQ(with_user.command({"AUTH", "default", "s3cret"}), "+OK");
  RespClient via_hello = connect();
  EXPECT_TRUE(StartsWith(via_hello.command({"HELLO", "2"}), "-NOAUTH"));
  EXPECT_TRUE(
      StartsWith(via_hello.command({"HELLO", "2", "AUTH", "default", "nope"}), "-WRONGPASS"));
  EXPECT_TRUE(
      StartsWith(via_hello.command({"HELLO", "2", "AUTH", "default", "s3cret"}), "[$server,"));
  EXPECT_EQ(via_hello.command({"PING"}), "+PONG");
}

TEST_F(ServerTest, AuthWithoutAConfiguredPasswordIsAnError) {
  RespClient c = connect();
  EXPECT_TRUE(StartsWith(c.command({"AUTH", "anything"}), "-ERR"));
  EXPECT_EQ(c.command({"PING"}), "+PONG");
}

// --- startup -------------------------------------------------------------------------------

TEST_F(ServerTest, SecondServerOnTheSameDirectoryIsRefused) {
  const auto second = Server::create(config(), fs_, clock_);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code(), ErrorCode::kFailedPrecondition);
  EXPECT_NE(second.error().message().find("in use by another baton process"), std::string::npos);
}

TEST_F(ServerTest, RestartKeepsStateAndReleasesTheLock) {
  {
    RespClient c = connect();
    ASSERT_EQ(c.command({"ENQUEUE", "q", "persist me", "KEY", "k"}), ":1");
  }
  stop();
  start(fs_);
  RespClient c = connect();
  EXPECT_EQ(c.command({"ENQUEUE", "q", "persist me", "KEY", "k"}), ":1")
      << "the idempotency key survived the restart";
  EXPECT_NE(c.command({"STATUS", "1", "PAYLOAD"}).find("$payload, $persist me"), std::string::npos);
  EXPECT_NE(c.command({"INFO", "persistence"}).find("recovered_records:1"), std::string::npos);
}

TEST(ServerStartupTest, RefusesToStartOnADamagedLog) {
  set_log_level(LogLevel::kOff);
  SimFs fs;
  const SystemClock clock;
  ServerConfig config;
  config.dir = "data";
  config.port = 0;
  {
    auto server = Server::create(config, fs, clock);
    ASSERT_TRUE(server.ok());
    std::thread loop([&] { (void)(*server)->run(); });
    RespClient c((*server)->port());
    for (int i = 0; i < 5; ++i) ASSERT_EQ(c.command({"ENQUEUE", "q", "x"})[0], ':');
    (*server)->stop();
    loop.join();
  }
  // A bit flips in the middle of the log.
  fs.flip_bit(join_path("data", segment_file_name(1)), kSegmentHeaderSize + 40, 3);

  const auto restarted = Server::create(config, fs, clock);
  ASSERT_FALSE(restarted.ok()) << "a damaged log must never be served";
  EXPECT_EQ(restarted.error().code(), ErrorCode::kCorruption);
  EXPECT_NE(restarted.error().message().find("refusing to drop acknowledged data"),
            std::string::npos)
      << restarted.error().message();
  set_log_level(LogLevel::kInfo);
}

TEST(ServerStartupTest, RefusesALogThatDoesNotFitTheStateMachine) {
  set_log_level(LogLevel::kOff);
  SimFs fs;
  ASSERT_TRUE(fs.create_dir_if_missing("data").ok());
  // A checksummed, well-formed record that acknowledges a job which never existed.
  std::string segment;
  append_segment_header(segment, 1);
  std::string payload;
  encode_record(JobSucceeded{.id = 7, .token = 3, .at = WallTime{1}}, payload);
  append_record(segment, 1, static_cast<uint8_t>(RecordType::kJobSucceeded), payload);
  fs.write_file(join_path("data", segment_file_name(1)), segment);

  ServerConfig config;
  config.dir = "data";
  config.port = 0;
  const SystemClock clock;
  const auto server = Server::create(config, fs, clock);
  ASSERT_FALSE(server.ok());
  EXPECT_NE(server.error().message().find("LSN 1 does not fit the state"), std::string::npos)
      << server.error().message();
  set_log_level(LogLevel::kInfo);
}

// --- backpressure ------------------------------------------------------------------------------

class ServerBackpressureTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.max_log_backlog_bytes = 64 * 1024;
    return c;
  }
};

// With the disk stalled, the server stops reading instead of buffering without
// bound; when the disk comes back, nothing was lost and nothing deadlocked.
TEST_F(ServerBackpressureTest, StalledDiskPausesReadsAndEverythingCompletesAfterwards) {
  fs_.hold_syncs();
  RespClient c = connect();
  constexpr int kJobs = 400;
  std::string pipeline;
  for (int i = 0; i < kJobs; ++i) {
    pipeline += RespClient::encode({"ENQUEUE", "q", std::string(4'000, 'b')});
  }
  std::thread sender([&] { c.send_raw(pipeline); });  // 1.6 MB: would block on a full socket
  EXPECT_TRUE(c.stays_silent(300));
  fs_.release_syncs();
  sender.join();
  for (int i = 1; i <= kJobs; ++i) ASSERT_EQ(c.read_reply(), ":" + std::to_string(i));
}

}  // namespace
}  // namespace baton
