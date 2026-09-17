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
  EXPECT_TRUE(StartsWith(c.command({"HELLO", "4"}), "-NOPROTO"));
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

// Clients that default to RESP3 (redis-py 8 does) open with HELLO 3 and treat a
// refusal as fatal. After HELLO 3 the connection gets maps and the RESP3 null;
// other connections are unaffected.
TEST_F(ServerTest, Resp3IsNegotiatedPerConnection) {
  RespClient v3 = connect();
  RespClient v2 = connect();
  EXPECT_TRUE(StartsWith(v3.command({"HELLO", "3"}), "{$server: $baton, $version: "));
  ASSERT_EQ(v3.command({"ENQUEUE", "q", "x"}), ":1");

  EXPECT_TRUE(StartsWith(v3.command({"STATUS", "1"}), "{$id: :1, $queue: $q, $state: $ready,"));
  EXPECT_TRUE(StartsWith(v2.command({"STATUS", "1"}), "[$id, :1, $queue, $q, $state, $ready,"));
  EXPECT_TRUE(StartsWith(v3.command({"STATS", "q"}), "{$queue: $q, $scheduled: :0, $ready: :1,"));
  EXPECT_TRUE(StartsWith(v3.command({"STATS"}), "[{$queue: $q,"));

  v3.send({"RESERVE", "0", "1000", "empty"});
  v2.send({"RESERVE", "0", "1000", "empty"});
  EXPECT_EQ(v3.read_reply(), "nil");
  EXPECT_EQ(v2.read_reply(), "nil");
  EXPECT_TRUE(StartsWith(v3.command({"RESERVE", "0", "1000", "q"}), "[:1, :1, $q, $x,"));

  // HELLO 2 switches back.
  EXPECT_TRUE(StartsWith(v3.command({"HELLO", "2"}), "[$server, $baton,"));
  EXPECT_TRUE(StartsWith(v3.command({"STATUS", "1"}), "[$id, :1,"));
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

// --- leases across restarts (docs/design.md 7.3) --------------------------------------------

class ServerLeaseGraceTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.engine.lease_grace_ms = 600;
    return c;
  }

  // The server is down for longer than the lease had left: no heartbeat could
  // have got through. (A real delay: downtime is the scenario.)
  void restart_after_downtime() {
    stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    start(fs_);
  }
};

TEST_F(ServerLeaseGraceTest, WorkerThatOutlivedTheServerCanStillFinishWithinTheGracePeriod) {
  {
    RespClient c = connect();
    ASSERT_EQ(c.command({"ENQUEUE", "q", "x", "BACKOFF", "0", "0"}), ":1");
    ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "100", "q"}), "[:1, :1,"));
  }
  restart_after_downtime();  // the 100 ms lease is nominally long expired

  RespClient c = connect();
  EXPECT_NE(c.command({"STATUS", "1"}).find("$state, $leased"), std::string::npos)
      << "a restart must not expire leases that could not be renewed while it was down";
  EXPECT_TRUE(StartsWith(c.command({"HEARTBEAT", "1", "1", "5000"}), ":17"));
  EXPECT_EQ(c.command({"ACK", "1", "1"}), "+OK");
}

TEST_F(ServerLeaseGraceTest, SilenceAfterTheGracePeriodStillExpiresTheLease) {
  {
    RespClient c = connect();
    ASSERT_EQ(c.command({"ENQUEUE", "q", "x", "BACKOFF", "0", "0"}), ":1");
    ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "100", "q"}), "[:1, :1,"));
  }
  restart_after_downtime();

  RespClient c = connect();
  // Blocks until the grace period ends and the job is redelivered.
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "10000", "5000", "q"}), "[:1, :2, $q, $x, :2,"));
  EXPECT_TRUE(StartsWith(c.command({"ACK", "1", "1"}), "-STALE")) << "the old holder is fenced";
  EXPECT_EQ(c.command({"ACK", "1", "2"}), "+OK");
}

// --- dead-letter queue ---------------------------------------------------------------------

