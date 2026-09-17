// Recovery from snapshot + log, and the crash-safety of the snapshot write
// protocol (docs/design.md 8.3, 8.4).
//
// The oracle throughout: whatever happens to snapshots, recovery must produce
// exactly the state that replaying the complete log produces.

#include "snapshot/recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "common/logging.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshotter.h"
#include "support/storage_fixture.h"

namespace baton {
namespace {

class StateRecoveryTest : public ::testing::Test {
 protected:
  StateRecoveryTest() : engine_(live_, sink_, clock_, EngineOptions{}, /*rng_seed=*/11) {}

  void SetUp() override {
    set_log_level(LogLevel::kOff);  // skipped snapshots and removed temp files warn
    ASSERT_TRUE(fs_.create_dir_if_missing(kStorageDir).ok());
    run_mixed_traffic(engine_, clock_, 400);
    records_ = sink_.entries().size();
    ASSERT_GT(records_, 500U);
    engine_.tick();
    expected_ = serialized(live_);
  }
  void TearDown() override { set_log_level(LogLevel::kInfo); }

  // Recovers from `fs` and returns the canonical serialization of the result.
  std::string recover(SimFs& fs, StateRecovery* out = nullptr) {
    auto recovered = recover_state(fs, kStorageDir, StateOptions{}, clock_.wall_now(),
                                   clock_.mono_now(), /*lease_grace_ms=*/0);
    EXPECT_TRUE(recovered.ok()) << (recovered.ok() ? "" : recovered.error().to_string());
    if (!recovered.ok()) return "<recovery failed: " + recovered.error().to_string() + ">";
    const Status invariants = recovered->state->check_invariants();
    EXPECT_TRUE(invariants.ok()) << (invariants.ok() ? "" : invariants.error().to_string());
    std::string bytes = serialized(*recovered->state);
    if (out != nullptr) *out = std::move(*recovered);
    return bytes;
  }

  void snapshot_at(SimFs& fs, size_t record_count) {
    ASSERT_TRUE(write_snapshot(fs, kStorageDir, image_after(sink_, record_count),
                               static_cast<Lsn>(record_count), clock_.wall_now())
                    .ok());
  }

  FakeClock clock_;
  MemoryRecordSink sink_;
  State live_;
  Engine engine_;
  SimFs fs_;
  size_t records_ = 0;
  std::string expected_;
};

TEST_F(StateRecoveryTest, LogAloneRebuildsTheLiveState) {
  write_log(fs_, sink_, 0, records_, 100);
  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.snapshot.lsn, 0U);
  EXPECT_EQ(info.log.records_replayed, records_);
}

TEST_F(StateRecoveryTest, SnapshotPlusTailEqualsFullReplay) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.snapshot.lsn, 250U);
  EXPECT_EQ(info.log.records_replayed, records_ - 250) << "only the tail is replayed";
}

TEST_F(StateRecoveryTest, SnapshotAtTheVeryEndNeedsNoReplay) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, records_);
  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.log.records_replayed, 0U);
  EXPECT_EQ(info.log.last_lsn, records_);
}

TEST_F(StateRecoveryTest, CompactedLogRecoversFromTheSnapshots) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  snapshot_at(fs_, 450);
  const auto report = compact(fs_, kStorageDir);
  ASSERT_TRUE(report.ok());
  EXPECT_EQ(report->segments_removed, 2U) << "[1,100] and [101,200] precede snapshot 250";
  EXPECT_TRUE(recover(fs_) == expected_);
}

TEST_F(StateRecoveryTest, UnreadableNewestSnapshotFallsBackToTheOlderOne) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  snapshot_at(fs_, 450);
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  fs_.flip_bit(join_path(kStorageDir, snapshot_file_name(450)), 200, 3);  // bit rot

  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.snapshot.lsn, 250U);
  EXPECT_EQ(info.snapshots_rejected, 1U);
}

TEST_F(StateRecoveryTest, TruncatedNewestSnapshotFallsBackToo) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  snapshot_at(fs_, 450);
  const std::string path = join_path(kStorageDir, snapshot_file_name(450));
  fs_.write_file(path, fs_.read_file(path).value().substr(0, 500));  // a torn snapshot
  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.snapshot.lsn, 250U);
}

