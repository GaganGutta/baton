#pragma once

// State: every job, queue and index baton holds in memory, mutated only by
// apply(record). Design: docs/design.md section 5.
//
// Two kinds of state live here, and the distinction is what makes recovery
// exact:
//
//  - DURABLE state is a pure function of the records applied so far: job
//    fields, the idempotency index, per-queue totals, the id and token counters.
//    apply() never reads a clock or an RNG while producing it.
//  - DERIVED state depends on the current time or exists only for speed: whether
//    a pending job is `scheduled` or `ready`, the ready heaps, the timers, the
//    garbage-collection queues, counts and the memory estimate. It is never
//    persisted and can always be rebuilt from durable state (rebuild_derived),
//    which is exactly what happens after replay, after loading a snapshot and
//    after a wall-clock jump.
//
// Single-threaded: owned by the event loop.

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/clock.h"
#include "common/result.h"
#include "sched/timing_wheel.h"
#include "state/job.h"
#include "state/ready_heap.h"
#include "state/records.h"

namespace baton {

inline constexpr size_t kJobStateCount = 6;

struct QueueTotals {
  uint64_t enqueued = 0;
  uint64_t succeeded = 0;
  uint64_t failed_attempts = 0;  // FAILs and lease expiries, whether retried or not
  uint64_t dead = 0;
  uint64_t cancelled = 0;

  friend bool operator==(const QueueTotals&, const QueueTotals&) = default;
};

struct Queue {
  std::string name;
  QueueTotals totals;                             // durable
  ReadyHeap ready;                                // derived
  std::set<JobId> dead;                           // derived: the dead-letter queue, by id
  std::array<uint64_t, kJobStateCount> counts{};  // derived: jobs currently in each state

  uint64_t count(JobState s) const { return counts[static_cast<size_t>(s)]; }
};

struct IdemEntry {
  std::string key;
  JobId job_id = 0;
  WallTime expires_at;
};

struct StateOptions {
  // How long finished jobs stay visible to STATUS before they are collected.
  DurationMs retain_finished_ms = DurationMs{10} * 60 * 1000;       // succeeded, cancelled
  DurationMs retain_dead_ms = DurationMs{7} * 24 * 60 * 60 * 1000;  // dead-letter queue
};

class State {
 public:
  // Timer kinds, as stored in the timing wheel (which treats them as opaque).
  static constexpr uint8_t kJobDue = 1;
  static constexpr uint8_t kLeaseExpiry = 2;

  explicit State(StateOptions options = {});
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  ~State() = default;

  // --- time -------------------------------------------------------------------
  // The owner samples the clocks once per event-loop iteration and tells State.
  // Derived state (scheduled vs ready, timer deadlines, GC) uses these values;
  // durable state never does.
  void set_now(WallTime wall, MonoTime mono);
  WallTime wall_now() const { return wall_now_; }

  // --- mutation -----------------------------------------------------------------
  // The single mutation path, live and during recovery. An error means the
  // record does not fit the state (e.g. it leases a job that does not exist):
  // live that is a baton bug, in recovery it is a log that cannot be trusted.
  // On error the state is unchanged.
  Status apply(const Record& record);

  // Between begin_replay() and end_replay(), apply() maintains durable state
  // only. end_replay() then rebuilds everything derived, using the current time.
  void begin_replay();
  void end_replay(DurationMs lease_grace_ms = 0);

  // Rebuilds all derived state from durable state and the current time. Leases
  // are given at least `lease_grace_ms` from now before they expire (M4: after a
  // restart or a clock jump, workers must get a chance to heartbeat).
  void rebuild_derived(DurationMs lease_grace_ms = 0);

  // --- timers and garbage collection (live) -------------------------------------
  // Fires due timers. Jobs whose run_at has arrived are promoted to ready here;
  // leases that ran out are reported, because ending one takes a logged record.
  void advance_timers(std::vector<JobId>& expired_leases);
  std::optional<MonoTime> next_timer_deadline() const;
  // Drops expired idempotency keys and finished jobs past their retention.
  void collect_garbage();

