// Recovery rules (docs/design.md section 4.4): repair a torn tail, refuse
// everything else. Segments are built by hand in SimFs so every case is exact.

#include "log/recovery.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/codec.h"
#include "common/crc32c.h"
#include "log/format.h"
#include "testing/sim_fs.h"

namespace baton {
namespace {

constexpr const char* kDir = "data";

struct Seen {
  Lsn lsn;
  uint8_t type;
  std::string payload;
  bool operator==(const Seen&) const = default;
};

std::string payload_for(Lsn lsn) { return "payload-" + std::to_string(lsn); }

// A segment holding records first..last (inclusive); empty if last < first.
std::string make_segment(Lsn first, Lsn last) {
  std::string data;
  append_segment_header(data, first);
  for (Lsn lsn = first; lsn <= last; ++lsn) {
    append_record(data, lsn, static_cast<uint8_t>(lsn % 7), payload_for(lsn));
  }
  return data;
}

class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.create_dir_if_missing(kDir).ok()); }

  static std::string path_for(Lsn first) { return join_path(kDir, segment_file_name(first)); }
  void put_segment(Lsn first, Lsn last) {
    fs_.write_file(path_for(first), make_segment(first, last));
  }

  Result<RecoveredLog> recover(Lsn replay_after = 0) {
    seen_.clear();
    return recover_log(fs_, kDir, LogRecoveryOptions{.replay_after = replay_after},
                       [this](const LogRecordView& r) -> Status {
                         seen_.push_back(Seen{r.lsn, r.type, std::string(r.payload)});
                         return {};
                       });
  }

  void expect_seen(Lsn first, Lsn last) const {
    std::vector<Seen> expected;
    for (Lsn lsn = first; lsn <= last; ++lsn) {
      expected.push_back(Seen{lsn, static_cast<uint8_t>(lsn % 7), payload_for(lsn)});
    }
    EXPECT_EQ(seen_, expected);
  }

  static void expect_refused(const Result<RecoveredLog>& result, const std::string& needle) {
    ASSERT_FALSE(result.ok()) << "recovery should have refused to start";
    EXPECT_EQ(result.error().code(), ErrorCode::kCorruption);
    EXPECT_NE(result.error().message().find(needle), std::string::npos) << result.error().message();
  }

  SimFs fs_;
  std::vector<Seen> seen_;
};

// --- the happy paths --------------------------------------------------------------

TEST_F(RecoveryTest, EmptyDirectoryIsAnEmptyLog) {
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok());
  EXPECT_EQ(recovered->last_lsn, 0U);
  EXPECT_TRUE(recovered->segments.empty());
  EXPECT_TRUE(seen_.empty());
}

TEST_F(RecoveryTest, ReplaysOneSegmentInOrder) {
  put_segment(1, 5);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 5U);
  EXPECT_EQ(recovered->records_replayed, 5U);
  EXPECT_EQ(recovered->torn_bytes_truncated, 0U);
  ASSERT_EQ(recovered->segments.size(), 1U);
  EXPECT_EQ(recovered->segments[0].first_lsn, 1U);
  EXPECT_EQ(recovered->segments[0].last_lsn, 5U);
  expect_seen(1, 5);
}

TEST_F(RecoveryTest, ReplaysAcrossSegments) {
  put_segment(1, 3);
  put_segment(4, 4);
  put_segment(5, 9);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 9U);
  EXPECT_EQ(recovered->segments.size(), 3U);
  expect_seen(1, 9);
}

TEST_F(RecoveryTest, EmptyActiveSegmentIsFine) {
  put_segment(1, 3);
  put_segment(4, 3);  // rolled, nothing appended yet
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 3U);
  ASSERT_EQ(recovered->segments.size(), 2U);
  EXPECT_EQ(recovered->segments[1].first_lsn, 4U);
  EXPECT_EQ(recovered->segments[1].last_lsn, 3U);
}

