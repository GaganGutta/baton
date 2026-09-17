#include "common/clock.h"

#include <ctime>

namespace baton {
namespace {

int64_t read_clock_ms(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return (static_cast<int64_t>(ts.tv_sec) * 1000) + (ts.tv_nsec / 1'000'000);
}

}  // namespace

WallTime SystemClock::wall_now() const { return WallTime{read_clock_ms(CLOCK_REALTIME)}; }

MonoTime SystemClock::mono_now() const { return MonoTime{read_clock_ms(CLOCK_MONOTONIC)}; }

}  // namespace baton
