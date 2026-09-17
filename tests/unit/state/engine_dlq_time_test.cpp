// M4 at the Engine level: the dead-letter queue, and what wall-clock jumps do to
// timers (docs/design.md sections 7.4 and 7.5). The fixture re-checks
// invariants and replay equivalence after every test.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/logging.h"
#include "state/engine.h"
#include "support/engine_fixture.h"

namespace baton {
namespace {

class DlqTest : public EngineFixture {
 protected:
  // Enqueues a job and fails it straight into the dead-letter queue.
  JobId make_dead(std::string_view queue = "q", std::string_view error = "boom") {
    const JobId id = enqueue(queue, "payload");
    const auto lease = reserve(queue);
    EXPECT_TRUE(lease.has_value());
    EXPECT_EQ(lease->id, id);
    const auto failed =
        engine_.fail({.id = id, .token = lease->token, .error = error, .no_retry = true});
    EXPECT_TRUE(failed.ok() && failed->dead);
    return id;
  }

  std::vector<JobId> listed(std::string_view queue, size_t offset = 0, size_t count = 100) {
    const auto page = engine_.dlq_list(queue, offset, count);
    EXPECT_TRUE(page.ok());
    std::vector<JobId> ids;
    if (page.ok()) {
      for (const Job* job : *page) ids.push_back(job->id);
    }
    return ids;
  }
};

TEST_F(DlqTest, ListPagesThroughDeadJobsOldestFirst) {
  std::vector<JobId> dead;
  for (int i = 0; i < 7; ++i) dead.push_back(make_dead());
  enqueue();           // a live job is not in the DLQ
  make_dead("other");  // nor is another queue's dead job

  EXPECT_EQ(listed("q"), dead);
  EXPECT_EQ(listed("q", 0, 3), (std::vector<JobId>{dead[0], dead[1], dead[2]}));
  EXPECT_EQ(listed("q", 5, 3), (std::vector<JobId>{dead[5], dead[6]}));
  EXPECT_TRUE(listed("q", 7, 3).empty());
  EXPECT_TRUE(listed("q", 1000, 3).empty());
  EXPECT_TRUE(listed("never-used").empty());
  EXPECT_EQ(engine_.dlq_list("bad name", 0, 10).error().code(), ErrorCode::kInvalidArgument);
}

TEST_F(DlqTest, RetryGivesTheJobAFreshSetOfAttemptsAndANewToken) {
  const JobId id = make_dead("q", "the database was down");
  const LeaseToken old_token = state_.next_token() - 1;

  const auto retried = engine_.dlq_retry(id);
  ASSERT_TRUE(retried.ok()) << retried.error().to_string();
  EXPECT_EQ(*retried, 1U);

  const Job* job = state_.find_job(id);
  EXPECT_EQ(job->state, JobState::kReady);
  EXPECT_EQ(job->attempts, 0U);
  EXPECT_EQ(job->last_error, "the database was down") << "kept for context";
  EXPECT_TRUE(listed("q").empty());

  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(lease->id, id);
  EXPECT_EQ(lease->attempt, 1U);
  EXPECT_GT(lease->token, old_token) << "tokens never restart, even though attempts do";
  EXPECT_EQ(engine_.ack(id, old_token).error().code(), ErrorCode::kStaleToken);
  EXPECT_TRUE(engine_.ack(id, lease->token).ok());
}

TEST_F(DlqTest, RetryAndPurgeRejectJobsThatAreNotDead) {
  const JobId alive = enqueue();
  EXPECT_EQ(engine_.dlq_retry(alive).error().code(), ErrorCode::kFailedPrecondition);
  EXPECT_EQ(engine_.dlq_purge(alive).error().code(), ErrorCode::kFailedPrecondition);
  EXPECT_EQ(engine_.dlq_retry(404).error().code(), ErrorCode::kNotFound);
  EXPECT_EQ(engine_.dlq_purge(404).error().code(), ErrorCode::kNotFound);
  EXPECT_EQ(engine_.dlq_retry_all("bad name").error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(engine_.dlq_retry_all("never-used").value(), 0U);
  EXPECT_EQ(engine_.dlq_purge_all("never-used").value(), 0U);
}

TEST_F(DlqTest, RetryAllAndPurgeAllWorkPerQueue) {
  for (int i = 0; i < 5; ++i) make_dead("a");
  for (int i = 0; i < 3; ++i) make_dead("b");

  EXPECT_EQ(engine_.dlq_retry_all("a").value(), 5U);
  EXPECT_TRUE(listed("a").empty());
  EXPECT_EQ(state_.find_queue("a")->count(JobState::kReady), 5U);
  EXPECT_EQ(listed("b").size(), 3U) << "other queues are untouched";

  const uint64_t memory_before = state_.memory_bytes();
  EXPECT_EQ(engine_.dlq_purge_all("b").value(), 3U);
  EXPECT_TRUE(listed("b").empty());
  EXPECT_LT(state_.memory_bytes(), memory_before);
  EXPECT_EQ(state_.find_queue("b")->totals.dead, 3U) << "totals are history, not inventory";
}

TEST_F(DlqTest, PurgeRemovesTheJobForGood) {
  const JobId id = make_dead();
  ASSERT_EQ(engine_.dlq_purge(id).value(), 1U);
  EXPECT_EQ(state_.find_job(id), nullptr);
  EXPECT_EQ(engine_.dlq_purge(id).error().code(), ErrorCode::kNotFound);
  advance(state_.options().retain_dead_ms + 1);  // the stale collection entry is harmless
}

TEST_F(DlqTest, PurgeAllOfALargeQueueIsChunked) {
  constexpr int kJobs = 10'500;  // more than one JobsPurged record holds
  for (int i = 0; i < kJobs; ++i) make_dead("big");
  const size_t records_before = sink_.entries().size();
  EXPECT_EQ(engine_.dlq_purge_all("big").value(), static_cast<uint64_t>(kJobs));
  EXPECT_EQ(sink_.entries().size() - records_before, 2U);
  EXPECT_EQ(state_.job_count(), 0U);
}

TEST_F(DlqTest, ARetriedJobCanDieAgain) {
  const JobId id = make_dead();
  ASSERT_TRUE(engine_.dlq_retry(id).ok());
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  ASSERT_TRUE(
      engine_.fail({.id = id, .token = lease->token, .error = "still broken", .no_retry = true})
          .ok());
  EXPECT_EQ(listed("q"), (std::vector<JobId>{id}));
  EXPECT_EQ(state_.find_job(id)->last_error, "still broken");
  // It is collected once, after the retention of its *second* death.
  advance(state_.options().retain_dead_ms - 1);
  EXPECT_NE(state_.find_job(id), nullptr);
  advance(1);
  EXPECT_EQ(state_.find_job(id), nullptr);
}

// --- wall-clock jumps ----------------------------------------------------------------------

class ClockJumpTest : public EngineFixture {
 protected:
  void SetUp() override { set_log_level(LogLevel::kError); }  // jumps log a warning
  void TearDown() override {
    EngineFixture::TearDown();
    set_log_level(LogLevel::kInfo);
  }

  // The wall clock is stepped; monotonic time is unaffected.
  void jump(DurationMs ms) {
    clock_.jump_wall(ms);
    engine_.tick();
  }
};

TEST_F(ClockJumpTest, ForwardJumpRunsDueJobsAndGivesLeasesAGracePeriod) {
  const JobId delayed = enqueue({.queue = "q", .payload = "x", .delay_ms = 60'000});
  const JobId working = enqueue({.queue = "w", .payload = "x"});
  ASSERT_TRUE(reserve("w", 10'000).has_value());

  jump(3'600'000);  // e.g. the machine resumed from an hour of suspend
  EXPECT_EQ(engine_.clock_jumps_detected(), 1U);
  EXPECT_EQ(state_of(delayed), JobState::kReady) << "its wall-clock time really has passed";
  EXPECT_EQ(state_of(working), JobState::kLeased)
      << "the worker could not heartbeat while time was jumping: no mass expiry";

  advance(engine_.options().lease_grace_ms - 1);
  EXPECT_EQ(state_of(working), JobState::kLeased);
  advance(1);
  EXPECT_NE(state_of(working), JobState::kLeased)
      << "after the grace period, silence still expires";
}

TEST_F(ClockJumpTest, HeartbeatAfterAForwardJumpKeepsTheLease) {
  const JobId id = enqueue();
  const auto lease = reserve("q", 10'000);
  ASSERT_TRUE(lease.has_value());
  jump(3'600'000);
  ASSERT_TRUE(engine_.heartbeat(id, lease->token, 10'000).ok());
  advance(9'999);
  EXPECT_EQ(state_of(id), JobState::kLeased);
  EXPECT_TRUE(engine_.ack(id, lease->token).ok());
}

TEST_F(ClockJumpTest, BackwardJumpMakesDelayedJobsWaitForTheWallClock) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .delay_ms = 10'000});
  jump(-60'000);
  EXPECT_EQ(engine_.clock_jumps_detected(), 1U);

  advance(10'000);
  EXPECT_EQ(state_of(id), JobState::kScheduled) << "its run_at is 60 s further away now";
  advance(59'999);
  EXPECT_EQ(state_of(id), JobState::kScheduled);
  advance(1);
  EXPECT_EQ(state_of(id), JobState::kReady);
}

TEST_F(ClockJumpTest, BackwardJumpNeverShortensALease) {
  const JobId id = enqueue();
  ASSERT_TRUE(reserve("q", 10'000).has_value());
  jump(-60'000);
  advance(10'000);
  EXPECT_EQ(state_of(id), JobState::kLeased) << "the persisted expiry is further away, not nearer";
  advance(60'000);
  EXPECT_NE(state_of(id), JobState::kLeased);
}

// Found by the chaos harness's log checker: the timer runs on the monotonic
// clock, the expiry is a wall-clock time, and when the wall clock was a
// millisecond behind, the lease was expired - and logged as expired - a
// millisecond before the expiry the worker had been given.
TEST_F(ClockJumpTest, ALeaseIsNeverExpiredBeforeItsWallClockExpiry) {
  const JobId id = enqueue();
  const auto lease = reserve("q", 10'000);
  ASSERT_TRUE(lease.has_value());
  clock_.jump_wall(-5);  // far below the jump detector's threshold: nobody notices

  advance(10'000);  // the timer fires now, but by the wall clock 5 ms remain
  EXPECT_EQ(state_of(id), JobState::kLeased);
  EXPECT_TRUE(engine_.heartbeat(id, lease->token, 1'000).ok()) << "the worker is still in time";
  EXPECT_EQ(engine_.clock_jumps_detected(), 0U);

  advance(999);
  EXPECT_EQ(state_of(id), JobState::kLeased);
  advance(1);
  EXPECT_EQ(state_of(id), JobState::kScheduled) << "expired, and retried after a backoff";

  // And the log agrees with itself: no expiry is dated before the lease's end.
  WallTime expires_at{};
  for (const auto& entry : sink_.entries()) {
    const Record record = decode_record(static_cast<uint8_t>(entry.type), entry.payload).value();
    if (const auto* extended = std::get_if<LeaseExtended>(&record)) {
      expires_at = extended->lease_expires_at;
    } else if (const auto* failed = std::get_if<AttemptFailed>(&record)) {
      EXPECT_EQ(failed->reason, FailureReason::kLeaseExpired);
      EXPECT_GE(failed->at, expires_at);
    }
  }
}

TEST_F(ClockJumpTest, SlewAndSmallStepsDoNotTriggerARebuild) {
  enqueue({.queue = "q", .payload = "x", .delay_ms = 500});
  for (int i = 0; i < 1'000; ++i) {
    clock_.advance(1);
    clock_.jump_wall(i % 2 == 0 ? 1 : -1);  // jitter
    engine_.tick();
  }
  jump(engine_.options().clock_jump_threshold_ms);  // exactly the threshold: still not a jump
  EXPECT_EQ(engine_.clock_jumps_detected(), 0U);
  jump(engine_.options().clock_jump_threshold_ms + 1);
  EXPECT_EQ(engine_.clock_jumps_detected(), 1U);
  jump(-(engine_.options().clock_jump_threshold_ms + 1));
  EXPECT_EQ(engine_.clock_jumps_detected(), 2U);
}

TEST_F(ClockJumpTest, SlowDriftIsAbsorbedNotAccumulated) {
  // 0.5 ms of drift per tick adds up to far more than the threshold overall, but
  // no single tick sees a step.
  for (int i = 0; i < 10'000; ++i) {
    clock_.advance(1'000);
    if (i % 2 == 0) clock_.jump_wall(1);
    engine_.tick();
  }
  EXPECT_EQ(engine_.clock_jumps_detected(), 0U);
}

}  // namespace
}  // namespace baton
