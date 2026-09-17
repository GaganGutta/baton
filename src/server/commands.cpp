// Command handlers: translate RESP arguments into Engine calls and Engine
// results into RESP replies, exactly as specified in docs/protocol.md. All the
// semantics live in the Engine; nothing here mutates state directly.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <utility>

#include "common/version.h"
#include "net/resp.h"
#include "server/server.h"

namespace baton {
namespace {

std::string upper(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char ch) { return std::toupper(ch); });
  return out;
}

std::optional<int64_t> parse_i64(std::string_view text) {
  int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
  return value;
}

std::optional<uint64_t> parse_u64(std::string_view text) {
  uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
  return value;
}

// The protocol's error code for each kind of engine error (docs/protocol.md).
std::string_view wire_code(ErrorCode code) {
  switch (code) {
    case ErrorCode::kNotFound:
      return "NOTFOUND";
    case ErrorCode::kStaleToken:
      return "STALE";
    case ErrorCode::kFailedPrecondition:
      return "STATE";
    case ErrorCode::kLimitExceeded:
      return "LIMIT";
    case ErrorCode::kUnauthenticated:
      return "NOAUTH";
    case ErrorCode::kUnavailable:
      return "UNAVAILABLE";
    default:
      return "ERR";
  }
}

void reply_error(std::string& reply, const Error& error) {
  resp_error(reply, wire_code(error.code()), error.message());
}

void syntax_error(std::string& reply, std::string_view what) { resp_error(reply, "ERR", what); }

void field(std::string& reply, std::string_view name, int64_t value) {
  resp_bulk(reply, name);
  resp_integer(reply, value);
}

void field(std::string& reply, std::string_view name, std::string_view value) {
  resp_bulk(reply, name);
  resp_bulk(reply, value);
}

}  // namespace

const Server::Command* Server::find_command(std::string_view upper_name) {
  static constexpr std::array<Command, 20> kCommands = {{
      {"DLQ.LIST", 2, true, &Server::cmd_dlq_list},
      {"DLQ.RETRY", 2, true, &Server::cmd_dlq_retry},
      {"DLQ.PURGE", 2, true, &Server::cmd_dlq_purge},
      {"PING", 1, true, &Server::cmd_ping},
      {"ECHO", 2, true, &Server::cmd_echo},
      {"AUTH", 2, false, &Server::cmd_auth},
      {"HELLO", 1, false, &Server::cmd_hello},
      {"QUIT", 1, false, &Server::cmd_quit},
      {"CLIENT", 2, true, &Server::cmd_client},
      {"COMMAND", 1, true, &Server::cmd_command},
      {"SELECT", 2, true, &Server::cmd_select},
      {"INFO", 1, true, &Server::cmd_info},
      {"ENQUEUE", 3, true, &Server::cmd_enqueue},
      {"RESERVE", 4, true, &Server::cmd_reserve},
      {"HEARTBEAT", 3, true, &Server::cmd_heartbeat},
      {"ACK", 3, true, &Server::cmd_ack},
      {"FAIL", 3, true, &Server::cmd_fail},
      {"CANCEL", 2, true, &Server::cmd_cancel},
      {"STATUS", 2, true, &Server::cmd_status},
      {"STATS", 1, true, &Server::cmd_stats},
  }};
  const auto* it = std::ranges::find(kCommands, upper_name, &Command::name);
  return it == kCommands.end() ? nullptr : it;
}

// --- connection and server commands ---------------------------------------------------
// Every handler has the same member-function signature so that it fits the
// dispatch table, whether or not it happens to touch the server.
// NOLINTBEGIN(readability-convert-member-functions-to-static)