TEST_F(RecoveryTest, ReplayAfterSkipsRecordsCoveredByASnapshot) {
  put_segment(1, 4);
  put_segment(5, 8);
  const auto recovered = recover(/*replay_after=*/6);
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 8U);
  EXPECT_EQ(recovered->records_replayed, 2U);
  expect_seen(7, 8);
}

TEST_F(RecoveryTest, LogMayStartAfterLsnOneIfTheSnapshotCoversTheGap) {
  put_segment(50, 60);
  ASSERT_TRUE(recover(/*replay_after=*/49).ok());
  expect_seen(50, 60);
  ASSERT_TRUE(recover(/*replay_after=*/55).ok());
  expect_seen(56, 60);
}

TEST_F(RecoveryTest, EmptyLogAfterSnapshotContinuesFromTheSnapshot) {
  const auto recovered = recover(/*replay_after=*/100);
  ASSERT_TRUE(recovered.ok());
  EXPECT_EQ(recovered->last_lsn, 100U);
}

TEST_F(RecoveryTest, IgnoresFilesThatAreNotSegments) {
  put_segment(1, 2);
  fs_.write_file(join_path(kDir, "LOCK"), "");
  fs_.write_file(join_path(kDir, "snap-00000000000000000001.snap"), "not a segment");
  fs_.write_file(join_path(kDir, "wal-1.log"), "misnamed");
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 2U);
}

TEST_F(RecoveryTest, CallbackErrorAbortsRecovery) {
  put_segment(1, 5);
  int calls = 0;
  const auto recovered = recover_log(fs_, kDir, {}, [&calls](const LogRecordView& r) -> Status {
    ++calls;
    if (r.lsn == 3) return Error{ErrorCode::kCorruption, "cannot decode record 3"};
    return {};
  });
  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().message(), "cannot decode record 3");
  EXPECT_EQ(calls, 3);
}

// --- torn tails are repaired --------------------------------------------------------

TEST_F(RecoveryTest, TruncationAtEveryByteOfTheFinalRecordIsRepaired) {
  const std::string full = make_segment(1, 4);
  const size_t last_record_start = make_segment(1, 3).size();

  for (size_t cut = last_record_start + 1; cut < full.size(); ++cut) {
    SCOPED_TRACE("cut at byte " + std::to_string(cut));
    fs_.write_file(path_for(1), full.substr(0, cut));

    const auto recovered = recover();
    ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
    EXPECT_EQ(recovered->last_lsn, 3U);
    EXPECT_EQ(recovered->torn_bytes_truncated, cut - last_record_start);
    EXPECT_EQ(recovered->segments.back().size, last_record_start);
    expect_seen(1, 3);

    // The repair is durable and idempotent: the file was cut back, and a second
    // recovery finds nothing to fix.
    EXPECT_EQ(fs_.file_size(path_for(1)).value(), last_record_start);
    const auto again = recover();
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->torn_bytes_truncated, 0U);
    EXPECT_EQ(again->last_lsn, 3U);
  }
}

TEST_F(RecoveryTest, GarbageAfterTheLastRecordIsATornTail) {
  std::string data = make_segment(1, 3);
  const size_t good = data.size();
  data += std::string(100, '\xA5');
  fs_.write_file(path_for(1), data);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 3U);
  EXPECT_EQ(recovered->torn_bytes_truncated, 100U);
  EXPECT_EQ(fs_.file_size(path_for(1)).value(), good);
}

TEST_F(RecoveryTest, ZeroFilledTailIsATornTail) {
  std::string data = make_segment(1, 3);
  data += std::string(4096, '\0');
  fs_.write_file(path_for(1), data);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 3U);
  EXPECT_EQ(recovered->torn_bytes_truncated, 4096U);
}

