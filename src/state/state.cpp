#include "state/state.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/check.h"
#include "common/codec.h"

namespace baton {
namespace {

constexpr uint8_t kSerializationVersion = 1;

// Approximate per-entry overheads of the node-based containers, for the memory
// estimate. M8 compares the estimate with the process RSS.
constexpr uint64_t kJobMapNodeBytes = 32;
constexpr uint64_t kReadyHeapSlotBytes = sizeof(Job*);
constexpr uint64_t kIdemNodeBytes = 96;  // list node + index node, excluding the key's heap

uint64_t string_heap_bytes(const std::string& s) {
  static const size_t inline_capacity = std::string().capacity();
  return s.capacity() <= inline_capacity ? 0 : s.capacity() + 1;
}

// The durable part of JobState: `scheduled` and `ready` are one state on disk.
enum class PersistedState : uint8_t { kPending = 0, kLeased, kSucceeded, kDead, kCancelled };

PersistedState to_persisted(JobState s) {
  switch (s) {
    case JobState::kScheduled:
    case JobState::kReady:
      return PersistedState::kPending;
    case JobState::kLeased:
      return PersistedState::kLeased;
    case JobState::kSucceeded:
      return PersistedState::kSucceeded;
    case JobState::kDead:
      return PersistedState::kDead;
    case JobState::kCancelled:
      return PersistedState::kCancelled;
  }
  BATON_UNREACHABLE();
}

JobState from_persisted(PersistedState s) {
  switch (s) {
    case PersistedState::kPending:
      return JobState::kScheduled;  // rebuild_derived() promotes it if it is due
    case PersistedState::kLeased:
      return JobState::kLeased;
    case PersistedState::kSucceeded:
      return JobState::kSucceeded;
    case PersistedState::kDead:
      return JobState::kDead;
    case PersistedState::kCancelled:
      return JobState::kCancelled;
  }
  BATON_UNREACHABLE();
}

Error mismatch(std::string what) { return Error{ErrorCode::kFailedPrecondition, std::move(what)}; }

}  // namespace

std::string_view to_string(JobState state) {
  switch (state) {
    case JobState::kScheduled:
      return "scheduled";
    case JobState::kReady:
      return "ready";
    case JobState::kLeased:
      return "leased";
    case JobState::kSucceeded:
      return "succeeded";
    case JobState::kDead:
      return "dead";
    case JobState::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

State::State(StateOptions options) : options_(options) {}

void State::set_now(WallTime wall, MonoTime mono) {
  wall_now_ = wall;
  mono_now_ = mono;
}

// --- helpers -------------------------------------------------------------------------

Job* State::find_mutable(JobId id) {
  const auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}

const Job* State::find_job(JobId id) const {
  const auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}

const Queue* State::find_queue(std::string_view name) const {
  const auto it = queues_.find(name);
  return it == queues_.end() ? nullptr : it->second.get();
}

Queue* State::find_queue(std::string_view name) {
  const auto it = queues_.find(name);
  return it == queues_.end() ? nullptr : it->second.get();
}

Queue& State::get_or_create_queue(std::string_view name) {
  if (Queue* existing = find_queue(name)) return *existing;
  auto queue = std::make_unique<Queue>();
  queue->name = std::string(name);
  Queue& ref = *queue;
  queues_.emplace(ref.name, std::move(queue));
  return ref;
}

const IdemEntry* State::find_idem(std::string_view key) const {
  const auto it = idem_index_.find(key);
  return it == idem_index_.end() ? nullptr : &*it->second;
}

Result<Job*> State::require_leased(JobId id, LeaseToken token, std::string_view what) {
  Job* job = find_mutable(id);
  if (job == nullptr) return mismatch(std::format("{}: job {} does not exist", what, id));
  if (job->state != JobState::kLeased || job->lease_token != token) {
    return mismatch(std::format("{}: job {} is {} with token {}, record has token {}", what, id,
                                to_string(job->state), job->lease_token, token));
  }
  return job;
}

void State::set_state(Job& job, JobState state) {
  --job.queue->counts[static_cast<size_t>(job.state)];
  job.state = state;
  ++job.queue->counts[static_cast<size_t>(state)];
}

uint64_t State::mono_deadline(WallTime wall_deadline) const {
  const DurationMs delay = std::max<DurationMs>(0, wall_deadline - wall_now_);
  return static_cast<uint64_t>(mono_now_.ms + delay);
}

void State::notify_ready(Queue* queue) {
  if (std::ranges::find(ready_notifications_, queue) == ready_notifications_.end()) {
    ready_notifications_.push_back(queue);
  }
}

// A pending job is `ready` (in the heap) if its run_at has arrived and
// `scheduled` (waiting on a due timer) otherwise. Which one is derived state:
// during replay the job just stays `scheduled`, and rebuild_derived() sorts it
// out afterwards against the real clock.
void State::enter_pending(Job& job) {
  if (replaying_ || job.run_at > wall_now_) {
    set_state(job, JobState::kScheduled);
    if (!replaying_) job.timer = wheel_.schedule(mono_deadline(job.run_at), kJobDue, job.id);
    return;
  }
  set_state(job, JobState::kReady);
  job.queue->ready.push(&job);
  notify_ready(job.queue);
}

void State::leave_indexes(Job& job) {
  if (job.heap_index != Job::kNotInHeap) job.queue->ready.remove(&job);
  if (job.timer.valid()) {
    wheel_.cancel(job.timer);
    job.timer = {};
  }
}

void State::schedule_lease_timer(Job& job, DurationMs grace_ms) {
  if (replaying_) return;
  const WallTime deadline = std::max(job.lease_expires_at, wall_now_ + grace_ms);
  job.timer = wheel_.schedule(mono_deadline(deadline), kLeaseExpiry, job.id);
}

void State::finish(Job& job, JobState terminal, WallTime at) {
  leave_indexes(job);
  set_state(job, terminal);
  job.finished_at = at;
  job.lease_token = 0;
  if (terminal == JobState::kDead) {
    job.queue->dead.insert(job.id);
    dead_gc_.push_back(FinishedRef{.id = job.id, .finished_at = at});
  } else {
    finished_gc_.push_back(FinishedRef{.id = job.id, .finished_at = at});
  }
}

void State::erase_job(JobId id) {
  const auto it = jobs_.find(id);
  BATON_CHECK(it != jobs_.end());
  Job& job = it->second;
  leave_indexes(job);
  if (job.state == JobState::kDead) job.queue->dead.erase(id);
  --job.queue->counts[static_cast<size_t>(job.state)];
  memory_bytes_ -= job_bytes(job);
  jobs_.erase(it);
}

uint64_t State::job_bytes(const Job& job) {
  return sizeof(Job) + kJobMapNodeBytes + kReadyHeapSlotBytes + TimingWheel::kBytesPerTimer +
         job.payload.allocated_bytes() + string_heap_bytes(job.idem_key) +
         string_heap_bytes(job.last_error);
}

uint64_t State::idem_bytes(const IdemEntry& entry) {
  return kIdemNodeBytes + string_heap_bytes(entry.key);
}

void State::upsert_idem(std::string_view key, JobId job_id, WallTime expires_at) {
  if (const auto existing = idem_index_.find(key); existing != idem_index_.end()) {
    // The index key is a view of the node's string: drop the index entry first.
    const auto node = existing->second;
    memory_bytes_ -= idem_bytes(*node);
    idem_index_.erase(existing);
    idem_order_.erase(node);
  }
  idem_order_.push_back(
      IdemEntry{.key = std::string(key), .job_id = job_id, .expires_at = expires_at});
  const auto node = std::prev(idem_order_.end());
  idem_index_.emplace(std::string_view(node->key), node);
  memory_bytes_ += idem_bytes(*node);
}

void State::erase_idem_front() {
  const auto node = idem_order_.begin();
  memory_bytes_ -= idem_bytes(*node);
  idem_index_.erase(std::string_view(node->key));
  idem_order_.erase(node);
}

// --- apply -----------------------------------------------------------------------------
// Every apply_record validates first and mutates second, so an error leaves the
// state untouched.

Status State::apply(const Record& record) {
  return std::visit([this](const auto& r) { return apply_record(r); }, record);
}

Status State::apply_record(const JobEnqueued& r) {
  if (r.id != next_job_id_) {
    return mismatch(std::format("enqueue: record has job id {}, expected {}", r.id, next_job_id_));
  }
  if (r.queue.empty()) return mismatch("enqueue: empty queue name");
  if (r.max_attempts == 0) return mismatch("enqueue: max_attempts is 0");

  Queue& queue = get_or_create_queue(r.queue);
  Job& job = jobs_[r.id];
  job.id = r.id;
  job.queue = &queue;
  job.payload = SharedBytes(r.payload);
  job.priority = r.priority;
  job.max_attempts = r.max_attempts;
  job.backoff_base_ms = r.backoff_base_ms;
  job.backoff_cap_ms = r.backoff_cap_ms;
  job.run_at = r.run_at;
  job.created_at = r.at;
  job.idem_key = std::string(r.idem_key);
  job.state = JobState::kScheduled;
  ++queue.counts[static_cast<size_t>(JobState::kScheduled)];
  memory_bytes_ += job_bytes(job);
  enter_pending(job);

  if (!r.idem_key.empty()) upsert_idem(r.idem_key, r.id, r.idem_expires_at);
  ++queue.totals.enqueued;
  next_job_id_ = r.id + 1;
  return {};
}

Status State::apply_record(const JobLeased& r) {
  Job* job = find_mutable(r.id);
  if (job == nullptr) return mismatch(std::format("lease: job {} does not exist", r.id));
  if (!is_pending(job->state)) {
    return mismatch(std::format("lease: job {} is {}", r.id, to_string(job->state)));
  }
  if (r.token != next_token_) {
    return mismatch(std::format("lease: record has token {}, expected {}", r.token, next_token_));
  }
  if (job->attempts >= job->max_attempts) {
    return mismatch(std::format("lease: job {} has no attempts left", r.id));
  }

  leave_indexes(*job);
  set_state(*job, JobState::kLeased);
  ++job->attempts;
  job->lease_token = r.token;
  job->lease_expires_at = r.lease_expires_at;
  schedule_lease_timer(*job, 0);
  next_token_ = r.token + 1;
  return {};
}

Status State::apply_record(const LeaseExtended& r) {
  BATON_ASSIGN_OR_RETURN(Job * job, require_leased(r.id, r.token, "heartbeat"));
  leave_indexes(*job);  // cancels the old expiry timer
  job->lease_expires_at = r.lease_expires_at;
  schedule_lease_timer(*job, 0);
  return {};
}

Status State::apply_record(const JobSucceeded& r) {
  BATON_ASSIGN_OR_RETURN(Job * job, require_leased(r.id, r.token, "ack"));
  finish(*job, JobState::kSucceeded, r.at);
  ++job->queue->totals.succeeded;
  return {};
}

Status State::apply_record(const AttemptFailed& r) {
  BATON_ASSIGN_OR_RETURN(Job * job, require_leased(r.id, r.token, "fail"));
  memory_bytes_ -= job_bytes(*job);
  job->last_error.assign(r.error);
  memory_bytes_ += job_bytes(*job);
  ++job->queue->totals.failed_attempts;

  if (r.dead) {
    finish(*job, JobState::kDead, r.at);
    ++job->queue->totals.dead;
    return {};
  }
  leave_indexes(*job);
  job->lease_token = 0;
  job->run_at = r.retry_at;
  enter_pending(*job);
  return {};
}

Status State::apply_record(const JobCancelled& r) {
  Job* job = find_mutable(r.id);
  if (job == nullptr) return mismatch(std::format("cancel: job {} does not exist", r.id));
  if (is_terminal(job->state)) {
    return mismatch(std::format("cancel: job {} is already {}", r.id, to_string(job->state)));
  }
  finish(*job, JobState::kCancelled, r.at);
  ++job->queue->totals.cancelled;
  return {};
}

Status State::apply_record(const DeadJobRetried& r) {
  Job* job = find_mutable(r.id);
  if (job == nullptr) return mismatch(std::format("dlq retry: job {} does not exist", r.id));
  if (job->state != JobState::kDead) {
    return mismatch(std::format("dlq retry: job {} is {}", r.id, to_string(job->state)));
  }
  job->queue->dead.erase(job->id);
  job->attempts = 0;
  job->finished_at = WallTime{};
  job->run_at = r.run_at;
  enter_pending(*job);  // its dead_gc_ entry is now stale; collect_garbage() skips it
  return {};
}

Status State::apply_record(const JobsPurged& r) {
  for (const JobId id : r.ids) {
    const Job* job = find_job(id);
    if (job == nullptr) return mismatch(std::format("purge: job {} does not exist", id));
    if (!is_terminal(job->state)) {
      return mismatch(std::format("purge: job {} is {}", id, to_string(job->state)));
    }
  }
  for (const JobId id : r.ids) {
    if (find_job(id) != nullptr) erase_job(id);  // tolerate a duplicate id in the record
  }
  return {};
}

// --- replay and derived state --------------------------------------------------------

void State::begin_replay() { replaying_ = true; }

void State::end_replay(DurationMs lease_grace_ms) {
  replaying_ = false;
  rebuild_derived(lease_grace_ms);
  collect_garbage();
}

void State::rebuild_derived(DurationMs lease_grace_ms) {
  BATON_CHECK(!replaying_);
  wheel_ = TimingWheel(static_cast<uint64_t>(mono_now_.ms));
  finished_gc_.clear();
  dead_gc_.clear();
  ready_notifications_.clear();
  memory_bytes_ = 0;
  for (auto& [name, queue] : queues_) {
    queue->ready = ReadyHeap();
    queue->dead.clear();
    queue->counts.fill(0);
  }

  for (auto& [id, job] : jobs_) {
    job.heap_index = Job::kNotInHeap;
    job.timer = {};
    memory_bytes_ += job_bytes(job);
    ++job.queue->counts[static_cast<size_t>(job.state)];
    if (is_pending(job.state)) {
      enter_pending(job);
    } else if (job.state == JobState::kLeased) {
      schedule_lease_timer(job, lease_grace_ms);
    } else if (job.state == JobState::kDead) {
      job.queue->dead.insert(id);
      dead_gc_.push_back(FinishedRef{.id = id, .finished_at = job.finished_at});
    } else {
      finished_gc_.push_back(FinishedRef{.id = id, .finished_at = job.finished_at});
    }
  }
  std::ranges::sort(finished_gc_, {}, &FinishedRef::finished_at);
  std::ranges::sort(dead_gc_, {}, &FinishedRef::finished_at);
  for (const IdemEntry& entry : idem_order_) memory_bytes_ += idem_bytes(entry);
}

void State::advance_timers(std::vector<JobId>& expired_leases) {
  fired_.clear();
  wheel_.advance(static_cast<uint64_t>(mono_now_.ms), fired_);
  for (const TimerEvent& event : fired_) {
    Job* job = find_mutable(event.id);
    // The job may have moved on since the timer was scheduled; its current
    // timer handle is the authority.
    if (job == nullptr || job->timer != event.handle) continue;
    job->timer = {};
    if (event.kind == kJobDue && job->state == JobState::kScheduled) {
      if (job->run_at > wall_now_) {
        // The wall clock is behind where the timer thought it would be (it was
        // stepped back). Wait out the difference rather than run early.
        job->timer = wheel_.schedule(mono_deadline(job->run_at), kJobDue, job->id);
      } else {
        enter_pending(*job);
      }
    } else if (event.kind == kLeaseExpiry && job->state == JobState::kLeased) {
      expired_leases.push_back(job->id);
    }
  }
}

std::optional<MonoTime> State::next_timer_deadline() const {
  const std::optional<uint64_t> next = wheel_.next_wakeup();
  if (!next) return std::nullopt;
  return MonoTime{static_cast<int64_t>(*next)};
}

void State::collect_garbage() {
  BATON_CHECK(!replaying_);
  const auto collect = [this](std::deque<FinishedRef>& refs, DurationMs retention, bool dead) {
    while (!refs.empty() && refs.front().finished_at + retention <= wall_now_) {
      const FinishedRef ref = refs.front();
      refs.pop_front();
      const Job* job = find_job(ref.id);
      // Skip references made stale by DLQ.RETRY or an explicit purge.
      if (job == nullptr || job->finished_at != ref.finished_at) continue;
      if (dead ? job->state != JobState::kDead : !is_terminal(job->state)) continue;
      erase_job(ref.id);
    }
  };
  collect(finished_gc_, options_.retain_finished_ms, /*dead=*/false);
  collect(dead_gc_, options_.retain_dead_ms, /*dead=*/true);
  while (!idem_order_.empty() && idem_order_.front().expires_at <= wall_now_) erase_idem_front();
}

std::vector<Queue*> State::take_ready_notifications() {
  return std::exchange(ready_notifications_, {});
}

// --- serialization ---------------------------------------------------------------------

void State::serialize(std::string& out) const {
  ByteWriter w(out);
  w.u8(kSerializationVersion);
  w.varint(next_job_id_);
  w.varint(next_token_);

  // Queues in name order (std::map); jobs refer to them by position.
  std::unordered_map<const Queue*, uint64_t> queue_index;
  w.varint(queues_.size());
  for (const auto& [name, queue] : queues_) {
    queue_index.emplace(queue.get(), queue_index.size());
    w.bytes(name);
    w.varint(queue->totals.enqueued);
    w.varint(queue->totals.succeeded);
    w.varint(queue->totals.failed_attempts);
    w.varint(queue->totals.dead);
    w.varint(queue->totals.cancelled);
  }

  std::vector<const Job*> jobs;
  jobs.reserve(jobs_.size());
  for (const auto& [id, job] : jobs_) jobs.push_back(&job);
  std::ranges::sort(jobs, {}, &Job::id);
  w.varint(jobs.size());
  for (const Job* job : jobs) {
    w.varint(job->id);
    w.varint(queue_index.at(job->queue));
    w.bytes(job->payload.view());
    w.svarint(job->priority);
    w.varint(job->attempts);
    w.varint(job->max_attempts);
    w.varint(job->backoff_base_ms);
    w.varint(job->backoff_cap_ms);
    w.u8(static_cast<uint8_t>(to_persisted(job->state)));
    w.svarint(job->run_at.ms);
    w.svarint(job->created_at.ms);
    w.svarint(job->finished_at.ms);
    w.svarint(job->lease_expires_at.ms);
    w.varint(job->lease_token);
    w.bytes(job->idem_key);
    w.bytes(job->last_error);
  }

  std::vector<const IdemEntry*> entries;
  entries.reserve(idem_order_.size());
  for (const IdemEntry& entry : idem_order_) entries.push_back(&entry);
  std::ranges::sort(entries, {}, &IdemEntry::key);
  w.varint(entries.size());
  for (const IdemEntry* entry : entries) {
    w.bytes(entry->key);
    w.varint(entry->job_id);
    w.svarint(entry->expires_at.ms);
  }
}

Result<std::unique_ptr<State>> State::deserialize(std::string_view data, StateOptions options) {
  const auto corrupt = [](std::string_view what) {
    return Error{ErrorCode::kCorruption, std::format("state: {}", what)};
  };
  ByteReader r(data);
  if (r.u8() != kSerializationVersion || !r.ok()) return corrupt("unknown version");

  auto state = std::make_unique<State>(options);
  state->replaying_ = true;
  state->next_job_id_ = r.varint();
  state->next_token_ = r.varint();

  std::vector<Queue*> queue_by_index;
  const uint64_t queue_count = r.varint();
  if (queue_count > r.remaining()) return corrupt("queue count exceeds input");
  for (uint64_t i = 0; i < queue_count; ++i) {
    const std::string_view name = r.bytes();
    if (!r.ok() || name.empty()) return corrupt("bad queue name");
    if (state->find_queue(name) != nullptr) return corrupt("duplicate queue");
    Queue& queue = state->get_or_create_queue(name);
    queue.totals.enqueued = r.varint();
    queue.totals.succeeded = r.varint();
    queue.totals.failed_attempts = r.varint();
    queue.totals.dead = r.varint();
    queue.totals.cancelled = r.varint();
    queue_by_index.push_back(&queue);
  }

  const uint64_t job_count = r.varint();
  if (job_count > r.remaining()) return corrupt("job count exceeds input");
  for (uint64_t i = 0; i < job_count; ++i) {
    const JobId id = r.varint();
    const uint64_t queue = r.varint();
    if (!r.ok() || id == 0 || id >= state->next_job_id_) return corrupt("bad job id");
    if (queue >= queue_by_index.size()) return corrupt("job refers to an unknown queue");
    const auto [it, inserted] = state->jobs_.try_emplace(id);
    if (!inserted) return corrupt("duplicate job id");

    Job& job = it->second;
    job.id = id;
    job.queue = queue_by_index[static_cast<size_t>(queue)];
    job.payload = SharedBytes(r.bytes());
    const int64_t priority = r.svarint();
    const uint64_t attempts = r.varint();
    const uint64_t max_attempts = r.varint();
    const uint64_t backoff_base = r.varint();
    const uint64_t backoff_cap = r.varint();
    const uint8_t persisted = r.u8();
    if (priority < INT32_MIN || priority > INT32_MAX || attempts > UINT32_MAX ||
        max_attempts > UINT32_MAX || backoff_base > UINT32_MAX || backoff_cap > UINT32_MAX ||
        persisted > static_cast<uint8_t>(PersistedState::kCancelled)) {
      return corrupt("job field out of range");
    }
    job.priority = static_cast<int32_t>(priority);
    job.attempts = static_cast<uint32_t>(attempts);
    job.max_attempts = static_cast<uint32_t>(max_attempts);
    job.backoff_base_ms = static_cast<uint32_t>(backoff_base);
    job.backoff_cap_ms = static_cast<uint32_t>(backoff_cap);
    job.state = from_persisted(static_cast<PersistedState>(persisted));
    job.run_at = WallTime{r.svarint()};
    job.created_at = WallTime{r.svarint()};
    job.finished_at = WallTime{r.svarint()};
    job.lease_expires_at = WallTime{r.svarint()};
    job.lease_token = r.varint();
    job.idem_key = std::string(r.bytes());
    job.last_error = std::string(r.bytes());
    if (!r.ok()) return corrupt("truncated job");
    for (const WallTime t : {job.run_at, job.created_at, job.finished_at, job.lease_expires_at}) {
      if (t.ms < 0 || t.ms > kMaxWallTimeMs) return corrupt("job timestamp out of range");
    }
    if (job.lease_token >= state->next_token_) return corrupt("bad lease token");
    if ((job.state == JobState::kLeased) != (job.lease_token != 0)) {
      return corrupt("lease token does not match job state");
    }
    if (job.attempts > job.max_attempts) return corrupt("job has more attempts than allowed");
    // Keep the bookkeeping that apply() maintains during replay in step.
    ++job.queue->counts[static_cast<size_t>(job.state)];
    if (job.state == JobState::kDead) job.queue->dead.insert(id);
    state->memory_bytes_ += job_bytes(job);
  }

  const uint64_t idem_count = r.varint();
  if (idem_count > r.remaining()) return corrupt("idempotency count exceeds input");
  for (uint64_t i = 0; i < idem_count; ++i) {
    const std::string_view key = r.bytes();
    const JobId job_id = r.varint();
    const WallTime expires_at{r.svarint()};
    if (!r.ok() || key.empty()) return corrupt("bad idempotency entry");
    if (expires_at.ms < 0 || expires_at.ms > kMaxWallTimeMs) {
      return corrupt("idempotency expiry out of range");
    }
    if (state->find_idem(key) != nullptr) return corrupt("duplicate idempotency key");
    state->upsert_idem(key, job_id, expires_at);
  }
  if (!r.ok()) return corrupt("truncated");
  if (!r.at_end()) return corrupt("trailing bytes");

  // Collection order is expiry order.
  state->idem_order_.sort(
      [](const IdemEntry& a, const IdemEntry& b) { return a.expires_at < b.expires_at; });
  return state;
}

// --- invariants ------------------------------------------------------------------------

Status State::check_invariants() const {
  const auto broken = [](std::string what) {
    return Error{ErrorCode::kInternal, "invariant violated: " + std::move(what)};
  };

  std::map<const Queue*, std::array<uint64_t, kJobStateCount>> recount;
  uint64_t memory = 0;
  size_t timers = 0;
  for (const auto& [id, job] : jobs_) {
    if (id != job.id) return broken(std::format("job {} is stored under id {}", job.id, id));
    if (id >= next_job_id_) return broken(std::format("job {} >= next_job_id", id));
    if (job.queue == nullptr || find_queue(job.queue->name) != job.queue) {
      return broken(std::format("job {} has a dangling queue", id));
    }
    if (job.attempts > job.max_attempts) return broken(std::format("job {} over-attempted", id));
    ++recount[job.queue][static_cast<size_t>(job.state)];
    memory += job_bytes(job);
    if (job.timer.valid()) ++timers;

    const bool in_heap = job.heap_index != Job::kNotInHeap;
    const bool in_dead = job.queue->dead.contains(id);
    const bool expect_timer =
        !replaying_ && (job.state == JobState::kScheduled || job.state == JobState::kLeased);
    if (in_heap != (job.state == JobState::kReady)) {
      return broken(
          std::format("job {} is {} but heap membership is {}", id, to_string(job.state), in_heap));
    }
    if (in_heap && (job.heap_index >= job.queue->ready.size() ||
                    job.queue->ready.jobs()[job.heap_index] != &job)) {
      return broken(std::format("job {} has a wrong heap index", id));
    }
    if (job.timer.valid() != expect_timer) {
      return broken(std::format("job {} is {} but timer presence is {}", id, to_string(job.state),
                                job.timer.valid()));
    }
    if (in_dead != (job.state == JobState::kDead)) {
      return broken(
          std::format("job {} is {} but DLQ membership is {}", id, to_string(job.state), in_dead));
    }
    if ((job.state == JobState::kLeased) != (job.lease_token != 0)) {
      return broken(std::format("job {} is {} with lease token {}", id, to_string(job.state),
                                job.lease_token));
    }
    if (job.lease_token >= next_token_) return broken(std::format("job {} token too high", id));
    if (job.state == JobState::kLeased && job.attempts == 0) {
      return broken(std::format("job {} is leased with zero attempts", id));
    }
  }
  if (timers != wheel_.size()) {
    return broken(std::format("{} jobs hold timers but the wheel has {}", timers, wheel_.size()));
  }

  for (const auto& [name, queue] : queues_) {
    if (name != queue->name) return broken("queue stored under the wrong name");
    if (!queue->ready.is_consistent()) return broken(std::format("heap of {} is broken", name));
    const auto counted = recount[queue.get()];
    if (counted != queue->counts) return broken(std::format("counts of queue {} are off", name));
    if (queue->ready.size() != queue->count(JobState::kReady)) {
      return broken(std::format("queue {} heap size != ready count", name));
    }
    if (queue->dead.size() != queue->count(JobState::kDead)) {
      return broken(std::format("queue {} DLQ size != dead count", name));
    }
  }

  if (idem_index_.size() != idem_order_.size()) return broken("idempotency index size mismatch");
  for (auto it = idem_order_.begin(); it != idem_order_.end(); ++it) {
    const auto found = idem_index_.find(std::string_view(it->key));
    if (found == idem_index_.end() || found->second != it) {
      return broken(std::format("idempotency key {} is not indexed", it->key));
    }
    memory += idem_bytes(*it);
  }
  if (memory != memory_bytes_) {
    return broken(std::format("memory estimate {} != recomputed {}", memory_bytes_, memory));
  }
  return {};
}

}  // namespace baton
