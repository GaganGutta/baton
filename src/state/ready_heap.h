#pragma once

// The ready queue of one named queue: a binary min-heap of Job* ordered by
// (priority desc, run_at asc, id asc), i.e. highest priority first and FIFO by
// the time a job became runnable within a priority.
//
// It is *indexed*: every job stores its position (Job::heap_index), so removing
// an arbitrary job (CANCEL of a ready job) is O(log n) rather than a scan or a
// tombstone. One pointer per job, contiguous, no per-node allocation.

#include <cstddef>
#include <vector>

#include "state/job.h"

namespace baton {

class ReadyHeap {
 public:
  bool empty() const { return heap_.empty(); }
  size_t size() const { return heap_.size(); }

  // The job RESERVE would hand out next, or nullptr.
  Job* top() const { return heap_.empty() ? nullptr : heap_.front(); }

  void push(Job* job);
  void remove(Job* job);  // the job must be in this heap

  // True if `a` should run before `b`.
  static bool before(const Job& a, const Job& b);

  // Verifies the heap property and the stored indexes. For tests and
  // State::check_invariants().
  bool is_consistent() const;

  const std::vector<Job*>& jobs() const { return heap_; }

 private:
  void place(size_t index, Job* job);
  void sift_up(size_t index);
  void sift_down(size_t index);

  std::vector<Job*> heap_;
};

}  // namespace baton