// A damaged final record cannot be told apart from a torn write, so it is
// dropped. This is the documented limit of what a checksum can decide.
TEST_F(RecoveryTest, DamageConfinedToTheFinalRecordIsTreatedAsTorn) {
  put_segment(1, 4);
  fs_.flip_bit(path_for(1), make_segment(1, 3).size() + 20, 0);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 3U);
  EXPECT_GT(recovered->torn_bytes_truncated, 0U);
}

TEST_F(RecoveryTest, TornTailInLastOfSeveralSegments) {
  put_segment(1, 3);
  const std::string second = make_segment(4, 6);
  fs_.write_file(path_for(4), second.substr(0, second.size() - 5));
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 5U);
  expect_seen(1, 5);
}

TEST_F(RecoveryTest, TornSegmentCreationIsRemoved) {
  std::string header;
  append_segment_header(header, 4);
  const std::vector<std::string> torn_headers = {
      "",                                     // created, nothing written
      header.substr(0, 1),                    // one byte
      header.substr(0, 31),                   // all but one byte
      std::string(kSegmentHeaderSize, '\0'),  // allocated but never written
      std::string(10, '\x5A'),                // garbage
  };
  for (size_t i = 0; i < torn_headers.size(); ++i) {
    SCOPED_TRACE("torn header variant " + std::to_string(i));
    put_segment(1, 3);
    fs_.write_file(path_for(4), torn_headers[i]);

    const auto recovered = recover();
    ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
    EXPECT_TRUE(recovered->removed_torn_segment);
    EXPECT_EQ(recovered->last_lsn, 3U);
    ASSERT_EQ(recovered->segments.size(), 1U);
    EXPECT_FALSE(fs_.exists(path_for(4)));
    // The removal is durable.
    EXPECT_FALSE(fs_.crash_image(CrashMode::kLoseUnsynced)->exists(path_for(4)));
    expect_seen(1, 3);
  }
}

TEST_F(RecoveryTest, TornCreationOfTheOnlySegment) {
  fs_.write_file(path_for(1), "BATON");
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 0U);
  EXPECT_TRUE(recovered->segments.empty());
  EXPECT_FALSE(fs_.exists(path_for(1)));
}

// --- everything else is refused -----------------------------------------------------

TEST_F(RecoveryTest, BitFlipAnywhereInAMidLogRecordIsRefused) {
  const size_t record_start = make_segment(1, 2).size();
  const size_t record_end = make_segment(1, 3).size();
  const std::string pristine = make_segment(1, 5);

  for (size_t byte = record_start; byte < record_end; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      SCOPED_TRACE("byte " + std::to_string(byte) + " bit " + std::to_string(bit));
      fs_.write_file(path_for(1), pristine);
      fs_.flip_bit(path_for(1), byte, bit);

      const auto recovered = recover();
      expect_refused(recovered, "refusing to drop acknowledged data");
      // Refusing must not modify anything.
      EXPECT_EQ(fs_.file_size(path_for(1)).value(), pristine.size());
    }
  }
}

TEST_F(RecoveryTest, DamageInANonFinalSegmentIsRefusedEvenAtItsTail) {
  put_segment(1, 3);
  put_segment(4, 6);
  fs_.flip_bit(path_for(1), make_segment(1, 3).size() - 1, 7);  // last byte of segment 1
  expect_refused(recover(), "checksum mismatch");
}

TEST_F(RecoveryTest, TruncatedNonFinalSegmentIsRefused) {
  const std::string first = make_segment(1, 3);
  fs_.write_file(path_for(1), first.substr(0, first.size() - 4));
  put_segment(4, 6);
  expect_refused(recover(), "truncated record");
}

// Garbage followed by a valid record: the one case where baton is stricter than
// strictly necessary (see the design doc), on purpose.
TEST_F(RecoveryTest, ValidRecordAfterGarbageIsRefused) {
  std::string data = make_segment(1, 2);
  data += std::string(23, '\x77');
  append_record(data, 3, 1, "orphan");
  fs_.write_file(path_for(1), data);
  expect_refused(recover(), "refusing to drop acknowledged data");
}

