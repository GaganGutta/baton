#include "state/engine.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iterator>

#include "common/check.h"
#include "common/logging.h"

namespace baton {
namespace {

Error invalid(std::string what) { return Error{ErrorCode::kInvalidArgument, std::move(what)}; }
Error limit(std::string what) { return Error{ErrorCode::kLimitExceeded, std::move(what)}; }

// A generous estimate of what one more job will cost, for the memory limit.
uint64_t estimated_job_bytes(const EnqueueRequest& request) {
  return sizeof(Job) + 256 + request.payload.size() + (2 * request.idem_key.size());
}

}  // namespace

bool is_valid_queue_name(std::string_view name) {
  if (name.empty() || name.size() > EngineOptions::kMaxQueueNameBytes) return false;
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == ':' || c == '-';
  });
}

Engine::Engine(State& state, RecordSink& sink, const Clock& clock, EngineOptions options,
               uint64_t rng_seed, Lsn last_lsn)
    : state_(state),
      sink_(sink),
      clock_(clock),
      options_(options),
      rng_(rng_seed),
      last_lsn_(last_lsn) {
  const WallTime wall = clock_.wall_now();
  const MonoTime mono = clock_.mono_now();
  state_.set_now(wall, mono);
  clock_offset_ms_ = wall.ms - mono.ms;
}

// Apply before append: if a handler ever built a record that does not fit the
// state, that is a baton bug and we stop here, before the record can reach the
// log and make the data directory unrecoverable.
void Engine::log_and_apply(const Record& record) {
  const Status applied = state_.apply(record);
  BATON_CHECK(applied.ok(), "handler produced an inapplicable record: {}",
              applied.error().to_string());
  scratch_.clear();
  encode_record(record, scratch_);
  last_lsn_ = sink_.append(type_of(record), scratch_);
}

// A wall-clock step is handled exactly like a restart: every timer is re-derived
// from its persisted wall-clock deadline, and leases get the grace period
// (docs/design.md 7.4). Slew never comes close to the threshold between ticks.
void Engine::detect_clock_jump() {
  const DurationMs offset = state_.wall_now().ms - clock_.mono_now().ms;
  const DurationMs change = offset - clock_offset_ms_;
  clock_offset_ms_ = offset;
  if (std::abs(change) <= options_.clock_jump_threshold_ms) return;
  ++clock_jumps_;
  BATON_WARN("engine", "wall clock stepped by {} ms: re-deriving timers from their deadlines",
             change);
  state_.rebuild_derived(options_.lease_grace_ms);
}

void Engine::tick() {
  state_.set_now(clock_.wall_now(), clock_.mono_now());
  detect_clock_jump();

  expired_leases_.clear();
  state_.advance_timers(expired_leases_);
  for (const JobId id : expired_leases_) {
    const Job* job = state_.find_job(id);
    if (job == nullptr || job->state != JobState::kLeased) continue;
    log_and_apply(decide_failure(*job, FailureReason::kLeaseExpired, "lease expired", std::nullopt,
                                 /*no_retry=*/false));
  }
  state_.collect_garbage();
}

Result<EnqueueResult> Engine::enqueue(const EnqueueRequest& request) {
  const WallTime now = state_.wall_now();
  if (!is_valid_queue_name(request.queue)) {
    return invalid("queue name must match [A-Za-z0-9._:-]{1,128}");
  }
  if (request.payload.size() > options_.max_payload_bytes) {
    return limit(std::format("payload of {} bytes exceeds max-payload of {} bytes",
                             request.payload.size(), options_.max_payload_bytes));
  }
  if (request.idem_key.size() > EngineOptions::kMaxIdemKeyBytes) {
    return invalid(
        std::format("idempotency key longer than {} bytes", EngineOptions::kMaxIdemKeyBytes));
  }
  if (request.delay_ms < 0 || request.delay_ms > options_.max_delay_ms) {
    return invalid(std::format("delay must be between 0 and {} ms", options_.max_delay_ms));
  }
  if (request.run_at && *request.run_at - now > options_.max_delay_ms) {
    return invalid(std::format("run-at is more than {} ms in the future", options_.max_delay_ms));
  }
  const uint32_t max_attempts = request.max_attempts.value_or(options_.default_max_attempts);
  if (max_attempts == 0 || max_attempts > options_.max_max_attempts) {
    return invalid(std::format("max attempts must be between 1 and {}", options_.max_max_attempts));
  }
  const uint32_t backoff_base = request.backoff_base_ms.value_or(options_.default_backoff_base_ms);
  const uint32_t backoff_cap =
      std::max(backoff_base, request.backoff_cap_ms.value_or(options_.default_backoff_cap_ms));

  // Idempotency: a live key answers with the job it already created.
  if (!request.idem_key.empty()) {
    const IdemEntry* entry = state_.find_idem(request.idem_key);
    if (entry != nullptr && entry->expires_at > now) {
      return EnqueueResult{.id = entry->job_id, .created = false};
    }
  }

  if (options_.max_memory_bytes != 0 &&
      state_.memory_bytes() + estimated_job_bytes(request) > options_.max_memory_bytes) {
    return limit(
        std::format("max-memory of {} bytes reached; ACK or purge jobs, or raise the limit",
                    options_.max_memory_bytes));
  }

  JobEnqueued record;
  record.id = state_.next_job_id();
  record.queue = request.queue;
  record.payload = request.payload;
  record.priority = request.priority;
  // A run-at in the past must not let a job jump the FIFO order.
  record.run_at = std::max(now, request.run_at.value_or(now + request.delay_ms));
  record.max_attempts = max_attempts;
  record.backoff_base_ms = backoff_base;
  record.backoff_cap_ms = backoff_cap;
  record.idem_key = request.idem_key;
  if (!request.idem_key.empty()) record.idem_expires_at = now + options_.idempotency_window_ms;
  record.at = now;
  log_and_apply(record);
  return EnqueueResult{.id = record.id, .created = true};
}

