#include "log/log_writer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/posix_fs.h"
#include "log/format.h"
#include "log/recovery.h"
#include "support/temp_dir.h"
#include "testing/sim_fs.h"

namespace baton {
namespace {

using namespace std::chrono_literals;

constexpr const char* kDir = "data";

// Lets a test block until the log thread has committed up to an LSN.
class CommitSignal {
 public:
  std::function<void()> callback() {
    return [this] {
      {
        const std::scoped_lock lock(mutex_);
      }
      cv_.notify_all();
    };
  }

  [[nodiscard]] bool wait_for(const LogWriter& writer, Lsn lsn) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 30s, [&] { return writer.committed_lsn() >= lsn; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};

std::vector<std::string> recover_payloads(FileSystem& fs, const std::string& dir,
                                          RecoveredLog* info = nullptr) {
  std::vector<std::string> payloads;
  const auto recovered = recover_log(fs, dir, {}, [&](const LogRecordView& r) -> Status {
    EXPECT_EQ(r.lsn, payloads.size() + 1);
    payloads.emplace_back(r.payload);
    return {};
  });
  EXPECT_TRUE(recovered.ok()) << (recovered.ok() ? "" : recovered.error().to_string());
  if (recovered.ok() && info != nullptr) *info = *recovered;
  return payloads;
}

class LogWriterTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.create_dir_if_missing(kDir).ok()); }

  std::unique_ptr<LogWriter> open(LogWriterOptions options = {}) {
    options.dir = kDir;
    if (!options.on_commit) options.on_commit = signal_.callback();
    const auto recovered =
        recover_log(fs_, kDir, {}, [](const LogRecordView&) { return Status{}; });
    EXPECT_TRUE(recovered.ok());
    auto writer = LogWriter::open(fs_, std::move(options), *recovered);
    EXPECT_TRUE(writer.ok()) << (writer.ok() ? "" : writer.error().to_string());
    return std::move(writer).value();
  }

  SimFs fs_;
  CommitSignal signal_;
};

TEST_F(LogWriterTest, AppendFlushCommitRecover) {
  auto writer = open();
  EXPECT_EQ(writer->committed_lsn(), 0U);
  EXPECT_EQ(writer->append(1, "one"), 1U);
  EXPECT_EQ(writer->append(2, "two"), 2U);
  EXPECT_EQ(writer->append(3, "three"), 3U);
  EXPECT_EQ(writer->last_appended_lsn(), 3U);
  writer->flush();
  ASSERT_TRUE(signal_.wait_for(*writer, 3));
  EXPECT_EQ(writer->backlog_bytes(), 0U);
  writer->stop();

  EXPECT_EQ(recover_payloads(fs_, kDir), (std::vector<std::string>{"one", "two", "three"}));
}

TEST_F(LogWriterTest, NothingReachesTheDiskBeforeFlush) {
  auto writer = open();
  writer->append(1, "pending");
  EXPECT_GT(writer->backlog_bytes(), 0U);
  // Without flush() the log thread has nothing to write: the segment holds
  // only its header. (No sleep needed: the hand-off has not happened.)
  EXPECT_EQ(fs_.file_size(join_path(kDir, segment_file_name(1))).value(), kSegmentHeaderSize);
  EXPECT_EQ(writer->committed_lsn(), 0U);
}

TEST_F(LogWriterTest, OneFlushOfManyRecordsIsOneBatchAndOneSync) {
  auto writer = open();
  for (int i = 0; i < 100; ++i) writer->append(1, "record");
  writer->flush();
  ASSERT_TRUE(signal_.wait_for(*writer, 100));

  const LogWriterStats stats = writer->stats();
  EXPECT_EQ(stats.batches, 1U);
  EXPECT_EQ(stats.records, 100U);
  EXPECT_EQ(stats.batch_records.max(), 100U);
  EXPECT_EQ(stats.syncs, 1U) << "group commit: one fsync covers the whole batch";
  EXPECT_EQ(stats.bytes, 100 * (kRecordHeaderSize + 6));
}

TEST_F(LogWriterTest, FirstSegmentIsDurableBeforeAnyRecord) {
  auto writer = open();
  const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
  ASSERT_TRUE(image->exists(join_path(kDir, segment_file_name(1))));
  EXPECT_EQ(image->file_size(join_path(kDir, segment_file_name(1))).value(), kSegmentHeaderSize);
}

// The core promise of the `always` policy: at the moment a commit is announced,
// a power failure cannot lose any record up to the committed LSN.
TEST_F(LogWriterTest, AlwaysPolicyCommitsOnlyWhatSurvivesPowerLoss) {
  std::atomic<LogWriter*> writer_ptr{nullptr};
  std::atomic<int> commits_checked{0};
  LogWriterOptions options;
  options.segment_size = 256;  // exercise rolling as well
  options.on_commit = [&] {
    const LogWriter* writer = writer_ptr.load();
    if (writer == nullptr) return;
    const Lsn committed = writer->committed_lsn();
    const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
    RecoveredLog info;
    recover_payloads(*image, kDir, &info);
    EXPECT_GE(info.last_lsn, committed);
    ++commits_checked;
    signal_.callback()();
  };
  auto writer = open(std::move(options));
  writer_ptr = writer.get();

  for (int i = 0; i < 200; ++i) {
    writer->append(1, "payload-" + std::to_string(i));
    if (i % 3 == 0) writer->flush();
  }
  writer->flush();
  ASSERT_TRUE(signal_.wait_for(*writer, 200));
  writer->stop();
  EXPECT_GT(commits_checked.load(), 0);
}

