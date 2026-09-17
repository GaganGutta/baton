#include "snapshot/snapshot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/codec.h"
#include "common/crc32c.h"
#include "common/logging.h"
#include "log/format.h"
#include "support/storage_fixture.h"

namespace baton {
namespace {

class SnapshotTest : public ::testing::Test {
 protected:
  SnapshotTest() : engine_(state_, sink_, clock_, EngineOptions{}, /*rng_seed=*/5) {}

  void SetUp() override {
    set_log_level(LogLevel::kError);
    ASSERT_TRUE(fs_.create_dir_if_missing(kStorageDir).ok());
  }
  void TearDown() override { set_log_level(LogLevel::kInfo); }

  static std::string path_for(Lsn lsn) { return join_path(kStorageDir, snapshot_file_name(lsn)); }

  // Loads a snapshot and returns the canonical serialization of what it held.
  std::string load_and_serialize(Lsn lsn, SimFs& fs) {
    auto loaded = load_snapshot(fs, path_for(lsn), StateOptions{});
    EXPECT_TRUE(loaded.ok()) << (loaded.ok() ? "" : loaded.error().to_string());
    if (!loaded.ok()) return {};
    loaded->state->set_now(clock_.wall_now(), clock_.mono_now());
    loaded->state->end_replay();
    const Status invariants = loaded->state->check_invariants();
    EXPECT_TRUE(invariants.ok()) << (invariants.ok() ? "" : invariants.error().to_string());
    return serialized(*loaded->state);
  }

