#include "state/state.h"

#include <gtest/gtest.h>

#include <string>

#include "support/engine_fixture.h"

namespace baton {
namespace {

JobEnqueued enqueue_record(JobId id, std::string_view queue = "q", int64_t run_at = 0) {
  JobEnqueued r;
  r.id = id;
  r.queue = queue;
  r.payload = "payload";
  r.run_at = WallTime{run_at};
  r.max_attempts = 3;
  r.backoff_base_ms = 1000;
  r.backoff_cap_ms = 60000;
  r.at = WallTime{run_at};
  return r;
}

class StateTest : public ::testing::Test {
 protected:
  void SetUp() override { state_.set_now(WallTime{1'000'000}, MonoTime{5'000}); }

  void expect_rejected(const Record& record, const std::string& needle) {
    const std::string before = serialized(state_);
    const Status applied = state_.apply(record);
    ASSERT_FALSE(applied.ok()) << "the record should not fit the state";
    EXPECT_EQ(applied.error().code(), ErrorCode::kFailedPrecondition);
    EXPECT_NE(applied.error().message().find(needle), std::string::npos)
        << applied.error().message();
    EXPECT_EQ(serialized(state_), before) << "a rejected record must leave the state untouched";
    EXPECT_TRUE(state_.check_invariants().ok());
  }

  State state_;
};

// During recovery a record that does not fit the state means the log and the
// code disagree. apply() must say so instead of guessing.
TEST_F(StateTest, RecordsThatDoNotFitAreRejectedAndChangeNothing) {
  ASSERT_TRUE(state_.apply(enqueue_record(1)).ok());
  ASSERT_TRUE(state_.apply(enqueue_record(2)).ok());
  ASSERT_TRUE(state_
                  .apply(JobLeased{.id = 1,
                                   .token = 1,
                                   .lease_expires_at = WallTime{9'000'000},
                                   .at = WallTime{1'000'000}})
                  .ok());

  expect_rejected(enqueue_record(7), "expected 3");
  expect_rejected(enqueue_record(2), "expected 3");
  JobEnqueued no_queue = enqueue_record(3);
  no_queue.queue = "";
  expect_rejected(no_queue, "empty queue name");
  JobEnqueued no_attempts = enqueue_record(3);
  no_attempts.max_attempts = 0;
  expect_rejected(no_attempts, "max_attempts is 0");

  expect_rejected(JobLeased{.id = 99, .token = 2}, "does not exist");
  expect_rejected(JobLeased{.id = 1, .token = 2}, "job 1 is leased");
  expect_rejected(JobLeased{.id = 2, .token = 5}, "expected 2");

  expect_rejected(JobSucceeded{.id = 2, .token = 1}, "job 2 is");
  expect_rejected(JobSucceeded{.id = 1, .token = 7}, "record has token 7");
  expect_rejected(LeaseExtended{.id = 1, .token = 7}, "record has token 7");
  expect_rejected(AttemptFailed{.id = 99, .token = 1}, "does not exist");

  expect_rejected(JobCancelled{.id = 99}, "does not exist");
  expect_rejected(DeadJobRetried{.id = 1}, "job 1 is leased");
  expect_rejected(JobsPurged{.ids = {2}}, "job 2 is");
  expect_rejected(JobsPurged{.ids = {99}}, "does not exist");

  ASSERT_TRUE(state_.apply(JobSucceeded{.id = 1, .token = 1, .at = WallTime{1'000'001}}).ok());
  expect_rejected(JobCancelled{.id = 1}, "already succeeded");
}

TEST_F(StateTest, LeaseCannotExceedMaxAttempts) {
  JobEnqueued once = enqueue_record(1);
  once.max_attempts = 1;
  ASSERT_TRUE(state_.apply(once).ok());
  ASSERT_TRUE(state_.apply(JobLeased{.id = 1, .token = 1}).ok());
  ASSERT_TRUE(state_.apply(AttemptFailed{.id = 1, .token = 1, .retry_at = WallTime{0}}).ok());
  expect_rejected(JobLeased{.id = 1, .token = 2}, "no attempts left");
}

TEST_F(StateTest, PurgeRemovesTerminalJobs) {
  ASSERT_TRUE(state_.apply(enqueue_record(1)).ok());
  ASSERT_TRUE(state_.apply(JobCancelled{.id = 1, .at = WallTime{1}}).ok());
  ASSERT_TRUE(state_.apply(JobsPurged{.ids = {1}}).ok());
  EXPECT_EQ(state_.find_job(1), nullptr);
  EXPECT_EQ(state_.memory_bytes(), 0U);
  EXPECT_TRUE(state_.check_invariants().ok());
}

TEST_F(StateTest, DeadJobRetryResetsAttempts) {
  ASSERT_TRUE(state_.apply(enqueue_record(1)).ok());
  ASSERT_TRUE(state_.apply(JobLeased{.id = 1, .token = 1}).ok());
  ASSERT_TRUE(state_.apply(AttemptFailed{.id = 1, .token = 1, .error = "x", .dead = true}).ok());
  ASSERT_EQ(state_.find_job(1)->state, JobState::kDead);
  ASSERT_TRUE(state_.find_queue("q")->dead.contains(1));

  ASSERT_TRUE(state_.apply(DeadJobRetried{.id = 1, .run_at = WallTime{0}, .at = WallTime{2}}).ok());
  const Job* job = state_.find_job(1);
  EXPECT_EQ(job->state, JobState::kReady);
  EXPECT_EQ(job->attempts, 0U);
  EXPECT_EQ(job->last_error, "x") << "the last error stays visible";
  EXPECT_FALSE(state_.find_queue("q")->dead.contains(1));
  EXPECT_TRUE(state_.check_invariants().ok());
}

// --- serialization ----------------------------------------------------------------------

class StateSerializationTest : public EngineFixture {
 protected:
  // Jobs in every state, several queues, idempotency keys, errors, binary payloads.
  void populate() {
    enqueue({.queue = "a", .payload = std::string_view("b\0in", 4), .priority = 5});
    enqueue({.queue = "a", .payload = "later", .delay_ms = 90'000, .idem_key = "k1"});
    enqueue({.queue = "b", .payload = "", .idem_key = "k2"});
    const JobId to_ack = enqueue("c", "ack me");
    const JobId to_kill = enqueue("c", "kill me");
    const JobId to_cancel = enqueue("c", "cancel me");
    const JobId to_hold = enqueue("c", "hold me");
    (void)to_hold;

    auto lease = reserve("c");
    ASSERT_TRUE(engine_.ack(to_ack, lease->token).ok());
    lease = reserve("c");
    ASSERT_TRUE(
        engine_.fail({.id = to_kill, .token = lease->token, .error = "bad", .no_retry = true})
            .ok());
    ASSERT_TRUE(engine_.cancel(to_cancel).ok());
    ASSERT_TRUE(reserve("c").has_value());  // stays leased
    lease = reserve("a");
    ASSERT_TRUE(engine_.fail({.id = lease->id, .token = lease->token, .error = "retry me"}).ok());
  }
};

TEST_F(StateSerializationTest, RoundTripPreservesEverythingDurable) {
  populate();
  const std::string bytes = serialized(state_);

  auto loaded = State::deserialize(bytes, state_.options());
  ASSERT_TRUE(loaded.ok()) << loaded.error().to_string();
  EXPECT_TRUE((*loaded)->check_invariants().ok()) << "valid even before derived state is built";
  (*loaded)->set_now(clock_.wall_now(), clock_.mono_now());
  (*loaded)->end_replay();

  EXPECT_EQ(serialized(**loaded), bytes);
  const Status invariants = (*loaded)->check_invariants();
  EXPECT_TRUE(invariants.ok()) << invariants.error().to_string();
  EXPECT_EQ((*loaded)->memory_bytes() > 0, true);
  EXPECT_EQ((*loaded)->next_job_id(), state_.next_job_id());
  EXPECT_EQ((*loaded)->next_token(), state_.next_token());
  EXPECT_EQ((*loaded)->pending_timers(), state_.pending_timers());
}

TEST_F(StateSerializationTest, LoadedStateContinuesExactlyLikeTheOriginal) {
  populate();
  auto loaded = State::deserialize(serialized(state_), state_.options());
  ASSERT_TRUE(loaded.ok());
  (*loaded)->set_now(clock_.wall_now(), clock_.mono_now());
  (*loaded)->end_replay();

  // Drive both with the same commands from here on (same RNG seed, same clock).
  MemoryRecordSink other_sink;
  Engine other(**loaded, other_sink, clock_, engine_.options(), /*rng_seed=*/77);
  Engine mine(state_, sink_, clock_, engine_.options(), /*rng_seed=*/77);
  for (Engine* engine : {&mine, &other}) {
    ASSERT_TRUE(engine->enqueue({.queue = "a", .payload = "new"}).ok());
    const std::vector<std::string_view> queues = {"b", "a"};
    auto lease = engine->reserve(queues, 1000);
    ASSERT_TRUE(lease.ok() && lease->has_value());
    ASSERT_TRUE(engine->fail({.id = (*lease)->id, .token = (*lease)->token, .error = "e"}).ok());
  }
  EXPECT_EQ(serialized(**loaded), serialized(state_));
}

TEST_F(StateSerializationTest, EveryTruncationAndTrailingByteIsRejected) {
  populate();
  const std::string bytes = serialized(state_);
  for (size_t size = 0; size < bytes.size(); ++size) {
    const auto loaded = State::deserialize(std::string_view(bytes).substr(0, size), {});
    ASSERT_FALSE(loaded.ok()) << "truncated to " << size << " of " << bytes.size();
    EXPECT_EQ(loaded.error().code(), ErrorCode::kCorruption);
  }
  EXPECT_FALSE(State::deserialize(bytes + "x", {}).ok());
}

TEST_F(StateSerializationTest, EmptyStateRoundTrips) {
  const std::string bytes = serialized(state_);
  const auto loaded = State::deserialize(bytes, {});
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(serialized(**loaded), bytes);
}

// --- derived state ------------------------------------------------------------------------

class StateRebuildTest : public EngineFixture {};

TEST_F(StateRebuildTest, RebuildReDerivesScheduledVersusReadyFromTheClock) {
  const JobId soon = enqueue({.queue = "q", .payload = "x", .delay_ms = 1'000});
  const JobId later = enqueue({.queue = "q", .payload = "x", .delay_ms = 50'000});

  // The wall clock is stepped forward by 10 s; monotonic time is unaffected.
  clock_.jump_wall(10'000);
  state_.set_now(clock_.wall_now(), clock_.mono_now());
  state_.rebuild_derived();

  EXPECT_EQ(state_of(soon), JobState::kReady) << "its wall-clock deadline has passed";
  EXPECT_EQ(state_of(later), JobState::kScheduled);
  EXPECT_TRUE(state_.check_invariants().ok());

  // ...and the remaining timer fires after the *remaining* wall-clock time.
  advance(39'999);
  EXPECT_EQ(state_of(later), JobState::kScheduled);
  advance(1);
  EXPECT_EQ(state_of(later), JobState::kReady);
}

TEST_F(StateRebuildTest, RebuildGivesLeasesAGracePeriod) {
  const JobId id = enqueue();
  ASSERT_TRUE(reserve("q", 1'000).has_value());

  clock_.advance(5'000);  // e.g. the server was down; the lease "expired" meanwhile
  state_.set_now(clock_.wall_now(), clock_.mono_now());
  state_.rebuild_derived(/*lease_grace_ms=*/2'000);

  advance(1'999);
  EXPECT_EQ(state_of(id), JobState::kLeased) << "the worker gets a chance to heartbeat";
  advance(1);
  EXPECT_NE(state_of(id), JobState::kLeased);
}

TEST_F(StateRebuildTest, DueTimerWaitsIfTheWallClockWasSteppedBack) {
  const JobId id = enqueue({.queue = "q", .payload = "x", .delay_ms = 1'000});
  clock_.jump_wall(-600);  // no rebuild: the monotonic timer still fires after 1000 ms
  advance(1'000);
  EXPECT_EQ(state_of(id), JobState::kScheduled) << "run_at has not arrived on the wall clock";
  advance(600);
  EXPECT_EQ(state_of(id), JobState::kReady);
}

}  // namespace
}  // namespace baton
