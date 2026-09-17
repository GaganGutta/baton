#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "sched/timing_wheel.h"

namespace {

// Schedule + cancel with `range(0)` timers already pending: the HEARTBEAT pattern
// (every heartbeat replaces a lease-expiry timer). Should not depend on the
// number of pending timers.
void BM_TimingWheelScheduleCancel(benchmark::State& state) {
  baton::TimingWheel wheel(0);
  std::mt19937_64 rng(1);
  for (int64_t i = 0; i < state.range(0); ++i) wheel.schedule(1 + (rng() % 3'600'000), 0, 0);
  for (auto _ : state) {
    const baton::TimerHandle handle = wheel.schedule(1 + (rng() % 60'000), 0, 1);
    wheel.cancel(handle);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimingWheelScheduleCancel)->Arg(0)->Arg(10'000)->Arg(1'000'000);

// Steady state: every tick a timer fires and a new one is scheduled.
void BM_TimingWheelSteadyState(benchmark::State& state) {
  baton::TimingWheel wheel(0);
  std::mt19937_64 rng(2);
  for (int64_t i = 0; i < state.range(0); ++i) wheel.schedule(1 + (rng() % 30'000), 0, 0);
  std::vector<baton::TimerEvent> fired;
  uint64_t now = 0;
  for (auto _ : state) {
    ++now;
    fired.clear();
    wheel.advance(now, fired);
    for (size_t i = 0; i < fired.size(); ++i) wheel.schedule(now + 1 + (rng() % 30'000), 0, 0);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimingWheelSteadyState)->Arg(1'000)->Arg(100'000);

void BM_TimingWheelNextWakeup(benchmark::State& state) {
  baton::TimingWheel wheel(0);
  std::mt19937_64 rng(3);
  for (int64_t i = 0; i < state.range(0); ++i) wheel.schedule(1 + (rng() % 86'400'000), 0, 0);
  for (auto _ : state) benchmark::DoNotOptimize(wheel.next_wakeup());
}
BENCHMARK(BM_TimingWheelNextWakeup)->Arg(10)->Arg(1'000'000);

}  // namespace
