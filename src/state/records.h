#pragma once

// The vocabulary of logged facts (docs/design.md section 5.3).
//
// A record describes something that happened, with every nondeterministic input
// already resolved by the command handler: ids, timestamps, lease tokens, the
// jittered retry time. State::apply() is therefore a pure function of
// (state, record), which is what makes replay reproduce the live state.
//
// String fields are views. Live, they point into the client's request; during
// recovery, into the log segment. Either way they are only valid for the
// duration of the apply() call, which copies what it keeps.

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/clock.h"
#include "common/result.h"
#include "state/job.h"

namespace baton {

// The largest timestamp a record or a snapshot may carry (about the year 37,000).
// Decoders reject anything outside [0, kMaxWallTimeMs], which keeps time
// arithmetic on decoded values free of overflow.
inline constexpr int64_t kMaxWallTimeMs = int64_t{1} << 50U;

// Wire values: never renumber, only append.
enum class RecordType : uint8_t {
  kJobEnqueued = 1,
  kJobLeased = 2,
  kLeaseExtended = 3,
  kJobSucceeded = 4,
  kAttemptFailed = 5,
  kJobCancelled = 6,
  kDeadJobRetried = 7,
  kJobsPurged = 8,
};

struct JobEnqueued {
  JobId id = 0;
  std::string_view queue{};
  std::string_view payload{};
  int32_t priority = 0;
  WallTime run_at{};
  uint32_t max_attempts = 0;
  uint32_t backoff_base_ms = 0;
  uint32_t backoff_cap_ms = 0;
  std::string_view idem_key{};  // empty: no idempotency key
  WallTime idem_expires_at{};   // meaningful only with a key
  WallTime at{};
};

struct JobLeased {
  JobId id = 0;
  LeaseToken token = 0;
  WallTime lease_expires_at{};
  WallTime at{};
};

struct LeaseExtended {
  JobId id = 0;
  LeaseToken token = 0;
  WallTime lease_expires_at{};
  WallTime at{};
};

struct JobSucceeded {
  JobId id = 0;
  LeaseToken token = 0;
  WallTime at{};
};

// An attempt ended without success: the worker sent FAIL, or the lease expired.
// The handler has already decided what happens next.
struct AttemptFailed {
  JobId id = 0;
  LeaseToken token = 0;
  FailureReason reason = FailureReason::kWorkerFailed;
  std::string_view error{};
  WallTime at{};
  bool dead = false;    // true: attempts exhausted (or NORETRY), job goes to the DLQ
  WallTime retry_at{};  // when !dead: the new run_at, jitter included
};

struct JobCancelled {
  JobId id = 0;
  WallTime at{};
};

struct DeadJobRetried {
  JobId id = 0;
  WallTime run_at{};
  WallTime at{};
};

struct JobsPurged {
  std::vector<JobId> ids{};
};

using Record = std::variant<JobEnqueued, JobLeased, LeaseExtended, JobSucceeded, AttemptFailed,
                            JobCancelled, DeadJobRetried, JobsPurged>;

RecordType type_of(const Record& record);

// Appends the payload encoding of `record` to `out` (the type travels separately,
// in the log record header).
void encode_record(const Record& record, std::string& out);

// Strict: a truncated field, an unknown version or enum value, or trailing bytes
// is an error. Views in the result point into `payload`.
Result<Record> decode_record(uint8_t type, std::string_view payload);

}  // namespace baton
