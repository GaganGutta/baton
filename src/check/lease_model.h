#pragma once

// An independent model of the lease rules, for checking a log after the fact.
//
// It shares no code with State::apply and knows almost nothing: a map from job
// to its current lease, the largest token seen, the next job id. Fed the
// records of a log in order, it reports every place where the server treated
// something as valid that the rules forbid (docs/design.md 10.2):
//
//   - a lease token that is reused or not larger than every token before it,
//   - a lease granted while another lease on the same job is current,
//   - a heartbeat, ack or failure recorded for anything but the current token,
//   - a lease expired before the expiry that the log itself recorded,
//   - a job id used twice, a purged or retried job that still had a lease.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "state/image.h"
#include "state/records.h"

namespace baton {

class LeaseModel {
 public:
  // Starts from a snapshot instead of from an empty server.
  void start_from(const StateImage& image);

  void on(Lsn lsn, const Record& record);

  // The first kMaxReported violations, in log order; violation_count() has them all.
  static constexpr size_t kMaxReported = 50;
  const std::vector<std::string>& violations() const { return violations_; }
  uint64_t violation_count() const { return violation_count_; }

  LeaseToken max_token() const { return max_token_; }
  size_t open_leases() const { return leases_.size(); }

  struct Counters {
    uint64_t enqueued = 0;
    uint64_t leases_granted = 0;
    uint64_t heartbeats = 0;
    uint64_t acks = 0;
    uint64_t worker_failures = 0;
    uint64_t lease_expiries = 0;
    uint64_t cancellations = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  struct Lease {
    LeaseToken token = 0;
    WallTime expires_at{};
  };

  void violation(std::string what);
  const Lease* current(JobId id, LeaseToken token, std::string_view what);

  void check(const JobEnqueued& r);
  void check(const JobLeased& r);
  void check(const LeaseExtended& r);
  void check(const JobSucceeded& r);
  void check(const AttemptFailed& r);
  void check(const JobCancelled& r);
  void check(const DeadJobRetried& r);
  void check(const JobsPurged& r);

  Lsn lsn_ = 0;
  JobId next_job_id_ = 1;
  LeaseToken max_token_ = 0;
  std::unordered_map<JobId, Lease> leases_;
  std::vector<std::string> violations_;
  uint64_t violation_count_ = 0;
  Counters counters_;
};

}  // namespace baton