  FakeClock clock_;
  MemoryRecordSink sink_;
  State state_;
  Engine engine_;
  SimFs fs_;
};

TEST_F(SnapshotTest, FileNames) {
  EXPECT_EQ(snapshot_file_name(42), "snapshot-00000000000000000042.snap");
  EXPECT_EQ(snapshot_temp_name(42), "snapshot-00000000000000000042.tmp");
  EXPECT_EQ(parse_snapshot_file_name(snapshot_file_name(7)), 7U);
  EXPECT_EQ(parse_snapshot_file_name(snapshot_temp_name(7)), std::nullopt);
  EXPECT_TRUE(is_snapshot_temp_name(snapshot_temp_name(7)));
  EXPECT_FALSE(is_snapshot_temp_name(snapshot_file_name(7)));
  EXPECT_EQ(parse_snapshot_file_name("wal-00000000000000000001.log"), std::nullopt);
  EXPECT_EQ(parse_snapshot_file_name("snapshot-1.snap"), std::nullopt);
}

TEST_F(SnapshotTest, RoundTripAcrossManyChunks) {
  run_mixed_traffic(engine_, clock_, 12'000);  // > 2 job chunks, > 1 key chunk... almost
  const Lsn lsn = engine_.last_lsn();
  const auto info = write_snapshot(fs_, kStorageDir, state_.capture_image(), lsn, WallTime{123});
  ASSERT_TRUE(info.ok()) << info.error().to_string();
  EXPECT_EQ(info->lsn, lsn);
  EXPECT_EQ(info->jobs, state_.job_count());
  EXPECT_EQ(info->idem_keys, state_.idem_count());
  EXPECT_GE(info->chunks, 4U) << "meta, several job chunks, keys, end";
  EXPECT_EQ(info->bytes, fs_.file_size(path_for(lsn)).value());

  auto loaded = load_snapshot(fs_, path_for(lsn), StateOptions{});
  ASSERT_TRUE(loaded.ok()) << loaded.error().to_string();
  EXPECT_EQ(loaded->info.lsn, lsn);
  EXPECT_EQ(loaded->info.created_at, WallTime{123});
  EXPECT_EQ(loaded->info.jobs, info->jobs);
  EXPECT_EQ(loaded->info.bytes, info->bytes);
  EXPECT_TRUE(load_and_serialize(lsn, fs_) == serialized(state_));
}

TEST_F(SnapshotTest, EmptyStateRoundTrips) {
  ASSERT_TRUE(write_snapshot(fs_, kStorageDir, state_.capture_image(), 0, WallTime{1}).ok());
  EXPECT_TRUE(load_and_serialize(0, fs_) == serialized(state_));
}

TEST_F(SnapshotTest, LargePayloadsAreSplitIntoChunksBySize) {
  const std::string big(1'000'000, 'B');
  for (int i = 0; i < 30; ++i) ASSERT_TRUE(engine_.enqueue({.queue = "q", .payload = big}).ok());
  const auto info = write_snapshot(fs_, kStorageDir, state_.capture_image(), 30, WallTime{1});
  ASSERT_TRUE(info.ok()) << info.error().to_string();
  EXPECT_GE(info->chunks, 8U) << "30 MB of payload must not end up in one giant chunk";
  EXPECT_TRUE(load_and_serialize(30, fs_) == serialized(state_));
}

TEST_F(SnapshotTest, IsDurableAndCompleteOnceWriteReturns) {
  run_mixed_traffic(engine_, clock_, 300);
  ASSERT_TRUE(write_snapshot(fs_, kStorageDir, state_.capture_image(), 300, WallTime{1}).ok());
  EXPECT_FALSE(fs_.exists(join_path(kStorageDir, snapshot_temp_name(300))));

  // Power fails right after write_snapshot() returned.
  const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
  EXPECT_TRUE(load_and_serialize(300, *image) == serialized(state_));
}

TEST_F(SnapshotTest, StaleTempFileFromAFailedAttemptIsReplaced) {
  fs_.write_file(join_path(kStorageDir, snapshot_temp_name(9)), "leftover garbage");
  run_mixed_traffic(engine_, clock_, 50);
  ASSERT_TRUE(write_snapshot(fs_, kStorageDir, state_.capture_image(), 9, WallTime{1}).ok());
  EXPECT_TRUE(load_and_serialize(9, fs_) == serialized(state_));
}

TEST_F(SnapshotTest, FailedWriteLeavesNothingVisible) {
  run_mixed_traffic(engine_, clock_, 200);
  fs_.set_capacity(2'000);  // the disk fills up mid-snapshot
  const auto info = write_snapshot(fs_, kStorageDir, state_.capture_image(), 200, WallTime{1});
  ASSERT_FALSE(info.ok());
  EXPECT_NE(info.error().message().find("No space left"), std::string::npos);
  EXPECT_FALSE(fs_.exists(path_for(200)));
  EXPECT_TRUE(list_snapshots(fs_, kStorageDir).value().empty());
}

// --- the loader trusts nothing ------------------------------------------------------------

class SnapshotDamageTest : public SnapshotTest {
 protected:
  void SetUp() override {
    SnapshotTest::SetUp();
    run_mixed_traffic(engine_, clock_, 25);  // small: the tests below try every byte and bit
    ASSERT_TRUE(write_snapshot(fs_, kStorageDir, state_.capture_image(), 60, WallTime{1}).ok());
    pristine_ = fs_.read_file(path_for(60)).value();
  }

  Result<LoadedSnapshot> load_bytes(const std::string& bytes, Lsn lsn = 60) {
    fs_.write_file(path_for(lsn), bytes);
    return load_snapshot(fs_, path_for(lsn), StateOptions{});
  }

  std::string pristine_;
};

TEST_F(SnapshotDamageTest, EveryTruncationIsRejected) {
  for (size_t size = 0; size < pristine_.size(); ++size) {
    const auto loaded = load_bytes(pristine_.substr(0, size));
    ASSERT_FALSE(loaded.ok()) << "truncated to " << size << " of " << pristine_.size();
    EXPECT_EQ(loaded.error().code(), ErrorCode::kCorruption);
  }
}

TEST_F(SnapshotDamageTest, EveryBitFlipIsRejected) {
  for (size_t byte = 0; byte < pristine_.size(); ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::string damaged = pristine_;
      damaged[byte] = static_cast<char>(static_cast<unsigned char>(damaged[byte]) ^ (1U << bit));
      ASSERT_FALSE(load_bytes(damaged).ok()) << "byte " << byte << " bit " << bit;
    }
  }
}

TEST_F(SnapshotDamageTest, TrailingBytesAreRejected) {
  const auto loaded = load_bytes(pristine_ + "x");
  ASSERT_FALSE(loaded.ok());
  EXPECT_NE(loaded.error().message().find("data after the end chunk"), std::string::npos);
}

// A file cut exactly between two chunks has only valid chunks in it. The end
// chunk is what distinguishes it from a complete snapshot.
TEST_F(SnapshotDamageTest, CutAtAChunkBoundaryIsNotMistakenForComplete) {
  size_t offset = kSnapshotHeaderSize;
  std::vector<size_t> boundaries;
  while (offset < pristine_.size()) {
    const RecordParseResult chunk = parse_record(std::string_view(pristine_).substr(offset));
    ASSERT_EQ(chunk.status, RecordParseStatus::kOk);
    boundaries.push_back(offset);
    offset += chunk.size;
  }
  ASSERT_GE(boundaries.size(), 3U);
  for (const size_t boundary : boundaries) {
    const auto loaded = load_bytes(pristine_.substr(0, boundary));
    ASSERT_FALSE(loaded.ok()) << "cut at " << boundary;
    EXPECT_NE(loaded.error().message().find("no end chunk"), std::string::npos);
  }
}

using Chunks = std::vector<std::pair<uint8_t, std::string>>;

Chunks chunks_of(const std::string& file) {
  Chunks chunks;
  for (size_t offset = kSnapshotHeaderSize; offset < file.size();) {
    const RecordParseResult chunk = parse_record(std::string_view(file).substr(offset));
    EXPECT_EQ(chunk.status, RecordParseStatus::kOk);
    if (chunk.status != RecordParseStatus::kOk) break;
    chunks.emplace_back(chunk.record.type, std::string(chunk.record.payload));
    offset += chunk.size;
  }
  return chunks;
}

// `chunks` behind `header`, with fresh sequence numbers and valid checksums.
std::string reframed(const std::string& header, const Chunks& chunks, Lsn first_sequence = 1) {
  std::string file = header;
  Lsn sequence = first_sequence;
  for (const auto& [type, payload] : chunks) append_record(file, sequence++, type, payload);
  return file;
}

TEST_F(SnapshotDamageTest, StructurallyValidButInconsistentFilesAreRejected) {
  // Re-frame the pristine chunks in ways a checksum cannot notice.
  const Chunks chunks = chunks_of(pristine_);
  const std::string header = pristine_.substr(0, kSnapshotHeaderSize);
  const auto build = [&](const std::vector<size_t>& order, Lsn first_sequence = 1) {
    Chunks picked;
    for (const size_t index : order) picked.push_back(chunks[index]);
    return reframed(header, picked, first_sequence);
  };
  const size_t last = chunks.size() - 1;
  ASSERT_GE(chunks.size(), 3U);
  ASSERT_EQ(chunks[0].first, static_cast<uint8_t>(SnapshotChunk::kMeta));

  std::vector<size_t> all(chunks.size());
  for (size_t i = 0; i < all.size(); ++i) all[i] = i;
  ASSERT_TRUE(load_bytes(build(all)).ok()) << "sanity: the rebuilt file is valid";

  EXPECT_FALSE(load_bytes(build(all, /*first_sequence=*/2)).ok()) << "sequence must start at 1";
  EXPECT_FALSE(load_bytes(build({0, last})).ok()) << "a jobs chunk is missing: totals differ";
  EXPECT_FALSE(load_bytes(build({1, 0, last})).ok()) << "jobs before meta";
  EXPECT_FALSE(load_bytes(build({0, 1, 1, last})).ok()) << "a chunk twice: duplicate job ids";
  EXPECT_FALSE(load_bytes(build({0, 0, 1, last})).ok()) << "two meta chunks";
}

// Every chunk has its own checksum and sequence number, so a chunk taken from
// the same position of a different snapshot passes both. The totals in the end
// chunk are what catches such a splice.
TEST_F(SnapshotDamageTest, ChunkSplicedInFromAnotherSnapshotIsRejected) {
  // The same traffic stopped earlier: fewer jobs and keys, otherwise compatible.
  FakeClock clock;
  MemoryRecordSink sink;
  State smaller;
  Engine engine(smaller, sink, clock, EngineOptions{}, /*rng_seed=*/5);
  run_mixed_traffic(engine, clock, 10);
  ASSERT_TRUE(write_snapshot(fs_, kStorageDir, smaller.capture_image(), 61, WallTime{1}).ok());

  const Chunks mine = chunks_of(pristine_);
  const Chunks other = chunks_of(fs_.read_file(path_for(61)).value());
  ASSERT_EQ(mine.size(), 4U) << "meta, jobs, keys, end";
  ASSERT_EQ(other.size(), 4U);
  ASSERT_NE(mine[1].second, other[1].second);
  ASSERT_NE(mine[2].second, other[2].second);
  const std::string header = pristine_.substr(0, kSnapshotHeaderSize);
  ASSERT_TRUE(load_bytes(reframed(header, mine)).ok()) << "sanity: the rebuilt file is valid";

  for (const size_t position : {size_t{1}, size_t{2}}) {
    Chunks spliced = mine;
    spliced[position] = other[position];
    const auto loaded = load_bytes(reframed(header, spliced));
    ASSERT_FALSE(loaded.ok()) << "foreign chunk at position " << position;
    EXPECT_NE(loaded.error().message().find("totals do not match"), std::string::npos)
        << loaded.error().message();
  }
}

TEST_F(SnapshotDamageTest, HeaderMustMatchTheFileName) {
  const auto loaded = load_bytes(pristine_, /*lsn=*/61);
  ASSERT_FALSE(loaded.ok());
  EXPECT_NE(loaded.error().message().find("does not match the file name"), std::string::npos);
}

TEST_F(SnapshotDamageTest, NewerFormatVersionIsNotCorruption) {
  std::string future;
  ByteWriter w(future);
  w.raw(kSnapshotMagic);
  w.u32(kSnapshotFormatVersion + 1);
  w.u32(0);
  w.u64(60);
  w.u64(1);
  w.u32(crc32c(future));
  w.u32(0);
  const auto loaded = load_bytes(future + pristine_.substr(kSnapshotHeaderSize));
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.error().code(), ErrorCode::kFailedPrecondition);
}

// --- compaction ------------------------------------------------------------------------------

class CompactionTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.create_dir_if_missing(kStorageDir).ok()); }

  void add_segments(const std::vector<Lsn>& first_lsns) {
    for (const Lsn lsn : first_lsns) {
      fs_.write_file(join_path(kStorageDir, segment_file_name(lsn)), "segment");
    }
  }
  void add_snapshots(const std::vector<Lsn>& lsns) {
    for (const Lsn lsn : lsns) {
      fs_.write_file(join_path(kStorageDir, snapshot_file_name(lsn)), "snapshot");
    }
  }
  std::vector<std::string> files() {
    auto names = fs_.list_dir(kStorageDir).value();
    std::ranges::sort(names);
    return names;
  }

  SimFs fs_;
};

