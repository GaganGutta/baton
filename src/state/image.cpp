#include "state/image.h"

#include <format>
#include <limits>
#include <utility>

#include "common/check.h"
#include "common/codec.h"

namespace baton {
namespace {

constexpr uint8_t kPieceVersion = 1;

Error corrupt(std::string_view what) {
  return Error{ErrorCode::kCorruption, std::format("state image: {}", what)};
}

bool time_in_range(WallTime t) { return t.ms >= 0 && t.ms <= kMaxWallTimeMs; }

}  // namespace

PersistedState to_persisted(JobState state) {
  switch (state) {
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

JobState from_persisted(PersistedState state) {
  switch (state) {
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

// --- encoding --------------------------------------------------------------------------

void encode_image_meta(const StateImage& image, std::string& out) {
  ByteWriter w(out);
  w.u8(kPieceVersion);
  w.varint(image.next_job_id);
  w.varint(image.next_token);
  w.varint(image.queues.size());
  for (const QueueImage& queue : image.queues) {
    w.bytes(queue.name);
    w.varint(queue.totals.enqueued);
    w.varint(queue.totals.succeeded);
    w.varint(queue.totals.failed_attempts);
    w.varint(queue.totals.dead);
    w.varint(queue.totals.cancelled);
  }
}

void encode_image_jobs(std::span<const JobImage> jobs, std::string& out) {
  ByteWriter w(out);
  w.u8(kPieceVersion);
  w.varint(jobs.size());
  for (const JobImage& job : jobs) {
    w.varint(job.id);
    w.varint(job.queue);
    w.bytes(job.payload.view());
    w.svarint(job.priority);
    w.varint(job.attempts);
    w.varint(job.max_attempts);
    w.varint(job.backoff_base_ms);
    w.varint(job.backoff_cap_ms);
    w.u8(static_cast<uint8_t>(job.state));
    w.svarint(job.run_at.ms);
    w.svarint(job.created_at.ms);
    w.svarint(job.finished_at.ms);
    w.svarint(job.lease_expires_at.ms);
    w.varint(job.lease_token);
    w.bytes(job.idem_key);
    w.bytes(job.last_error);
  }
}

void encode_image_idem(std::span<const IdemEntry> entries, std::string& out) {
  ByteWriter w(out);
  w.u8(kPieceVersion);
  w.varint(entries.size());
  for (const IdemEntry& entry : entries) {
    w.bytes(entry.key);
    w.varint(entry.job_id);
    w.svarint(entry.expires_at.ms);
  }
}

// --- decoding ----------------------------------------------------------------------------

StateBuilder::StateBuilder(StateOptions options) : state_(std::make_unique<State>(options)) {
  state_->replaying_ = true;
}

Status StateBuilder::add_meta(std::string_view piece) {
  if (have_meta_) return corrupt("more than one meta piece");
  ByteReader r(piece);
  if (r.u8() != kPieceVersion || !r.ok()) return corrupt("unknown meta version");
  state_->next_job_id_ = r.varint();
  state_->next_token_ = r.varint();
  if (!r.ok() || state_->next_job_id_ == 0 || state_->next_token_ == 0) {
    return corrupt("bad counters");
  }

  const uint64_t queue_count = r.varint();
  if (queue_count > r.remaining()) return corrupt("queue count exceeds input");
  for (uint64_t i = 0; i < queue_count; ++i) {
    const std::string_view name = r.bytes();
    if (!r.ok() || name.empty()) return corrupt("bad queue name");
    if (state_->find_queue(name) != nullptr) return corrupt("duplicate queue");
    Queue& queue = state_->get_or_create_queue(name);
    queue.totals.enqueued = r.varint();
    queue.totals.succeeded = r.varint();
    queue.totals.failed_attempts = r.varint();
    queue.totals.dead = r.varint();
    queue.totals.cancelled = r.varint();
    queue_by_index_.push_back(&queue);
  }
  if (!r.ok()) return corrupt("truncated meta piece");
  if (!r.at_end()) return corrupt("trailing bytes in meta piece");
  have_meta_ = true;
  return {};
}

Status StateBuilder::add_jobs(std::string_view piece) {
  if (!have_meta_) return corrupt("jobs before meta");
  ByteReader r(piece);
  if (r.u8() != kPieceVersion || !r.ok()) return corrupt("unknown jobs version");
  const uint64_t count = r.varint();
  if (count > r.remaining()) return corrupt("job count exceeds input");

  for (uint64_t i = 0; i < count; ++i) {
    const JobId id = r.varint();
    const uint64_t queue = r.varint();
    if (!r.ok() || id == 0 || id >= state_->next_job_id_) return corrupt("bad job id");
    if (queue >= queue_by_index_.size()) return corrupt("job refers to an unknown queue");
    const auto [it, inserted] = state_->jobs_.try_emplace(id);
    if (!inserted) return corrupt("duplicate job id");

    Job& job = it->second;
    job.id = id;
    job.queue = queue_by_index_[static_cast<size_t>(queue)];
    job.payload = SharedBytes(r.bytes());
    const int64_t priority = r.svarint();
    const uint64_t attempts = r.varint();
    const uint64_t max_attempts = r.varint();
    const uint64_t backoff_base = r.varint();
    const uint64_t backoff_cap = r.varint();
    const uint8_t persisted = r.u8();
    constexpr uint64_t kMax32 = std::numeric_limits<uint32_t>::max();
    if (priority < std::numeric_limits<int32_t>::min() ||
        priority > std::numeric_limits<int32_t>::max() || attempts > kMax32 ||
        max_attempts > kMax32 || backoff_base > kMax32 || backoff_cap > kMax32 ||
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
    if (!time_in_range(job.run_at) || !time_in_range(job.created_at) ||
        !time_in_range(job.finished_at) || !time_in_range(job.lease_expires_at)) {
      return corrupt("job timestamp out of range");
    }
    if (job.lease_token >= state_->next_token_) return corrupt("bad lease token");
    if (holds_token(job.state) != (job.lease_token != 0)) {
      return corrupt("lease token does not match job state");
    }
    if (job.attempts > job.max_attempts) return corrupt("job has more attempts than allowed");

    // Keep the bookkeeping that apply() maintains during replay in step.
    ++job.queue->counts[static_cast<size_t>(job.state)];
    if (job.state == JobState::kDead) job.queue->dead.insert(id);
    state_->memory_bytes_ += State::job_bytes(job);
    ++jobs_added_;
  }
  if (!r.at_end()) return corrupt("trailing bytes in jobs piece");
  return {};
}

Status StateBuilder::add_idem(std::string_view piece) {
  if (!have_meta_) return corrupt("idempotency keys before meta");
  ByteReader r(piece);
  if (r.u8() != kPieceVersion || !r.ok()) return corrupt("unknown idempotency version");
  const uint64_t count = r.varint();
  if (count > r.remaining()) return corrupt("idempotency count exceeds input");
  for (uint64_t i = 0; i < count; ++i) {
    const std::string_view key = r.bytes();
    const JobId job_id = r.varint();
    const WallTime expires_at{r.svarint()};
    if (!r.ok() || key.empty()) return corrupt("bad idempotency entry");
    if (!time_in_range(expires_at)) return corrupt("idempotency expiry out of range");
    if (state_->find_idem(key) != nullptr) return corrupt("duplicate idempotency key");
    state_->upsert_idem(key, job_id, expires_at);
    ++idem_added_;
  }
  if (!r.at_end()) return corrupt("trailing bytes in idempotency piece");
  return {};
}

Result<std::unique_ptr<State>> StateBuilder::finish() {
  if (!have_meta_) return corrupt("no meta piece");
  // Collection pops from the front, so the list has to be in expiry order.
  state_->idem_order_.sort(
      [](const IdemEntry& a, const IdemEntry& b) { return a.expires_at < b.expires_at; });
  return std::move(state_);
}

}  // namespace baton