TEST_F(LogWriterTest, IntervalPolicyCommitsBeforeSyncAndStopMakesItDurable) {
  LogWriterOptions options;
  options.fsync_policy = FsyncPolicy::kInterval;
  options.fsync_interval_ms = 3'600'000;  // never fires during this test
  auto writer = open(std::move(options));
  writer->append(1, "fast ack");
  writer->flush();
  ASSERT_TRUE(signal_.wait_for(*writer, 1));

  // Committed means "survives a process crash"...
  EXPECT_EQ(recover_payloads(*fs_.crash_image(CrashMode::kKeepUnsynced), kDir).size(), 1U);
  // ...but not yet a power failure. This is the documented trade of `interval`.
  EXPECT_EQ(recover_payloads(*fs_.crash_image(CrashMode::kLoseUnsynced), kDir).size(), 0U);

  writer->stop();
  EXPECT_EQ(recover_payloads(*fs_.crash_image(CrashMode::kLoseUnsynced), kDir).size(), 1U);
}

TEST_F(LogWriterTest, IntervalPolicySyncsInTheBackground) {
  LogWriterOptions options;
  options.fsync_policy = FsyncPolicy::kInterval;
  options.fsync_interval_ms = 5;
  auto writer = open(std::move(options));
  writer->append(1, "x");
  writer->flush();
  ASSERT_TRUE(signal_.wait_for(*writer, 1));

  // Poll (bounded) until the background sync has made the record power-safe.
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (recover_payloads(*fs_.crash_image(CrashMode::kLoseUnsynced), kDir).empty()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "background fsync never happened";
    std::this_thread::sleep_for(1ms);
  }
}

TEST_F(LogWriterTest, RollsSegmentsAndRecoversAcrossThem) {
  LogWriterOptions options;
  options.segment_size = 200;
  auto writer = open(std::move(options));
  std::vector<std::string> expected;
  for (int i = 0; i < 50; ++i) {
    expected.push_back("payload-" + std::to_string(i));
    writer->append(1, expected.back());
    writer->flush();
    ASSERT_TRUE(signal_.wait_for(*writer, static_cast<Lsn>(i + 1)));
  }
  writer->stop();

  RecoveredLog info;
  EXPECT_EQ(recover_payloads(fs_, kDir, &info), expected);
  EXPECT_GT(info.segments.size(), 3U);
  EXPECT_EQ(writer->stats().segments_created, info.segments.size());
  for (size_t i = 1; i < info.segments.size(); ++i) {
    EXPECT_EQ(info.segments[i].first_lsn, info.segments[i - 1].last_lsn + 1);
  }
  // Every segment, including the newest, survives a power failure.
  EXPECT_EQ(recover_payloads(*fs_.crash_image(CrashMode::kLoseUnsynced), kDir), expected);
}

