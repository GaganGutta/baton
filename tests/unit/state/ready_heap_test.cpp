#include "state/ready_heap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

namespace baton {
namespace {

std::unique_ptr<Job> make_job(JobId id, int32_t priority, int64_t run_at) {
  auto job = std::make_unique<Job>();
  job->id = id;
  job->priority = priority;
  job->run_at = WallTime{run_at};
  return job;
}

TEST(ReadyHeapTest, OrdersByPriorityThenRunAtThenId) {
  const auto low = make_job(1, 0, 100);
  const auto high = make_job(2, 5, 900);
  const auto early = make_job(4, 0, 50);
  const auto tie_a = make_job(7, 0, 100);

  EXPECT_TRUE(ReadyHeap::before(*high, *low)) << "higher priority first";
  EXPECT_TRUE(ReadyHeap::before(*early, *low)) << "earlier run_at first within a priority";
  EXPECT_TRUE(ReadyHeap::before(*low, *tie_a)) << "lower id breaks the tie";
  EXPECT_FALSE(ReadyHeap::before(*low, *low));
}

TEST(ReadyHeapTest, TopIsTheBestJob) {
  ReadyHeap heap;
  EXPECT_EQ(heap.top(), nullptr);
  auto a = make_job(1, 0, 300);
  auto b = make_job(2, 0, 100);
  auto c = make_job(3, 9, 500);
  heap.push(a.get());
  EXPECT_EQ(heap.top(), a.get());
  heap.push(b.get());
  EXPECT_EQ(heap.top(), b.get());
  heap.push(c.get());
  EXPECT_EQ(heap.top(), c.get());
  EXPECT_EQ(heap.size(), 3U);
  EXPECT_TRUE(heap.is_consistent());
}

TEST(ReadyHeapTest, RemoveClearsTheIndexAndKeepsTheHeapValid) {
  ReadyHeap heap;
  std::vector<std::unique_ptr<Job>> jobs;
  for (JobId id = 1; id <= 20; ++id) {
    jobs.push_back(make_job(id, static_cast<int32_t>(id % 3), static_cast<int64_t>(100 - id)));
    heap.push(jobs.back().get());
  }
  heap.remove(jobs[10].get());  // from the middle
  EXPECT_EQ(jobs[10]->heap_index, Job::kNotInHeap);
  heap.remove(heap.top());  // the root
  heap.remove(jobs[0].get());
  EXPECT_EQ(heap.size(), 17U);
  EXPECT_TRUE(heap.is_consistent());
}

TEST(ReadyHeapTest, RandomizedAgainstSorting) {
  std::mt19937_64 rng(5);
  for (int round = 0; round < 50; ++round) {
    ReadyHeap heap;
    std::vector<std::unique_ptr<Job>> jobs;
    std::vector<Job*> members;
    for (JobId id = 1; id <= 200; ++id) {
      jobs.push_back(
          make_job(id, static_cast<int32_t>(rng() % 4) - 2, static_cast<int64_t>(rng() % 50)));
      heap.push(jobs.back().get());
      members.push_back(jobs.back().get());
      // Interleave removals of arbitrary members, the way CANCEL does.
      if (rng() % 3 == 0) {
        const size_t victim = rng() % members.size();
        heap.remove(members[victim]);
        members.erase(members.begin() + static_cast<long>(victim));
      }
      ASSERT_TRUE(heap.is_consistent());
    }

    std::ranges::sort(members,
                      [](const Job* a, const Job* b) { return ReadyHeap::before(*a, *b); });
    for (Job* expected : members) {
      ASSERT_EQ(heap.top(), expected);
      heap.remove(expected);
    }
    EXPECT_TRUE(heap.empty());
  }
}

TEST(ReadyHeapDeathTest, DoublePushAndForeignRemoveAreBugs) {
  ReadyHeap heap;
  ReadyHeap other;
  auto job = make_job(1, 0, 0);
  heap.push(job.get());
  EXPECT_DEATH(heap.push(job.get()), "already in a ready heap");
  EXPECT_DEATH(other.remove(job.get()), "not in this ready heap");
}

}  // namespace
}  // namespace baton