TEST_F(ServerTest, DeadLetterQueueOverTheWire) {
  RespClient c = connect();
  for (int i = 1; i <= 4; ++i) {
    const std::string id = std::to_string(i);
    ASSERT_EQ(c.command({"ENQUEUE", "mail", "payload-" + id}), ":" + id);
    ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "1000", "mail"}), "[:" + id + ", :" + id));
    ASSERT_EQ(c.command({"FAIL", id, id, "smtp down", "NORETRY"}), "[$dead, :0]");
  }
  EXPECT_NE(c.command({"STATS", "mail"}).find("$dead, :4"), std::string::npos);

  const std::string all = c.command({"DLQ.LIST", "mail"});
  EXPECT_TRUE(StartsWith(all, "[[$id, :1, $queue, $mail, $state, $dead,")) << all;
  EXPECT_NE(all.find("$last_error, $smtp down"), std::string::npos);
  EXPECT_TRUE(StartsWith(c.command({"DLQ.LIST", "mail", "2", "1"}), "[[$id, :3,"));
  EXPECT_EQ(c.command({"DLQ.LIST", "mail", "9"}), "[]");
  EXPECT_EQ(c.command({"dlq.list", "empty-queue"}), "[]");
  EXPECT_TRUE(StartsWith(c.command({"DLQ.LIST", "mail", "0", "5000"}), "-ERR usage:"));

  EXPECT_EQ(c.command({"DLQ.RETRY", "1"}), ":1");
  EXPECT_TRUE(StartsWith(c.command({"DLQ.RETRY", "1"}), "-STATE job 1 is ready, not dead"));
  EXPECT_TRUE(StartsWith(c.command({"DLQ.RETRY", "99"}), "-NOTFOUND"));
  EXPECT_TRUE(
      StartsWith(c.command({"RESERVE", "0", "1000", "mail"}), "[:1, :5, $mail, $payload-1, :1,"))
      << "retried: a new token, and attempts start over";

  EXPECT_EQ(c.command({"DLQ.PURGE", "2"}), ":1");
  EXPECT_TRUE(StartsWith(c.command({"STATUS", "2"}), "-NOTFOUND"));
  EXPECT_EQ(c.command({"DLQ.RETRY", "mail", "ALL"}), ":2");
  EXPECT_EQ(c.command({"DLQ.PURGE", "mail", "all"}), ":0");
  EXPECT_TRUE(StartsWith(c.command({"DLQ.PURGE", "mail", "SOME"}), "-ERR usage:"));
  EXPECT_NE(c.command({"STATS", "mail"}).find("$ready, :2"), std::string::npos);

  // RESP3 connections get each entry as a map.
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "1000", "mail"}), "[:3,"));
  ASSERT_EQ(c.command({"FAIL", "3", "6", "again", "NORETRY"}), "[$dead, :0]");
  ASSERT_TRUE(StartsWith(c.command({"HELLO", "3"}), "{$server:"));
  EXPECT_TRUE(StartsWith(c.command({"DLQ.LIST", "mail"}), "[{$id: :3, $queue: $mail,"));
}

TEST_F(ServerTest, DeadLetterOperationsSurviveARestart) {
  {
    RespClient c = connect();
    for (int i = 1; i <= 3; ++i) {
      const std::string id = std::to_string(i);
      ASSERT_EQ(c.command({"ENQUEUE", "q", "x"}), ":" + id);
      ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "1000", "q"}), "[:" + id));
      ASSERT_EQ(c.command({"FAIL", id, id, "e", "NORETRY"}), "[$dead, :0]");
    }
    ASSERT_EQ(c.command({"DLQ.RETRY", "1"}), ":1");
    ASSERT_EQ(c.command({"DLQ.PURGE", "2"}), ":1");
  }
  stop();
  start(fs_);
  RespClient c = connect();
  EXPECT_NE(c.command({"STATUS", "1"}).find("$state, $ready"), std::string::npos);
  EXPECT_TRUE(StartsWith(c.command({"STATUS", "2"}), "-NOTFOUND"));
  EXPECT_TRUE(StartsWith(c.command({"DLQ.LIST", "q"}), "[[$id, :3,"));
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

// --- snapshots and compaction ------------------------------------------------------------------

// The value of one "name:value" line of INFO.
std::string info_field(RespClient& c, std::string_view name) {
  const std::string info = c.command({"INFO"});
  const std::string key = "\n" + std::string(name) + ":";
  const size_t at = info.find(key);
  if (at == std::string::npos) return "<no such field>";
  const size_t start = at + key.size();
  return info.substr(start, info.find('\r', start) - start);
}

