// Model-based test of the state machine (docs/design.md section 5.9).
//
// Long random sequences of commands and clock movements, shaped like real
// traffic: several queues, priorities, delays, duplicate idempotency keys,
// workers that ack, fail, heartbeat, go silent or come back as zombies.
//
// After EVERY step:   State::check_invariants() must hold.
// Every so often:     a State rebuilt by replaying the logged records alone must
//                     serialize to the same bytes as the live one (replay
//                     equivalence: apply() used nothing that is not in a record).
// And throughout, the client-visible contract is checked against a tiny model:
//   - lease tokens only ever increase;
//   - only the current token of a job can ack, fail or heartbeat it;
//   - an idempotency key maps to one job for as long as its window lasts.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "common/logging.h"
#include "support/engine_fixture.h"

namespace baton {
namespace {

struct Held {
  JobId id = 0;
  LeaseToken token = 0;
};

struct KeyModel {
  JobId id = 0;
  WallTime expires_at;
};

class ModelRun {
 public:
  explicit ModelRun(uint64_t seed)
      : rng_(seed),
        state_(StateOptions{.retain_finished_ms = 20'000, .retain_dead_ms = 60'000}),
        engine_(state_, sink_, clock_, options(), seed) {}

  void run(int steps) {
    for (int step = 0; step < steps; ++step) {
      SCOPED_TRACE("step " + std::to_string(step));
      one_step();
      const Status invariants = state_.check_invariants();
      ASSERT_TRUE(invariants.ok()) << invariants.error().to_string();
      if (step % 97 == 0) check_replay_equivalence();
      if (::testing::Test::HasFatalFailure()) return;
    }
    check_replay_equivalence();
  }

  size_t records_logged() const { return sink_.entries().size(); }

 private:
  static EngineOptions options() {
    EngineOptions o;
    o.default_max_attempts = 3;
    o.default_backoff_base_ms = 50;
    o.default_backoff_cap_ms = 2'000;
    o.idempotency_window_ms = 15'000;
    return o;
  }

  uint64_t pick(uint64_t n) { return rng_() % n; }
  std::string queue_name() { return "queue-" + std::to_string(pick(3)); }