TEST_F(CompactionTest, ASingleSnapshotDeletesNothing) {
  add_segments({1, 101, 201});
  add_snapshots({250});
  const auto report = compact(fs_, kStorageDir);
  ASSERT_TRUE(report.ok());
  EXPECT_EQ(report->segments_removed, 0U) << "no fallback snapshot yet: keep the whole log";
  EXPECT_EQ(files().size(), 4U);
}

TEST_F(CompactionTest, SegmentsGoOnlyOnTheStrengthOfTheOlderSnapshot) {
  add_segments({1, 101, 201, 301, 401});
  add_snapshots({150, 350});
  const auto report = compact(fs_, kStorageDir);
  ASSERT_TRUE(report.ok());
  // The older snapshot covers LSN 150: only segment [1,100] lies entirely before it.
  EXPECT_EQ(report->segments_removed, 1U);
  EXPECT_EQ(files(), (std::vector<std::string>{snapshot_file_name(150), snapshot_file_name(350),
                                               segment_file_name(101), segment_file_name(201),
                                               segment_file_name(301), segment_file_name(401)}));
}

TEST_F(CompactionTest, OnlyTheNewestTwoSnapshotsAreKept) {
  add_segments({1, 101, 201, 301, 401});
  add_snapshots({50, 150, 250, 350});
  const auto report = compact(fs_, kStorageDir);
  ASSERT_TRUE(report.ok());
  EXPECT_EQ(report->snapshots_removed, 2U);
  EXPECT_EQ(report->segments_removed, 2U) << "[1,100] and [101,200] are covered by snapshot 250";
  EXPECT_EQ(files(), (std::vector<std::string>{snapshot_file_name(250), snapshot_file_name(350),
                                               segment_file_name(201), segment_file_name(301),
                                               segment_file_name(401)}));
  // And the deletions are durable.
  EXPECT_FALSE(fs_.crash_image(CrashMode::kLoseUnsynced)
                   ->exists(join_path(kStorageDir, segment_file_name(1))));
}

