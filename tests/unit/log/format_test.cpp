#include "log/format.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "common/codec.h"
#include "common/crc32c.h"

namespace baton {
namespace {

TEST(SegmentNameTest, RoundTripsAndSortsLexically) {
  EXPECT_EQ(segment_file_name(1), "wal-00000000000000000001.log");
  EXPECT_EQ(segment_file_name(std::numeric_limits<Lsn>::max()), "wal-18446744073709551615.log");
  EXPECT_EQ(parse_segment_file_name(segment_file_name(42)), 42U);
  EXPECT_EQ(parse_segment_file_name(segment_file_name(std::numeric_limits<Lsn>::max())),
            std::numeric_limits<Lsn>::max());
  EXPECT_LT(segment_file_name(9), segment_file_name(10));
}

TEST(SegmentNameTest, RejectsOtherFiles) {
  for (const char* name : {"", "LOCK", "wal-1.log", "wal-0000000000000000000x.log",
                           "wal-00000000000000000001.tmp", "snap-00000000000000000001.log",
                           "wal-00000000000000000001.log.bak", "wal-99999999999999999999.log"}) {
    EXPECT_EQ(parse_segment_file_name(name), std::nullopt) << name;
  }
}

TEST(SegmentHeaderTest, RoundTrip) {
  std::string header;
  append_segment_header(header, 12345);
  ASSERT_EQ(header.size(), kSegmentHeaderSize);
  EXPECT_EQ(header.substr(0, 8), "BATONLOG");
  EXPECT_EQ(parse_segment_header(header).value(), 12345U);
  // Trailing record bytes do not matter to the header parser.
  EXPECT_EQ(parse_segment_header(header + "records...").value(), 12345U);
}

TEST(SegmentHeaderTest, EveryBitFlipIsDetected) {
  std::string header;
  append_segment_header(header, 7);
  for (size_t byte = 0; byte < 28; ++byte) {  // the final 4 bytes are reserved padding
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::string damaged = header;
      damaged[byte] = static_cast<char>(static_cast<unsigned char>(damaged[byte]) ^ (1U << bit));
      EXPECT_FALSE(parse_segment_header(damaged).ok()) << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(SegmentHeaderTest, TruncatedHeaderIsCorruption) {
  std::string header;
  append_segment_header(header, 7);
  for (size_t size = 0; size < kSegmentHeaderSize; ++size) {
    const auto parsed = parse_segment_header(header.substr(0, size));
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code(), ErrorCode::kCorruption);
  }
}

// A header that is intact but from a newer format must not look like garbage:
// recovery deletes garbage final segments, and must never delete this one.
TEST(SegmentHeaderTest, UnsupportedVersionIsNotCorruption) {
  // A well-formed header, checksum included, the way a future baton would write it.
  std::string future;
  ByteWriter w(future);
  w.raw(kSegmentMagic);
  w.u32(kLogFormatVersion + 1);
  w.u32(0);
  w.u64(7);
  w.u32(crc32c(future));
  w.u32(0);
  const auto parsed = parse_segment_header(future);
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code(), ErrorCode::kFailedPrecondition);
}

TEST(RecordTest, RoundTrip) {
  std::string buffer;
  append_record(buffer, 5, 0x2A, "payload");
  EXPECT_EQ(buffer.size(), kRecordHeaderSize + 7);

  const RecordParseResult parsed = parse_record(buffer);
  ASSERT_EQ(parsed.status, RecordParseStatus::kOk);
  EXPECT_EQ(parsed.record.lsn, 5U);
  EXPECT_EQ(parsed.record.type, 0x2A);
  EXPECT_EQ(parsed.record.payload, "payload");
  EXPECT_EQ(parsed.size, buffer.size());
}

TEST(RecordTest, EmptyAndBinaryPayloads) {
  std::string buffer;
  append_record(buffer, 1, 0, "");
  append_record(buffer, 2, 255, std::string("\0\xFF\0", 3));

  const RecordParseResult first = parse_record(buffer);
  ASSERT_EQ(first.status, RecordParseStatus::kOk);
  EXPECT_TRUE(first.record.payload.empty());

  const RecordParseResult second = parse_record(std::string_view(buffer).substr(first.size));
  ASSERT_EQ(second.status, RecordParseStatus::kOk);
  EXPECT_EQ(second.record.lsn, 2U);
  EXPECT_EQ(second.record.type, 255);
  EXPECT_EQ(second.record.payload, std::string("\0\xFF\0", 3));
}

TEST(RecordTest, EmptyInputIsCleanEnd) {
  EXPECT_EQ(parse_record("").status, RecordParseStatus::kEndOfData);
}

TEST(RecordTest, EveryTruncationIsDetected) {
  std::string buffer;
  append_record(buffer, 9, 1, "some payload bytes");
  for (size_t size = 1; size < buffer.size(); ++size) {
    EXPECT_EQ(parse_record(std::string_view(buffer).substr(0, size)).status,
              RecordParseStatus::kTruncated)
        << "size " << size;
  }
}

TEST(RecordTest, EveryBitFlipIsDetected) {
  std::string buffer;
  append_record(buffer, 9, 1, "some payload bytes");
  for (size_t byte = 0; byte < buffer.size(); ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::string damaged = buffer;
      damaged[byte] = static_cast<char>(static_cast<unsigned char>(damaged[byte]) ^ (1U << bit));
      EXPECT_NE(parse_record(damaged).status, RecordParseStatus::kOk)
          << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(RecordTest, ImplausibleLengthIsRejectedWithoutReading) {
  std::string buffer;
  append_record(buffer, 1, 1, "x");
  buffer[3] = '\x7F';  // length becomes ~2 GiB
  EXPECT_EQ(parse_record(buffer).status, RecordParseStatus::kBadLength);
}

TEST(RecordDeathTest, LsnZeroIsABug) {
  std::string buffer;
  EXPECT_DEATH(append_record(buffer, 0, 1, "x"), "check failed");
}

}  // namespace
}  // namespace baton
