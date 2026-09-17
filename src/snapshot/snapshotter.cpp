#include "snapshot/snapshotter.h"

#include <chrono>
#include <csignal>
#include <utility>

#include "common/check.h"

namespace baton {

Snapshotter::Snapshotter(FileSystem& fs, std::string dir, std::function<void()> on_done)
    : fs_(fs), dir_(std::move(dir)), on_done_(std::move(on_done)) {}

Snapshotter::~Snapshotter() {
  if (thread_.joinable()) thread_.join();
}

void Snapshotter::start(StateImage image, Lsn lsn, WallTime created_at) {
  BATON_CHECK(!busy_.load(), "a snapshot is already running");
  if (thread_.joinable()) thread_.join();  // a finished run whose result was never taken
  busy_.store(true);
  thread_ = std::thread([this, image = std::move(image), lsn, created_at]() mutable {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);  // signals belong to the event loop

    const auto started = std::chrono::steady_clock::now();
    Result<SnapshotReport> outcome = Error{ErrorCode::kInternal, "snapshot did not run"};
    Result<SnapshotInfo> info = write_snapshot(fs_, dir_, image, lsn, created_at);
    // Let go of the copy here, before reporting back: dropping a million payload
    // references takes a while, and the event loop joins this thread.
    image = StateImage{};
    if (!info.ok()) {
      outcome = info.error();
    } else {
      // Only now, with the new snapshot durable, may older files go.
      Result<CompactionReport> compaction = compact(fs_, dir_);
      if (!compaction.ok()) {
        outcome = compaction.error();
      } else {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        outcome = SnapshotReport{
            .info = *info,
            .compaction = *compaction,
            .write_micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()};
      }
    }
    {
      const std::scoped_lock lock(mutex_);
      result_ = std::move(outcome);
    }
    busy_.store(false);
    if (on_done_) on_done_();
  });
}

void Snapshotter::wait() {
  if (thread_.joinable()) thread_.join();
}

std::optional<Result<SnapshotReport>> Snapshotter::take_result() {
  std::optional<Result<SnapshotReport>> result;
  {
    const std::scoped_lock lock(mutex_);
    result.swap(result_);
  }
  if (result && thread_.joinable()) thread_.join();
  return result;
}

}  // namespace baton