TEST_F(LogWriterTest, ReopenContinuesLsnsInTheSameSegment) {
  {
    auto writer = open();
    writer->append(1, "a");
    writer->append(1, "b");
    writer->stop();
  }
  {
    auto writer = open();
    EXPECT_EQ(writer->committed_lsn(), 2U);
    EXPECT_EQ(writer->append(1, "c"), 3U);
    writer->stop();
  }
  RecoveredLog info;
  EXPECT_EQ(recover_payloads(fs_, kDir, &info), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(info.segments.size(), 1U);
}

TEST_F(LogWriterTest, StopDrainsEverythingAndIsIdempotent) {
  auto writer = open();
  for (int i = 0; i < 1000; ++i) writer->append(1, "r");
  writer->stop();  // no explicit flush, no waiting
  writer->stop();
  EXPECT_EQ(writer->committed_lsn(), 1000U);
  EXPECT_EQ(recover_payloads(*fs_.crash_image(CrashMode::kLoseUnsynced), kDir).size(), 1000U);
}

TEST_F(LogWriterTest, ManySmallFlushesUnderASlowDiskStillCommitInOrder) {
  // Appends race with the log thread; whatever the batching, LSNs commit in
  // order and nothing is lost. Run under TSan this is also the data-race test.
  auto writer = open();
  std::atomic<bool> done{false};
  std::thread observer([&] {
    Lsn last = 0;
    while (!done.load()) {
      const Lsn now = writer->committed_lsn();
      EXPECT_GE(now, last);
      last = now;
      (void)writer->stats();
    }
  });
  constexpr int kRecords = 20'000;
  for (int i = 0; i < kRecords; ++i) {
    writer->append(static_cast<uint8_t>(i % 5), "payload");
    writer->flush();
  }
  ASSERT_TRUE(signal_.wait_for(*writer, kRecords));
  done = true;
  observer.join();
  writer->stop();

  const LogWriterStats stats = writer->stats();
  EXPECT_EQ(stats.records, static_cast<uint64_t>(kRecords));
  EXPECT_LE(stats.batches, static_cast<uint64_t>(kRecords));
  EXPECT_EQ(recover_payloads(fs_, kDir).size(), static_cast<size_t>(kRecords));
}

TEST_F(LogWriterTest, RejectsNonsenseOptions) {
  const auto recovered = recover_log(fs_, kDir, {}, [](const LogRecordView&) { return Status{}; });
  ASSERT_TRUE(recovered.ok());
  LogWriterOptions tiny;
  tiny.dir = kDir;
  tiny.segment_size = 8;
  EXPECT_FALSE(LogWriter::open(fs_, tiny, *recovered).ok());

  LogWriterOptions bad_interval;
  bad_interval.dir = kDir;
  bad_interval.fsync_policy = FsyncPolicy::kInterval;
  bad_interval.fsync_interval_ms = 0;
  EXPECT_FALSE(LogWriter::open(fs_, bad_interval, *recovered).ok());
}

TEST(LogWriterPosixTest, WorksOnARealFileSystem) {
  const TempDir dir;
  PosixFs fs;
  CommitSignal signal;
  {
    const auto recovered =
        recover_log(fs, dir.path(), {}, [](const LogRecordView&) { return Status{}; });
    ASSERT_TRUE(recovered.ok());
    LogWriterOptions options;
    options.dir = dir.path();
    options.segment_size = 4096;
    options.on_commit = signal.callback();
    auto writer = LogWriter::open(fs, std::move(options), *recovered);
    ASSERT_TRUE(writer.ok()) << writer.error().to_string();
    // Segments roll between batches, so wait for each batch to commit: otherwise
    // a slow log thread may legitimately merge everything into one big batch,
    // and one segment.
    for (int i = 0; i < 500; ++i) {
      (*writer)->append(1, "payload-" + std::to_string(i));
      if (i % 10 == 9) {
        (*writer)->flush();
        ASSERT_TRUE(signal.wait_for(**writer, static_cast<Lsn>(i + 1)));
      }
    }
    EXPECT_GT((*writer)->stats().sync_micros.count(), 0U);
  }
  RecoveredLog info;
  const auto payloads = recover_payloads(fs, dir.path(), &info);
  ASSERT_EQ(payloads.size(), 500U);
  EXPECT_EQ(payloads.front(), "payload-0");
  EXPECT_EQ(payloads.back(), "payload-499");
  EXPECT_GT(info.segments.size(), 1U);
}

// --- failures abort --------------------------------------------------------------
// The log thread is what aborts, so these use the thread-safe death test style
// (the child re-executes the test binary instead of forking a threaded process).

class LogWriterDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }

  // Opens a writer on a SimFs prepared by `inject`, writes a record and waits.
  static void write_one_record(const std::function<void(SimFs&)>& inject) {
    SimFs fs;
    ASSERT_TRUE(fs.create_dir_if_missing(kDir).ok());
    const auto recovered = recover_log(fs, kDir, {}, [](const LogRecordView&) { return Status{}; });
    ASSERT_TRUE(recovered.ok());
    CommitSignal signal;
    LogWriterOptions options;
    options.dir = kDir;
    options.on_commit = signal.callback();
    auto writer = LogWriter::open(fs, std::move(options), *recovered);
    ASSERT_TRUE(writer.ok());
    inject(fs);
    (*writer)->append(1, std::string(100, 'x'));
    (*writer)->flush();
    (void)signal.wait_for(**writer, 1);
  }
};

TEST_F(LogWriterDeathTest, FsyncFailureAborts) {
  EXPECT_DEATH(write_one_record([](SimFs& fs) { fs.fail_sync_after(1); }),
               "fatal: fsync of the log: io error: fsync: Input/output error");
}

TEST_F(LogWriterDeathTest, WriteFailureAborts) {
  EXPECT_DEATH(write_one_record([](SimFs& fs) { fs.fail_append_after(1); }),
               "fatal: writing the log: io error: write: Input/output error");
}

TEST_F(LogWriterDeathTest, FullDiskAborts) {
  EXPECT_DEATH(write_one_record([](SimFs& fs) { fs.set_capacity(kSegmentHeaderSize + 10); }),
               "fatal: writing the log: io error: write: No space left on device");
}

TEST_F(LogWriterDeathTest, AppendAfterStopIsABug) {
  EXPECT_DEATH(
      {
        SimFs fs;
        (void)fs.create_dir_if_missing(kDir);
        const auto recovered =
            recover_log(fs, kDir, {}, [](const LogRecordView&) { return Status{}; });
        LogWriterOptions options;
        options.dir = kDir;
        auto writer = LogWriter::open(fs, std::move(options), *recovered);
        (*writer)->stop();
        (*writer)->append(1, "too late");
      },
      "append\\(\\) after stop\\(\\)");
}

}  // namespace
}  // namespace baton
