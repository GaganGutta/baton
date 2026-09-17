#include "state/engine.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "support/engine_fixture.h"

namespace baton {
namespace {

class EngineTest : public EngineFixture {};

// --- enqueue ---------------------------------------------------------------------------

TEST_F(EngineTest, EnqueueAssignsIncreasingIdsAndAppliesDefaults) {
  const JobId first = enqueue("emails", "hello");
  const JobId second = enqueue("emails", "world");
  EXPECT_EQ(first, 1U);
  EXPECT_EQ(second, 2U);

  const Job* job = state_.find_job(first);
  ASSERT_NE(job, nullptr);
  EXPECT_EQ(job->queue->name, "emails");
  EXPECT_EQ(job->payload.view(), "hello");
  EXPECT_EQ(job->state, JobState::kReady);
  EXPECT_EQ(job->priority, 0);
  EXPECT_EQ(job->attempts, 0U);
  EXPECT_EQ(job->max_attempts, engine_.options().default_max_attempts);
  EXPECT_EQ(job->backoff_base_ms, engine_.options().default_backoff_base_ms);
  EXPECT_EQ(job->created_at, clock_.wall_now());
  EXPECT_EQ(state_.find_queue("emails")->totals.enqueued, 2U);
  EXPECT_EQ(engine_.last_lsn(), 2U);
}

TEST_F(EngineTest, EnqueueRejectsBadInputWithoutLoggingAnything) {
  const auto expect_error = [this](const EnqueueRequest& request, ErrorCode code) {
    const auto result = engine_.enqueue(request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), code) << result.error().to_string();
  };
  expect_error({.queue = "", .payload = "x"}, ErrorCode::kInvalidArgument);
  expect_error({.queue = "has space", .payload = "x"}, ErrorCode::kInvalidArgument);
  expect_error({.queue = std::string(129, 'q'), .payload = "x"}, ErrorCode::kInvalidArgument);
  expect_error({.queue = "q", .payload = "x", .delay_ms = -1}, ErrorCode::kInvalidArgument);
  expect_error({.queue = "q", .payload = "x", .max_attempts = 0}, ErrorCode::kInvalidArgument);
  expect_error({.queue = "q", .payload = "x", .max_attempts = 1'000'000},
               ErrorCode::kInvalidArgument);
  expect_error({.queue = "q", .payload = "x", .idem_key = std::string(257, 'k')},
               ErrorCode::kInvalidArgument);
  expect_error({.queue = "q", .payload = "x", .run_at = clock_.wall_now() + (DurationMs{1} << 40)},
               ErrorCode::kInvalidArgument);

  const std::string big(engine_.options().max_payload_bytes + 1, 'x');
  expect_error({.queue = "q", .payload = big}, ErrorCode::kLimitExceeded);

