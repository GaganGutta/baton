#include "server/server.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <format>
#include <random>
#include <utility>

#include "common/check.h"
#include "common/logging.h"
#include "log/recovery.h"
#include "net/socket.h"
#include "state/records.h"

namespace baton {
namespace {

constexpr size_t kReadChunk = size_t{64} << 10U;
constexpr size_t kMaxReadsPerEvent = 4;  // fairness: one busy client cannot starve the rest
constexpr int kMaxPollTimeoutMs = 1000;
constexpr DurationMs kDiskCheckIntervalMs = 1000;

}  // namespace

// Adapts the LogWriter to the interface the Engine logs through.
class Server::LogSink final : public RecordSink {
 public:
  explicit LogSink(LogWriter& log) : log_(log) {}
  Lsn append(RecordType type, std::string_view payload) override {
    return log_.append(static_cast<uint8_t>(type), payload);
  }

 private:
  LogWriter& log_;
};

Result<std::unique_ptr<Server>> Server::create(ServerConfig config, FileSystem& fs,
                                               const Clock& clock) {
  auto server = std::make_unique<Server>(Private{}, std::move(config), fs, clock);
  BATON_ASSIGN_OR_RETURN(server->poller_, Poller::create());
  BATON_ASSIGN_OR_RETURN(auto pipe, make_wakeup_pipe());
  server->wake_read_ = std::move(pipe.first);
  server->wake_write_ = std::move(pipe.second);
  BATON_RETURN_IF_ERROR(server->poller_->add(server->wake_read_.get(), true, false));
  BATON_RETURN_IF_ERROR(server->open_storage());
  BATON_RETURN_IF_ERROR(server->start_listening());
  return server;
}

Server::Server(Private /*key*/, ServerConfig config, FileSystem& fs, const Clock& clock)
    : config_(std::move(config)),
      fs_(fs),
      clock_(clock),
      reserve_timeouts_(static_cast<uint64_t>(clock.mono_now().ms)),
      next_disk_check_(clock.mono_now()),
      started_at_(clock.mono_now()) {}

Server::~Server() {
  // The log thread calls wake() on commit; stop it before anything it touches goes away.
  if (log_) log_->stop();
}

// Recovery: replay the log through the same apply() the live path uses.
Status Server::open_storage() {
  BATON_RETURN_IF_ERROR(fs_.create_dir_if_missing(config_.dir));
  BATON_ASSIGN_OR_RETURN(dir_lock_, fs_.lock_dir(config_.dir));

  const MonoTime started = clock_.mono_now();
  state_ = std::make_unique<State>(config_.state);
  state_->begin_replay();
  const auto replay = [this](const LogRecordView& view) -> Status {
    auto record = decode_record(view.type, view.payload);
    if (!record.ok()) {
      return Error{ErrorCode::kCorruption,
                   std::format("LSN {}: {}", view.lsn, record.error().message())};
    }
    if (const Status applied = state_->apply(*record); !applied.ok()) {
      return Error{ErrorCode::kCorruption,
                   std::format("LSN {} does not fit the state rebuilt so far: {}", view.lsn,
                               applied.error().message())};
    }
    return {};
  };
  BATON_ASSIGN_OR_RETURN(const RecoveredLog recovered,
                         recover_log(fs_, config_.dir, LogRecoveryOptions{}, replay));
  state_->set_now(clock_.wall_now(), clock_.mono_now());
  state_->end_replay(config_.lease_grace_ms);

  LogWriterOptions log_options;
  log_options.dir = config_.dir;
  log_options.fsync_policy = config_.fsync;
  log_options.fsync_interval_ms = config_.fsync_interval_ms;
  log_options.segment_size = config_.segment_size;
  log_options.on_commit = [this] { wake(); };
  BATON_ASSIGN_OR_RETURN(log_, LogWriter::open(fs_, std::move(log_options), recovered));
  committed_lsn_ = log_->committed_lsn();

  sink_ = std::make_unique<LogSink>(*log_);
  engine_ = std::make_unique<Engine>(*state_, *sink_, clock_, config_.engine,
                                     std::random_device{}(), recovered.last_lsn);

  recovery_ = RecoveryReport{.records_replayed = recovered.records_replayed,
                             .torn_bytes_truncated = recovered.torn_bytes_truncated,
                             .segments = recovered.segments.size(),
                             .elapsed_ms = clock_.mono_now() - started};
  BATON_INFO("server", "recovered dir={} records={} segments={} last_lsn={} jobs={} ms={}",
             config_.dir, recovery_.records_replayed, recovery_.segments, recovered.last_lsn,
             state_->job_count(), recovery_.elapsed_ms);
  return {};
}

Status Server::start_listening() {
  BATON_ASSIGN_OR_RETURN(listener_, listen_tcp(config_.bind, config_.port));
  BATON_ASSIGN_OR_RETURN(port_, local_port(listener_.get()));
  BATON_RETURN_IF_ERROR(poller_->add(listener_.get(), true, false));
  BATON_INFO("server", "listening addr={}:{} fsync={} auth={}", config_.bind, port_,
             config_.fsync == FsyncPolicy::kAlways ? "always" : "interval",
             config_.requirepass.empty() ? "off" : "on");
  return {};
}

// --- cross-thread control ------------------------------------------------------------

void Server::wake() {
  // Async-signal-safe: an atomic exchange and a write(). The flag keeps a burst
  // of commits from filling the pipe.
  if (!wake_pending_.exchange(true)) {
    const char byte = 1;
    // If the pipe is full the loop is about to wake up anyway.
    [[maybe_unused]] const ssize_t written = ::write(wake_write_.get(), &byte, 1);
  }
}

void Server::stop() {
  stop_requested_.store(true);
  wake_pending_.store(false);  // make sure this wake-up is written
  wake();
}

// --- the loop ---------------------------------------------------------------------------

Status Server::run() {
  std::vector<PollEvent> events;
  std::array<char, 256> drain{};
  while (!stop_requested_.load()) {
    events.clear();
    BATON_RETURN_IF_ERROR(poller_->wait(poll_timeout_ms(), events));
    wake_pending_.store(false);
    while (read_some(wake_read_.get(), drain.data(), drain.size()).status == IoStatus::kOk) {
    }

    engine_->tick();
    serve_ready_waiters();  // jobs that became due, retries whose backoff ended

    for (const PollEvent& event : events) {
      if (event.fd == wake_read_.get()) continue;
      if (event.fd == listener_.get()) {
        accept_connections();
        continue;
      }
      // Look the connection up for every event: an earlier event in this batch
      // may have closed it.
      auto it = connections_.find(event.fd);
      if (it == connections_.end()) continue;
      if (event.readable || event.hangup) on_readable(*it->second);
      it = connections_.find(event.fd);
      if (it != connections_.end() && event.writable) on_writable(*it->second);
    }

    expire_reserve_timeouts();
    while (!resume_.empty()) {
      const uint64_t id = resume_.back();
      resume_.pop_back();
      if (Connection* c = find(id)) process_input(*c);
    }

    // Everything this iteration logged goes to the log thread as one batch.
    log_->flush();
    release_durable_replies();
    apply_backpressure();
    check_disk_space();
  }
  shutdown();
  return {};
}

int Server::poll_timeout_ms() const {
  if (!resume_.empty()) return 0;
  const MonoTime now = clock_.mono_now();
  DurationMs timeout = kMaxPollTimeoutMs;
  if (const auto deadline = engine_->next_deadline()) {
    timeout = std::min(timeout, *deadline - now);
  }
  if (const auto deadline = reserve_timeouts_.next_wakeup()) {
    timeout = std::min(timeout, static_cast<DurationMs>(*deadline) - now.ms);
  }
  return static_cast<int>(std::clamp<DurationMs>(timeout, 0, kMaxPollTimeoutMs));
}

void Server::accept_connections() {
  for (;;) {
    auto accepted = accept_connection(listener_.get());
    if (!accepted.ok()) {
      BATON_WARN("server", "accept failed: {}", accepted.error().to_string());
      return;
    }
    if (!accepted->has_value()) return;
    Fd fd = std::move(**accepted);

    if (connections_.size() >= config_.max_connections) {
      ++counters_.connections_rejected;
      std::string refusal;
      resp_error(refusal, "LIMIT", "max-connections reached, try again later");
      (void)write_some(fd.get(), refusal);  // best effort; the socket closes right after
      continue;
    }

    RespLimits limits;
    limits.max_bulk_bytes = config_.engine.max_payload_bytes;
    limits.max_request_bytes = config_.engine.max_payload_bytes + (size_t{64} << 10U);
    const int raw = fd.get();
    auto connection = std::make_unique<Connection>(next_connection_id_++, std::move(fd), limits);
    connection->authenticated = config_.requirepass.empty();
    if (const Status added = poller_->add(raw, true, false); !added.ok()) {
      BATON_WARN("server", "cannot watch new connection: {}", added.error().to_string());
      continue;
    }
    connection->registered_read = true;
    by_id_[connection->id] = connection.get();
    connections_[raw] = std::move(connection);
    ++counters_.connections_accepted;
  }
}

void Server::on_readable(Connection& c) {
  if (reads_paused_) {
    // The log is behind: stop reading so that TCP pushes back on the client.
    c.read_paused = true;
    paused_reads_.push_back(c.id);
    update_interest(c);
    return;
  }
  for (size_t reads = 0; reads < kMaxReadsPerEvent; ++reads) {
    const size_t old_size = c.in.size();
    c.in.resize(old_size + kReadChunk);
    const IoResult result = read_some(c.fd.get(), c.in.data() + old_size, kReadChunk);
    c.in.resize(old_size + (result.status == IoStatus::kOk ? result.bytes : 0));
    if (result.status == IoStatus::kWouldBlock) break;
    if (result.status != IoStatus::kOk) {
      close_connection(c);  // EOF or error: the client is gone
      return;
    }
    if (result.bytes < kReadChunk) break;
  }

  // A parked connection only buffers. If it pipelines a lot behind its RESERVE,
  // stop reading until it is unparked.
  if (c.blocked && c.in.size() - c.in_offset > config_.engine.max_payload_bytes) {
    c.read_paused = true;
    update_interest(c);
    return;
  }
  process_input(c);
}

void Server::on_writable(Connection& c) { flush(c); }

void Server::process_input(Connection& c) {
  const uint64_t id = c.id;
  RespRequest request;
  std::string error;
  while (!c.blocked && !c.close_after_flush) {
    const std::string_view unparsed = std::string_view(c.in).substr(c.in_offset);
    const RespParser::Outcome outcome = c.parser.parse(unparsed, request, error);
    c.in_offset += outcome.consumed;
    if (outcome.status == RespParser::Status::kNeedMore) break;
    if (outcome.status == RespParser::Status::kError) {
      ++counters_.protocol_errors;
      std::string reply;
      resp_error(reply, "ERR", "Protocol error: " + error);
      c.close_after_flush = true;
      queue_reply(c, reply);
      break;
    }
    execute(c, request);
    if (find(id) == nullptr) return;  // the command closed the connection
  }

  if (c.in_offset == c.in.size()) {
    c.in.clear();
    c.in_offset = 0;
  } else if (c.in_offset >= kReadChunk) {
    c.in.erase(0, c.in_offset);
    c.in_offset = 0;
  }
  update_interest(c);
}

void Server::execute(Connection& c, const RespRequest& request) {
  ++counters_.commands_processed;
  std::string& reply = reply_scratch_;
  reply.clear();

  if (request.oversized) {
    resp_error(
        reply, "LIMIT",
        std::format("argument exceeds max-payload of {} bytes", config_.engine.max_payload_bytes));
  } else {
    std::string name(request.args[0].substr(0, 32));
    std::ranges::transform(name, name.begin(), [](unsigned char ch) { return std::toupper(ch); });
    const Command* command = find_command(name);
    if (command == nullptr) {
      resp_error(reply, "ERR", std::format("unknown command '{}'", name));
    } else if (request.args.size() < command->min_args) {
      resp_error(reply, "ERR", std::format("wrong number of arguments for '{}'", name));
    } else if (command->needs_auth && !c.authenticated) {
      resp_error(reply, "NOAUTH", "Authentication required.");
    } else if ((this->*command->handler)(c, request.args, reply) == Verdict::kParked) {
      return;  // a RESERVE that is waiting: its reply comes later
    }
  }
  queue_reply(c, reply);
  serve_ready_waiters();  // this command may have made jobs ready for parked RESERVEs
}

// --- replies ------------------------------------------------------------------------------

// Every reply waits for the LSN of the last record logged so far: the log
// commits in order, so that covers everything the reply could depend on.
void Server::queue_reply(Connection& c, std::string_view bytes) {
  const Lsn lsn = engine_->last_lsn();
  if (c.marks.empty() && lsn <= committed_lsn_) {
    c.out.append(bytes);  // nothing in flight: no need to wait
  } else {
    c.gated.append(bytes);
    if (!c.marks.empty() && c.marks.back().lsn == lsn) {
      c.marks.back().end = c.gated.size();
    } else {
      c.marks.push_back(Connection::Mark{.lsn = lsn, .end = c.gated.size()});
      gated_.push_back(Gated{.lsn = lsn, .connection = c.id});
    }
  }
  if (!c.dirty) {
    c.dirty = true;
    dirty_.push_back(c.id);
  }
}

void Server::release_durable_replies() {
  committed_lsn_ = log_->committed_lsn();
  while (!gated_.empty() && gated_.front().lsn <= committed_lsn_) {
    const uint64_t id = gated_.front().connection;
    gated_.pop_front();
    Connection* c = find(id);
    if (c == nullptr) continue;
    size_t end = 0;
    while (!c->marks.empty() && c->marks.front().lsn <= committed_lsn_) {
      end = c->marks.front().end;
      c->marks.pop_front();
    }
    if (end == 0) continue;  // an earlier entry already released these
    c->out.append(c->gated, 0, end);
    c->gated.erase(0, end);
    for (Connection::Mark& mark : c->marks) mark.end -= end;
    if (!c->dirty) {
      c->dirty = true;
      dirty_.push_back(id);
    }
  }

  // One write per connection per iteration, however many replies it got.
  std::vector<uint64_t> dirty;
  dirty.swap(dirty_);
  for (const uint64_t id : dirty) {
    Connection* c = find(id);
    if (c == nullptr) continue;
    c->dirty = false;
    if (c->unsent_bytes() > config_.max_output_buffer_bytes) {
      BATON_WARN("server", "closing connection id={}: client is not reading replies", id);
      close_connection(*c);
      continue;
    }
    flush(*c);
  }
}

void Server::flush(Connection& c) {
  while (c.out_offset < c.out.size()) {
    const IoResult result = write_some(c.fd.get(), std::string_view(c.out).substr(c.out_offset));
    if (result.status == IoStatus::kWouldBlock) break;
    if (result.status != IoStatus::kOk) {
      close_connection(c);
      return;
    }
    c.out_offset += result.bytes;
  }
  if (c.out_offset == c.out.size()) {
    c.out.clear();
    c.out_offset = 0;
  }
  c.want_write = c.out_offset < c.out.size();
  if (c.close_after_flush && !c.want_write && c.gated.empty()) {
    close_connection(c);
    return;
  }
  update_interest(c);
}

// --- connections -----------------------------------------------------------------------------

Connection* Server::find(uint64_t id) {
  const auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

void Server::update_interest(Connection& c) {
  const bool want_read = !c.read_paused && !c.close_after_flush;
  if (want_read == c.registered_read && c.want_write == c.registered_write) return;
  if (const Status s = poller_->modify(c.fd.get(), want_read, c.want_write); !s.ok()) {
    BATON_WARN("server", "poller modify failed: {}", s.error().to_string());
    return;
  }
  c.registered_read = want_read;
  c.registered_write = c.want_write;
}

void Server::close_connection(Connection& c) {
  if (c.blocked) unpark(c);
  poller_->remove(c.fd.get());
  by_id_.erase(c.id);
  connections_.erase(c.fd.get());  // destroys c and closes the socket
}

// --- parked RESERVEs -----------------------------------------------------------------------------

bool Server::try_reserve(Connection& /*c*/, const std::vector<std::string>& queues,
                         DurationMs lease_ms, std::string& reply) {
  std::vector<std::string_view> names(queues.begin(), queues.end());
  const auto reserved = engine_->reserve(names, lease_ms);
  if (!reserved.ok()) {
    resp_error(reply, "ERR", reserved.error().message());
    return true;
  }
  if (!reserved->has_value()) return false;
  const Reservation& r = **reserved;
  resp_array_header(reply, 7);
  resp_integer(reply, static_cast<int64_t>(r.id));
  resp_integer(reply, static_cast<int64_t>(r.token));
  resp_bulk(reply, r.queue);
  resp_bulk(reply, r.payload.view());
  resp_integer(reply, r.attempt);
  resp_integer(reply, r.max_attempts);
  resp_integer(reply, r.lease_expires_at.ms);
  return true;
}

void Server::park(Connection& c, Connection::Blocked blocked, DurationMs timeout_ms) {
  // De-duplicate so a queue named twice does not hold two places in line.
  std::vector<std::string> unique;
  for (std::string& queue : blocked.queues) {
    if (std::ranges::find(unique, queue) == unique.end()) unique.push_back(std::move(queue));
  }
  blocked.queues = std::move(unique);
  for (const std::string& queue : blocked.queues) waiters_[queue].push_back(c.id);
  blocked.timeout =
      reserve_timeouts_.schedule(static_cast<uint64_t>(clock_.mono_now().ms + timeout_ms), 0, c.id);
  c.blocked = std::move(blocked);
}

void Server::unpark(Connection& c) {
  BATON_CHECK(c.blocked.has_value());
  for (const std::string& queue : c.blocked->queues) {
    const auto it = waiters_.find(queue);
    if (it != waiters_.end()) std::erase(it->second, c.id);
  }
  reserve_timeouts_.cancel(c.blocked->timeout);
  c.blocked.reset();
  if (c.read_paused && !reads_paused_) c.read_paused = false;
}

// Hands newly ready jobs to parked RESERVEs, longest-waiting connection first.
// Unparked connections are resumed by the loop, not here, so this never recurses.
void Server::serve_ready_waiters() {
  for (const Queue* queue : state_->take_ready_notifications()) {
    const auto it = waiters_.find(queue->name);
    if (it == waiters_.end()) continue;
    while (!it->second.empty() && !queue->ready.empty()) {
      Connection* c = find(it->second.front());
      BATON_CHECK(c != nullptr, "stale waiter on queue {}", queue->name);
      BATON_CHECK(c->blocked.has_value(), "waiter on queue {} is not parked", queue->name);
      std::string reply;
      if (!try_reserve(*c, c->blocked->queues, c->blocked->lease_ms, reply)) break;
      unpark(*c);
      queue_reply(*c, reply);
      resume_.push_back(c->id);
    }
  }
}

void Server::expire_reserve_timeouts() {
  fired_.clear();
  reserve_timeouts_.advance(static_cast<uint64_t>(clock_.mono_now().ms), fired_);
  for (const TimerEvent& event : fired_) {
    Connection* c = find(event.id);
    if (c == nullptr || !c->blocked || c->blocked->timeout != event.handle) continue;
    unpark(*c);
    std::string reply;
    resp_null_array(reply);
    queue_reply(*c, reply);
    resume_.push_back(c->id);
  }
}

// --- backpressure, disk, shutdown -------------------------------------------------------------

void Server::apply_backpressure() {
  const bool behind = log_->backlog_bytes() > config_.max_log_backlog_bytes;
  if (behind && !reads_paused_) {
    reads_paused_ = true;
    BATON_WARN("server", "log backlog {} bytes: pausing client reads", log_->backlog_bytes());
  } else if (!behind && reads_paused_) {
    reads_paused_ = false;
    for (const uint64_t id : paused_reads_) {
      Connection* c = find(id);
      if (c == nullptr) continue;
      c->read_paused = false;
      update_interest(*c);
    }
    paused_reads_.clear();
  }
}

void Server::check_disk_space() {
  const MonoTime now = clock_.mono_now();
  if (now < next_disk_check_) return;
  next_disk_check_ = now + kDiskCheckIntervalMs;
  const auto available = fs_.available_bytes(config_.dir);
  if (!available.ok()) return;
  const bool low = *available < 2 * config_.segment_size;
  if (low != disk_low_) {
    BATON_WARN("server", "disk space {}: {} bytes free; ENQUEUE is {}", low ? "low" : "recovered",
               *available, low ? "refused" : "accepted again");
  }
  disk_low_ = low;
}

void Server::shutdown() {
  BATON_INFO("server", "shutting down connections={}", connections_.size());
  std::vector<uint64_t> ids;
  ids.reserve(by_id_.size());
  for (const auto& [id, connection] : by_id_) ids.push_back(id);
  for (const uint64_t id : ids) {
    Connection* c = find(id);
    if (c == nullptr || !c->blocked) continue;
    unpark(*c);
    std::string reply;
    resp_null_array(reply);
    queue_reply(*c, reply);
  }
  log_->stop();               // flush, fsync, join: everything logged is now durable
  release_durable_replies();  // ...so every waiting reply may go
  connections_.clear();
  by_id_.clear();
  BATON_INFO("server", "stopped last_lsn={}", engine_->last_lsn());
}

}  // namespace baton
