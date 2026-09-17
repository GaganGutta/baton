#pragma once

// Engine: baton's command logic, one typed method per command.
//
// Every mutating method follows the same four steps (docs/design.md section 5.1):
//
//   1. validate the request against the current state - errors change nothing;
//   2. resolve all nondeterminism (clock, RNG, id and token counters) into a
//      record;
//   3. append the record to the RecordSink, which assigns its LSN;
//   4. apply the record to State.
//
// The Engine is the only code that reads the clock or the RNG. It does no I/O
// and knows nothing about connections, so it can be driven by tests at full
// speed. last_lsn() tells the caller which LSN a reply must wait for.

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>

#include "common/clock.h"
#include "common/result.h"
#include "common/types.h"
#include "state/records.h"
#include "state/state.h"

namespace baton {

class RecordSink {
 public:
  RecordSink() = default;
  RecordSink(const RecordSink&) = delete;
  RecordSink& operator=(const RecordSink&) = delete;
  virtual ~RecordSink() = default;

  // Appends an encoded record and returns the LSN assigned to it.
  virtual Lsn append(RecordType type, std::string_view payload) = 0;
};

struct EngineOptions {
  // Applied when ENQUEUE does not say otherwise.
  uint32_t default_max_attempts = 10;
  uint32_t default_backoff_base_ms = 1'000;
  uint32_t default_backoff_cap_ms = 600'000;
  DurationMs default_lease_ms = 30'000;
  DurationMs idempotency_window_ms = DurationMs{24} * 60 * 60 * 1000;

  // Limits. Hitting one is a clear error, never silent degradation.
  size_t max_payload_bytes = size_t{1} << 20U;
  uint64_t max_memory_bytes = 0;  // 0: unlimited
  DurationMs min_lease_ms = 100;
  DurationMs max_lease_ms = DurationMs{24} * 60 * 60 * 1000;
  DurationMs max_delay_ms = DurationMs{366} * 24 * 60 * 60 * 1000;
  uint32_t max_max_attempts = 1'000;

  // Time (docs/design.md 7.3, 7.4). After a restart or a wall-clock jump every
  // lease gets at least this long before it can expire, so that workers which
  // could not heartbeat meanwhile are not punished for it.
  DurationMs lease_grace_ms = 5'000;
  // A change of (wall clock - monotonic clock) larger than this between two
  // ticks is a clock step, not slew, and makes the engine re-derive all timers.
  DurationMs clock_jump_threshold_ms = 1'000;

  static constexpr size_t kMaxQueueNameBytes = 128;
  static constexpr size_t kMaxIdemKeyBytes = 256;
  static constexpr size_t kMaxErrorBytes = 1024;  // longer FAIL messages are truncated
};

struct EnqueueRequest {
  std::string_view queue{};
  std::string_view payload{};
  int32_t priority = 0;
  DurationMs delay_ms = 0;           // run no earlier than now + delay
  std::optional<WallTime> run_at{};  // or at an absolute time (wins over delay_ms)
  std::optional<uint32_t> max_attempts{};
  std::optional<uint32_t> backoff_base_ms{};
  std::optional<uint32_t> backoff_cap_ms{};
  std::string_view idem_key{};
};

struct EnqueueResult {
  JobId id = 0;
  bool created = false;  // false: the idempotency key matched an earlier job
};

struct Reservation {
  JobId id = 0;
  LeaseToken token = 0;
  std::string_view queue{};  // owned by State; valid while the queue exists (forever)
  SharedBytes payload{};
  uint32_t attempt = 0;
  uint32_t max_attempts = 0;
  WallTime lease_expires_at{};
};

struct FailRequest {
  JobId id = 0;
  LeaseToken token = 0;
  std::string_view error{};
  std::optional<DurationMs> retry_in_ms{};  // overrides the computed backoff
  bool no_retry = false;                    // straight to the dead-letter queue
};

struct FailResult {
  bool dead = false;
  WallTime retry_at{};  // when !dead
};

// True if `name` is a legal queue name: [A-Za-z0-9._:-]{1,128}.
bool is_valid_queue_name(std::string_view name);

class Engine {
 public:
  Engine(State& state, RecordSink& sink, const Clock& clock, EngineOptions options,
         uint64_t rng_seed, Lsn last_lsn = 0);

  // Samples the clocks, promotes due jobs, expires leases (logging a record for
  // each) and collects garbage. Call once per event-loop iteration, before
  // handling commands.
  void tick();
  // When tick() next has timer work to do, if ever.
  std::optional<MonoTime> next_deadline() const { return state_.next_timer_deadline(); }

  Result<EnqueueResult> enqueue(const EnqueueRequest& request);
  // Leases the best ready job from the first queue in `queues` that has one.
  // nullopt means nothing is ready (the caller may block and retry when
  // State::take_ready_notifications() names one of the queues).
  Result<std::optional<Reservation>> reserve(std::span<const std::string_view> queues,
                                             DurationMs lease_ms);
  // Extends the lease to now + lease_ms; returns the new expiry.
  Result<WallTime> heartbeat(JobId id, LeaseToken token, DurationMs lease_ms);
  Status ack(JobId id, LeaseToken token);
  Result<FailResult> fail(const FailRequest& request);
  Status cancel(JobId id);

  // --- dead-letter queue (docs/design.md 7.5) -----------------------------------------
  // Up to `count` dead jobs of `queue`, oldest first, skipping `offset`.
  Result<std::vector<const Job*>> dlq_list(std::string_view queue, size_t offset,
                                           size_t count) const;
  // Makes dead jobs ready again now, with a fresh set of attempts. Returns how many.
  Result<uint64_t> dlq_retry(JobId id);
  Result<uint64_t> dlq_retry_all(std::string_view queue);
  // Deletes dead jobs. Returns how many.
  Result<uint64_t> dlq_purge(JobId id);
  Result<uint64_t> dlq_purge_all(std::string_view queue);

  // How many wall-clock steps tick() has detected (and re-anchored timers for).
  uint64_t clock_jumps_detected() const { return clock_jumps_; }

  // The LSN of the most recently appended record. A reply generated now must
  // not be sent before this LSN is durable.
  Lsn last_lsn() const { return last_lsn_; }

  const State& state() const { return state_; }
  const EngineOptions& options() const { return options_; }

  // Upper bound (exclusive of jitter) of the retry delay after `attempts` leases:
  // min(cap, base * 2^(attempts-1)). Exposed for tests.
  static DurationMs backoff_ceiling(uint32_t attempts, uint32_t base_ms, uint32_t cap_ms);

 private:
  void log_and_apply(const Record& record);
  void detect_clock_jump();
  // Checks that `token` holds the current lease on job `id`.
  Result<const Job*> find_leased(JobId id, LeaseToken token) const;
  Result<const Job*> find_dead(JobId id) const;
  AttemptFailed decide_failure(const Job& job, FailureReason reason, std::string_view error,
                               std::optional<DurationMs> retry_in_ms, bool no_retry);

  State& state_;
  RecordSink& sink_;
  const Clock& clock_;
  EngineOptions options_;
  std::mt19937_64 rng_;
  Lsn last_lsn_;
  DurationMs clock_offset_ms_ = 0;  // wall - monotonic, as of the previous tick
  uint64_t clock_jumps_ = 0;
  std::string scratch_;                // record encoding buffer
  std::vector<JobId> expired_leases_;  // scratch for tick()
};

}  // namespace baton
