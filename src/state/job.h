#pragma once

// The job model. Field-by-field rationale: docs/design.md section 5.2.

#include <cstdint>
#include <string>
#include <string_view>

#include "common/clock.h"
#include "sched/timing_wheel.h"
#include "state/shared_bytes.h"

namespace baton {

using JobId = uint64_t;
using LeaseToken = uint64_t;

struct Queue;

enum class JobState : uint8_t {
  kScheduled,  // pending, run_at is in the future (a timer will promote it)
  kReady,      // pending, in its queue's ready heap
  kLeased,     // handed to a worker; lease_token/lease_expires_at are valid
  kSucceeded,  // terminal
  kDead,       // terminal: attempts exhausted; in the dead-letter queue
  kCancelled,  // terminal
};

std::string_view to_string(JobState state);

constexpr bool is_pending(JobState s) { return s == JobState::kScheduled || s == JobState::kReady; }
constexpr bool is_terminal(JobState s) {
  return s == JobState::kSucceeded || s == JobState::kDead || s == JobState::kCancelled;
}

// Why an attempt ended without success.
enum class FailureReason : uint8_t {
  kWorkerFailed = 0,  // the worker sent FAIL
  kLeaseExpired = 1,  // no ACK/FAIL/HEARTBEAT before the lease ran out
};

struct Job {
  static constexpr uint32_t kNotInHeap = UINT32_MAX;

  // --- durable fields (logged, snapshotted) -----------------------------------
  JobId id = 0;
  Queue* queue = nullptr;  // owned by State; stable for the life of the process
  SharedBytes payload;
  int32_t priority = 0;
  uint32_t attempts = 0;  // leases granted so far
  uint32_t max_attempts = 0;
  uint32_t backoff_base_ms = 0;
  uint32_t backoff_cap_ms = 0;
  JobState state = JobState::kScheduled;
  WallTime run_at;  // not handed out before this
  WallTime created_at;
  WallTime finished_at;  // when the job reached a terminal state
  WallTime lease_expires_at;
  LeaseToken lease_token = 0;  // current lease, 0 if none
  std::string idem_key;
  std::string last_error;

  // --- derived fields (rebuilt after replay, never persisted) -------------------
  uint32_t heap_index = kNotInHeap;  // position in queue->ready while kReady
  TimerHandle timer;                 // due timer while kScheduled, expiry timer while kLeased
};

}  // namespace baton
