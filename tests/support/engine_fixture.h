#pragma once

// Shared fixture for Engine tests. After every test it verifies two things no
// matter what the test itself was about:
//
//   1. State::check_invariants() holds.
//   2. Replay equivalence: a fresh State rebuilt from the logged records alone
//      serializes to exactly the same bytes as the live State.
//
// So every behavioural test doubles as a recovery test.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "state/engine.h"
#include "state/state.h"
#include "testing/memory_record_sink.h"

namespace baton {

inline std::string serialized(const State& state) {
  std::string out;
  state.serialize(out);
  return out;
}

// Rebuilds a State from the records in `sink`, the way recovery does.
inline std::unique_ptr<State> replay(const MemoryRecordSink& sink, const Clock& clock,
                                     StateOptions options) {
  auto state = std::make_unique<State>(options);
  state->begin_replay();
  const Status replayed = sink.replay_into(*state);
  EXPECT_TRUE(replayed.ok()) << (replayed.ok() ? "" : replayed.error().to_string());
  state->set_now(clock.wall_now(), clock.mono_now());
  state->end_replay();
  return state;
}

class EngineFixture : public ::testing::Test {
 protected:
  explicit EngineFixture(EngineOptions options = {}, StateOptions state_options = {})
      : state_(state_options), engine_(state_, sink_, clock_, options, /*rng_seed=*/1234) {}

  void TearDown() override {
    engine_.tick();  // bring the live state's garbage collection up to date
    const Status invariants = state_.check_invariants();
    EXPECT_TRUE(invariants.ok()) << (invariants.ok() ? "" : invariants.error().to_string());

    const auto rebuilt = replay(sink_, clock_, state_.options());
    EXPECT_EQ(serialized(*rebuilt), serialized(state_)) << "replayed state differs from live state";
    const Status rebuilt_invariants = rebuilt->check_invariants();
    EXPECT_TRUE(rebuilt_invariants.ok())
        << (rebuilt_invariants.ok() ? "" : rebuilt_invariants.error().to_string());
  }

  // Time passes and the engine notices.
  void advance(DurationMs ms) {
    clock_.advance(ms);
    engine_.tick();
  }

  JobId enqueue(std::string_view queue = "q", std::string_view payload = "payload") {
    const auto result = engine_.enqueue(EnqueueRequest{.queue = queue, .payload = payload});
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().to_string());
    return result.ok() ? result->id : 0;
  }

  JobId enqueue(const EnqueueRequest& request) {
    const auto result = engine_.enqueue(request);
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().to_string());
    return result.ok() ? result->id : 0;
  }

  std::optional<Reservation> reserve(std::string_view queue = "q", DurationMs lease_ms = 1000) {
    const std::vector<std::string_view> queues = {queue};
    auto result = engine_.reserve(queues, lease_ms);
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().to_string());
    return result.ok() ? *result : std::nullopt;
  }

  JobState state_of(JobId id) const {
    const Job* job = state_.find_job(id);
    EXPECT_NE(job, nullptr) << "job " << id << " does not exist";
    return job == nullptr ? JobState::kCancelled : job->state;
  }

  FakeClock clock_;
  MemoryRecordSink sink_;
  State state_;
  Engine engine_;
};

}  // namespace baton