Result<std::optional<Reservation>> Engine::reserve(std::span<const std::string_view> queues,
                                                   DurationMs lease_ms) {
  if (queues.empty()) return invalid("at least one queue is required");
  for (const std::string_view name : queues) {
    if (!is_valid_queue_name(name)) return invalid("queue name must match [A-Za-z0-9._:-]{1,128}");
  }
  if (lease_ms < options_.min_lease_ms || lease_ms > options_.max_lease_ms) {
    return invalid(std::format("lease must be between {} and {} ms", options_.min_lease_ms,
                               options_.max_lease_ms));
  }

  const WallTime now = state_.wall_now();
  for (const std::string_view name : queues) {
    const Queue* queue = state_.find_queue(name);
    if (queue == nullptr || queue->ready.empty()) continue;
    const Job* job = queue->ready.top();

    log_and_apply(JobLeased{.id = job->id,
                            .token = state_.next_token(),
                            .lease_expires_at = now + lease_ms,
                            .at = now});
    return std::optional<Reservation>(Reservation{.id = job->id,
                                                  .token = job->lease_token,
                                                  .queue = queue->name,
                                                  .payload = job->payload,
                                                  .attempt = job->attempts,
                                                  .max_attempts = job->max_attempts,
                                                  .lease_expires_at = job->lease_expires_at});
  }
  return std::optional<Reservation>();
}

Result<const Job*> Engine::find_leased(JobId id, LeaseToken token) const {
  const Job* job = state_.find_job(id);
  if (job == nullptr) return Error{ErrorCode::kNotFound, std::format("no such job: {}", id)};
  if (job->state != JobState::kLeased) {
    return Error{ErrorCode::kStaleToken, std::format("job {} is {}, not leased: the lease was lost",
                                                     id, to_string(job->state))};
  }
  if (job->lease_token != token) {
    return Error{
        ErrorCode::kStaleToken,
        std::format("token {} is not the current lease of job {}: it was leased again", token, id)};
  }
  return job;
}

Result<WallTime> Engine::heartbeat(JobId id, LeaseToken token, DurationMs lease_ms) {
  if (lease_ms < options_.min_lease_ms || lease_ms > options_.max_lease_ms) {
    return invalid(std::format("lease must be between {} and {} ms", options_.min_lease_ms,
                               options_.max_lease_ms));
  }
  BATON_ASSIGN_OR_RETURN(const Job* job, find_leased(id, token));
  const WallTime now = state_.wall_now();
  const WallTime expires_at = now + lease_ms;
  log_and_apply(
      LeaseExtended{.id = job->id, .token = token, .lease_expires_at = expires_at, .at = now});
  return expires_at;
}

Status Engine::ack(JobId id, LeaseToken token) {
  BATON_ASSIGN_OR_RETURN(const Job* job, find_leased(id, token));
  log_and_apply(JobSucceeded{.id = job->id, .token = token, .at = state_.wall_now()});
  return {};
}

Result<FailResult> Engine::fail(const FailRequest& request) {
  if (request.retry_in_ms &&
      (*request.retry_in_ms < 0 || *request.retry_in_ms > options_.max_delay_ms)) {
    return invalid(std::format("retry-in must be between 0 and {} ms", options_.max_delay_ms));
  }
  BATON_ASSIGN_OR_RETURN(const Job* job, find_leased(request.id, request.token));
  const AttemptFailed record = decide_failure(*job, FailureReason::kWorkerFailed, request.error,
                                              request.retry_in_ms, request.no_retry);
  log_and_apply(record);
  return FailResult{.dead = record.dead, .retry_at = record.retry_at};
}

Status Engine::cancel(JobId id) {
  const Job* job = state_.find_job(id);
  if (job == nullptr) return Error{ErrorCode::kNotFound, std::format("no such job: {}", id)};
  if (is_terminal(job->state)) {
    return Error{ErrorCode::kFailedPrecondition,
                 std::format("job {} is already {}", id, to_string(job->state))};
  }
  log_and_apply(JobCancelled{.id = id, .at = state_.wall_now()});
  return {};
}

