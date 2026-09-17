#include "state/ready_heap.h"

#include "common/check.h"

namespace baton {

bool ReadyHeap::before(const Job& a, const Job& b) {
  if (a.priority != b.priority) return a.priority > b.priority;
  if (a.run_at != b.run_at) return a.run_at < b.run_at;
  return a.id < b.id;
}

void ReadyHeap::place(size_t index, Job* job) {
  heap_[index] = job;
  job->heap_index = static_cast<uint32_t>(index);
}

void ReadyHeap::push(Job* job) {
  BATON_CHECK(job->heap_index == Job::kNotInHeap, "job {} is already in a ready heap", job->id);
  BATON_CHECK(heap_.size() < Job::kNotInHeap);
  heap_.push_back(job);
  job->heap_index = static_cast<uint32_t>(heap_.size() - 1);
  sift_up(heap_.size() - 1);
}

void ReadyHeap::remove(Job* job) {
  const size_t index = job->heap_index;
  BATON_CHECK(index < heap_.size(), "job {} is not in this ready heap", job->id);
  BATON_CHECK(heap_[index] == job, "job {} is not in this ready heap", job->id);
  job->heap_index = Job::kNotInHeap;
  Job* last = heap_.back();
  heap_.pop_back();
  if (last == job) return;

  // Move the last element into the hole; it may need to go either way.
  place(index, last);
  sift_up(index);
  sift_down(last->heap_index);
}

void ReadyHeap::sift_up(size_t index) {
  Job* job = heap_[index];
  while (index > 0) {
    const size_t parent = (index - 1) / 2;
    if (!before(*job, *heap_[parent])) break;
    place(index, heap_[parent]);
    index = parent;
  }
  place(index, job);
}

void ReadyHeap::sift_down(size_t index) {
  Job* job = heap_[index];
  for (;;) {
    size_t child = (2 * index) + 1;
    if (child >= heap_.size()) break;
    if (child + 1 < heap_.size() && before(*heap_[child + 1], *heap_[child])) ++child;
    if (!before(*heap_[child], *job)) break;
    place(index, heap_[child]);
    index = child;
  }
  place(index, job);
}

bool ReadyHeap::is_consistent() const {
  for (size_t i = 0; i < heap_.size(); ++i) {
    if (heap_[i]->heap_index != i) return false;
    if (i > 0 && before(*heap_[i], *heap_[(i - 1) / 2])) return false;
  }
  return true;
}

}  // namespace baton
