#include "log/log_writer.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <utility>

#include "common/check.h"
#include "common/logging.h"
#include "log/format.h"

namespace baton {
namespace {

using SteadyClock = std::chrono::steady_clock;

// A failed write or fsync leaves the process unable to know what is durable
// (docs/design.md section 4.3). Stop immediately; recovery will read back what
// actually reached the disk.
[[noreturn]] void die(std::string_view what, const Error& error) {
  BATON_ERROR("log", "fatal: {}: {} -- aborting so that recovery can establish what is durable",
              what, error.to_string());
  std::abort();
}

// Signals are handled by the event loop thread; keep them away from this one so
// that blocking I/O here is never interrupted.
void block_all_signals() {
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);
}

}  // namespace

Result<std::unique_ptr<LogWriter>> LogWriter::open(FileSystem& fs, LogWriterOptions options,
                                                   const RecoveredLog& recovered) {
  if (options.segment_size < kSegmentHeaderSize + kRecordHeaderSize) {
    return Error{ErrorCode::kInvalidArgument, "segment_size is too small to hold a record"};
  }
  if (options.fsync_policy == FsyncPolicy::kInterval && options.fsync_interval_ms <= 0) {
    return Error{ErrorCode::kInvalidArgument, "fsync_interval_ms must be positive"};
  }
  auto writer =
      std::make_unique<LogWriter>(Private{}, fs, std::move(options), recovered.last_lsn + 1);
  BATON_RETURN_IF_ERROR(writer->open_active_segment(recovered));
  writer->thread_ = std::thread([w = writer.get()] { w->run(); });
  return writer;
}

LogWriter::LogWriter(Private /*key*/, FileSystem& fs, LogWriterOptions options, Lsn next_lsn)
    : fs_(fs), options_(std::move(options)), next_lsn_(next_lsn), committed_lsn_(next_lsn - 1) {}

LogWriter::~LogWriter() { stop(); }

Status LogWriter::open_active_segment(const RecoveredLog& recovered) {
  if (recovered.segments.empty()) return create_segment(next_lsn_);
  const std::string path = join_path(options_.dir, recovered.segments.back().name);
  BATON_ASSIGN_OR_RETURN(active_, fs_.open_append(path, OpenMode::kAppendExisting));
  return {};
}

// Creates a segment and makes its existence durable before any record is
// appended to it: header, fsync of the file, then fsync of the directory.
Status LogWriter::create_segment(Lsn first_lsn) {
  const std::string path = join_path(options_.dir, segment_file_name(first_lsn));
  BATON_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file,
                         fs_.open_append(path, OpenMode::kCreateNew));
  std::string header;
  append_segment_header(header, first_lsn);
  BATON_RETURN_IF_ERROR(file->append(header));
  BATON_RETURN_IF_ERROR(file->sync());
  BATON_RETURN_IF_ERROR(fs_.sync_dir(options_.dir));
  active_ = std::move(file);
  dirty_ = false;
  {
    const std::scoped_lock lock(stats_mutex_);
    ++stats_.segments_created;
  }
  return {};
}

Lsn LogWriter::append(uint8_t type, std::string_view payload) {
  BATON_CHECK(!stopped_, "append() after stop()");
  const Lsn lsn = next_lsn_++;
  const size_t before = pending_.bytes.size();
  append_record(pending_.bytes, lsn, type, payload);
  appended_bytes_ += pending_.bytes.size() - before;
  if (pending_.records == 0) pending_.first_lsn = lsn;
  pending_.last_lsn = lsn;
  ++pending_.records;
  return lsn;
}

void LogWriter::flush() {
  if (pending_.empty()) return;
  {
    const std::scoped_lock lock(mutex_);
    if (inbox_.empty()) {
      std::swap(inbox_, pending_);  // also recycles the buffer the log thread gave back
    } else {
      inbox_.bytes += pending_.bytes;
      inbox_.last_lsn = pending_.last_lsn;
      inbox_.records += pending_.records;
    }
  }
  pending_.clear();
  wake_.notify_one();
}

void LogWriter::stop() {
  if (stopped_) return;
  flush();
  {
    const std::scoped_lock lock(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
  stopped_ = true;
}

LogWriterStats LogWriter::stats() const {
  const std::scoped_lock lock(stats_mutex_);
  return stats_;
}

// --- log thread -----------------------------------------------------------------

void LogWriter::run() {
  block_all_signals();
  const auto interval = std::chrono::milliseconds(options_.fsync_interval_ms);
  SteadyClock::time_point next_sync = SteadyClock::now() + interval;

  Batch batch;
  std::unique_lock lock(mutex_);
  for (;;) {
    const auto ready = [this] { return !inbox_.empty() || stop_; };
    if (dirty_) {
      wake_.wait_until(lock, next_sync, ready);  // kInterval with unsynced data
    } else {
      wake_.wait(lock, ready);
    }

    if (!inbox_.empty()) {
      std::swap(batch, inbox_);
      lock.unlock();
      write_batch(batch);
      batch.clear();
      lock.lock();
    }

    const bool stopping = stop_ && inbox_.empty();
    if (dirty_ && (stopping || SteadyClock::now() >= next_sync)) {
      lock.unlock();
      sync_active();
      next_sync = SteadyClock::now() + interval;
      lock.lock();
    }
    if (stop_ && inbox_.empty()) return;
  }
}

void LogWriter::write_batch(const Batch& batch) {
  if (active_->size() >= options_.segment_size) {
    // The old segment must be durable before the log continues elsewhere;
    // recovery relies on only the newest segment ever having a torn tail.
    if (dirty_) sync_active();
    active_.reset();
    if (const Status s = create_segment(batch.first_lsn); !s.ok())
      die("creating a log segment", s.error());
  }

  if (const Status s = active_->append(batch.bytes); !s.ok()) die("writing the log", s.error());
  dirty_ = true;
  if (options_.fsync_policy == FsyncPolicy::kAlways) sync_active();

  {
    const std::scoped_lock lock(stats_mutex_);
    ++stats_.batches;
    stats_.records += batch.records;
    stats_.bytes += batch.bytes.size();
    stats_.batch_records.record(batch.records);
  }
  committed_bytes_.fetch_add(batch.bytes.size(), std::memory_order_relaxed);
  committed_lsn_.store(batch.last_lsn, std::memory_order_release);
  if (options_.on_commit) options_.on_commit();
}

void LogWriter::sync_active() {
  const auto start = SteadyClock::now();
  if (const Status s = active_->sync(); !s.ok()) die("fsync of the log", s.error());
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start).count();
  dirty_ = false;
  const std::scoped_lock lock(stats_mutex_);
  ++stats_.syncs;
  stats_.sync_micros.record(static_cast<uint64_t>(micros));
}

}  // namespace baton