  void one_step() {
    const uint64_t dice = pick(100);
    if (dice < 30) {
      do_enqueue();
    } else if (dice < 55) {
      do_reserve();
    } else if (dice < 70) {
      do_finish(/*current=*/true);
    } else if (dice < 76) {
      do_finish(/*current=*/false);  // zombies and duplicates
    } else if (dice < 82) {
      do_heartbeat();
    } else if (dice < 86) {
      do_cancel();
    } else if (dice < 89) {
      do_dlq();
    } else if (dice < 90) {
      // The wall clock is stepped forward (NTP, resume from suspend). Only
      // forward: after a backward step, things that had already expired can
      // become visible again after a restart (design.md 7.7), which is exactly
      // the divergence the replay check would report.
      clock_.jump_wall(2'000 + static_cast<DurationMs>(pick(20'000)));
      engine_.tick();
    } else {
      // Time passes: usually a little, sometimes enough to expire leases, keys
      // and retention all at once.
      clock_.advance(pick(10) == 0 ? static_cast<DurationMs>(pick(30'000))
                                   : static_cast<DurationMs>(pick(400)));
      engine_.tick();
    }
  }

  // An operator works the dead-letter queue: retry or purge, one job or all.
  void do_dlq() {
    const std::string queue = queue_name();
    const auto dead = engine_.dlq_list(queue, 0, 1000);
    ASSERT_TRUE(dead.ok());
    const size_t before = dead->size();
    const bool purge = pick(2) == 0;
    if (pick(3) == 0) {
      const auto affected = purge ? engine_.dlq_purge_all(queue) : engine_.dlq_retry_all(queue);
      ASSERT_TRUE(affected.ok());
      ASSERT_EQ(*affected, before);
    } else if (before > 0) {
      const JobId id = (*dead)[pick(before)]->id;
      const auto affected = purge ? engine_.dlq_purge(id) : engine_.dlq_retry(id);
      ASSERT_TRUE(affected.ok());
      const Job* job = state_.find_job(id);
      ASSERT_EQ(job == nullptr, purge);
      if (!purge) {
        ASSERT_TRUE(is_pending(job->state) && job->attempts == 0);
      }
    } else {
      // Nothing dead in this queue: aim at an arbitrary job instead.
      const JobId id = 1 + pick(state_.next_job_id());
      const Job* job = state_.find_job(id);
      const bool is_dead = job != nullptr && job->state == JobState::kDead;
      ASSERT_EQ(engine_.dlq_retry(id).ok(), is_dead);
    }
    const auto after = engine_.dlq_list(queue, 0, 1000);
    ASSERT_TRUE(after.ok());
    ASSERT_LE(after->size(), before);
  }

  void do_enqueue() {
    EnqueueRequest request;
    const std::string queue = queue_name();
    const std::string key = pick(3) == 0 ? "key-" + std::to_string(pick(12)) : "";
    request.queue = queue;
    request.payload = "payload";
    request.priority = static_cast<int32_t>(pick(3)) - 1;
    request.delay_ms = pick(3) == 0 ? static_cast<DurationMs>(pick(3'000)) : 0;
    request.idem_key = key;
    const auto result = engine_.enqueue(request);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    if (key.empty()) {
      ASSERT_TRUE(result->created);
      return;
    }
    const WallTime now = state_.wall_now();
    const auto known = keys_.find(key);
    if (known != keys_.end() && known->second.expires_at > now) {
      ASSERT_FALSE(result->created) << "key " << key << " created a second job inside its window";
      ASSERT_EQ(result->id, known->second.id);
    } else {
      ASSERT_TRUE(result->created) << "key " << key << " was refused outside its window";
      keys_[key] = KeyModel{.id = result->id, .expires_at = now + options().idempotency_window_ms};
    }
  }

  void do_reserve() {
    const std::string first = queue_name();
    const std::string second = queue_name();
    const std::vector<std::string_view> queues = {first, second};
    const auto lease = engine_.reserve(queues, 200 + static_cast<DurationMs>(pick(2'000)));
    ASSERT_TRUE(lease.ok()) << lease.error().to_string();
    if (!lease->has_value()) return;

    const Reservation& r = **lease;
    ASSERT_GT(r.token, highest_token_) << "lease tokens must only ever increase";
    highest_token_ = r.token;
    // Whoever held this job before is now a zombie.
    for (Held& held : held_) {
      if (held.id == r.id) stale_.push_back(held);
    }
    std::erase_if(held_, [&](const Held& h) { return h.id == r.id; });
    held_.push_back(Held{.id = r.id, .token = r.token});
  }

  // True if `held` is what the server currently considers the lease of its job.
  bool is_current(const Held& held) const {
    const Job* job = state_.find_job(held.id);
    return job != nullptr && job->state == JobState::kLeased && job->lease_token == held.token;
  }

  void do_finish(bool current) {
    std::vector<Held>& pool = current ? held_ : stale_;
    if (pool.empty()) return;
    const size_t index = pick(pool.size());
    const Held held = pool[index];
    const bool should_work = is_current(held);

    Status outcome;
    if (pick(2) == 0) {
      outcome = engine_.ack(held.id, held.token);
    } else {
      const auto failed = engine_.fail(
          {.id = held.id, .token = held.token, .error = "model failure", .no_retry = pick(8) == 0});
      if (!failed.ok()) outcome = failed.error();
    }

    if (should_work) {
      ASSERT_TRUE(outcome.ok()) << "the current lease holder was rejected: "
                                << outcome.error().to_string();
      pool.erase(pool.begin() + static_cast<long>(index));
      stale_.push_back(held);  // from now on this token must never work again
    } else {
      ASSERT_FALSE(outcome.ok()) << "a stale token was accepted for job " << held.id;
      const ErrorCode code = outcome.error().code();
      ASSERT_TRUE(code == ErrorCode::kStaleToken || code == ErrorCode::kNotFound);
    }
  }

  void do_heartbeat() {
    if (held_.empty()) return;
    const Held held = held_[pick(held_.size())];
    const bool should_work = is_current(held);
    const auto result =
        engine_.heartbeat(held.id, held.token, 500 + static_cast<DurationMs>(pick(1'500)));
    ASSERT_EQ(result.ok(), should_work);
  }

  void do_cancel() {
    const JobId id = 1 + pick(state_.next_job_id());
    const Job* job = state_.find_job(id);
    const bool should_work = job != nullptr && !is_terminal(job->state);
    ASSERT_EQ(engine_.cancel(id).ok(), should_work);
  }

  void check_replay_equivalence() {
    engine_.tick();  // live garbage collection up to date with the clock
    const auto rebuilt = replay(sink_, clock_, state_.options());
    ASSERT_EQ(serialized(*rebuilt), serialized(state_)) << "replay diverged from the live state";
    const Status invariants = rebuilt->check_invariants();
    ASSERT_TRUE(invariants.ok()) << invariants.error().to_string();

    // A snapshot in the middle changes nothing either: load it, replay the rest.
    const size_t cut = sink_.entries().size() / 2;
    State prefix(state_.options());
    prefix.begin_replay();
    ASSERT_TRUE(sink_.replay_into(prefix, 0, cut).ok());
    auto from_snapshot = State::deserialize(serialized(prefix), state_.options());
    ASSERT_TRUE(from_snapshot.ok()) << from_snapshot.error().to_string();
    ASSERT_TRUE(sink_.replay_into(**from_snapshot, cut).ok());
    (*from_snapshot)->set_now(clock_.wall_now(), clock_.mono_now());
    (*from_snapshot)->end_replay();
    ASSERT_EQ(serialized(**from_snapshot), serialized(state_))
        << "snapshot + log tail diverged from the live state";
  }

  std::mt19937_64 rng_;
  FakeClock clock_;
  MemoryRecordSink sink_;
  State state_;
  Engine engine_;

  std::vector<Held> held_;   // leases the model believes may be current
  std::vector<Held> stale_;  // tokens that must never work again
  std::map<std::string, KeyModel> keys_;
  LeaseToken highest_token_ = 0;
};

TEST(StateModelTest, RandomTrafficKeepsInvariantsAndReplaysExactly) {
  set_log_level(LogLevel::kError);  // every simulated clock step logs a warning
  size_t records = 0;
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    ModelRun run(seed);
    run.run(2'500);
    if (::testing::Test::HasFatalFailure()) return;
    records += run.records_logged();
  }
  // Sanity: the run really exercised the machine.
  EXPECT_GT(records, 12U * 1'000);
  set_log_level(LogLevel::kInfo);
}

}  // namespace
}  // namespace baton