TEST_F(StateRecoveryTest, NoReadableSnapshotFallsBackToTheWholeLogIfItIsStillThere) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);  // a single snapshot: compaction keeps the whole log
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  fs_.flip_bit(join_path(kStorageDir, snapshot_file_name(250)), 100, 0);
  StateRecovery info;
  EXPECT_TRUE(recover(fs_, &info) == expected_);
  EXPECT_EQ(info.snapshot.lsn, 0U);
  EXPECT_EQ(info.log.records_replayed, records_);
}

TEST_F(StateRecoveryTest, RefusesWhenNeitherSnapshotNorLogCanCoverTheGap) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  snapshot_at(fs_, 450);
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  fs_.flip_bit(join_path(kStorageDir, snapshot_file_name(250)), 100, 0);
  fs_.flip_bit(join_path(kStorageDir, snapshot_file_name(450)), 100, 0);
  const auto recovered =
      recover_state(fs_, kStorageDir, StateOptions{}, clock_.wall_now(), clock_.mono_now(), 0);
  ASSERT_FALSE(recovered.ok()) << "the early log is gone and both snapshots are unreadable";
  EXPECT_EQ(recovered.error().code(), ErrorCode::kCorruption);
  EXPECT_NE(recovered.error().message().find("segments are missing"), std::string::npos);
}

TEST_F(StateRecoveryTest, LeftoverTempFilesAreRemoved) {
  write_log(fs_, sink_, 0, records_, 100);
  fs_.write_file(join_path(kStorageDir, snapshot_temp_name(300)), "half a snapshot");
  EXPECT_TRUE(recover(fs_) == expected_);
  EXPECT_FALSE(fs_.exists(join_path(kStorageDir, snapshot_temp_name(300))));
}

TEST_F(StateRecoveryTest, LogDamageIsNeverPaperedOverByASnapshot) {
  write_log(fs_, sink_, 0, records_, 100);
  snapshot_at(fs_, 250);
  // Damage in the tail the snapshot does NOT cover.
  fs_.flip_bit(join_path(kStorageDir, segment_file_name(301)), 80, 1);
  const auto recovered =
      recover_state(fs_, kStorageDir, StateOptions{}, clock_.wall_now(), clock_.mono_now(), 0);
  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code(), ErrorCode::kCorruption);
}

// --- a crash at every step of the snapshot protocol --------------------------------------
//
// For every file-system operation of three consecutive snapshot cycles (the
// second and third delete segments and an old snapshot), take a crash image
// right after that operation and recover from it. All records are already
// durable in the log, so every image must recover to the same state. This is
// the test that fails if something is deleted before the snapshot that makes it
// dispensable is durable.

struct CrashCase {
  CrashMode mode;
  const char* name;
};

class SnapshotCrashTest : public StateRecoveryTest,
                          public ::testing::WithParamInterface<CrashCase> {};

TEST_P(SnapshotCrashTest, EveryCrashPointRecoversTheSameState) {
  write_log(fs_, sink_, 0, records_, 60);

  // `base` accumulates completed cycles; each crash experiment runs on a clone.
  std::unique_ptr<SimFs> base = fs_.crash_image(CrashMode::kKeepUnsynced);
  size_t crash_points = 0;
  for (const size_t cut : {size_t{150}, size_t{330}, records_}) {
    SCOPED_TRACE("snapshot at LSN " + std::to_string(cut));
    const StateImage image = image_after(sink_, cut);

    for (uint64_t op = 1;; ++op) {
      std::unique_ptr<SimFs> scratch = base->crash_image(CrashMode::kKeepUnsynced);
      scratch->capture_crash_image_after(op, GetParam().mode, /*seed=*/op * 7919 + cut, nullptr);
      ASSERT_TRUE(
          write_snapshot(*scratch, kStorageDir, image, static_cast<Lsn>(cut), WallTime{1}).ok());
      ASSERT_TRUE(compact(*scratch, kStorageDir).ok());

      const std::unique_ptr<SimFs> crashed = scratch->take_captured_image();
      if (crashed == nullptr) break;  // the cycle has fewer than `op` operations
      ++crash_points;
      ASSERT_TRUE(recover(*crashed) == expected_) << "crash after file-system operation " << op;
    }

    // The cycle completes; the next one builds on it.
    ASSERT_TRUE(write_snapshot(*base, kStorageDir, image, static_cast<Lsn>(cut), WallTime{1}).ok());
    ASSERT_TRUE(compact(*base, kStorageDir).ok());
    ASSERT_TRUE(recover(*base) == expected_);
  }
  EXPECT_GT(crash_points, 20U) << "sanity: the cycles really were interrupted step by step";
  EXPECT_EQ(list_snapshots(*base, kStorageDir).value().size(), kSnapshotKeepCount);
}

