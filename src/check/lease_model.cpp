#include "check/lease_model.h"

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

namespace baton {

void LeaseModel::start_from(const StateImage& image) {
  next_job_id_ = image.next_job_id;
  max_token_ = image.next_token - 1;
  leases_.clear();
  for (const JobImage& job : image.jobs) {
    if (job.state == PersistedState::kLeased) {
      leases_[job.id] = Lease{.token = job.lease_token, .expires_at = job.lease_expires_at};
    }
  }
}

void LeaseModel::on(Lsn lsn, const Record& record) {
  lsn_ = lsn;
  std::visit([this](const auto& r) { check(r); }, record);
}

void LeaseModel::violation(std::string what) {
  ++violation_count_;
  if (violations_.size() < kMaxReported) {
    violations_.push_back(std::format("LSN {}: {}", lsn_, what));
  }
}

// The lease that a record carrying `token` must refer to; nullptr, and a
// violation, if it refers to anything else.
const LeaseModel::Lease* LeaseModel::current(JobId id, LeaseToken token, std::string_view what) {
  const auto it = leases_.find(id);
  if (it == leases_.end()) {
    violation(std::format("{} for job {} with token {}, but the job has no current lease", what, id,
                          token));
    return nullptr;
  }
  if (it->second.token != token) {
    violation(
        std::format("{} for job {} with stale token {} was accepted; the current token "
                    "is {}",
                    what, id, token, it->second.token));
    return nullptr;
  }
  return &it->second;
}

void LeaseModel::check(const JobEnqueued& r) {
  ++counters_.enqueued;
  if (r.id < next_job_id_) violation(std::format("job id {} was used before", r.id));
  next_job_id_ = std::max(next_job_id_, r.id + 1);
}

void LeaseModel::check(const JobLeased& r) {
  ++counters_.leases_granted;
  if (r.token <= max_token_) {
    violation(std::format("job {} got token {}, which is not larger than the earlier token {}",
                          r.id, r.token, max_token_));
  }
  max_token_ = std::max(max_token_, r.token);
  if (const auto it = leases_.find(r.id); it != leases_.end()) {
    violation(
        std::format("job {} was leased with token {} while token {} was still current: "
                    "two valid leases",
                    r.id, r.token, it->second.token));
  }
  leases_[r.id] = Lease{.token = r.token, .expires_at = r.lease_expires_at};
}

void LeaseModel::check(const LeaseExtended& r) {
  ++counters_.heartbeats;
  if (current(r.id, r.token, "heartbeat") != nullptr) {
    leases_[r.id].expires_at = r.lease_expires_at;
  }
}

void LeaseModel::check(const JobSucceeded& r) {
  ++counters_.acks;
  current(r.id, r.token, "ack");
  leases_.erase(r.id);
}

void LeaseModel::check(const AttemptFailed& r) {
  const bool expired = r.reason == FailureReason::kLeaseExpired;
  ++(expired ? counters_.lease_expiries : counters_.worker_failures);
  const Lease* lease = current(r.id, r.token, expired ? "lease expiry" : "failure");
  if (lease != nullptr && expired && r.at < lease->expires_at) {
    violation(
        std::format("the lease of job {} (token {}) was expired at {} ms, before its "
                    "recorded expiry at {} ms",
                    r.id, r.token, r.at.ms, lease->expires_at.ms));
  }
  leases_.erase(r.id);
}

void LeaseModel::check(const JobCancelled& r) {
  ++counters_.cancellations;
  leases_.erase(r.id);  // cancelling a leased job ends its lease
}

void LeaseModel::check(const DeadJobRetried& r) {
  if (leases_.contains(r.id)) {
    violation(std::format("dead job {} was retried while it had a lease", r.id));
  }
}

void LeaseModel::check(const JobsPurged& r) {
  for (const JobId id : r.ids) {
    if (leases_.contains(id)) violation(std::format("job {} was purged while leased", id));
  }
}

}  // namespace baton
