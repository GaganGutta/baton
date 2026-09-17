#pragma once

// On-disk format of the durable log. The authoritative description, with the
// reasoning, is docs/design.md section 4.1.
//
// Segment file  = segment header, then records back to back.
// Segment header (32 bytes): magic "BATONLOG" | u32 version | u32 reserved |
//                            u64 first_lsn | u32 crc32c(bytes 0..23) | u32 reserved
// Record (17 + n bytes):     u32 n | u32 crc32c | u8 type | u64 lsn | payload[n]
//                            where the CRC covers everything except itself.
// All integers are little-endian.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.h"
#include "common/types.h"

namespace baton {

inline constexpr std::string_view kSegmentMagic = "BATONLOG";
inline constexpr uint32_t kLogFormatVersion = 1;
inline constexpr size_t kSegmentHeaderSize = 32;
inline constexpr size_t kRecordHeaderSize = 17;
// Upper bound on a record payload. Anything larger is treated as corruption,
// which bounds what a damaged length field can make the reader do.
inline constexpr size_t kMaxRecordPayload = (size_t{64} << 20U) + (size_t{64} << 10U);

// A decoded record. `payload` points into the buffer that was parsed.
struct LogRecordView {
  Lsn lsn = 0;
  uint8_t type = 0;
  std::string_view payload;
};

// "wal-00000000000000000042.log". Zero-padded so lexical order is LSN order.
std::string segment_file_name(Lsn first_lsn);
std::optional<Lsn> parse_segment_file_name(std::string_view name);

void append_segment_header(std::string& out, Lsn first_lsn);
// Returns the first LSN recorded in the header.
Result<Lsn> parse_segment_header(std::string_view segment);

void append_record(std::string& out, Lsn lsn, uint8_t type, std::string_view payload);

enum class RecordParseStatus : uint8_t {
  kOk,
  kEndOfData,  // zero bytes left: a clean end
  kTruncated,  // the header or the payload runs past the end of the data
  kBadLength,  // the length field exceeds kMaxRecordPayload
  kBadCrc,
};

struct RecordParseResult {
  RecordParseStatus status = RecordParseStatus::kEndOfData;
  LogRecordView record;  // valid when status == kOk
  size_t size = 0;       // total encoded size when status == kOk
};

// Parses the record at the start of `data`.
RecordParseResult parse_record(std::string_view data);

std::string_view to_string(RecordParseStatus status);

}  // namespace baton