  EXPECT_TRUE(sink_.entries().empty());
  EXPECT_EQ(state_.job_count(), 0U);
  EXPECT_EQ(engine_.last_lsn(), 0U);
}

TEST_F(EngineTest, QueueNameRules) {
  EXPECT_TRUE(is_valid_queue_name("default"));
  EXPECT_TRUE(is_valid_queue_name("A-z_0.9:x"));
  EXPECT_TRUE(is_valid_queue_name(std::string(128, 'q')));
  EXPECT_FALSE(is_valid_queue_name(""));
  EXPECT_FALSE(is_valid_queue_name("white space"));
  EXPECT_FALSE(is_valid_queue_name("new\nline"));
  EXPECT_FALSE(is_valid_queue_name("sl/ash"));
  EXPECT_FALSE(is_valid_queue_name("\xC3\xA9"));
}

TEST_F(EngineTest, PayloadAtTheLimitIsAccepted) {
  const std::string exact(engine_.options().max_payload_bytes, 'x');
  EXPECT_TRUE(engine_.enqueue({.queue = "q", .payload = exact}).ok());
}

// --- delayed jobs ------------------------------------------------------------------------

TEST_F(EngineTest, DelayedJobIsHandedOutOnlyAfterItsDelay) {
  const JobId id = enqueue({.queue = "q", .payload = "later", .delay_ms = 5'000});
  EXPECT_EQ(state_of(id), JobState::kScheduled);
  EXPECT_FALSE(reserve().has_value());

  advance(4'999);
  EXPECT_FALSE(reserve().has_value());
  advance(1);
  EXPECT_EQ(state_of(id), JobState::kReady);
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(lease->id, id);
}

TEST_F(EngineTest, AbsoluteRunAtWorksAndThePastIsClampedToNow) {
  const WallTime now = clock_.wall_now();
  const JobId future = enqueue({.queue = "q", .payload = "x", .run_at = now + 250});
  const JobId past = enqueue({.queue = "q", .payload = "x", .run_at = now + (-10'000)});
  EXPECT_EQ(state_.find_job(future)->run_at, now + 250);
  EXPECT_EQ(state_.find_job(past)->run_at, now) << "a past run-at must not jump the FIFO";
  EXPECT_EQ(state_of(past), JobState::kReady);
  EXPECT_EQ(state_of(future), JobState::kScheduled);
}

TEST_F(EngineTest, NextDeadlineTracksTheEarliestTimer) {
  EXPECT_EQ(engine_.next_deadline(), std::nullopt);
  enqueue({.queue = "q", .payload = "x", .delay_ms = 3'000});
  const auto deadline = engine_.next_deadline();
  ASSERT_TRUE(deadline.has_value());
  EXPECT_LE(*deadline, clock_.mono_now() + 3'000);
  EXPECT_GT(*deadline, clock_.mono_now());
}

// --- reserve -------------------------------------------------------------------------------

TEST_F(EngineTest, ReserveFollowsPriorityThenFifo) {
  const JobId normal_1 = enqueue({.queue = "q", .payload = "n1"});
  advance(1);
  const JobId normal_2 = enqueue({.queue = "q", .payload = "n2"});
  const JobId urgent = enqueue({.queue = "q", .payload = "u", .priority = 10});
  const JobId lazy = enqueue({.queue = "q", .payload = "l", .priority = -10});

  std::vector<JobId> order;
  while (const auto lease = reserve()) order.push_back(lease->id);
  EXPECT_EQ(order, (std::vector<JobId>{urgent, normal_1, normal_2, lazy}));
}

TEST_F(EngineTest, ReserveChecksQueuesInTheOrderGiven) {
  const JobId in_b = enqueue("b", "x");
  const JobId in_a = enqueue("a", "x");
  const std::vector<std::string_view> b_then_a = {"missing", "b", "a"};
  auto first = engine_.reserve(b_then_a, 1000);
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->id, in_b);
  EXPECT_EQ((*first)->queue, "b");
  auto second = engine_.reserve(b_then_a, 1000);
  ASSERT_TRUE(second.ok() && second->has_value());
  EXPECT_EQ((*second)->id, in_a);
  auto third = engine_.reserve(b_then_a, 1000);
  ASSERT_TRUE(third.ok());
  EXPECT_FALSE(third->has_value());
}

TEST_F(EngineTest, ReservationCarriesEverythingAWorkerNeeds) {
  const JobId id = enqueue({.queue = "q", .payload = "work", .max_attempts = 3});
  const auto lease = reserve("q", 2'500);
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(lease->id, id);
  EXPECT_GT(lease->token, 0U);
  EXPECT_EQ(lease->queue, "q");
  EXPECT_EQ(lease->payload.view(), "work");
  EXPECT_EQ(lease->attempt, 1U);
  EXPECT_EQ(lease->max_attempts, 3U);
  EXPECT_EQ(lease->lease_expires_at, clock_.wall_now() + 2'500);
  EXPECT_EQ(state_of(id), JobState::kLeased);
}

TEST_F(EngineTest, ReserveValidatesItsArguments) {
  const std::vector<std::string_view> none;
  const std::vector<std::string_view> bad = {"ok", "not ok"};
  const std::vector<std::string_view> good = {"q"};
  EXPECT_EQ(engine_.reserve(none, 1000).error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(engine_.reserve(bad, 1000).error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(engine_.reserve(good, 0).error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(engine_.reserve(good, DurationMs{1} << 40).error().code(), ErrorCode::kInvalidArgument);
}

TEST_F(EngineTest, TokensIncreaseAcrossJobsAndAttempts) {
  enqueue();
  enqueue();
  LeaseToken previous = 0;
  std::vector<Reservation> first_attempts;
  for (int i = 0; i < 2; ++i) {
    const auto lease = reserve();
    ASSERT_TRUE(lease.has_value());
    EXPECT_GT(lease->token, previous);
    EXPECT_EQ(lease->attempt, 1U);
    previous = lease->token;
    first_attempts.push_back(*lease);
  }
  for (const Reservation& lease : first_attempts) {
    ASSERT_TRUE(engine_.fail({.id = lease.id, .token = lease.token, .retry_in_ms = 0}).ok());
  }
  advance(1);
  for (int i = 0; i < 2; ++i) {
    const auto lease = reserve();
    ASSERT_TRUE(lease.has_value());
    EXPECT_GT(lease->token, previous) << "a re-lease must get a fresh, larger token";
    EXPECT_EQ(lease->attempt, 2U);
    previous = lease->token;
  }
}

// --- ack ------------------------------------------------------------------------------------

TEST_F(EngineTest, AckCompletesTheJobExactlyOnce) {
  const JobId id = enqueue();
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  ASSERT_TRUE(engine_.ack(id, lease->token).ok());
  EXPECT_EQ(state_of(id), JobState::kSucceeded);
  EXPECT_EQ(state_.find_job(id)->finished_at, clock_.wall_now());
  EXPECT_EQ(state_.find_queue("q")->totals.succeeded, 1U);

  const Status again = engine_.ack(id, lease->token);
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code(), ErrorCode::kStaleToken);
  EXPECT_EQ(engine_.ack(999, 1).error().code(), ErrorCode::kNotFound);
  EXPECT_FALSE(reserve().has_value()) << "a finished job is never handed out again";
}

// The fencing guarantee: once a job has been leased again, the previous holder
// can do nothing to it.
TEST_F(EngineTest, ZombieWorkerIsFencedOffAfterItsLeaseExpires) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .backoff_base_ms = 0});
  const auto zombie = reserve("q", 1'000);
  ASSERT_TRUE(zombie.has_value());

  advance(1'001);  // the zombie stalls past its lease
  EXPECT_NE(state_of(id), JobState::kLeased);
  EXPECT_EQ(state_.find_job(id)->last_error, "lease expired");
  advance(1);
  const auto successor = reserve("q", 1'000);
  ASSERT_TRUE(successor.has_value());
  EXPECT_EQ(successor->id, id);
  EXPECT_GT(successor->token, zombie->token);
  EXPECT_EQ(successor->attempt, 2U);

  // The zombie wakes up and tries everything.
  EXPECT_EQ(engine_.ack(id, zombie->token).error().code(), ErrorCode::kStaleToken);
  EXPECT_EQ(engine_.heartbeat(id, zombie->token, 1'000).error().code(), ErrorCode::kStaleToken);
  EXPECT_EQ(engine_.fail({.id = id, .token = zombie->token, .error = "boo"}).error().code(),
            ErrorCode::kStaleToken);
  EXPECT_EQ(state_of(id), JobState::kLeased) << "the successor's lease is untouched";
  EXPECT_EQ(state_.find_job(id)->lease_token, successor->token);

  EXPECT_TRUE(engine_.ack(id, successor->token).ok());
}

TEST_F(EngineTest, StaleTokenIsRejectedEvenBeforeTheJobIsLeasedAgain) {
  const JobId id = enqueue();
  const auto lease = reserve("q", 1'000);
  ASSERT_TRUE(lease.has_value());
  advance(1'001);
  ASSERT_TRUE(is_pending(state_of(id)));
  EXPECT_EQ(engine_.ack(id, lease->token).error().code(), ErrorCode::kStaleToken);
}

// --- heartbeat ------------------------------------------------------------------------------

TEST_F(EngineTest, HeartbeatKeepsALeaseAlive) {
  const JobId id = enqueue();
  const auto lease = reserve("q", 1'000);
  ASSERT_TRUE(lease.has_value());

  for (int beat = 0; beat < 5; ++beat) {
    advance(800);
    const auto expiry = engine_.heartbeat(id, lease->token, 1'000);
    ASSERT_TRUE(expiry.ok()) << expiry.error().to_string();
    EXPECT_EQ(*expiry, clock_.wall_now() + 1'000);
  }
  EXPECT_EQ(state_of(id), JobState::kLeased) << "4 seconds in, thanks to heartbeats";
  EXPECT_EQ(state_.pending_timers(), 1U) << "each heartbeat replaces the expiry timer";

  advance(1'000);
  EXPECT_NE(state_of(id), JobState::kLeased) << "no heartbeat, no lease";
}

TEST_F(EngineTest, HeartbeatValidatesLeaseLength) {
  const JobId id = enqueue();
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(engine_.heartbeat(id, lease->token, 0).error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(engine_.heartbeat(12345, 1, 1000).error().code(), ErrorCode::kNotFound);
}

// --- fail, retries, dead letters ----------------------------------------------------------------

TEST_F(EngineTest, FailSchedulesARetryWithinTheBackoffCeiling) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .backoff_base_ms = 1'000});
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  const auto failed = engine_.fail({.id = id, .token = lease->token, .error = "boom"});
  ASSERT_TRUE(failed.ok());
  EXPECT_FALSE(failed->dead);
  EXPECT_GE(failed->retry_at, clock_.wall_now());
  EXPECT_LE(failed->retry_at, clock_.wall_now() + 1'000) << "first retry: at most base";

  const Job* job = state_.find_job(id);
  EXPECT_EQ(job->last_error, "boom");
  EXPECT_EQ(job->run_at, failed->retry_at);
  EXPECT_EQ(job->lease_token, 0U);
  EXPECT_EQ(state_.find_queue("q")->totals.failed_attempts, 1U);
}

TEST_F(EngineTest, BackoffCeilingDoublesAndIsCapped) {
  EXPECT_EQ(Engine::backoff_ceiling(1, 1'000, 60'000), 1'000);
  EXPECT_EQ(Engine::backoff_ceiling(2, 1'000, 60'000), 2'000);
  EXPECT_EQ(Engine::backoff_ceiling(5, 1'000, 60'000), 16'000);
  EXPECT_EQ(Engine::backoff_ceiling(7, 1'000, 60'000), 60'000);
  EXPECT_EQ(Engine::backoff_ceiling(40, 1'000, 60'000), 60'000) << "no shift overflow";
  EXPECT_EQ(Engine::backoff_ceiling(1'000'000, 1, 5), 5);
  EXPECT_EQ(Engine::backoff_ceiling(3, 0, 60'000), 0);
}

TEST_F(EngineTest, JitterIsFullAndStaysWithinBounds) {
  std::set<DurationMs> delays;
  for (int i = 0; i < 200; ++i) {
    const JobId id = enqueue({.queue = "q", .payload = "x", .backoff_base_ms = 10'000});
    const auto lease = reserve();
    ASSERT_TRUE(lease.has_value());
    const auto failed = engine_.fail({.id = id, .token = lease->token});
    ASSERT_TRUE(failed.ok());
    const DurationMs delay = failed->retry_at - clock_.wall_now();
    ASSERT_GE(delay, 0);
    ASSERT_LE(delay, 10'000);
    delays.insert(delay);
    ASSERT_TRUE(engine_.cancel(id).ok());
  }
  EXPECT_GT(delays.size(), 150U) << "jitter should spread retries out";
  EXPECT_LT(*delays.begin(), 1'500) << "full jitter reaches down towards zero";
  EXPECT_GT(*delays.rbegin(), 8'500) << "...and up towards the ceiling";
}

TEST_F(EngineTest, ExhaustedAttemptsSendTheJobToTheDeadLetterQueue) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .max_attempts = 2, .backoff_base_ms = 0});
  auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  auto failed = engine_.fail({.id = id, .token = lease->token, .error = "first"});
  ASSERT_TRUE(failed.ok());
  EXPECT_FALSE(failed->dead);

  advance(1);
  lease = reserve();
  ASSERT_TRUE(lease.has_value());
  failed = engine_.fail({.id = id, .token = lease->token, .error = "second"});
  ASSERT_TRUE(failed.ok());
  EXPECT_TRUE(failed->dead);

  EXPECT_EQ(state_of(id), JobState::kDead);
  EXPECT_EQ(state_.find_job(id)->last_error, "second");
  EXPECT_TRUE(state_.find_queue("q")->dead.contains(id));
  EXPECT_EQ(state_.find_queue("q")->totals.dead, 1U);
  EXPECT_FALSE(reserve().has_value());
}

TEST_F(EngineTest, NoRetryAndRetryInOverrideTheBackoff) {
  const JobId doomed = enqueue();
  auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  const auto dead = engine_.fail({.id = doomed, .token = lease->token, .no_retry = true});
  ASSERT_TRUE(dead.ok());
  EXPECT_TRUE(dead->dead);
  EXPECT_EQ(state_of(doomed), JobState::kDead);

  const JobId patient = enqueue();
  lease = reserve();
  ASSERT_TRUE(lease.has_value());
  const auto later = engine_.fail({.id = patient, .token = lease->token, .retry_in_ms = 7'777});
  ASSERT_TRUE(later.ok());
  EXPECT_EQ(later->retry_at, clock_.wall_now() + 7'777);
  EXPECT_EQ(engine_.fail({.id = patient, .token = 1, .retry_in_ms = -5}).error().code(),
            ErrorCode::kInvalidArgument);
}

TEST_F(EngineTest, LeaseExpiryConsumesAnAttemptAndCanKillTheJob) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .max_attempts = 1});
  ASSERT_TRUE(reserve("q", 500).has_value());
  advance(501);
  EXPECT_EQ(state_of(id), JobState::kDead);
  EXPECT_EQ(state_.find_job(id)->last_error, "lease expired");
  EXPECT_EQ(state_.find_queue("q")->totals.failed_attempts, 1U);
}

TEST_F(EngineTest, LongErrorMessagesAreTruncated) {
  const JobId id = enqueue();
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  const std::string essay(5'000, 'e');
  ASSERT_TRUE(engine_.fail({.id = id, .token = lease->token, .error = essay}).ok());
  EXPECT_EQ(state_.find_job(id)->last_error.size(), EngineOptions::kMaxErrorBytes);
}

// --- cancel ---------------------------------------------------------------------------------

TEST_F(EngineTest, CancelWorksInEveryLiveState) {
  const JobId ready = enqueue();
  const JobId scheduled = enqueue({.queue = "q", .payload = "x", .delay_ms = 60'000});
  const JobId leased = enqueue({.queue = "other", .payload = "x"});
  const auto lease = reserve("other");
  ASSERT_TRUE(lease.has_value());

  for (const JobId id : {ready, scheduled, leased}) {
    ASSERT_TRUE(engine_.cancel(id).ok());
    EXPECT_EQ(state_of(id), JobState::kCancelled);
  }
  EXPECT_EQ(state_.pending_timers(), 0U) << "due and lease timers must be cancelled too";
  EXPECT_FALSE(reserve().has_value());

  // The worker that held the lease finds out on its next call.
  EXPECT_EQ(engine_.heartbeat(leased, lease->token, 1000).error().code(), ErrorCode::kStaleToken);
  EXPECT_EQ(engine_.ack(leased, lease->token).error().code(), ErrorCode::kStaleToken);

  EXPECT_EQ(engine_.cancel(ready).error().code(), ErrorCode::kFailedPrecondition);
  EXPECT_EQ(engine_.cancel(404).error().code(), ErrorCode::kNotFound);
  EXPECT_EQ(state_.find_queue("q")->totals.cancelled, 2U);
}

// --- idempotent enqueue -------------------------------------------------------------------------

TEST_F(EngineTest, SameKeyReturnsTheSameJobWithinTheWindow) {
  const auto first = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "order-1"});
  ASSERT_TRUE(first.ok());
  EXPECT_TRUE(first->created);
  const Lsn lsn_after_first = engine_.last_lsn();

  advance(1'000);
  const auto again = engine_.enqueue({.queue = "q", .payload = "different", .idem_key = "order-1"});
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->created);
  EXPECT_EQ(again->id, first->id);
  EXPECT_EQ(engine_.last_lsn(), lsn_after_first) << "a duplicate logs nothing";
  EXPECT_EQ(state_.job_count(), 1U);

  const auto other = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "order-2"});
  ASSERT_TRUE(other.ok());
  EXPECT_TRUE(other->created);
  EXPECT_NE(other->id, first->id);
}

TEST_F(EngineTest, KeyStillMatchesAfterTheJobFinishedAndWasCollected) {
  const auto first = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "k"});
  ASSERT_TRUE(first.ok());
  const auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  ASSERT_TRUE(engine_.ack(first->id, lease->token).ok());
  advance(state_.options().retain_finished_ms + 1);
  ASSERT_EQ(state_.find_job(first->id), nullptr) << "the finished job has been collected";

  const auto again = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "k"});
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->created) << "the key outlives the job: the work must not run twice";
  EXPECT_EQ(again->id, first->id);
}

TEST_F(EngineTest, KeyExpiresAfterTheWindow) {
  const auto first = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "k"});
  ASSERT_TRUE(first.ok());
  advance(engine_.options().idempotency_window_ms - 1);
  EXPECT_FALSE(engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "k"})->created);
  advance(1);
  EXPECT_EQ(state_.idem_count(), 0U) << "expired keys are collected";
  const auto fresh = engine_.enqueue({.queue = "q", .payload = "a", .idem_key = "k"});
  ASSERT_TRUE(fresh.ok());
  EXPECT_TRUE(fresh->created);
  EXPECT_NE(fresh->id, first->id);
}

// Expiry is defined by the timestamp, not by whether the entry has been collected
// yet. Collection can lag: after the idempotency window is shortened (a config
// change between restarts), an older long-lived key sits at the front of the
// collection queue and the expired short-lived key behind it is still physically
// present. It must count as absent all the same.
TEST_F(EngineTest, ExpiredKeyCountsAsAbsentEvenBeforeItIsCollected) {
  EngineOptions long_window = engine_.options();
  long_window.idempotency_window_ms = 100'000;
  EngineOptions short_window = engine_.options();
  short_window.idempotency_window_ms = 1'000;
  Engine before_restart(state_, sink_, clock_, long_window, /*rng_seed=*/1);
  Engine after_restart(state_, sink_, clock_, short_window, /*rng_seed=*/2);

  ASSERT_TRUE(before_restart.enqueue({.queue = "q", .payload = "x", .idem_key = "slow"}).ok());
  const auto first = after_restart.enqueue({.queue = "q", .payload = "x", .idem_key = "fast"});
  ASSERT_TRUE(first.ok());

  advance(5'000);
  ASSERT_NE(state_.find_idem("fast"), nullptr) << "precondition: not collected yet";
  ASSERT_LE(state_.find_idem("fast")->expires_at, clock_.wall_now());

  const auto second = after_restart.enqueue({.queue = "q", .payload = "x", .idem_key = "fast"});
  ASSERT_TRUE(second.ok());
  EXPECT_TRUE(second->created) << "an expired key must not block a new job";
  EXPECT_NE(second->id, first->id);
  EXPECT_EQ(state_.find_idem("fast")->job_id, second->id);
}

// --- retention and memory ------------------------------------------------------

TEST_F(EngineTest, FinishedJobsAreCollectedAfterRetentionAndDeadOnesLater) {
  const JobId done = enqueue();
  auto lease = reserve();
  ASSERT_TRUE(lease.has_value());
  ASSERT_TRUE(engine_.ack(done, lease->token).ok());

  const JobId dead = enqueue();
  lease = reserve();
  ASSERT_TRUE(lease.has_value());
  ASSERT_TRUE(engine_.fail({.id = dead, .token = lease->token, .no_retry = true}).ok());
  const uint64_t with_both = state_.memory_bytes();

  advance(state_.options().retain_finished_ms - 1);
  EXPECT_NE(state_.find_job(done), nullptr);
  advance(1);
  EXPECT_EQ(state_.find_job(done), nullptr);
  EXPECT_NE(state_.find_job(dead), nullptr) << "dead jobs wait for a human much longer";
  EXPECT_LT(state_.memory_bytes(), with_both);

  advance(state_.options().retain_dead_ms);
  EXPECT_EQ(state_.find_job(dead), nullptr);
  EXPECT_EQ(state_.memory_bytes(), 0U);
  EXPECT_EQ(state_.find_queue("q")->totals.succeeded, 1U) << "totals outlive the jobs";
}

TEST_F(EngineTest, ReadyNotificationsNameQueuesThatGainedWork) {
  EXPECT_TRUE(state_.take_ready_notifications().empty());
  enqueue("a", "x");
  enqueue("a", "y");
  enqueue({.queue = "b", .payload = "x", .delay_ms = 100});
  auto notified = state_.take_ready_notifications();
  ASSERT_EQ(notified.size(), 1U);
  EXPECT_EQ(notified[0]->name, "a");
  EXPECT_TRUE(state_.take_ready_notifications().empty());

  advance(100);
  notified = state_.take_ready_notifications();
  ASSERT_EQ(notified.size(), 1U);
  EXPECT_EQ(notified[0]->name, "b");
}

class EngineMemoryLimitTest : public EngineFixture {
 protected:
  EngineMemoryLimitTest() : EngineFixture(EngineOptions{.max_memory_bytes = 64 * 1024}) {}
};

TEST_F(EngineMemoryLimitTest, EnqueueFailsClearlyAtTheLimitAndRecoversWhenMemoryIsFreed) {
  const std::string payload(4'096, 'p');
  std::vector<JobId> ids;
  for (;;) {
    const auto result = engine_.enqueue({.queue = "q", .payload = payload});
    if (!result.ok()) {
      EXPECT_EQ(result.error().code(), ErrorCode::kLimitExceeded);
      EXPECT_NE(result.error().message().find("max-memory"), std::string::npos);
      break;
    }
    ids.push_back(result->id);
    ASSERT_LT(ids.size(), 100U) << "the limit never triggered";
  }
  EXPECT_GT(ids.size(), 5U);
  EXPECT_LE(state_.memory_bytes(), 64U * 1024);

  // Finish a few jobs; once they are collected there is room again.
  for (int i = 0; i < 4; ++i) {
    const auto lease = reserve();
    ASSERT_TRUE(lease.has_value());
    ASSERT_TRUE(engine_.ack(lease->id, lease->token).ok());
  }
  advance(state_.options().retain_finished_ms + 1);
  EXPECT_TRUE(engine_.enqueue({.queue = "q", .payload = payload}).ok());
}

}  // namespace
}  // namespace baton
