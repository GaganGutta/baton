#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <vector>

#include "state/ready_heap.h"

namespace {

std::vector<std::unique_ptr<baton::Job>> make_jobs(int64_t count) {
  std::mt19937_64 rng(1);
  std::vector<std::unique_ptr<baton::Job>> jobs;
  jobs.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    auto job = std::make_unique<baton::Job>();
    job->id = static_cast<baton::JobId>(i + 1);
    job->priority = static_cast<int32_t>(rng() % 3);
    job->run_at = baton::WallTime{static_cast<int64_t>(rng() % 1'000'000)};
    jobs.push_back(std::move(job));
  }
  return jobs;
}

// ENQUEUE + RESERVE against a backlog of range(0) ready jobs.
void BM_ReadyHeapPushPop(benchmark::State& state) {
  auto jobs = make_jobs(state.range(0) + 1);
  baton::ReadyHeap heap;
  for (size_t i = 0; i + 1 < jobs.size(); ++i) heap.push(jobs[i].get());
  baton::Job* extra = jobs.back().get();
  for (auto _ : state) {
    heap.push(extra);
    baton::Job* top = heap.top();
    heap.remove(top);
    extra = top;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReadyHeapPushPop)->Arg(100)->Arg(100'000)->Arg(1'000'000);

// CANCEL of an arbitrary ready job, the operation the index exists for.
void BM_ReadyHeapRemoveArbitrary(benchmark::State& state) {
  auto jobs = make_jobs(state.range(0));
  baton::ReadyHeap heap;
  for (auto& job : jobs) heap.push(job.get());
  std::mt19937_64 rng(2);
  for (auto _ : state) {
    baton::Job* victim = jobs[rng() % jobs.size()].get();
    heap.remove(victim);
    heap.push(victim);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReadyHeapRemoveArbitrary)->Arg(100'000);

}  // namespace