  // Queues that gained a ready job since the last call (for blocked RESERVEs).
  std::vector<Queue*> take_ready_notifications();

  // --- queries --------------------------------------------------------------------
  const Job* find_job(JobId id) const;
  const Queue* find_queue(std::string_view name) const;
  Queue* find_queue(std::string_view name);
  const std::map<std::string, std::unique_ptr<Queue>, std::less<>>& queues() const {
    return queues_;
  }
  // The entry for `key` if one is physically present. Callers must compare
  // expires_at with the clock: an expired entry counts as absent.
  const IdemEntry* find_idem(std::string_view key) const;

  JobId next_job_id() const { return next_job_id_; }
  LeaseToken next_token() const { return next_token_; }
  size_t job_count() const { return jobs_.size(); }
  size_t idem_count() const { return idem_index_.size(); }
  size_t pending_timers() const { return wheel_.size(); }
  uint64_t memory_bytes() const { return memory_bytes_; }
  const StateOptions& options() const { return options_; }

  // --- persistence and verification ---------------------------------------------
  // Canonical encoding of the durable state: equal states give equal bytes.
  // The body of a snapshot (M5) and the oracle of the replay-equivalence tests.
  void serialize(std::string& out) const;
  // The returned state is in replay mode, exactly as if the records that
  // produced it had just been applied: apply the rest of the log (if any), then
  // call set_now() and end_replay().
  static Result<std::unique_ptr<State>> deserialize(std::string_view data, StateOptions options);

  // Verifies every structural invariant. O(n); for tests and debugging.
  Status check_invariants() const;

 private:
  struct FinishedRef {
    JobId id = 0;
    WallTime finished_at;
  };

  Status apply_record(const JobEnqueued& r);
  Status apply_record(const JobLeased& r);
  Status apply_record(const LeaseExtended& r);
  Status apply_record(const JobSucceeded& r);
  Status apply_record(const AttemptFailed& r);
  Status apply_record(const JobCancelled& r);
  Status apply_record(const DeadJobRetried& r);
  Status apply_record(const JobsPurged& r);

  Job* find_mutable(JobId id);
  Result<Job*> require_leased(JobId id, LeaseToken token, std::string_view what);
  Queue& get_or_create_queue(std::string_view name);

  static void set_state(Job& job, JobState state);  // keeps the queue's counts in step
  void enter_pending(Job& job);  // heap or due timer, depending on run_at and the clock
  void leave_indexes(Job& job);  // out of the heap / cancel its timer
  void schedule_lease_timer(Job& job, DurationMs grace_ms);
  void finish(Job& job, JobState terminal, WallTime at);
  void erase_job(JobId id);
  void notify_ready(Queue* queue);
  uint64_t mono_deadline(WallTime wall_deadline) const;

  void upsert_idem(std::string_view key, JobId job_id, WallTime expires_at);
  void erase_idem_front();

  static uint64_t job_bytes(const Job& job);
  static uint64_t idem_bytes(const IdemEntry& entry);

  StateOptions options_;
  WallTime wall_now_;
  MonoTime mono_now_;
  bool replaying_ = false;

  // Durable.
  std::unordered_map<JobId, Job> jobs_;
  std::map<std::string, std::unique_ptr<Queue>, std::less<>> queues_;
  JobId next_job_id_ = 1;
  LeaseToken next_token_ = 1;
  // Idempotency index. The list is in insertion order, which is also expiry
  // order while the window is constant, so collection pops from the front; the
  // map's keys are views of the strings the list nodes own.
  std::list<IdemEntry> idem_order_;
  std::unordered_map<std::string_view, std::list<IdemEntry>::iterator> idem_index_;

  // Derived.
  TimingWheel wheel_;
  std::vector<TimerEvent> fired_;        // scratch buffer for advance_timers
  std::deque<FinishedRef> finished_gc_;  // succeeded + cancelled, oldest first
  std::deque<FinishedRef> dead_gc_;      // dead, oldest first
  std::vector<Queue*> ready_notifications_;
  uint64_t memory_bytes_ = 0;
};

}  // namespace baton
