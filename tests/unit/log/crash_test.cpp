// Randomized crash testing of the log (docs/design.md section 4.5).
//
// For many seeds: run a writer against SimFs, take a crash image after a random
// number of file-system operations (remembering what had been committed at that
// instant), then recover from the image and check:
//
//   1. Recovery succeeds. A crash can only tear the tail, never the middle.
//   2. Every record committed before the crash is present and intact.
//   3. Recovered LSNs are contiguous from 1.
//   4. The repaired log accepts new records and recovers again.
//
// This is the test that fails if a directory fsync is missing, if a segment is
// rolled before the old one is durable, or if a commit is announced early.

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>

#include "log/format.h"
#include "log/log_writer.h"
#include "log/recovery.h"
#include "testing/sim_fs.h"

namespace baton {
namespace {

constexpr const char* kDir = "data";

// Payloads are a pure function of the LSN, so recovery can verify content
// without remembering what was written.
std::string payload_for(Lsn lsn) {
  std::mt19937_64 rng(lsn);
  std::string payload = "lsn=" + std::to_string(lsn) + ";";
  payload.append(rng() % 120, static_cast<char>('a' + (lsn % 26)));
  return payload;
}

struct Verified {
  RecoveredLog log;
  bool ok = false;
};

Verified recover_and_verify(SimFs& fs) {
  Verified out;
  Lsn expected = 1;
  bool content_ok = true;
  const auto recovered = recover_log(fs, kDir, {}, [&](const LogRecordView& r) -> Status {
    if (r.lsn != expected || r.payload != payload_for(r.lsn)) content_ok = false;
    ++expected;
    return {};
  });
  if (!recovered.ok()) {
    ADD_FAILURE() << "recovery refused: " << recovered.error().to_string();
    return out;
  }
  EXPECT_TRUE(content_ok) << "a recovered record had the wrong LSN or contents";
  EXPECT_EQ(recovered->last_lsn, expected - 1);
  out.log = *recovered;
  out.ok = content_ok;
  return out;
}

struct Scenario {
  FsyncPolicy policy;
  CrashMode mode;
};

class LogCrashTest : public ::testing::TestWithParam<Scenario> {};

TEST_P(LogCrashTest, CommittedRecordsSurviveACrashAtAnyPoint) {
  const Scenario scenario = GetParam();
  constexpr int kSeeds = 150;

  for (uint64_t seed = 1; seed <= kSeeds; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937_64 rng(seed);

    SimFs fs;
    ASSERT_TRUE(fs.create_dir_if_missing(kDir).ok());
    const auto empty = recover_log(fs, kDir, {}, [](const LogRecordView&) { return Status{}; });
    ASSERT_TRUE(empty.ok());

    LogWriterOptions options;
    options.dir = kDir;
    options.fsync_policy = scenario.policy;
    options.fsync_interval_ms = 1;
    options.segment_size = 300 + (rng() % 700);  // small, so rolls happen constantly
    auto opened = LogWriter::open(fs, std::move(options), *empty);
    ASSERT_TRUE(opened.ok());
    LogWriter& writer = **opened;

    // Freeze a crash image somewhere in the run. `committed_at_crash` is read
    // inside the file-system operation that triggers the capture, so it is
    // exactly what had been acknowledged when the "power went out".
    std::atomic<Lsn> committed_at_crash{0};
    std::atomic<bool> captured{false};
    fs.capture_crash_image_after(1 + (rng() % 80), scenario.mode, seed, [&] {
      committed_at_crash = writer.committed_lsn();
      captured = true;
    });

    // Even seeds run in lockstep (wait for every commit), which under the
    // `always` policy makes the whole sequence of file-system operations, and so
    // the crash point, a pure function of the seed: a failure reproduces
    // exactly. (`interval` adds timer-driven syncs.) Odd seeds let the log
    // thread race the appends, which covers merged batches.
    const bool lockstep = seed % 2 == 0;
    const int records = 40 + static_cast<int>(rng() % 60);
    for (int i = 0; i < records; ++i) {
      const Lsn lsn = writer.last_appended_lsn() + 1;
      ASSERT_EQ(writer.append(static_cast<uint8_t>(lsn % 3), payload_for(lsn)), lsn);
      if (rng() % 4 != 0) {
        writer.flush();
        while (lockstep && writer.committed_lsn() < lsn) std::this_thread::yield();
      }
    }
    writer.stop();

    std::unique_ptr<SimFs> image = fs.take_captured_image();
    if (!captured) {
      // The run was shorter than the trigger: crash at the very end instead.
      committed_at_crash = writer.committed_lsn();
      image = fs.crash_image(scenario.mode, seed);
    }
    ASSERT_NE(image, nullptr);

    const Verified first = recover_and_verify(*image);
    ASSERT_TRUE(first.ok);
    if (scenario.policy == FsyncPolicy::kAlways || scenario.mode == CrashMode::kKeepUnsynced) {
      // `always` survives power loss; `interval` promises only process crashes.
      EXPECT_GE(first.log.last_lsn, committed_at_crash.load()) << "an acknowledged record was lost";
    }

    // Life goes on: the repaired log takes new records and recovers again.
    LogWriterOptions again;
    again.dir = kDir;
    auto reopened = LogWriter::open(*image, std::move(again), first.log);
    ASSERT_TRUE(reopened.ok()) << reopened.error().to_string();
    for (int i = 0; i < 10; ++i) {
      const Lsn lsn = (*reopened)->last_appended_lsn() + 1;
      (*reopened)->append(1, payload_for(lsn));
    }
    (*reopened)->stop();
    const Verified second = recover_and_verify(*image);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.log.last_lsn, first.log.last_lsn + 10);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, LogCrashTest,
    ::testing::Values(Scenario{FsyncPolicy::kAlways, CrashMode::kTorn},
                      Scenario{FsyncPolicy::kAlways, CrashMode::kLoseUnsynced},
                      Scenario{FsyncPolicy::kAlways, CrashMode::kKeepUnsynced},
                      Scenario{FsyncPolicy::kInterval, CrashMode::kTorn},
                      Scenario{FsyncPolicy::kInterval, CrashMode::kLoseUnsynced},
                      Scenario{FsyncPolicy::kInterval, CrashMode::kKeepUnsynced}),
    [](const ::testing::TestParamInfo<Scenario>& info) {
      const std::string policy = info.param.policy == FsyncPolicy::kAlways ? "Always" : "Interval";
      switch (info.param.mode) {
        case CrashMode::kTorn:
          return policy + "TornPowerLoss";
        case CrashMode::kLoseUnsynced:
          return policy + "CleanPowerLoss";
        case CrashMode::kKeepUnsynced:
          return policy + "ProcessCrash";
      }
      return policy;
    });

}  // namespace
}  // namespace baton