// ...but stale bytes that happen to form an old record are not "valid records
// after the damage": their LSN cannot belong there.
TEST_F(RecoveryTest, StaleRecordWithOldLsnAfterGarbageIsStillATornTail) {
  std::string data = make_segment(1, 5);
  data += std::string(9, '\x77');
  append_record(data, 2, 1, "stale");
  fs_.write_file(path_for(1), data);
  const auto recovered = recover();
  ASSERT_TRUE(recovered.ok()) << recovered.error().to_string();
  EXPECT_EQ(recovered->last_lsn, 5U);
}

TEST_F(RecoveryTest, LsnGapInsideASegmentIsRefused) {
  std::string data;
  append_segment_header(data, 1);
  append_record(data, 1, 1, "a");
  append_record(data, 2, 1, "b");
  append_record(data, 4, 1, "d");
  fs_.write_file(path_for(1), data);
  expect_refused(recover(), "expected LSN 3 but found 4");
}

TEST_F(RecoveryTest, DuplicateLsnIsRefused) {
  std::string data;
  append_segment_header(data, 1);
  append_record(data, 1, 1, "a");
  append_record(data, 1, 1, "a again");
  fs_.write_file(path_for(1), data);
  expect_refused(recover(), "expected LSN 2 but found 1");
}

TEST_F(RecoveryTest, MissingMiddleSegmentIsRefused) {
  put_segment(1, 3);
  put_segment(7, 9);
  expect_refused(recover(), "should start at LSN 4");
}

TEST_F(RecoveryTest, OverlappingSegmentsAreRefused) {
  put_segment(1, 5);
  put_segment(4, 9);
  expect_refused(recover(), "should start at LSN 6");
}

TEST_F(RecoveryTest, HeaderLsnMustMatchTheFileName) {
  fs_.write_file(path_for(1), make_segment(2, 4));
  expect_refused(recover(), "header says first LSN 2");
}

TEST_F(RecoveryTest, MissingSegmentsBeforeTheFirstAreRefused) {
  put_segment(50, 60);
  expect_refused(recover(/*replay_after=*/10), "segments are missing");
  expect_refused(recover(), "segments are missing");
}

TEST_F(RecoveryTest, LogShorterThanTheSnapshotIsRefused) {
  put_segment(1, 50);
  expect_refused(recover(/*replay_after=*/100), "lost acknowledged records");
}

TEST_F(RecoveryTest, GarbageHeaderInANonFinalSegmentIsRefused) {
  fs_.write_file(path_for(1), std::string(200, '\x11'));
  put_segment(4, 6);
  expect_refused(recover(), "bad magic");
}

TEST_F(RecoveryTest, GarbageHeaderWithValidRecordsBehindItIsRefused) {
  std::string data = make_segment(1, 3);
  data[0] = 'X';  // damaged magic, records intact
  fs_.write_file(path_for(1), data);
  expect_refused(recover(), "bad magic");
  EXPECT_TRUE(fs_.exists(path_for(1))) << "a segment holding records must never be deleted";
}

// An intact final segment written by a newer baton has no records this version
// can read. It must not be mistaken for a torn creation and deleted.
TEST_F(RecoveryTest, SegmentFromANewerFormatIsRefusedAndKept) {
  put_segment(1, 3);
  std::string future;
  ByteWriter w(future);
  w.raw(kSegmentMagic);
  w.u32(kLogFormatVersion + 1);
  w.u32(0);
  w.u64(4);
  w.u32(crc32c(future));
  w.u32(0);
  fs_.write_file(path_for(4), future + "records in a format this version cannot parse");

  const auto recovered = recover();
  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code(), ErrorCode::kFailedPrecondition);
  EXPECT_NE(recovered.error().message().find("unsupported format version"), std::string::npos);
  EXPECT_TRUE(fs_.exists(path_for(4)));
}

}  // namespace
}  // namespace baton
