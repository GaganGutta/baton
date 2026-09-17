#pragma once

// LogWriter: the write side of the durable log, with group commit.
//
// Two threads are involved (docs/design.md section 4.2):
//
//  - The *owner* thread (the event loop) calls append() to encode records into
//    a pending batch and flush() to hand the batch over. Neither blocks on I/O.
//  - The internal *log thread* writes each batch, makes it durable according to
//    the fsync policy, advances committed_lsn() and calls on_commit.
//
// While the log thread is inside fsync the owner keeps appending, so batches
// grow exactly as much as the disk is slow.
//
// Any write or fsync failure aborts the process; see docs/design.md section 4.3
// for why that is the only safe reaction.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common/clock.h"
#include "common/fs.h"
#include "common/histogram.h"
#include "common/result.h"
#include "common/types.h"
#include "log/recovery.h"

namespace baton {

enum class FsyncPolicy : uint8_t {
  kAlways,    // a record is committed once fdatasync covering it has returned
  kInterval,  // a record is committed once write() has returned; fdatasync runs on a timer
};

struct LogWriterOptions {
  std::string dir;
  FsyncPolicy fsync_policy = FsyncPolicy::kAlways;
  DurationMs fsync_interval_ms = 100;           // kInterval only
  uint64_t segment_size = uint64_t{64} << 20U;  // roll once the active segment reaches this
  // Called on the log thread (no locks held) each time committed_lsn() has
  // advanced. The server uses it to wake the event loop.
  std::function<void()> on_commit;
};

struct LogWriterStats {
  uint64_t batches = 0;
  uint64_t records = 0;
  uint64_t bytes = 0;
  uint64_t syncs = 0;
  uint64_t segments_created = 0;
  Histogram batch_records;  // records per batch: shows group commit at work
  Histogram sync_micros;    // fdatasync latency
};

class LogWriter {
  // Passkey: lets open() use std::make_unique while keeping construction private.
  struct Private {
    explicit Private() = default;
  };

 public:
  // Continues the log described by `recovered` (from recover_log), creating the
  // first segment if there is none, and starts the log thread.
  static Result<std::unique_ptr<LogWriter>> open(FileSystem& fs, LogWriterOptions options,
                                                 const RecoveredLog& recovered);

  LogWriter(Private key, FileSystem& fs, LogWriterOptions options, Lsn next_lsn);
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;
  ~LogWriter();  // stop()

  // --- owner thread only ------------------------------------------------------
  // Encodes a record into the pending batch and returns its LSN. Nothing
  // reaches the log thread until flush().
  Lsn append(uint8_t type, std::string_view payload);
  // Hands the pending batch to the log thread. Cheap when nothing is pending.
  void flush();
  Lsn last_appended_lsn() const { return next_lsn_ - 1; }
  // Bytes appended but not yet committed: the backpressure signal.
  uint64_t backlog_bytes() const {
    return appended_bytes_ - committed_bytes_.load(std::memory_order_relaxed);
  }

  // --- any thread ---------------------------------------------------------------
  // Every record with LSN <= committed_lsn() is durable as defined by the policy.
  Lsn committed_lsn() const { return committed_lsn_.load(std::memory_order_acquire); }
  LogWriterStats stats() const;

  // Flushes what is pending, makes it durable (with fdatasync under either
  // policy) and joins the log thread. Idempotent. Owner thread only.
  void stop();

 private:
  struct Batch {
    std::string bytes;
    Lsn first_lsn = 0;
    Lsn last_lsn = 0;
    uint32_t records = 0;

    bool empty() const { return records == 0; }
    void clear() {
      bytes.clear();
      first_lsn = last_lsn = 0;
      records = 0;
    }
  };

  Status open_active_segment(const RecoveredLog& recovered);
  Status create_segment(Lsn first_lsn);

  void run();
  void write_batch(const Batch& batch);
  void sync_active();

  FileSystem& fs_;
  const LogWriterOptions options_;

  // Owner-thread state.
  Batch pending_;
  Lsn next_lsn_;
  uint64_t appended_bytes_ = 0;
  bool stopped_ = false;

  // Hand-off between the owner and the log thread.
  std::mutex mutex_;
  std::condition_variable wake_;
  Batch inbox_;        // guarded by mutex_
  bool stop_ = false;  // guarded by mutex_

  // Log-thread state.
  std::unique_ptr<WritableFile> active_;
  bool dirty_ = false;  // written since the last sync

  std::atomic<Lsn> committed_lsn_;
  std::atomic<uint64_t> committed_bytes_{0};

  mutable std::mutex stats_mutex_;
  LogWriterStats stats_;  // guarded by stats_mutex_

  std::thread thread_;
};

}  // namespace baton
