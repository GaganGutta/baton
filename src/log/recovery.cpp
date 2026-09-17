#include "log/recovery.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

#include "common/logging.h"

namespace baton {
namespace {

struct SegmentFile {
  std::string name;
  Lsn name_lsn = 0;
};

Error corruption(const std::string& segment, size_t offset, std::string_view what) {
  return Error{ErrorCode::kCorruption,
               std::format("log segment {} at offset {}: {}", segment, offset, what)};
}

// True if a structurally valid record with an LSN of at least `min_lsn` starts
// anywhere in data[from, end). Used to tell a torn tail (nothing valid follows)
// from damage in the middle of the log (something valid follows).
//
// Every byte offset is a candidate, because a corrupted length field means the
// position of the next record is unknown. Candidates are filtered by header
// plausibility before paying for a CRC, so the scan is a linear pass.
bool valid_record_follows(std::string_view data, size_t from, Lsn min_lsn) {
  if (data.size() < kRecordHeaderSize) return false;
  // No record can have an LSN further ahead than the number of minimal records
  // that fit in the remaining bytes.
  const Lsn max_lsn = min_lsn + ((data.size() - std::min(from, data.size())) / kRecordHeaderSize);
  for (size_t offset = from; offset + kRecordHeaderSize <= data.size(); ++offset) {
    uint32_t length = 0;
    std::memcpy(&length, data.data() + offset, sizeof(length));
    if (length > data.size() - offset - kRecordHeaderSize) continue;
    Lsn lsn = 0;
    std::memcpy(&lsn, data.data() + offset + 9, sizeof(lsn));
    if (lsn < min_lsn || lsn > max_lsn) continue;
    if (parse_record(data.substr(offset)).status == RecordParseStatus::kOk) return true;
  }
  return false;
}

Result<std::vector<SegmentFile>> list_segments(FileSystem& fs, const std::string& dir) {
  BATON_ASSIGN_OR_RETURN(const std::vector<std::string> names, fs.list_dir(dir));
  std::vector<SegmentFile> segments;
  for (const std::string& name : names) {
    if (const auto lsn = parse_segment_file_name(name)) {
      segments.push_back(SegmentFile{.name = name, .name_lsn = *lsn});
    }
  }
  std::ranges::sort(segments, {}, &SegmentFile::name_lsn);
  return segments;
}

}  // namespace

Result<RecoveredLog> recover_log(FileSystem& fs, const std::string& dir,
                                 const LogRecoveryOptions& options,
                                 const RecordCallback& on_record) {
  BATON_ASSIGN_OR_RETURN(const std::vector<SegmentFile> files, list_segments(fs, dir));

  RecoveredLog recovered;
  Lsn expected_lsn = 0;  // LSN the next record must carry; 0 until the first segment is read

  // Start at the last segment that begins at or before the first LSN we need.
  // Every segment before it lies entirely within what the snapshot covers: it
  // is garbage that compaction has not removed yet, or has only partly removed
  // (a crash may persist some of a batch of unlinks and not others, so such
  // leftovers can even have gaps between them). They are neither needed nor
  // validated.
  size_t first_needed = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].name_lsn <= options.replay_after + 1) first_needed = i;
  }

  for (size_t i = first_needed; i < files.size(); ++i) {
    const SegmentFile& file = files[i];
    const bool is_last = i + 1 == files.size();
    const std::string path = join_path(dir, file.name);
    BATON_ASSIGN_OR_RETURN(const std::string data, fs.read_file(path));

    // The first LSN this segment must start with, if earlier segments pin it down.
    if (expected_lsn != 0 && file.name_lsn != expected_lsn) {
      return Error{ErrorCode::kCorruption,
                   std::format("log segment {} should start at LSN {}: a segment is missing or "
                               "segments overlap",
                               file.name, expected_lsn)};
    }

    const Result<Lsn> header = parse_segment_header(data);
    if (!header.ok()) {
      // A crash while creating the newest segment leaves a short or garbage
      // header and no records. Anything else is damage.
      const bool torn_creation = is_last && header.error().code() == ErrorCode::kCorruption &&
                                 !valid_record_follows(data, 0, file.name_lsn);
      if (!torn_creation) {
        return Error{header.error().code(),
                     std::format("log segment {}: {}", file.name, header.error().message())};
      }
      BATON_WARN("log", "removing torn segment name={} size={}", file.name, data.size());
      BATON_RETURN_IF_ERROR(fs.remove(path));
      BATON_RETURN_IF_ERROR(fs.sync_dir(dir));
      recovered.removed_torn_segment = true;
      break;
    }
    if (*header != file.name_lsn) {
      return Error{ErrorCode::kCorruption,
                   std::format("log segment {}: header says first LSN {}", file.name, *header)};
    }
    if (expected_lsn == 0) {
      if (file.name_lsn > options.replay_after + 1) {
        return Error{ErrorCode::kCorruption,
                     std::format("log starts at LSN {} but records after LSN {} are needed: "
                                 "segments are missing",
                                 file.name_lsn, options.replay_after)};
      }
      expected_lsn = file.name_lsn;
    }

    SegmentInfo info{
        .name = file.name, .first_lsn = file.name_lsn, .last_lsn = 0, .size = data.size()};
    size_t offset = kSegmentHeaderSize;
    for (;;) {
      const RecordParseResult parsed = parse_record(std::string_view(data).substr(offset));
      if (parsed.status == RecordParseStatus::kEndOfData) break;

      if (parsed.status != RecordParseStatus::kOk) {
        if (!is_last) return corruption(file.name, offset, to_string(parsed.status));
        if (valid_record_follows(data, offset + 1, expected_lsn)) {
          return corruption(file.name, offset,
                            std::format("{} followed by valid records: the log is damaged in "
                                        "the middle, refusing to drop acknowledged data",
                                        to_string(parsed.status)));
        }
        const uint64_t torn = data.size() - offset;
        BATON_WARN("log", "truncating torn tail segment={} offset={} bytes={} reason=\"{}\"",
                   file.name, offset, torn, to_string(parsed.status));
        BATON_RETURN_IF_ERROR(fs.truncate(path, offset));
        recovered.torn_bytes_truncated = torn;
        info.size = offset;
        break;
      }

      if (parsed.record.lsn != expected_lsn) {
        return corruption(
            file.name, offset,
            std::format("expected LSN {} but found {}", expected_lsn, parsed.record.lsn));
      }
      if (parsed.record.lsn > options.replay_after) {
        BATON_RETURN_IF_ERROR(on_record(parsed.record));
        ++recovered.records_replayed;
      }
      ++expected_lsn;
      offset += parsed.size;
    }
    info.last_lsn = expected_lsn - 1;
    recovered.segments.push_back(std::move(info));
  }

  recovered.last_lsn = expected_lsn == 0 ? options.replay_after : expected_lsn - 1;
  if (recovered.last_lsn < options.replay_after) {
    return Error{ErrorCode::kCorruption,
                 std::format("log ends at LSN {} but the snapshot covers LSN {}: the log lost "
                             "acknowledged records",
                             recovered.last_lsn, options.replay_after)};
  }
  return recovered;
}

}  // namespace baton
