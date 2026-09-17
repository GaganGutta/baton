#include "sched/timing_wheel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <vector>

namespace baton {
namespace {

std::vector<uint64_t> ids_of(const std::vector<TimerEvent>& events) {
  std::vector<uint64_t> ids;
  ids.reserve(events.size());
  for (const TimerEvent& e : events) ids.push_back(e.id);
  return ids;
}

TEST(TimingWheelTest, EmptyWheelHasNothingToDo) {
  TimingWheel wheel(100);
  std::vector<TimerEvent> fired;
  EXPECT_TRUE(wheel.empty());
  EXPECT_EQ(wheel.next_wakeup(), std::nullopt);
  wheel.advance(1'000'000, fired);
  EXPECT_TRUE(fired.empty());
  EXPECT_EQ(wheel.now(), 1'000'000U);
}

TEST(TimingWheelTest, FiresOnItsDeadlineNotBefore) {
  TimingWheel wheel(1024);  // 1050 is in the same 64-tick window: no cascade needed
  std::vector<TimerEvent> fired;
  const TimerHandle handle = wheel.schedule(1050, /*kind=*/3, /*id=*/77);
  EXPECT_EQ(wheel.size(), 1U);
  EXPECT_EQ(wheel.next_wakeup(), 1050U);

  wheel.advance(1049, fired);
  EXPECT_TRUE(fired.empty());
  wheel.advance(1050, fired);
  ASSERT_EQ(fired.size(), 1U);
  EXPECT_EQ(fired[0].id, 77U);
  EXPECT_EQ(fired[0].kind, 3);
  EXPECT_EQ(fired[0].deadline, 1050U);
  EXPECT_EQ(fired[0].handle, handle);
  EXPECT_TRUE(wheel.empty());
}

TEST(TimingWheelTest, PastDeadlineFiresOnNextAdvanceEvenIfTimeStandsStill) {
  TimingWheel wheel(500);
  std::vector<TimerEvent> fired;
  wheel.schedule(500, 0, 1);  // now
  wheel.schedule(10, 0, 2);   // long ago
  EXPECT_EQ(wheel.next_wakeup(), 500U);
  wheel.advance(500, fired);
  EXPECT_EQ(fired.size(), 2U);
}

TEST(TimingWheelTest, FiresInDeadlineOrderAcrossLevels) {
  TimingWheel wheel(0);
  // One timer per level, scheduled out of order.
  const std::vector<uint64_t> deadlines = {70'000'000'000, 5,           300'000, 4'000,
                                           20'000'000,     900'000'000, 63,      64};
  for (size_t i = 0; i < deadlines.size(); ++i) wheel.schedule(deadlines[i], 0, deadlines[i]);

  std::vector<TimerEvent> fired;
  wheel.advance(100'000'000'000, fired);
  std::vector<uint64_t> expected = deadlines;
  std::ranges::sort(expected);
  EXPECT_EQ(ids_of(fired), expected);
}

TEST(TimingWheelTest, CancelPreventsFiringAndStaleHandlesAreHarmless) {
  TimingWheel wheel(0);
  std::vector<TimerEvent> fired;
  const TimerHandle a = wheel.schedule(100, 0, 1);
  const TimerHandle b = wheel.schedule(100, 0, 2);
  EXPECT_TRUE(wheel.cancel(a));
  EXPECT_FALSE(wheel.cancel(a)) << "double cancel";
  EXPECT_EQ(wheel.size(), 1U);

  // The slab entry of `a` is reused; the old handle must not touch the new timer.
  const TimerHandle c = wheel.schedule(200, 0, 3);
  EXPECT_EQ(c.index, a.index);
  EXPECT_NE(c.generation, a.generation);
  EXPECT_FALSE(wheel.cancel(a));

  wheel.advance(150, fired);
  EXPECT_EQ(ids_of(fired), (std::vector<uint64_t>{2}));
  EXPECT_FALSE(wheel.cancel(b)) << "already fired";
  EXPECT_FALSE(wheel.cancel(TimerHandle{})) << "default handle";
  EXPECT_FALSE(wheel.cancel(TimerHandle{.index = 9999, .generation = 0})) << "out of range";

  fired.clear();
  wheel.advance(200, fired);
  EXPECT_EQ(ids_of(fired), (std::vector<uint64_t>{3}));
}

TEST(TimingWheelTest, TimeNeverMovesBackwards) {
  TimingWheel wheel(1000);
  std::vector<TimerEvent> fired;
  wheel.schedule(1010, 0, 1);
  wheel.advance(900, fired);
  EXPECT_EQ(wheel.now(), 1000U);
  EXPECT_TRUE(fired.empty());
}

TEST(TimingWheelTest, WakeupsAreForCascadesOrDeadlinesAndNeverLate) {
  TimingWheel wheel(60);
  wheel.schedule(70, 0, 1);  // crosses a 64-tick boundary: needs a cascade at 64
  EXPECT_EQ(wheel.next_wakeup(), 64U);
  std::vector<TimerEvent> fired;
  wheel.advance(64, fired);
  EXPECT_TRUE(fired.empty());
  EXPECT_EQ(wheel.next_wakeup(), 70U);
  wheel.advance(70, fired);
  EXPECT_EQ(fired.size(), 1U);
}

TEST(TimingWheelTest, DeadlinesBeyondTheSpanAreParkedAndStillFireExactly) {
  const uint64_t start = 123'456'789;
  TimingWheel wheel(start);
  const uint64_t far = start + (3 * TimingWheel::kSpan) + 12'345;
  wheel.schedule(far, 0, 42);
  wheel.schedule(start + TimingWheel::kSpan - 1, 0, 7);  // the last tick inside the span

  std::vector<TimerEvent> fired;
  int wakeups = 0;
  while (const auto next = wheel.next_wakeup()) {
    ASSERT_LE(*next, far);
    ASSERT_GT(*next, wheel.now());
    wheel.advance(*next, fired);
    ASSERT_LT(++wakeups, 200) << "parking must not cause a wakeup storm";
  }
  ASSERT_EQ(fired.size(), 2U);
  EXPECT_EQ(fired[0].id, 7U);
  EXPECT_EQ(fired[1].id, 42U);
  EXPECT_EQ(wheel.now(), far);
}

TEST(TimingWheelTest, WrapsAroundTheTopLevelCorrectly) {
  // Start just before the top level rolls over, with deadlines on both sides.
  const uint64_t start = (5 * TimingWheel::kSpan) - 10;
  TimingWheel wheel(start);
  const std::vector<uint64_t> deadlines = {start + 5,
                                           start + 10,
                                           start + 11,
                                           start + 100'000,
                                           start + (TimingWheel::kSpan / 2),
                                           start + TimingWheel::kSpan - 1};
  for (const uint64_t d : deadlines) wheel.schedule(d, 0, d);

  std::vector<TimerEvent> fired;
  while (const auto next = wheel.next_wakeup()) {
    const size_t before = fired.size();
    wheel.advance(*next, fired);
    for (size_t i = before; i < fired.size(); ++i) {
      EXPECT_EQ(fired[i].deadline, *next) << "fired late or early";
    }
  }
  EXPECT_EQ(ids_of(fired), deadlines);
}

// The wheel against a trivially correct model, under random traffic shaped like
// baton's: many short timers, frequent cancels (heartbeats), a few far ones,
// small time steps and the occasional big jump.
TEST(TimingWheelTest, MatchesReferenceModelUnderRandomOperations) {
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937_64 rng(seed);
    uint64_t now = rng() % (uint64_t{1} << 40U);
    TimingWheel wheel(now);

    std::multimap<uint64_t, uint64_t> model;                    // deadline -> id
    std::map<uint64_t, std::pair<TimerHandle, uint64_t>> live;  // id -> (handle, deadline)
    std::vector<TimerHandle> stale;
    uint64_t next_id = 1;
    std::vector<TimerEvent> fired;

    for (int step = 0; step < 4000; ++step) {
      const uint64_t dice = rng() % 100;
      if (dice < 50) {
        uint64_t delay = 0;
        switch (rng() % 6) {
          case 0:
            delay = rng() % 3;
            break;  // due or almost
          case 1:
            delay = rng() % 64;
            break;  // level 0
          case 2:
            delay = rng() % 5'000;
            break;  // typical lease
          case 3:
            delay = rng() % 4'000'000;
            break;  // backoff
          case 4:
            delay = rng() % (uint64_t{1} << 37U);
            break;  // around the span
          default:
            delay = rng() % (TimingWheel::kSpan * 4);
            break;  // far beyond it
        }
        const uint64_t deadline =
            (rng() % 50 == 0 && now > 100) ? now - (rng() % 100) : now + delay;
        const uint64_t id = next_id++;
        live[id] = {wheel.schedule(deadline, static_cast<uint8_t>(id % 4), id), deadline};
        model.emplace(deadline, id);
      } else if (dice < 75 && !live.empty()) {
        auto it = live.begin();
        std::advance(it, static_cast<long>(rng() % live.size()));
        const auto [handle, deadline] = it->second;
        ASSERT_TRUE(wheel.cancel(handle));
        stale.push_back(handle);
        const auto range = model.equal_range(deadline);
        for (auto m = range.first; m != range.second; ++m) {
          if (m->second == it->first) {
            model.erase(m);
            break;
          }
        }
        live.erase(it);
      } else if (dice < 80 && !stale.empty()) {
        ASSERT_FALSE(wheel.cancel(stale[rng() % stale.size()]));
      } else {
        const uint64_t before = now;
        now += (rng() % 20 == 0) ? rng() % (uint64_t{1} << 38U) : rng() % 300;
        fired.clear();
        wheel.advance(now, fired);

        std::vector<uint64_t> expected;
        while (!model.empty() && model.begin()->first <= now) {
          expected.push_back(model.begin()->second);
          model.erase(model.begin());
        }
        std::vector<uint64_t> got = ids_of(fired);
        // Order: non-decreasing deadlines, once overdue timers are clamped to
        // the time the advance started from.
        uint64_t previous = 0;
        for (const TimerEvent& e : fired) {
          const uint64_t effective = std::max(e.deadline, before);
          ASSERT_GE(effective, previous) << "fired out of order";
          previous = effective;
          ASSERT_EQ(live.at(e.id).first, e.handle);
          ASSERT_EQ(e.kind, e.id % 4);
          stale.push_back(e.handle);
          live.erase(e.id);
        }
        std::ranges::sort(expected);
        std::ranges::sort(got);
        ASSERT_EQ(got, expected) << "wrong set of timers fired at step " << step;
      }

      ASSERT_EQ(wheel.size(), model.size());
      const auto wakeup = wheel.next_wakeup();
      if (model.empty()) {
        ASSERT_EQ(wakeup, std::nullopt);
      } else {
        ASSERT_TRUE(wakeup.has_value());
        ASSERT_LE(*wakeup, std::max(model.begin()->first, wheel.now()))
            << "sleeping until next_wakeup would fire a timer late";
        ASSERT_GE(*wakeup, wheel.now());
      }
    }
  }
}

// An event loop that sleeps exactly until next_wakeup() fires every timer on its
// own tick: never early, never late, however the deadlines straddle levels.
TEST(TimingWheelTest, SleepingUntilNextWakeupFiresEveryTimerExactlyOnTime) {
  std::mt19937_64 rng(99);
  uint64_t start = (uint64_t{1} << 36U) - 5000;  // close to a top-level rollover
  TimingWheel wheel(start);
  size_t scheduled = 0;
  for (int i = 0; i < 3000; ++i) {
    wheel.schedule(start + 1 + (rng() % 40'000'000), 0, 0);
    ++scheduled;
  }
  std::vector<TimerEvent> fired;
  size_t total = 0;
  while (const auto next = wheel.next_wakeup()) {
    fired.clear();
    wheel.advance(*next, fired);
    for (const TimerEvent& e : fired) ASSERT_EQ(e.deadline, *next);
    total += fired.size();
    // Keep the wheel busy the way a server does: new work arrives as time passes.
    if (total % 7 == 0 && scheduled < 4000) {
      wheel.schedule(*next + 1 + (rng() % 100'000), 0, 0);
      ++scheduled;
    }
  }
  EXPECT_EQ(total, scheduled);
}

TEST(TimingWheelTest, SlabIsReusedSoMemoryStaysBounded) {
  TimingWheel wheel(0);
  std::vector<TimerEvent> fired;
  uint32_t max_index = 0;
  for (uint64_t round = 0; round < 10'000; ++round) {
    const TimerHandle handle = wheel.schedule(round + 10, 0, round);
    max_index = std::max(max_index, handle.index);
    if (round % 2 == 0) wheel.cancel(handle);
    wheel.advance(round, fired);
  }
  EXPECT_LT(max_index, 32U) << "cancelled and fired timers must return to the free list";
}

}  // namespace
}  // namespace baton
