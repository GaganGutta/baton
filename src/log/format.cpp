#include "log/format.h"

#include <charconv>
#include <cstring>
#include <format>

#include "common/codec.h"
#include "common/crc32c.h"

namespace baton {
namespace {

constexpr std::string_view kSegmentPrefix = "wal-";
constexpr std::string_view kSegmentSuffix = ".log";
constexpr size_t kLsnDigits = 20;  // enough for any uint64_t

uint32_t load_u32(std::string_view data, size_t offset) {
  uint32_t v = 0;
  std::memcpy(&v, data.data() + offset, sizeof(v));
  return v;
}

// The record CRC covers the length field and everything after the CRC field.
uint32_t record_crc(std::string_view length_field, std::string_view after_crc) {
  return crc32c_extend(crc32c(length_field), after_crc);
}

}  // namespace

std::string segment_file_name(Lsn first_lsn) {
  return std::format("{}{:020}{}", kSegmentPrefix, first_lsn, kSegmentSuffix);
}

std::optional<Lsn> parse_segment_file_name(std::string_view name) {
  if (name.size() != kSegmentPrefix.size() + kLsnDigits + kSegmentSuffix.size())
    return std::nullopt;
  if (!name.starts_with(kSegmentPrefix) || !name.ends_with(kSegmentSuffix)) return std::nullopt;
  const std::string_view digits = name.substr(kSegmentPrefix.size(), kLsnDigits);
  Lsn lsn = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), lsn);
  if (ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
  return lsn;
}

void append_segment_header(std::string& out, Lsn first_lsn) {
  const size_t start = out.size();
  ByteWriter w(out);
  w.raw(kSegmentMagic);
  w.u32(kLogFormatVersion);
  w.u32(0);
  w.u64(first_lsn);
  w.u32(crc32c(std::string_view(out).substr(start, 24)));
  w.u32(0);
}

Result<Lsn> parse_segment_header(std::string_view segment) {
  if (segment.size() < kSegmentHeaderSize) {
    return Error{ErrorCode::kCorruption, std::format("segment header truncated: {} of {} bytes",
                                                     segment.size(), kSegmentHeaderSize)};
  }
  ByteReader r(segment.substr(0, kSegmentHeaderSize));
  if (r.raw(kSegmentMagic.size()) != kSegmentMagic) {
    return Error{ErrorCode::kCorruption, "segment header: bad magic"};
  }
  const uint32_t version = r.u32();
  r.u32();  // reserved
  const Lsn first_lsn = r.u64();
  const uint32_t stored_crc = r.u32();
  if (stored_crc != crc32c(segment.substr(0, 24))) {
    return Error{ErrorCode::kCorruption, "segment header: bad checksum"};
  }
  // Checked after the CRC so that a damaged version field reads as corruption,
  // not as "written by a newer baton". The error code differs on purpose:
  // recovery may delete a final segment whose header is garbage (a torn
  // creation), but must never delete an intact segment it merely cannot read.
  if (version != kLogFormatVersion) {
    return Error{ErrorCode::kFailedPrecondition,
                 std::format("segment header: unsupported format version {}", version)};
  }
  if (first_lsn == 0) return Error{ErrorCode::kCorruption, "segment header: first LSN is 0"};
  return first_lsn;
}

void append_record(std::string& out, Lsn lsn, uint8_t type, std::string_view payload) {
  BATON_CHECK(payload.size() <= kMaxRecordPayload,
              "record payload of {} bytes exceeds the format limit", payload.size());
  BATON_CHECK(lsn != 0);
  const size_t start = out.size();
  ByteWriter w(out);
  w.u32(static_cast<uint32_t>(payload.size()));
  w.u32(0);  // CRC placeholder
  w.u8(type);
  w.u64(lsn);
  w.raw(payload);

  const std::string_view encoded = std::string_view(out).substr(start);
  const uint32_t crc = record_crc(encoded.substr(0, 4), encoded.substr(8));
  std::memcpy(out.data() + start + 4, &crc, sizeof(crc));
}

RecordParseResult parse_record(std::string_view data) {
  RecordParseResult result;
  if (data.empty()) {
    result.status = RecordParseStatus::kEndOfData;
    return result;
  }
  if (data.size() < kRecordHeaderSize) {
    result.status = RecordParseStatus::kTruncated;
    return result;
  }
  const uint32_t length = load_u32(data, 0);
  if (length > kMaxRecordPayload) {
    result.status = RecordParseStatus::kBadLength;
    return result;
  }
  const size_t total = kRecordHeaderSize + length;
  if (data.size() < total) {
    result.status = RecordParseStatus::kTruncated;
    return result;
  }
  const std::string_view encoded = data.substr(0, total);
  if (load_u32(encoded, 4) != record_crc(encoded.substr(0, 4), encoded.substr(8))) {
    result.status = RecordParseStatus::kBadCrc;
    return result;
  }

  ByteReader r(encoded.substr(8));
  result.record.type = r.u8();
  result.record.lsn = r.u64();
  result.record.payload = r.raw(length);
  result.size = total;
  result.status = RecordParseStatus::kOk;
  return result;
}

std::string_view to_string(RecordParseStatus status) {
  switch (status) {
    case RecordParseStatus::kOk:
      return "ok";
    case RecordParseStatus::kEndOfData:
      return "end of data";
    case RecordParseStatus::kTruncated:
      return "truncated record";
    case RecordParseStatus::kBadLength:
      return "implausible record length";
    case RecordParseStatus::kBadCrc:
      return "checksum mismatch";
  }
  return "unknown";
}

}  // namespace baton
