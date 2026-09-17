// LeaseModel is the oracle of the chaos harness's lease invariant, so it is
// tested from both sides: real histories produced by the Engine must pass, and
// every kind of forbidden history, written by hand, must be caught.

#include "check/lease_model.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "support/storage_fixture.h"

namespace baton {
namespace {

::testing::AssertionResult Mentions(const LeaseModel& model, std::string_view text) {
  for (const std::string& violation : model.violations()) {
    if (violation.find(text) != std::string::npos) return ::testing::AssertionSuccess();
  }
  auto failure = ::testing::AssertionFailure() << "no violation mentions \"" << text << "\" in:";
  for (const std::string& violation : model.violations()) failure << "\n  " << violation;
  return failure;
}

JobEnqueued enqueued(JobId id) { return JobEnqueued{.id = id, .queue = "q", .max_attempts = 5}; }
JobLeased leased(JobId id, LeaseToken token, int64_t expires_at) {
  return JobLeased{.id = id, .token = token, .lease_expires_at = WallTime{expires_at}, .at = {}};
}
AttemptFailed expired(JobId id, LeaseToken token, int64_t at) {
  return AttemptFailed{.id = id,
                       .token = token,
                       .reason = FailureReason::kLeaseExpired,
                       .at = WallTime{at},
                       .retry_at = WallTime{at}};
}

class LeaseModelTest : public ::testing::Test {
 protected:
  void feed(const Record& record) { model_.on(++lsn_, record); }

  LeaseModel model_;
  Lsn lsn_ = 0;
};

// --- real histories pass ------------------------------------------------------------------

class LeaseModelHistoryTest : public ::testing::Test {
 protected:
  LeaseModelHistoryTest() : engine_(state_, sink_, clock_, EngineOptions{}, /*rng_seed=*/3) {}

