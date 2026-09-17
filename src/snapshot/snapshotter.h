#pragma once

// Runs one snapshot at a time on a background thread: write, then compact.
//
// The thread only ever sees the StateImage it was given (a private copy) and the
// FileSystem; it shares nothing mutable with the event loop, so there is nothing
// to lock. When it finishes it stores the result and calls `on_done`, which the
// server uses to wake the event loop; the loop then collects the result with
// take_result(), which also joins the thread.

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/clock.h"
#include "common/fs.h"
#include "common/result.h"
#include "snapshot/snapshot.h"
#include "state/image.h"

namespace baton {

struct SnapshotReport {
  SnapshotInfo info;
  CompactionReport compaction;
  int64_t write_micros = 0;  // serialize + write + fsync + rename + compaction
};

class Snapshotter {
 public:
  Snapshotter(FileSystem& fs, std::string dir, std::function<void()> on_done);
  Snapshotter(const Snapshotter&) = delete;
  Snapshotter& operator=(const Snapshotter&) = delete;
  ~Snapshotter();  // waits for a running snapshot to finish

  bool busy() const { return busy_.load(); }

  // Starts a snapshot of `image`, which covers the log up to and including `lsn`.
  // Requires !busy(), and that the log has already committed `lsn`.
  void start(StateImage image, Lsn lsn, WallTime created_at);

  // The result of the snapshot that finished, once; nullopt otherwise.
  std::optional<Result<SnapshotReport>> take_result();

  // Blocks until no snapshot is running. The result can still be taken.
  void wait();

 private:
  FileSystem& fs_;
  const std::string dir_;
  const std::function<void()> on_done_;

  std::thread thread_;
  std::atomic<bool> busy_{false};
  std::mutex mutex_;
  std::optional<Result<SnapshotReport>> result_;  // guarded by mutex_
};

}  // namespace baton