// Snapshots are written in the background; INFO is how anyone finds out.
::testing::AssertionResult WaitForInfo(RespClient& c, std::string_view name,
                                       std::string_view value) {
  std::string last;
  for (int attempt = 0; attempt < 2'000; ++attempt) {
    last = info_field(c, name);
    if (last == value) return ::testing::AssertionSuccess();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return ::testing::AssertionFailure() << name << " stayed at " << last << ", wanted " << value;
}

std::vector<std::string> files_with_prefix(SimFs& fs, std::string_view prefix) {
  std::vector<std::string> names = fs.list_dir("data").value();
  std::erase_if(names, [&](const std::string& name) { return !name.starts_with(prefix); });
  return names;
}

class ServerSnapshotTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.snapshot_every_bytes = 0;  // only on request: these tests decide when
    return c;
  }
};

TEST_F(ServerSnapshotTest, RestartRecoversFromTheSnapshotPlusTheLogAfterIt) {
  RespClient c = connect();
  for (int i = 1; i <= 100; ++i) {
    ASSERT_EQ(c.command({"ENQUEUE", "q", "job-" + std::to_string(i)}), ":" + std::to_string(i));
  }
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:1, :1,"));
  ASSERT_EQ(c.command({"ACK", "1", "1"}), "+OK");

  ASSERT_EQ(c.command({"SNAPSHOT"}), "+OK");
  ASSERT_TRUE(WaitForInfo(c, "snapshots_taken", "1"));
  EXPECT_EQ(info_field(c, "last_snapshot_lsn"), "102") << "100 enqueues, a lease, an ack";
  EXPECT_EQ(info_field(c, "last_snapshot_jobs"), "100");
  EXPECT_EQ(info_field(c, "snapshot_in_progress"), "0");
  EXPECT_EQ(c.command({"SNAPSHOT"}), "+OK") << "nothing new to snapshot is not an error";

  // Six more records that only the log has.
  ASSERT_TRUE(StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:2, :2,"));
  for (int i = 101; i <= 105; ++i) {
    ASSERT_EQ(c.command({"ENQUEUE", "q", "job-" + std::to_string(i)}), ":" + std::to_string(i));
  }

  image_ = fs_.crash_image(CrashMode::kLoseUnsynced);
  stop();
  start(*image_);

  RespClient after = connect();
  EXPECT_EQ(info_field(after, "recovered_from_snapshot_lsn"), "102");
  EXPECT_EQ(info_field(after, "recovered_records"), "6") << "only the tail is replayed";
  EXPECT_NE(after.command({"STATUS", "1"}).find("$state, $succeeded"), std::string::npos);
  EXPECT_NE(after.command({"STATUS", "2"}).find("$state, $leased"), std::string::npos);
  EXPECT_NE(after.command({"STATUS", "77", "PAYLOAD"}).find("$payload, $job-77"),
            std::string::npos);
  EXPECT_NE(after.command({"STATUS", "105"}).find("$state, $ready"), std::string::npos);
  EXPECT_EQ(after.command({"ENQUEUE", "q", "next"}), ":106");
  EXPECT_EQ(after.command({"ACK", "2", "2"}), "+OK") << "the lease from the tail is intact";
  EXPECT_TRUE(StartsWith(after.command({"RESERVE", "0", "60000", "q"}), "[:3, :3,"))
      << "fencing tokens continue after a snapshot recovery";
}

// A snapshot that got ahead of the durable log would describe a state that a
// power failure can still take back, and recovery would find a snapshot newer
// than its log. The log is stalled here; snapshot files are not.
TEST_F(ServerSnapshotTest, SnapshotIsNeverAheadOfTheDurableLog) {
  RespClient c = connect();
  ASSERT_EQ(c.command({"ENQUEUE", "q", "durable"}), ":1");

  fs_.hold_syncs("wal-");
  c.send({"ENQUEUE", "q", "written but not durable"});
  c.send({"SNAPSHOT"});
  EXPECT_TRUE(c.stays_silent(300));
  EXPECT_TRUE(files_with_prefix(fs_, "snapshot-").empty())
      << "a snapshot at LSN 2 was started while LSN 2 could still be lost";
  {
    const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
    auto after_crash = Server::create(config(), *image, clock_);
    ASSERT_TRUE(after_crash.ok()) << after_crash.error().to_string();
    EXPECT_EQ((*after_crash)->recovery().snapshot_lsn, 0U);
    EXPECT_EQ((*after_crash)->recovery().records_replayed, 1U);
  }

  fs_.release_syncs();
  EXPECT_EQ(c.read_reply(), ":2");
  EXPECT_EQ(c.read_reply(), "+OK");
  ASSERT_TRUE(WaitForInfo(c, "snapshots_taken", "1"));
  EXPECT_EQ(info_field(c, "last_snapshot_lsn"), "2");
}

// The point of writing snapshots on another thread: a slow snapshot costs the
// clients nothing. Here the snapshot's fsync stalls; the log's does not.
TEST_F(ServerSnapshotTest, TrafficContinuesWhileASnapshotIsBeingWritten) {
  RespClient c = connect();
  ASSERT_EQ(c.command({"ENQUEUE", "q", "a"}), ":1");

  fs_.hold_syncs("snapshot-");
  ASSERT_EQ(c.command({"SNAPSHOT"}), "+OK");
  ASSERT_TRUE(WaitForInfo(c, "snapshot_in_progress", "1"));
  EXPECT_EQ(c.command({"ENQUEUE", "q", "b"}), ":2");
  EXPECT_TRUE(StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:1, :1,"));
  EXPECT_TRUE(StartsWith(c.command({"SNAPSHOT"}), "-STATE a snapshot is already in progress"));
  EXPECT_EQ(info_field(c, "snapshots_taken"), "0");

  fs_.release_syncs();
  ASSERT_TRUE(WaitForInfo(c, "snapshots_taken", "1"));
  EXPECT_EQ(info_field(c, "last_snapshot_lsn"), "1") << "the state as of the SNAPSHOT command";
}

class ServerAutoSnapshotTest : public ServerTest {
 protected:
  ServerConfig config() const override {
    ServerConfig c = ServerTest::config();
    c.segment_size = 16 * 1024;
    c.snapshot_every_bytes = 64 * 1024;
    return c;
  }
};

// Left alone, the server snapshots as the log grows, deletes the segments it no
// longer needs, keeps serving meanwhile - and a power failure at the end loses
// nothing.
TEST_F(ServerAutoSnapshotTest, LogIsCompactedInTheBackgroundAndNothingIsLost) {
  RespClient c = connect();
  const std::string padding(1'000, 'p');
  constexpr int kJobs = 600;  // about 600 KB of log: several snapshot cycles
  int finished = 0;
  for (int i = 1; i <= kJobs; ++i) {
    ASSERT_EQ(c.command({"ENQUEUE", "q", padding + std::to_string(i)}), ":" + std::to_string(i));
    if (i % 3 == 0) {
      const std::string id = std::to_string(++finished);  // oldest first; tokens count up too
      ASSERT_TRUE(
          StartsWith(c.command({"RESERVE", "0", "60000", "q"}), "[:" + id + ", :" + id + ","));
      ASSERT_EQ(c.command({"ACK", id, id}), "+OK");
    }
  }
  ASSERT_TRUE(WaitForInfo(c, "snapshot_in_progress", "0"));
  EXPECT_GE(std::stoi(info_field(c, "snapshots_taken")), 3);
  EXPECT_EQ(info_field(c, "snapshots_failed"), "0");
  EXPECT_GT(std::stoi(info_field(c, "log_segments_removed")), 0);
  EXPECT_LE(files_with_prefix(fs_, "snapshot-").size(), 2U);
  EXPECT_FALSE(fs_.exists(join_path("data", segment_file_name(1)))) << "the log only ever grew";

  image_ = fs_.crash_image(CrashMode::kLoseUnsynced);
  stop();
  start(*image_);

  RespClient after = connect();
  EXPECT_NE(info_field(after, "recovered_from_snapshot_lsn"), "0");
  EXPECT_LT(std::stoi(info_field(after, "recovered_records")), kJobs);
  EXPECT_EQ(info_field(after, "jobs_succeeded"), std::to_string(finished));
  EXPECT_EQ(info_field(after, "jobs_ready"), std::to_string(kJobs - finished));
  EXPECT_NE(after.command({"STATUS", "1"}).find("$state, $succeeded"), std::string::npos);
  EXPECT_NE(after.command({"STATUS", "600", "PAYLOAD"}).find(padding + "600"), std::string::npos);
  EXPECT_EQ(after.command({"ENQUEUE", "q", "next"}), ":" + std::to_string(kJobs + 1));
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