// --- dead-letter queue ---------------------------------------------------------------

namespace {

constexpr size_t kPurgeChunk = 10'000;  // ids per JobsPurged record

}  // namespace

Result<const Job*> Engine::find_dead(JobId id) const {
  const Job* job = state_.find_job(id);
  if (job == nullptr) return Error{ErrorCode::kNotFound, std::format("no such job: {}", id)};
  if (job->state != JobState::kDead) {
    return Error{ErrorCode::kFailedPrecondition,
                 std::format("job {} is {}, not dead", id, to_string(job->state))};
  }
  return job;
}

Result<std::vector<const Job*>> Engine::dlq_list(std::string_view queue, size_t offset,
                                                 size_t count) const {
  if (!is_valid_queue_name(queue)) return invalid("queue name must match [A-Za-z0-9._:-]{1,128}");
  std::vector<const Job*> page;
  const Queue* q = state_.find_queue(queue);
  if (q == nullptr || offset >= q->dead.size()) return page;
  auto it = q->dead.begin();
  std::advance(it, static_cast<std::ptrdiff_t>(offset));
  for (; it != q->dead.end() && page.size() < count; ++it) page.push_back(state_.find_job(*it));
  return page;
}

Result<uint64_t> Engine::dlq_retry(JobId id) {
  BATON_ASSIGN_OR_RETURN(const Job* job, find_dead(id));
  const WallTime now = state_.wall_now();
  log_and_apply(DeadJobRetried{.id = job->id, .run_at = now, .at = now});
  return uint64_t{1};
}

Result<uint64_t> Engine::dlq_retry_all(std::string_view queue) {
  if (!is_valid_queue_name(queue)) return invalid("queue name must match [A-Za-z0-9._:-]{1,128}");
  const Queue* q = state_.find_queue(queue);
  if (q == nullptr) return uint64_t{0};
  // Copy the ids: applying a retry removes the job from the set being walked.
  const std::vector<JobId> ids(q->dead.begin(), q->dead.end());
  const WallTime now = state_.wall_now();
  for (const JobId id : ids) log_and_apply(DeadJobRetried{.id = id, .run_at = now, .at = now});
  return static_cast<uint64_t>(ids.size());
}

Result<uint64_t> Engine::dlq_purge(JobId id) {
  BATON_ASSIGN_OR_RETURN(const Job* job, find_dead(id));
  log_and_apply(JobsPurged{.ids = {job->id}});
  return uint64_t{1};
}

Result<uint64_t> Engine::dlq_purge_all(std::string_view queue) {
  if (!is_valid_queue_name(queue)) return invalid("queue name must match [A-Za-z0-9._:-]{1,128}");
  const Queue* q = state_.find_queue(queue);
  if (q == nullptr) return uint64_t{0};
  const std::vector<JobId> ids(q->dead.begin(), q->dead.end());
  for (size_t start = 0; start < ids.size(); start += kPurgeChunk) {
    const size_t end = std::min(ids.size(), start + kPurgeChunk);
    log_and_apply(JobsPurged{.ids = std::vector<JobId>(ids.begin() + static_cast<long>(start),
                                                       ids.begin() + static_cast<long>(end))});
  }
  return static_cast<uint64_t>(ids.size());
}

DurationMs Engine::backoff_ceiling(uint32_t attempts, uint32_t base_ms, uint32_t cap_ms) {
  const uint32_t doublings = attempts == 0 ? 0 : attempts - 1;
  if (doublings >= 32) return cap_ms;
  const uint64_t ceiling = uint64_t{base_ms} << doublings;
  return static_cast<DurationMs>(std::min<uint64_t>(ceiling, cap_ms));
}

// Decides what a failed attempt leads to, and resolves the retry time here so
// that replay never needs the RNG. Backoff is exponential with full jitter: a
// uniform draw from [0, min(cap, base * 2^(attempts-1))], which spreads a burst
// of failures out instead of retrying them in lockstep.
AttemptFailed Engine::decide_failure(const Job& job, FailureReason reason, std::string_view error,
                                     std::optional<DurationMs> retry_in_ms, bool no_retry) {
  const WallTime now = state_.wall_now();
  AttemptFailed record;
  record.id = job.id;
  record.token = job.lease_token;
  record.reason = reason;
  record.error = error.substr(0, EngineOptions::kMaxErrorBytes);
  record.at = now;
  if (no_retry || job.attempts >= job.max_attempts) {
    record.dead = true;
    return record;
  }
  DurationMs delay = 0;
  if (retry_in_ms) {
    delay = *retry_in_ms;
  } else {
    const DurationMs ceiling =
        backoff_ceiling(job.attempts, job.backoff_base_ms, job.backoff_cap_ms);
    delay = std::uniform_int_distribution<DurationMs>(0, ceiling)(rng_);
  }
  record.retry_at = now + delay;
  return record;
}

}  // namespace baton
