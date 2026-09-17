#pragma once

// baton uses two clocks and keeps them apart with distinct types, because
// mixing them up is a classic source of timer bugs:
//
//  - WallTime: milliseconds since the Unix epoch. This is what gets persisted
//    (deadlines must survive a restart) and what clients see. It can jump.
//  - MonoTime: milliseconds from an arbitrary origin. It never goes backwards,
//    so it drives the timing wheel. It is meaningless across restarts and is
//    never persisted.
//
// docs/design.md ("Time") explains how the two are reconciled.

#include <atomic>
#include <compare>
#include <cstdint>

namespace baton {

using DurationMs = int64_t;

struct WallTime {
  int64_t ms = 0;
  friend auto operator<=>(WallTime, WallTime) = default;
};

struct MonoTime {
  int64_t ms = 0;
  friend auto operator<=>(MonoTime, MonoTime) = default;
};

constexpr WallTime operator+(WallTime t, DurationMs d) { return WallTime{t.ms + d}; }
constexpr DurationMs operator-(WallTime a, WallTime b) { return a.ms - b.ms; }
constexpr MonoTime operator+(MonoTime t, DurationMs d) { return MonoTime{t.ms + d}; }
constexpr DurationMs operator-(MonoTime a, MonoTime b) { return a.ms - b.ms; }

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  virtual WallTime wall_now() const = 0;
  virtual MonoTime mono_now() const = 0;
};

// The real clocks: CLOCK_REALTIME and CLOCK_MONOTONIC.
class SystemClock final : public Clock {
 public:
  WallTime wall_now() const override;
  MonoTime mono_now() const override;
};

// A clock that only moves when told to. Used by tests to make timer behaviour
// deterministic and to simulate wall-clock jumps.
class FakeClock final : public Clock {
 public:
  explicit FakeClock(WallTime wall = WallTime{1'700'000'000'000}, MonoTime mono = MonoTime{1'000})
      : wall_ms_(wall.ms), mono_ms_(mono.ms) {}

  WallTime wall_now() const override { return WallTime{wall_ms_.load()}; }
  MonoTime mono_now() const override { return MonoTime{mono_ms_.load()}; }

  // Time passes normally: both clocks move forward together.
  void advance(DurationMs d) {
    wall_ms_ += d;
    mono_ms_ += d;
  }

  // The wall clock is stepped (NTP correction, operator error) while the
  // monotonic clock is unaffected. `d` may be negative.
  void jump_wall(DurationMs d) { wall_ms_ += d; }

 private:
  std::atomic<int64_t> wall_ms_;
  std::atomic<int64_t> mono_ms_;
};

}  // namespace baton
