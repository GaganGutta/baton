#include "common/clock.h"

#include <gtest/gtest.h>

namespace baton {
namespace {

TEST(ClockTest, TimeArithmetic) {
  const WallTime a{1000};
  const WallTime b = a + 250;
  EXPECT_EQ(b.ms, 1250);
  EXPECT_EQ(b - a, 250);
  EXPECT_EQ(a - b, -250);
  EXPECT_LT(a, b);

  const MonoTime m{5};
  EXPECT_EQ((m + 10).ms, 15);
  EXPECT_EQ((m + 10) - m, 10);
}

TEST(ClockTest, SystemClockIsSane) {
  const SystemClock clock;
  // After 2020-01-01 and before 2100-01-01: catches unit mistakes (s vs ms vs ns).
  EXPECT_GT(clock.wall_now().ms, 1'577'836'800'000);
  EXPECT_LT(clock.wall_now().ms, 4'102'444'800'000);

  const MonoTime first = clock.mono_now();
  const MonoTime second = clock.mono_now();
  EXPECT_LE(first, second);
}

TEST(ClockTest, FakeClockAdvancesBothClocks) {
  FakeClock clock(WallTime{10'000}, MonoTime{500});
  clock.advance(250);
  EXPECT_EQ(clock.wall_now().ms, 10'250);
  EXPECT_EQ(clock.mono_now().ms, 750);
}

TEST(ClockTest, FakeClockWallJumpLeavesMonotonicAlone) {
  FakeClock clock(WallTime{10'000}, MonoTime{500});
  clock.jump_wall(-3'000);
  EXPECT_EQ(clock.wall_now().ms, 7'000);
  EXPECT_EQ(clock.mono_now().ms, 500);
  clock.jump_wall(60'000);
  EXPECT_EQ(clock.wall_now().ms, 67'000);
  EXPECT_EQ(clock.mono_now().ms, 500);
}

}  // namespace
}  // namespace baton
