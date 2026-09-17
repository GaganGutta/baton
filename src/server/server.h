#pragma once

// The baton server: one event loop that owns all state (docs/design.md 6).
//
// Responsibilities, in the order they happen each iteration:
//   tick the engine -> accept -> read/parse/execute -> wake parked RESERVEs ->
//   hand the iteration's records to the log thread -> release the replies whose
//   records are durable -> write to sockets.
//
// The one rule everything here serves: a reply leaves only after the LSN it
// depends on has been committed by the log.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/clock.h"
#include "common/fs.h"
#include "common/result.h"
#include "log/log_writer.h"
#include "net/poller.h"
#include "sched/timing_wheel.h"
#include "server/config.h"
#include "server/connection.h"
#include "state/engine.h"
#include "state/state.h"

namespace baton {

struct ServerCounters {
  uint64_t connections_accepted = 0;
  uint64_t connections_rejected = 0;
  uint64_t commands_processed = 0;
  uint64_t protocol_errors = 0;
};

struct RecoveryReport {
  uint64_t records_replayed = 0;
  uint64_t torn_bytes_truncated = 0;
  size_t segments = 0;
  DurationMs elapsed_ms = 0;
};

class Server {
  struct Private {
    explicit Private() = default;
  };

 public:
  // Locks and recovers the data directory, replays the log, starts the log
  // thread and begins listening. Does not serve until run().
  static Result<std::unique_ptr<Server>> create(ServerConfig config, FileSystem& fs,
                                                const Clock& clock);

  Server(Private key, ServerConfig config, FileSystem& fs, const Clock& clock);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  // Serves until stop() is called, then shuts down cleanly: the log is flushed
  // and synced and every reply that was waiting on it is sent.
  Status run();

  // Both are safe from any thread and from a signal handler.
  void stop();
  void wake();  // forces a loop iteration (tests that move a FakeClock)

  uint16_t port() const { return port_; }
  const RecoveryReport& recovery() const { return recovery_; }

 private:
  class LogSink;
  enum class Verdict : uint8_t { kReplied, kParked };
  using Args = const std::vector<std::string_view>&;
  using Handler = Verdict (Server::*)(Connection&, Args, std::string&);
  struct Command {
    constexpr Command(std::string_view command_name, size_t min_argument_count, bool auth,
                      Handler function)
        : name(command_name), min_args(min_argument_count), needs_auth(auth), handler(function) {}

    std::string_view name;
    size_t min_args;  // including the command name
    bool needs_auth;
    Handler handler;
  };

  Status open_storage();
  Status start_listening();

  // --- loop steps -------------------------------------------------------------------
  int poll_timeout_ms() const;
  void accept_connections();
  void on_readable(Connection& c);
  void on_writable(Connection& c);
  void process_input(Connection& c);
  void execute(Connection& c, const RespRequest& request);
  void serve_ready_waiters();
  void expire_reserve_timeouts();
  void release_durable_replies();
  void flush(Connection& c);
  void apply_backpressure();
  void check_disk_space();
  void shutdown();

  // --- connections --------------------------------------------------------------------
  Connection* find(uint64_t id);
  void queue_reply(Connection& c, std::string_view bytes);
  void close_connection(Connection& c);
  void update_interest(Connection& c);

  // --- parked RESERVEs -----------------------------------------------------------------
  void park(Connection& c, Connection::Blocked blocked, DurationMs timeout_ms);
  void unpark(Connection& c);
  bool try_reserve(Connection& c, const std::vector<std::string>& queues, DurationMs lease_ms,
                   std::string& reply);

  // --- commands (server/commands.cpp) -----------------------------------------------------
  static const Command* find_command(std::string_view upper_name);
  Verdict cmd_ping(Connection& c, Args args, std::string& reply);
  Verdict cmd_echo(Connection& c, Args args, std::string& reply);
  Verdict cmd_auth(Connection& c, Args args, std::string& reply);
  Verdict cmd_hello(Connection& c, Args args, std::string& reply);
  Verdict cmd_client(Connection& c, Args args, std::string& reply);
  Verdict cmd_command(Connection& c, Args args, std::string& reply);
  Verdict cmd_select(Connection& c, Args args, std::string& reply);
  Verdict cmd_quit(Connection& c, Args args, std::string& reply);
  Verdict cmd_info(Connection& c, Args args, std::string& reply);
  Verdict cmd_enqueue(Connection& c, Args args, std::string& reply);
  Verdict cmd_reserve(Connection& c, Args args, std::string& reply);
  Verdict cmd_heartbeat(Connection& c, Args args, std::string& reply);
  Verdict cmd_ack(Connection& c, Args args, std::string& reply);
  Verdict cmd_fail(Connection& c, Args args, std::string& reply);
  Verdict cmd_cancel(Connection& c, Args args, std::string& reply);
  Verdict cmd_status(Connection& c, Args args, std::string& reply);
  Verdict cmd_stats(Connection& c, Args args, std::string& reply);
  Verdict cmd_dlq_list(Connection& c, Args args, std::string& reply);
  Verdict cmd_dlq_retry(Connection& c, Args args, std::string& reply);
  Verdict cmd_dlq_purge(Connection& c, Args args, std::string& reply);
  // Shared by DLQ.RETRY and DLQ.PURGE: `<job_id>` or `<queue> ALL`.
  Verdict dlq_one_or_all(Args args, std::string& reply, bool purge);

  bool check_password(std::string_view candidate) const;
  std::string build_info(std::string_view section) const;
  static void append_job_status(std::string& reply, const Job& job, bool with_payload, bool resp3);
  static void append_queue_stats(std::string& reply, std::string_view name, const Queue* queue,
                                 bool resp3);

  ServerConfig config_;
  FileSystem& fs_;
  const Clock& clock_;

  // Storage and state. Declaration order matters for destruction: the engine and
  // sink refer to the log and the state; the log thread must stop first.
  std::unique_ptr<DirLock> dir_lock_;
  std::unique_ptr<State> state_;
  std::unique_ptr<LogWriter> log_;
  std::unique_ptr<LogSink> sink_;
  std::unique_ptr<Engine> engine_;
  RecoveryReport recovery_;

  // Networking.
  std::unique_ptr<Poller> poller_;
  Fd listener_;
  uint16_t port_ = 0;
  Fd wake_read_;
  Fd wake_write_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> wake_pending_{false};

  std::unordered_map<int, std::unique_ptr<Connection>> connections_;  // by descriptor
  std::unordered_map<uint64_t, Connection*> by_id_;
  uint64_t next_connection_id_ = 1;

  // Replies waiting for durability, in LSN order across all connections.
  struct Gated {
    Lsn lsn = 0;
    uint64_t connection = 0;
  };
  std::deque<Gated> gated_;
  Lsn committed_lsn_ = 0;        // sampled once per iteration
  std::vector<uint64_t> dirty_;  // connections with replies to flush this iteration

  // Parked RESERVEs: per queue, connection ids in arrival order.
  std::map<std::string, std::deque<uint64_t>, std::less<>> waiters_;
  TimingWheel reserve_timeouts_;
  std::vector<TimerEvent> fired_;       // scratch
  std::vector<uint64_t> resume_;        // unparked connections with input left to process
  std::vector<uint64_t> paused_reads_;  // connections whose reads are paused for backpressure
  bool reads_paused_ = false;

  bool disk_low_ = false;
  MonoTime next_disk_check_;
  MonoTime started_at_;
  ServerCounters counters_;
  std::string reply_scratch_;
};

}  // namespace baton
