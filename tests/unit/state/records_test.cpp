#include "state/records.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace baton {
namespace {

// One fully populated example of every record type, with awkward values:
// negative priority, binary payload, extreme timestamps.
std::vector<Record> sample_records() {
  JobEnqueued enqueued;
  enqueued.id = 42;
  enqueued.queue = "emails.high";
  enqueued.payload = std::string_view("bin\0ary\xFF", 8);
  enqueued.priority = -7;
  enqueued.run_at = WallTime{1'700'000'123'456};
  enqueued.max_attempts = 25;
  enqueued.backoff_base_ms = 1'000;
  enqueued.backoff_cap_ms = 600'000;
  enqueued.idem_key = "order-991";
  enqueued.idem_expires_at = WallTime{1'700'086'523'456};
  enqueued.at = WallTime{1'700'000'000'000};

  AttemptFailed failed;
  failed.id = 42;
  failed.token = 9;
  failed.reason = FailureReason::kLeaseExpired;
  failed.error = "lease expired";
  failed.at = WallTime{5};
  failed.dead = false;
  failed.retry_at = WallTime{kMaxWallTimeMs};

  AttemptFailed dead = failed;
  dead.reason = FailureReason::kWorkerFailed;
  dead.error = "";
  dead.dead = true;
  dead.retry_at = WallTime{};

  return {
      enqueued,
      JobLeased{.id = 42, .token = 9, .lease_expires_at = WallTime{77}, .at = WallTime{70}},
      LeaseExtended{.id = 42, .token = 9, .lease_expires_at = WallTime{99}, .at = WallTime{80}},
      JobSucceeded{.id = 42, .token = 9, .at = WallTime{0}},
      failed,
      dead,
      JobCancelled{.id = std::numeric_limits<JobId>::max(), .at = WallTime{1}},
      DeadJobRetried{.id = 3, .run_at = WallTime{10}, .at = WallTime{9}},
      JobsPurged{.ids = {}},
      JobsPurged{.ids = {1, 2, 300, std::numeric_limits<JobId>::max()}},
  };
}

// Records hold views, so compare through a second encoding.
std::string encoded(const Record& record) {
  std::string out;
  encode_record(record, out);
  return out;
}

TEST(RecordsTest, TypeValuesAreStable) {
  // These numbers are on disk. Changing one breaks every existing log.
  EXPECT_EQ(static_cast<int>(RecordType::kJobEnqueued), 1);
  EXPECT_EQ(static_cast<int>(RecordType::kJobLeased), 2);
  EXPECT_EQ(static_cast<int>(RecordType::kLeaseExtended), 3);
  EXPECT_EQ(static_cast<int>(RecordType::kJobSucceeded), 4);
  EXPECT_EQ(static_cast<int>(RecordType::kAttemptFailed), 5);
  EXPECT_EQ(static_cast<int>(RecordType::kJobCancelled), 6);
  EXPECT_EQ(static_cast<int>(RecordType::kDeadJobRetried), 7);
  EXPECT_EQ(static_cast<int>(RecordType::kJobsPurged), 8);

  EXPECT_EQ(type_of(Record{JobEnqueued{}}), RecordType::kJobEnqueued);
  EXPECT_EQ(type_of(Record{AttemptFailed{}}), RecordType::kAttemptFailed);
  EXPECT_EQ(type_of(Record{JobsPurged{}}), RecordType::kJobsPurged);
}

TEST(RecordsTest, EveryTypeRoundTrips) {
  for (const Record& original : sample_records()) {
    const auto type = static_cast<uint8_t>(type_of(original));
    const std::string bytes = encoded(original);
    const auto decoded = decode_record(type, bytes);
    ASSERT_TRUE(decoded.ok()) << "type " << int{type} << ": " << decoded.error().to_string();
    EXPECT_EQ(decoded->index(), original.index());
    EXPECT_EQ(encoded(*decoded), bytes) << "type " << int{type};
  }
}

TEST(RecordsTest, DecodedFieldsMatch) {
  const Record original = sample_records().front();
  const std::string bytes = encoded(original);
  const auto decoded = decode_record(1, bytes);
  ASSERT_TRUE(decoded.ok());
  const auto& got = std::get<JobEnqueued>(*decoded);
  const auto& want = std::get<JobEnqueued>(original);
  EXPECT_EQ(got.id, want.id);
  EXPECT_EQ(got.queue, want.queue);
  EXPECT_EQ(got.payload, want.payload);
  EXPECT_EQ(got.priority, want.priority);
  EXPECT_EQ(got.run_at, want.run_at);
  EXPECT_EQ(got.max_attempts, want.max_attempts);
  EXPECT_EQ(got.backoff_base_ms, want.backoff_base_ms);
  EXPECT_EQ(got.backoff_cap_ms, want.backoff_cap_ms);
  EXPECT_EQ(got.idem_key, want.idem_key);
  EXPECT_EQ(got.idem_expires_at, want.idem_expires_at);
  EXPECT_EQ(got.at, want.at);
}

TEST(RecordsTest, EveryTruncationIsRejected) {
  for (const Record& original : sample_records()) {
    const auto type = static_cast<uint8_t>(type_of(original));
    const std::string bytes = encoded(original);
    for (size_t size = 0; size < bytes.size(); ++size) {
      const auto decoded = decode_record(type, std::string_view(bytes).substr(0, size));
      ASSERT_FALSE(decoded.ok()) << "type " << int{type} << " truncated to " << size;
      EXPECT_EQ(decoded.error().code(), ErrorCode::kCorruption);
    }
  }
}

TEST(RecordsTest, TrailingBytesAreRejected) {
  for (const Record& original : sample_records()) {
    const auto type = static_cast<uint8_t>(type_of(original));
    const auto decoded = decode_record(type, encoded(original) + '\0');
    ASSERT_FALSE(decoded.ok()) << "type " << int{type};
    EXPECT_NE(decoded.error().message().find("trailing bytes"), std::string::npos);
  }
}

TEST(RecordsTest, UnknownTypeVersionAndEnumsAreRejected) {
  const std::string bytes = encoded(JobCancelled{.id = 1, .at = WallTime{1}});
  EXPECT_FALSE(decode_record(0, bytes).ok());
  EXPECT_FALSE(decode_record(99, bytes).ok());

  std::string wrong_version = bytes;
  wrong_version[0] = 2;
  EXPECT_FALSE(decode_record(static_cast<uint8_t>(RecordType::kJobCancelled), wrong_version).ok());

  AttemptFailed failed;
  failed.id = 1;
  failed.token = 1;
  std::string bad_reason = encoded(failed);
  bad_reason[3] = 7;  // version, id, token, then the reason byte
  EXPECT_FALSE(decode_record(static_cast<uint8_t>(RecordType::kAttemptFailed), bad_reason).ok());
}

TEST(RecordsTest, OutOfRangeNarrowFieldsAreRejected) {
  // Hand-encode a JobEnqueued whose max_attempts does not fit in 32 bits.
  std::string bytes = encoded(sample_records().front());
  JobEnqueued huge = std::get<JobEnqueued>(sample_records().front());
  huge.max_attempts = std::numeric_limits<uint32_t>::max();
  ASSERT_TRUE(decode_record(1, encoded(huge)).ok());
  // The encoder cannot produce the overflow, so flip the varint by hand: the
  // five-byte varint of UINT32_MAX becomes a larger value when its last byte grows.
  std::string overflow = encoded(huge);
  const size_t pos = overflow.find(std::string("\xFF\xFF\xFF\xFF\x0F", 5));
  ASSERT_NE(pos, std::string::npos);
  overflow[pos + 4] = '\x1F';
  const auto decoded = decode_record(1, overflow);
  ASSERT_FALSE(decoded.ok());
  EXPECT_NE(decoded.error().message().find("out of range"), std::string::npos);
}

// Decoded timestamps feed signed arithmetic (deadline - now). Rejecting absurd
// values at the boundary means a damaged record can never cause an overflow.
TEST(RecordsTest, TimestampsOutsideTheSaneRangeAreRejected) {
  const auto type = static_cast<uint8_t>(RecordType::kJobCancelled);
  EXPECT_TRUE(decode_record(type, encoded(JobCancelled{.id = 1, .at = WallTime{0}})).ok());
  EXPECT_TRUE(
      decode_record(type, encoded(JobCancelled{.id = 1, .at = WallTime{kMaxWallTimeMs}})).ok());
  for (const int64_t bad : {int64_t{-1}, kMaxWallTimeMs + 1, std::numeric_limits<int64_t>::max(),
                            std::numeric_limits<int64_t>::min()}) {
    const auto decoded = decode_record(type, encoded(JobCancelled{.id = 1, .at = WallTime{bad}}));
    ASSERT_FALSE(decoded.ok()) << bad;
    EXPECT_NE(decoded.error().message().find("out of range"), std::string::npos);
  }
}

TEST(RecordsTest, HostilePurgeCountDoesNotAllocate) {
  std::string bytes;
  bytes.push_back(1);                                               // version
  bytes += std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7F", 9);  // count = 2^63 - 1
  EXPECT_FALSE(decode_record(static_cast<uint8_t>(RecordType::kJobsPurged), bytes).ok());
}

}  // namespace
}  // namespace baton