  void SetUp() override {
    // Every kind of record: acks, failures, dead-lettering, cancellations of
    // leased jobs, leases that expire, heartbeats, DLQ retry and purge.
    run_mixed_traffic(engine_, clock_, 600);
    const std::vector<std::string_view> queues = {"a", "b"};
    for (int i = 0; i < 40; ++i) {
      auto lease = engine_.reserve(queues, 1'000);
      ASSERT_TRUE(lease.ok());
      if (!lease->has_value()) break;
      if (i % 4 == 0) {
        ASSERT_TRUE(engine_.heartbeat((*lease)->id, (*lease)->token, 5'000).ok());
      } else if (i % 4 == 1) {
        ASSERT_TRUE(engine_.cancel((*lease)->id).ok());
      }
    }
    clock_.advance(60'000);  // the rest expire
    engine_.tick();
    (void)engine_.dlq_retry_all("a");
    (void)engine_.dlq_purge_all("b");
  }

  void feed(LeaseModel& model, size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      const auto& entry = sink_.entries()[i];
      const auto record = decode_record(static_cast<uint8_t>(entry.type), entry.payload);
      ASSERT_TRUE(record.ok()) << record.error().to_string();
      model.on(entry.lsn, *record);
    }
  }

  FakeClock clock_;
  MemoryRecordSink sink_;
  State state_;
  Engine engine_;
};

TEST_F(LeaseModelHistoryTest, WhatTheEngineLogsNeverViolatesTheRules) {
  LeaseModel model;
  feed(model, 0, sink_.entries().size());
  EXPECT_EQ(model.violation_count(), 0U) << model.violations().front();
  EXPECT_GT(model.counters().leases_granted, 200U);
  EXPECT_GT(model.counters().acks, 0U);
  EXPECT_GT(model.counters().worker_failures, 0U);
  EXPECT_GT(model.counters().lease_expiries, 0U);
  EXPECT_GT(model.counters().heartbeats, 0U);
  EXPECT_GT(model.counters().cancellations, 0U);
  EXPECT_EQ(model.max_token(), model.counters().leases_granted);
}

// The log checker starts from a snapshot when compaction has removed the
// beginning of the log: leases that were open at that point must carry over.
TEST_F(LeaseModelHistoryTest, StartingFromASnapshotGivesTheSameVerdict) {
  for (const size_t cut : {size_t{1}, size_t{333}, size_t{700}, sink_.entries().size()}) {
    LeaseModel model;
    model.start_from(image_after(sink_, cut));
    feed(model, cut, sink_.entries().size());
    EXPECT_EQ(model.violation_count(), 0U)
        << "cut at " << cut << ": " << model.violations().front();
  }
}

TEST_F(LeaseModelHistoryTest, ALostRecordShowsUpAsAViolation) {
  // Drop the record that ended some lease of a job that was leased again later,
  // as a log that lost a record would: the later lease then overlaps the one
  // that was never seen to end.
  const auto decoded = [&](size_t i) {
    const auto& entry = sink_.entries()[i];
    return decode_record(static_cast<uint8_t>(entry.type), entry.payload).value();
  };
  size_t dropped = sink_.entries().size();
  for (size_t i = 0; i < sink_.entries().size() && dropped == sink_.entries().size(); ++i) {
    const Record ended = decoded(i);
    const auto* failed = std::get_if<AttemptFailed>(&ended);
    if (failed == nullptr || failed->dead) continue;
    for (size_t later = i + 1; later < sink_.entries().size(); ++later) {
      const Record next = decoded(later);
      const auto* lease = std::get_if<JobLeased>(&next);
      if (lease != nullptr && lease->id == failed->id) {
        dropped = i;
        break;
      }
    }
  }
  ASSERT_LT(dropped, sink_.entries().size()) << "the traffic never retried a failed job";
  LeaseModel model;
  feed(model, 0, dropped);
  feed(model, dropped + 1, sink_.entries().size());
  EXPECT_GT(model.violation_count(), 0U);
  EXPECT_TRUE(Mentions(model, "two valid leases"));
}

// --- forbidden histories are caught -------------------------------------------------------

TEST_F(LeaseModelTest, TwoValidLeasesOnOneJob) {
  feed(enqueued(1));
  feed(leased(1, 1, 5'000));
  feed(leased(1, 2, 9'000));
  EXPECT_EQ(model_.violation_count(), 1U);
  EXPECT_TRUE(Mentions(model_, "LSN 3: job 1 was leased with token 2 while token 1 was still"));
}

TEST_F(LeaseModelTest, ALeaseAfterTheOldOneEndedIsFine) {
  feed(enqueued(1));
  feed(leased(1, 1, 5'000));
  feed(expired(1, 1, 5'000));
  feed(leased(1, 2, 9'000));
  feed(JobSucceeded{.id = 1, .token = 2, .at = {}});
  EXPECT_EQ(model_.violation_count(), 0U);
  EXPECT_EQ(model_.open_leases(), 0U);
}

TEST_F(LeaseModelTest, AStaleAckThatWasAccepted) {
  feed(enqueued(1));
  feed(leased(1, 1, 5'000));
  feed(expired(1, 1, 5'000));
  feed(leased(1, 2, 9'000));
  feed(JobSucceeded{.id = 1, .token = 1, .at = {}});  // the zombie's token
  EXPECT_TRUE(Mentions(model_,
                       "ack for job 1 with stale token 1 was accepted; the current "
                       "token is 2"));
}

TEST_F(LeaseModelTest, AStaleHeartbeatOrFailureThatWasAccepted) {
  feed(enqueued(1));
  feed(leased(1, 4, 5'000));
  feed(LeaseExtended{.id = 1, .token = 3, .lease_expires_at = WallTime{9'000}, .at = {}});
  feed(AttemptFailed{.id = 1, .token = 2, .reason = FailureReason::kWorkerFailed});
  EXPECT_EQ(model_.violation_count(), 2U);
  EXPECT_TRUE(Mentions(model_, "heartbeat for job 1 with stale token 3"));
  EXPECT_TRUE(Mentions(model_, "failure for job 1 with stale token 2"));
}

TEST_F(LeaseModelTest, ActingOnAJobThatIsNotLeased) {
  feed(enqueued(1));
  feed(JobSucceeded{.id = 1, .token = 1, .at = {}});
  feed(LeaseExtended{.id = 7, .token = 1, .lease_expires_at = {}, .at = {}});
  EXPECT_EQ(model_.violation_count(), 2U);
  EXPECT_TRUE(Mentions(model_, "ack for job 1 with token 1, but the job has no current lease"));
}

TEST_F(LeaseModelTest, TokensMustGrowAcrossTheWholeServer) {
  feed(enqueued(1));
  feed(enqueued(2));
  feed(leased(1, 5, 5'000));
  feed(leased(2, 5, 5'000));  // reused
  feed(JobSucceeded{.id = 1, .token = 5, .at = {}});
  feed(leased(1, 3, 5'000));  // went backwards
  EXPECT_EQ(model_.violation_count(), 2U);
  EXPECT_TRUE(Mentions(model_, "job 2 got token 5, which is not larger than the earlier token 5"));
  EXPECT_TRUE(Mentions(model_, "job 1 got token 3"));
}

TEST_F(LeaseModelTest, ALeaseExpiredBeforeItsTime) {
  feed(enqueued(1));
  feed(leased(1, 1, 5'000));
  feed(LeaseExtended{.id = 1, .token = 1, .lease_expires_at = WallTime{8'000}, .at = {}});
  feed(expired(1, 1, 7'999));  // the heartbeat had moved the expiry to 8000
  EXPECT_TRUE(Mentions(model_, "was expired at 7999 ms, before its recorded expiry at 8000 ms"));

  feed(leased(1, 2, 20'000));
  feed(expired(1, 2, 20'000));  // exactly on time is fine, and so is late
  EXPECT_EQ(model_.violation_count(), 1U);
}

TEST_F(LeaseModelTest, CancellingEndsALease) {
  feed(enqueued(1));
  feed(leased(1, 1, 5'000));
  feed(JobCancelled{.id = 1, .at = {}});
  EXPECT_EQ(model_.open_leases(), 0U);
  feed(JobSucceeded{.id = 1, .token = 1, .at = {}});  // an ack over a cancellation
  EXPECT_TRUE(Mentions(model_, "no current lease"));
}

TEST_F(LeaseModelTest, JobIdsAreNeverReusedAndLeasedJobsNeverPurged) {
  feed(enqueued(1));
  feed(enqueued(2));
  feed(enqueued(2));
  feed(leased(1, 1, 5'000));
  feed(JobsPurged{.ids = {1}});
  feed(DeadJobRetried{.id = 1, .run_at = {}, .at = {}});
  EXPECT_EQ(model_.violation_count(), 3U);
  EXPECT_TRUE(Mentions(model_, "job id 2 was used before"));
  EXPECT_TRUE(Mentions(model_, "job 1 was purged while leased"));
  EXPECT_TRUE(Mentions(model_, "dead job 1 was retried while it had a lease"));
}

TEST_F(LeaseModelTest, ReportsAreCappedButTheCountIsNot) {
  for (int i = 0; i < 200; ++i) feed(JobSucceeded{.id = 9, .token = 1, .at = {}});
  EXPECT_EQ(model_.violation_count(), 200U);
  EXPECT_EQ(model_.violations().size(), LeaseModel::kMaxReported);
}

}  // namespace
}  // namespace baton
