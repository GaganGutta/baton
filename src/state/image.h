#pragma once

// StateImage: a self-contained copy of the DURABLE part of a State, and the
// one encoding of it that both State::serialize() and snapshot files use.
//
// An image shares nothing mutable with the State it came from (payload bytes
// are immutable and reference-counted), so it can be handed to the snapshot
// thread and serialized there while the event loop keeps going
// (docs/design.md 8.1).
//
// The encoding is chunkable: a meta piece, any number of jobs pieces and any
// number of idempotency pieces. StateBuilder consumes pieces in that order and
// validates everything, because on the way back in they come from disk.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "state/state.h"

namespace baton {

// The durable part of JobState: `scheduled` and `ready` are one state on disk,
// because which of the two a pending job is depends on the clock.
enum class PersistedState : uint8_t { kPending = 0, kLeased, kSucceeded, kDead, kCancelled };

PersistedState to_persisted(JobState state);
JobState from_persisted(PersistedState state);

struct QueueImage {
  std::string name;
  QueueTotals totals;
};

struct JobImage {
  JobId id = 0;
  uint32_t queue = 0;  // index into StateImage::queues
  SharedBytes payload;
  int32_t priority = 0;
  uint32_t attempts = 0;
  uint32_t max_attempts = 0;
  uint32_t backoff_base_ms = 0;
  uint32_t backoff_cap_ms = 0;
  PersistedState state = PersistedState::kPending;
  WallTime run_at;
  WallTime created_at;
  WallTime finished_at;
  WallTime lease_expires_at;
  LeaseToken lease_token = 0;
  std::string idem_key;
  std::string last_error;
};

struct StateImage {
  JobId next_job_id = 1;
  LeaseToken next_token = 1;
  std::vector<QueueImage> queues;
  std::vector<JobImage> jobs;  // in no particular order
  std::vector<IdemEntry> idem;
};

void encode_image_meta(const StateImage& image, std::string& out);
void encode_image_jobs(std::span<const JobImage> jobs, std::string& out);
void encode_image_idem(std::span<const IdemEntry> entries, std::string& out);

// Rebuilds a State from encoded pieces. The meta piece comes first. The result
// is in replay mode, exactly as if the records that produced the image had just
// been applied: apply the rest of the log (if any), then set_now() and
// end_replay().
class StateBuilder {
 public:
  explicit StateBuilder(StateOptions options);

  Status add_meta(std::string_view piece);
  Status add_jobs(std::string_view piece);
  Status add_idem(std::string_view piece);
  Result<std::unique_ptr<State>> finish();

  uint64_t jobs_added() const { return jobs_added_; }
  uint64_t idem_added() const { return idem_added_; }

 private:
  std::unique_ptr<State> state_;
  std::vector<Queue*> queue_by_index_;
  bool have_meta_ = false;
  uint64_t jobs_added_ = 0;
  uint64_t idem_added_ = 0;
};

}  // namespace baton