Server::Verdict Server::cmd_ping(Connection& /*c*/, Args args, std::string& reply) {
  if (args.size() > 1) {
    resp_bulk(reply, args[1]);
  } else {
    resp_simple(reply, "PONG");
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_echo(Connection& /*c*/, Args args, std::string& reply) {
  resp_bulk(reply, args[1]);
  return Verdict::kReplied;
}

// Compares in time that depends only on the length of the real password.
bool Server::check_password(std::string_view candidate) const {
  const std::string& expected = config_.requirepass;
  unsigned difference = candidate.size() == expected.size() ? 0U : 1U;
  for (size_t i = 0; i < expected.size(); ++i) {
    const char given = i < candidate.size() ? candidate[i] : '\0';
    difference |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(given);
  }
  return difference == 0;
}

Server::Verdict Server::cmd_auth(Connection& c, Args args, std::string& reply) {
  // AUTH <password> or AUTH <username> <password>; the username is ignored.
  const std::string_view password = args.size() >= 3 ? args[2] : args[1];
  if (config_.requirepass.empty()) {
    syntax_error(reply, "AUTH called but no password is configured (see --requirepass)");
  } else if (check_password(password)) {
    c.authenticated = true;
    resp_simple(reply, "OK");
  } else {
    c.authenticated = false;
    resp_error(reply, "WRONGPASS", "invalid password");
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_hello(Connection& c, Args args, std::string& reply) {
  size_t i = 1;
  bool resp3 = c.resp3;
  if (i < args.size()) {
    if (args[i] != "2" && args[i] != "3") {
      resp_error(reply, "NOPROTO", "unsupported protocol version (baton speaks RESP 2 and 3)");
      return Verdict::kReplied;
    }
    resp3 = args[i] == "3";
    ++i;
  }
  while (i < args.size()) {
    const std::string option = upper(args[i]);
    if (option == "AUTH" && i + 2 < args.size()) {
      if (!config_.requirepass.empty() && !check_password(args[i + 2])) {
        resp_error(reply, "WRONGPASS", "invalid password");
        return Verdict::kReplied;
      }
      c.authenticated = true;
      i += 3;
    } else if (option == "SETNAME" && i + 1 < args.size()) {
      c.name = std::string(args[i + 1]);
      i += 2;
    } else {
      syntax_error(reply, "syntax error in HELLO");
      return Verdict::kReplied;
    }
  }
  if (!c.authenticated) {
    resp_error(reply, "NOAUTH", "HELLO must be called with AUTH when a password is set");
    return Verdict::kReplied;
  }
  c.resp3 = resp3;  // only once the handshake has succeeded
  resp_map_header(reply, 7, c.resp3);
  field(reply, "server", "baton");
  field(reply, "version", kVersion);
  field(reply, "proto", int64_t{c.resp3 ? 3 : 2});
  field(reply, "id", static_cast<int64_t>(c.id));
  field(reply, "mode", "standalone");
  field(reply, "role", "master");
  resp_bulk(reply, "modules");
  resp_array_header(reply, 0);
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_quit(Connection& c, Args /*args*/, std::string& reply) {
  c.close_after_flush = true;
  resp_simple(reply, "OK");
  return Verdict::kReplied;
}

// Sent automatically by client libraries when they connect.
Server::Verdict Server::cmd_client(Connection& c, Args args, std::string& reply) {
  const std::string sub = upper(args[1]);
  if (sub == "SETNAME" && args.size() == 3) {
    c.name = std::string(args[2]);
    resp_simple(reply, "OK");
  } else if (sub == "GETNAME") {
    resp_bulk(reply, c.name);
  } else if (sub == "ID") {
    resp_integer(reply, static_cast<int64_t>(c.id));
  } else if (sub == "SETINFO") {
    resp_simple(reply, "OK");
  } else {
    syntax_error(reply, std::format("unsupported CLIENT subcommand '{}'", sub));
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_command(Connection& /*c*/, Args /*args*/, std::string& reply) {
  resp_array_header(reply, 0);  // redis-cli asks for command docs at startup
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_select(Connection& /*c*/, Args args, std::string& reply) {
  if (args[1] == "0") {
    resp_simple(reply, "OK");
  } else {
    syntax_error(reply, "baton has a single database (0)");
  }
  return Verdict::kReplied;
}

// NOLINTEND(readability-convert-member-functions-to-static)

Server::Verdict Server::cmd_info(Connection& /*c*/, Args args, std::string& reply) {
  resp_bulk(reply, build_info(args.size() > 1 ? upper(args[1]) : ""));
  return Verdict::kReplied;
}

std::string Server::build_info(std::string_view section) const {
  const auto wanted = [section](std::string_view name) {
    return section.empty() || section == "ALL" || section == "EVERYTHING" || section == name;
  };
  std::string out;
  const auto line = [&out](std::string_view key, const auto& value) {
    out += std::format("{}:{}\r\n", key, value);
  };

  if (wanted("SERVER")) {
    out += "# Server\r\n";
    line("baton_version", kVersion);
    line("process_id", ::getpid());
    line("tcp_port", port_);
    line("data_dir", config_.dir);
    line("uptime_seconds", (clock_.mono_now() - started_at_) / 1000);
    out += "\r\n";
  }
  if (wanted("CLIENTS")) {
    size_t blocked = 0;
    for (const auto& [fd, connection] : connections_) blocked += connection->blocked ? 1 : 0;
    out += "# Clients\r\n";
    line("connected_clients", connections_.size());
    line("blocked_clients", blocked);
    line("total_connections_received", counters_.connections_accepted);
    line("rejected_connections", counters_.connections_rejected);
    line("total_commands_processed", counters_.commands_processed);
    line("protocol_errors", counters_.protocol_errors);
    out += "\r\n";
  }
  if (wanted("MEMORY")) {
    out += "# Memory\r\n";
    line("used_memory_estimate", state_->memory_bytes());
    line("max_memory", config_.engine.max_memory_bytes);
    line("max_payload", config_.engine.max_payload_bytes);
    out += "\r\n";
  }
  if (wanted("PERSISTENCE")) {
    const LogWriterStats stats = log_->stats();
    out += "# Persistence\r\n";
    line("fsync_policy", config_.fsync == FsyncPolicy::kAlways ? "always" : "interval");
    line("last_lsn", engine_->last_lsn());
    line("durable_lsn", log_->committed_lsn());
    line("log_backlog_bytes", log_->backlog_bytes());
    line("log_segments_created", stats.segments_created);
    line("log_batches", stats.batches);
    line("log_records", stats.records);
    line("log_bytes", stats.bytes);
    line("log_batch_records_avg", std::format("{:.2f}", stats.batch_records.mean()));
    line("log_batch_records_max", stats.batch_records.max());
    line("log_fsync_count", stats.syncs);
    line("log_fsync_p50_us", stats.sync_micros.percentile(0.50));
    line("log_fsync_p99_us", stats.sync_micros.percentile(0.99));
    line("recovery_ms", recovery_.elapsed_ms);
    line("recovered_records", recovery_.records_replayed);
    line("recovered_torn_bytes", recovery_.torn_bytes_truncated);
    line("disk_low", disk_low_ ? 1 : 0);
    line("clock_jumps_detected", engine_->clock_jumps_detected());
    out += "\r\n";
  }
  if (wanted("JOBS")) {
    std::array<uint64_t, kJobStateCount> counts{};
    for (const auto& [name, queue] : state_->queues()) {
      for (size_t i = 0; i < counts.size(); ++i) counts[i] += queue->counts[i];
    }
    out += "# Jobs\r\n";
    line("queues", state_->queues().size());
    line("jobs_in_memory", state_->job_count());
    for (size_t i = 0; i < counts.size(); ++i) {
      line(std::format("jobs_{}", to_string(static_cast<JobState>(i))), counts[i]);
    }
    line("idempotency_keys", state_->idem_count());
    line("pending_timers", state_->pending_timers());
    line("next_job_id", state_->next_job_id());
    out += "\r\n";
  }
  return out;
}

// --- job commands ------------------------------------------------------------------------

Server::Verdict Server::cmd_enqueue(Connection& /*c*/, Args args, std::string& reply) {
  EnqueueRequest request;
  request.queue = args[1];
  request.payload = args[2];
  bool has_delay = false;

  for (size_t i = 3; i < args.size();) {
    const std::string option = upper(args[i]);
    const auto value = [&](size_t offset) -> std::optional<int64_t> {
      return i + offset < args.size() ? parse_i64(args[i + offset]) : std::nullopt;
    };
    if (option == "PRIORITY") {
      const auto n = value(1);
      if (!n || *n < std::numeric_limits<int32_t>::min() ||
          *n > std::numeric_limits<int32_t>::max()) {
        syntax_error(reply, "PRIORITY needs a 32-bit integer");
        return Verdict::kReplied;
      }
      request.priority = static_cast<int32_t>(*n);
      i += 2;
    } else if (option == "DELAY" || option == "AT") {
      const auto n = value(1);
      if (!n || *n < 0 || has_delay) {
        syntax_error(reply, "DELAY and AT need a non-negative integer and exclude each other");
        return Verdict::kReplied;
      }
      has_delay = true;
      if (option == "DELAY") {
        request.delay_ms = *n;
      } else {
        request.run_at = WallTime{std::min(*n, kMaxWallTimeMs)};
      }
      i += 2;
    } else if (option == "MAXATTEMPTS") {
      const auto n = value(1);
      if (!n || *n < 1 || std::cmp_greater(*n, std::numeric_limits<uint32_t>::max())) {
        syntax_error(reply, "MAXATTEMPTS needs a positive integer");
        return Verdict::kReplied;
      }
      request.max_attempts = static_cast<uint32_t>(*n);
      i += 2;
    } else if (option == "BACKOFF") {
      const auto base = value(1);
      const auto cap = value(2);
      const int64_t limit = std::numeric_limits<uint32_t>::max();
      if (!base || !cap || *base < 0 || *cap < *base || *cap > limit) {
        syntax_error(reply, "BACKOFF needs <base_ms> <cap_ms> with 0 <= base <= cap");
        return Verdict::kReplied;
      }
      request.backoff_base_ms = static_cast<uint32_t>(*base);
      request.backoff_cap_ms = static_cast<uint32_t>(*cap);
      i += 3;
    } else if (option == "KEY" && i + 1 < args.size()) {
      if (args[i + 1].empty()) {
        syntax_error(reply, "KEY must not be empty");
        return Verdict::kReplied;
      }
      request.idem_key = args[i + 1];
      i += 2;
    } else {
      syntax_error(reply, std::format("syntax error near '{}'", option));
      return Verdict::kReplied;
    }
  }

  if (disk_low_) {
    resp_error(reply, "LIMIT", "the data directory is almost out of disk space");
    return Verdict::kReplied;
  }
  const auto result = engine_->enqueue(request);
  if (!result.ok()) {
    reply_error(reply, result.error());
  } else {
    resp_integer(reply, static_cast<int64_t>(result->id));
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_reserve(Connection& c, Args args, std::string& reply) {
  const auto timeout_ms = parse_i64(args[1]);
  const auto lease_ms = parse_i64(args[2]);
  if (!timeout_ms || *timeout_ms < 0 || *timeout_ms > config_.max_reserve_timeout_ms) {
    syntax_error(
        reply, std::format("timeout must be between 0 and {} ms", config_.max_reserve_timeout_ms));
    return Verdict::kReplied;
  }
  if (!lease_ms) {
    syntax_error(reply, "lease must be an integer number of milliseconds");
    return Verdict::kReplied;
  }

  Connection::Blocked blocked;
  blocked.lease_ms = *lease_ms;
  for (size_t i = 3; i < args.size(); ++i) blocked.queues.emplace_back(args[i]);

  // An error (bad queue name, bad lease) or a job both answer right away.
  if (try_reserve(c, blocked.queues, blocked.lease_ms, reply)) return Verdict::kReplied;
  if (*timeout_ms == 0) {
    resp_null(reply, c.resp3);
    return Verdict::kReplied;
  }
  park(c, std::move(blocked), *timeout_ms);
  return Verdict::kParked;
}

Server::Verdict Server::cmd_heartbeat(Connection& /*c*/, Args args, std::string& reply) {
  const auto id = parse_u64(args[1]);
  const auto token = parse_u64(args[2]);
  const auto lease_ms =
      args.size() > 3 ? parse_i64(args[3]) : std::optional(config_.engine.default_lease_ms);
  if (!id || !token || !lease_ms || args.size() > 4) {
    syntax_error(reply, "usage: HEARTBEAT <job_id> <token> [<lease_ms>]");
    return Verdict::kReplied;
  }
  const auto expiry = engine_->heartbeat(*id, *token, *lease_ms);
  if (!expiry.ok()) {
    reply_error(reply, expiry.error());
  } else {
    resp_integer(reply, expiry->ms);
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_ack(Connection& /*c*/, Args args, std::string& reply) {
  const auto id = parse_u64(args[1]);
  const auto token = parse_u64(args[2]);
  if (!id || !token || args.size() != 3) {
    syntax_error(reply, "usage: ACK <job_id> <token>");
    return Verdict::kReplied;
  }
  if (const Status acked = engine_->ack(*id, *token); !acked.ok()) {
    reply_error(reply, acked.error());
  } else {
    resp_simple(reply, "OK");
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_fail(Connection& /*c*/, Args args, std::string& reply) {
  constexpr std::string_view kUsage =
      "usage: FAIL <job_id> <token> [<error> [RETRYIN <ms> | NORETRY]]";
  FailRequest request;
  const auto id = parse_u64(args[1]);
  const auto token = parse_u64(args[2]);
  if (!id || !token) {
    syntax_error(reply, kUsage);
    return Verdict::kReplied;
  }
  request.id = *id;
  request.token = *token;
  if (args.size() > 3) request.error = args[3];
  if (args.size() > 4) {
    const std::string option = upper(args[4]);
    const std::optional<int64_t> retry_in = args.size() == 6 ? parse_i64(args[5]) : std::nullopt;
    if (option == "NORETRY" && args.size() == 5) {
      request.no_retry = true;
    } else if (option == "RETRYIN" && retry_in.has_value()) {
      request.retry_in_ms = retry_in;
    } else {
      syntax_error(reply, kUsage);
      return Verdict::kReplied;
    }
  }
  const auto failed = engine_->fail(request);
  if (!failed.ok()) {
    reply_error(reply, failed.error());
    return Verdict::kReplied;
  }
  resp_array_header(reply, 2);
  resp_bulk(reply, failed->dead ? "dead" : "retry");
  resp_integer(reply, failed->dead ? 0 : failed->retry_at.ms);
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_cancel(Connection& /*c*/, Args args, std::string& reply) {
  const auto id = parse_u64(args[1]);
  if (!id || args.size() != 2) {
    syntax_error(reply, "usage: CANCEL <job_id>");
    return Verdict::kReplied;
  }
  if (const Status cancelled = engine_->cancel(*id); !cancelled.ok()) {
    reply_error(reply, cancelled.error());
  } else {
    resp_simple(reply, "OK");
  }
  return Verdict::kReplied;
}

void Server::append_job_status(std::string& reply, const Job& job, bool with_payload, bool resp3) {
  resp_map_header(reply, with_payload ? 14 : 13, resp3);
  field(reply, "id", static_cast<int64_t>(job.id));
  field(reply, "queue", job.queue->name);
  field(reply, "state", to_string(job.state));
  field(reply, "priority", int64_t{job.priority});
  field(reply, "attempts", int64_t{job.attempts});
  field(reply, "max_attempts", int64_t{job.max_attempts});
  field(reply, "run_at", job.run_at.ms);
  field(reply, "created_at", job.created_at.ms);
  field(reply, "finished_at", job.finished_at.ms);
  field(reply, "lease_expires_at", job.state == JobState::kLeased ? job.lease_expires_at.ms : 0);
  field(reply, "last_error", job.last_error);
  field(reply, "key", job.idem_key);
  field(reply, "payload_size", static_cast<int64_t>(job.payload.size()));
  if (with_payload) field(reply, "payload", job.payload.view());
}

Server::Verdict Server::cmd_status(Connection& c, Args args, std::string& reply) {
  const auto id = parse_u64(args[1]);
  const bool with_payload = args.size() == 3 && upper(args[2]) == "PAYLOAD";
  if (!id || args.size() > 3 || (args.size() == 3 && !with_payload)) {
    syntax_error(reply, "usage: STATUS <job_id> [PAYLOAD]");
    return Verdict::kReplied;
  }
  const Job* job = state_->find_job(*id);
  if (job == nullptr) {
    resp_error(reply, "NOTFOUND", std::format("no such job: {}", *id));
  } else {
    append_job_status(reply, *job, with_payload, c.resp3);
  }
  return Verdict::kReplied;
}

void Server::append_queue_stats(std::string& reply, std::string_view name, const Queue* queue,
                                bool resp3) {
  static const Queue empty_queue{};
  const Queue& q = queue != nullptr ? *queue : empty_queue;
  resp_map_header(reply, 12, resp3);
  field(reply, "queue", name);
  for (size_t i = 0; i < kJobStateCount; ++i) {
    field(reply, to_string(static_cast<JobState>(i)), static_cast<int64_t>(q.counts[i]));
  }
  field(reply, "total_enqueued", static_cast<int64_t>(q.totals.enqueued));
  field(reply, "total_succeeded", static_cast<int64_t>(q.totals.succeeded));
  field(reply, "total_failed_attempts", static_cast<int64_t>(q.totals.failed_attempts));
  field(reply, "total_dead", static_cast<int64_t>(q.totals.dead));
  field(reply, "total_cancelled", static_cast<int64_t>(q.totals.cancelled));
}

Server::Verdict Server::cmd_stats(Connection& c, Args args, std::string& reply) {
  if (args.size() > 2) {
    syntax_error(reply, "usage: STATS [<queue>]");
  } else if (args.size() == 2) {
    append_queue_stats(reply, args[1], state_->find_queue(args[1]), c.resp3);
  } else {
    resp_array_header(reply, state_->queues().size());
    for (const auto& [name, queue] : state_->queues()) {
      append_queue_stats(reply, name, queue.get(), c.resp3);
    }
  }
  return Verdict::kReplied;
}

// --- dead-letter queue ----------------------------------------------------------------------

Server::Verdict Server::cmd_dlq_list(Connection& c, Args args, std::string& reply) {
  constexpr uint64_t kDefaultCount = 100;
  constexpr uint64_t kMaxCount = 1000;
  const auto offset = args.size() > 2 ? parse_u64(args[2]) : std::optional<uint64_t>(0);
  const auto count = args.size() > 3 ? parse_u64(args[3]) : std::optional(kDefaultCount);
  if (!offset || !count || *count == 0 || *count > kMaxCount || args.size() > 4) {
    syntax_error(reply, "usage: DLQ.LIST <queue> [<offset> [<count>]] with 1 <= count <= 1000");
    return Verdict::kReplied;
  }
  const auto page =
      engine_->dlq_list(args[1], static_cast<size_t>(*offset), static_cast<size_t>(*count));
  if (!page.ok()) {
    reply_error(reply, page.error());
    return Verdict::kReplied;
  }
  resp_array_header(reply, page->size());
  for (const Job* job : *page) append_job_status(reply, *job, /*with_payload=*/false, c.resp3);
  return Verdict::kReplied;
}

Server::Verdict Server::dlq_one_or_all(Args args, std::string& reply, bool purge) {
  Result<uint64_t> affected = uint64_t{0};
  if (args.size() == 3 && upper(args[2]) == "ALL") {
    affected = purge ? engine_->dlq_purge_all(args[1]) : engine_->dlq_retry_all(args[1]);
  } else if (const auto id = parse_u64(args[1]); id && args.size() == 2) {
    affected = purge ? engine_->dlq_purge(*id) : engine_->dlq_retry(*id);
  } else {
    syntax_error(reply, std::format("usage: DLQ.{0} <job_id> | DLQ.{0} <queue> ALL",
                                    purge ? "PURGE" : "RETRY"));
    return Verdict::kReplied;
  }
  if (!affected.ok()) {
    reply_error(reply, affected.error());
  } else {
    resp_integer(reply, static_cast<int64_t>(*affected));
  }
  return Verdict::kReplied;
}

Server::Verdict Server::cmd_dlq_retry(Connection& /*c*/, Args args, std::string& reply) {
  return dlq_one_or_all(args, reply, /*purge=*/false);
}

Server::Verdict Server::cmd_dlq_purge(Connection& /*c*/, Args args, std::string& reply) {
  return dlq_one_or_all(args, reply, /*purge=*/true);
}

}  // namespace baton