TEST_F(CompactionTest, TheNewestSegmentIsNeverDeleted) {
  add_segments({1, 101});
  add_snapshots({500, 900});  // both snapshots are beyond everything in the log
  const auto report = compact(fs_, kStorageDir);
  ASSERT_TRUE(report.ok());
  EXPECT_EQ(report->segments_removed, 1U);
  EXPECT_TRUE(fs_.exists(join_path(kStorageDir, segment_file_name(101))))
      << "the log thread is still appending to it";
}

TEST_F(CompactionTest, SegmentStraddlingTheSnapshotIsKept) {
  add_segments({1, 101, 201});
  add_snapshots({100, 150});  // older snapshot ends exactly where segment 1 ends
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  EXPECT_FALSE(fs_.exists(join_path(kStorageDir, segment_file_name(1))));
  EXPECT_TRUE(fs_.exists(join_path(kStorageDir, segment_file_name(101))));

  add_snapshots({199});  // now retained: 150 and 199; [101,200] holds LSN 200 > 150
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  EXPECT_TRUE(fs_.exists(join_path(kStorageDir, segment_file_name(101))));
}

TEST_F(CompactionTest, TempFilesAreRemovedAtStartupOnly) {
  fs_.write_file(join_path(kStorageDir, snapshot_temp_name(5)), "half written");
  add_snapshots({3});
  ASSERT_TRUE(compact(fs_, kStorageDir).ok());
  EXPECT_TRUE(fs_.exists(join_path(kStorageDir, snapshot_temp_name(5))));
  set_log_level(LogLevel::kError);
  ASSERT_TRUE(remove_snapshot_temp_files(fs_, kStorageDir).ok());
  set_log_level(LogLevel::kInfo);
  EXPECT_FALSE(fs_.exists(join_path(kStorageDir, snapshot_temp_name(5))));
  EXPECT_TRUE(fs_.exists(join_path(kStorageDir, snapshot_file_name(3))));
}

}  // namespace
}  // namespace baton