INSTANTIATE_TEST_SUITE_P(Modes, SnapshotCrashTest,
                         ::testing::Values(CrashCase{CrashMode::kLoseUnsynced, "CleanPowerLoss"},
                                           CrashCase{CrashMode::kTorn, "TornPowerLoss"},
                                           CrashCase{CrashMode::kKeepUnsynced, "ProcessCrash"}),
                         [](const auto& info) { return std::string(info.param.name); });

// Several seeds of the torn model for each crash point: which unsynced directory
// operations survive is random, and the dangerous combination (a deletion
// survives, the rename that justified it does not) has to come up.
TEST_F(StateRecoveryTest, TornCrashesWithManySeeds) {
  write_log(fs_, sink_, 0, records_, 60);
  std::unique_ptr<SimFs> base = fs_.crash_image(CrashMode::kKeepUnsynced);
  for (const size_t cut : {size_t{150}, size_t{330}, records_}) {
    const StateImage image = image_after(sink_, cut);
    for (uint64_t op = 1;; ++op) {
      bool ran_out = false;
      for (uint64_t seed = 0; seed < 12 && !ran_out; ++seed) {
        std::unique_ptr<SimFs> scratch = base->crash_image(CrashMode::kKeepUnsynced);
        scratch->capture_crash_image_after(op, CrashMode::kTorn, (seed * 1'000'003) + op, nullptr);
        ASSERT_TRUE(
            write_snapshot(*scratch, kStorageDir, image, static_cast<Lsn>(cut), WallTime{1}).ok());
        ASSERT_TRUE(compact(*scratch, kStorageDir).ok());
        const std::unique_ptr<SimFs> crashed = scratch->take_captured_image();
        if (crashed == nullptr) {
          ran_out = true;
        } else {
          ASSERT_TRUE(recover(*crashed) == expected_)
              << "cut " << cut << " op " << op << " seed " << seed;
        }
      }
      if (ran_out) break;
    }
    ASSERT_TRUE(write_snapshot(*base, kStorageDir, image, static_cast<Lsn>(cut), WallTime{1}).ok());
    ASSERT_TRUE(compact(*base, kStorageDir).ok());
  }
}

// --- the background thread ---------------------------------------------------------------

TEST_F(StateRecoveryTest, SnapshotterWritesAndCompactsInTheBackground) {
  write_log(fs_, sink_, 0, records_, 100);
  std::mutex mutex;
  std::condition_variable done;
  int notifications = 0;
  Snapshotter snapshotter(fs_, kStorageDir, [&] {
    const std::scoped_lock lock(mutex);
    ++notifications;
    done.notify_all();
  });
  EXPECT_FALSE(snapshotter.busy());
  EXPECT_FALSE(snapshotter.take_result().has_value());

  for (const size_t cut : {size_t{200}, size_t{400}}) {
    const int before = notifications;
    snapshotter.start(image_after(sink_, cut), static_cast<Lsn>(cut), WallTime{1});
    {
      std::unique_lock lock(mutex);
      ASSERT_TRUE(
          done.wait_for(lock, std::chrono::seconds(30), [&] { return notifications > before; }));
    }
    const auto result = snapshotter.take_result();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().to_string();
    EXPECT_EQ((*result)->info.lsn, cut);
    EXPECT_FALSE(snapshotter.busy());
    EXPECT_FALSE(snapshotter.take_result().has_value()) << "a result is handed out once";
  }
  EXPECT_TRUE(recover(fs_) == expected_);
}

TEST_F(StateRecoveryTest, SnapshotterReportsFailuresInsteadOfCrashing) {
  write_log(fs_, sink_, 0, records_, 100);
  fs_.set_capacity(0);
  Snapshotter snapshotter(fs_, kStorageDir, nullptr);
  snapshotter.start(image_after(sink_, 200), 200, WallTime{1});
  snapshotter.wait();
  const auto result = snapshotter.take_result();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->ok());
  fs_.set_capacity(std::nullopt);
  EXPECT_TRUE(recover(fs_) == expected_) << "a failed snapshot costs nothing but the attempt";
}

}  // namespace
}  // namespace baton
