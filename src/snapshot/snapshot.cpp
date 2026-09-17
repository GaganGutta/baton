#include "snapshot/snapshot.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "common/codec.h"
#include "common/crc32c.h"
#include "common/logging.h"
#include "log/format.h"

namespace baton {
namespace {

constexpr std::string_view kPrefix = "snapshot-";
constexpr std::string_view kFinalSuffix = ".snap";
constexpr std::string_view kTempSuffix = ".tmp";
constexpr size_t kLsnDigits = 20;

// Jobs are grouped into chunks by count and by size, so that a chunk never
// comes near the record size limit however large the payloads are.
constexpr size_t kTargetChunkBytes = size_t{4} << 20U;
constexpr size_t kWriteBufferBytes = size_t{1} << 20U;
constexpr size_t kReadBytes = size_t{256} << 10U;

Error corrupt(const std::string& path, std::string_view what) {
  return Error{ErrorCode::kCorruption, std::format("snapshot {}: {}", path, what)};
}

std::optional<Lsn> parse_name(std::string_view name, std::string_view suffix) {
  if (name.size() != kPrefix.size() + kLsnDigits + suffix.size()) return std::nullopt;
  if (!name.starts_with(kPrefix) || !name.ends_with(suffix)) return std::nullopt;
  const std::string_view digits = name.substr(kPrefix.size(), kLsnDigits);
  Lsn lsn = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), lsn);
  if (ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
  return lsn;
}

void append_header(std::string& out, Lsn lsn, WallTime created_at) {
  const size_t start = out.size();
  ByteWriter w(out);
  w.raw(kSnapshotMagic);
  w.u32(kSnapshotFormatVersion);
  w.u32(0);
  w.u64(lsn);
  w.u64(static_cast<uint64_t>(created_at.ms));
  w.u32(crc32c(std::string_view(out).substr(start, 32)));
  w.u32(0);
}

// Appends chunks to a file through a bounded buffer.
class ChunkWriter {
 public:
  explicit ChunkWriter(WritableFile& file) : file_(file) {}

  Status add(SnapshotChunk type, std::string_view payload) {
    append_record(buffer_, ++sequence_, static_cast<uint8_t>(type), payload);
    return buffer_.size() >= kWriteBufferBytes ? flush() : Status{};
  }

  Status flush() {
    BATON_RETURN_IF_ERROR(file_.append(buffer_));
    bytes_ += buffer_.size();
    buffer_.clear();
    return {};
  }

  void add_raw(std::string_view bytes) { buffer_.append(bytes); }
  uint64_t chunks() const { return sequence_; }
  uint64_t bytes() const { return bytes_ + buffer_.size(); }

 private:
  WritableFile& file_;
  std::string buffer_;
  uint64_t sequence_ = 0;
  uint64_t bytes_ = 0;
};

Status write_chunks(ChunkWriter& writer, const StateImage& image) {
  std::string piece;
  encode_image_meta(image, piece);
  BATON_RETURN_IF_ERROR(writer.add(SnapshotChunk::kMeta, piece));

  const std::span<const JobImage> jobs(image.jobs);
  for (size_t start = 0; start < jobs.size();) {
    size_t end = start;
    size_t bytes = 0;
    while (end < jobs.size() && end - start < kSnapshotJobsPerChunk && bytes < kTargetChunkBytes) {
      bytes += jobs[end].payload.size() + 256;
      ++end;
    }
    piece.clear();
    encode_image_jobs(jobs.subspan(start, end - start), piece);
    BATON_RETURN_IF_ERROR(writer.add(SnapshotChunk::kJobs, piece));
    start = end;
  }

  const std::span<const IdemEntry> keys(image.idem);
  for (size_t start = 0; start < keys.size(); start += kSnapshotJobsPerChunk) {
    piece.clear();
    encode_image_idem(keys.subspan(start, std::min(kSnapshotJobsPerChunk, keys.size() - start)),
                      piece);
    BATON_RETURN_IF_ERROR(writer.add(SnapshotChunk::kIdem, piece));
  }

  piece.clear();
  ByteWriter end(piece);
  end.varint(writer.chunks());
  end.varint(image.jobs.size());
  end.varint(image.idem.size());
  BATON_RETURN_IF_ERROR(writer.add(SnapshotChunk::kEnd, piece));
  return writer.flush();
}

// Sequential access to a file with a bounded window held in memory.
class ChunkReader {
 public:
  explicit ChunkReader(ReadableFile& file) : file_(file) {}

  // Makes at least `n` unread bytes available if the file has them.
  Result<bool> ensure(size_t n) {
    while (buffer_.size() - pos_ < n) {
      if (pos_ >= kWriteBufferBytes) {
        buffer_.erase(0, pos_);
        pos_ = 0;
      }
      BATON_ASSIGN_OR_RETURN(const size_t got, file_.read(std::max(kReadBytes, n), buffer_));
      if (got == 0) return false;
    }
    return true;
  }

  std::string_view peek(size_t n) const { return std::string_view(buffer_).substr(pos_, n); }
  void consume(size_t n) { pos_ += n; }

 private:
  ReadableFile& file_;
  std::string buffer_;
  size_t pos_ = 0;
};

}  // namespace

std::string snapshot_file_name(Lsn lsn) {
  return std::format("{}{:020}{}", kPrefix, lsn, kFinalSuffix);
}

std::string snapshot_temp_name(Lsn lsn) {
  return std::format("{}{:020}{}", kPrefix, lsn, kTempSuffix);
}

std::optional<Lsn> parse_snapshot_file_name(std::string_view name) {
  return parse_name(name, kFinalSuffix);
}

bool is_snapshot_temp_name(std::string_view name) {
  return parse_name(name, kTempSuffix).has_value();
}

Result<SnapshotInfo> write_snapshot(FileSystem& fs, const std::string& dir, const StateImage& image,
                                    Lsn lsn, WallTime created_at) {
  const std::string temp_path = join_path(dir, snapshot_temp_name(lsn));
  const std::string final_path = join_path(dir, snapshot_file_name(lsn));
  if (fs.file_size(temp_path).ok()) BATON_RETURN_IF_ERROR(fs.remove(temp_path));  // stale attempt

  BATON_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file,
                         fs.open_append(temp_path, OpenMode::kCreateNew));
  ChunkWriter writer(*file);
  std::string header;
  append_header(header, lsn, created_at);
  writer.add_raw(header);

  Status written = write_chunks(writer, image);
  if (written.ok()) written = file->sync();
  file.reset();
  if (written.ok()) written = fs.rename(temp_path, final_path);
  if (!written.ok()) {
    (void)fs.remove(temp_path);  // best effort; startup removes leftovers anyway
    return written.error();
  }
  // The rename is what makes the snapshot exist; the directory fsync is what
  // makes that fact durable. Nothing may be deleted on its account before this.
  BATON_RETURN_IF_ERROR(fs.sync_dir(dir));

  return SnapshotInfo{.lsn = lsn,
                      .created_at = created_at,
                      .bytes = writer.bytes(),
                      .chunks = writer.chunks(),
                      .jobs = image.jobs.size(),
                      .idem_keys = image.idem.size()};
}

Result<LoadedSnapshot> load_snapshot(FileSystem& fs, const std::string& path,
                                     StateOptions options) {
  BATON_ASSIGN_OR_RETURN(const std::unique_ptr<ReadableFile> file, fs.open_read(path));
  ChunkReader reader(*file);

  // --- header
  BATON_ASSIGN_OR_RETURN(const bool have_header, reader.ensure(kSnapshotHeaderSize));
  if (!have_header) return corrupt(path, "truncated header");
  const std::string_view raw_header = reader.peek(kSnapshotHeaderSize);
  ByteReader header(raw_header);
  if (header.raw(kSnapshotMagic.size()) != kSnapshotMagic) return corrupt(path, "bad magic");
  const uint32_t version = header.u32();
  header.u32();
  LoadedSnapshot loaded;
  loaded.info.lsn = header.u64();
  loaded.info.created_at = WallTime{static_cast<int64_t>(header.u64())};
  if (header.u32() != crc32c(raw_header.substr(0, 32))) return corrupt(path, "bad header checksum");
  if (header.u32() != 0) return corrupt(path, "bad header padding");
  if (version != kSnapshotFormatVersion) {
    return Error{ErrorCode::kFailedPrecondition,
                 std::format("snapshot {}: unsupported format version {}", path, version)};
  }
  const size_t slash = path.find_last_of('/');
  const std::string_view name =
      std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1);
  if (parse_snapshot_file_name(name) != loaded.info.lsn) {
    return corrupt(path, "header LSN does not match the file name");
  }
  reader.consume(kSnapshotHeaderSize);
  loaded.info.bytes = kSnapshotHeaderSize;

  // --- chunks
  StateBuilder builder(options);
  uint64_t expected_sequence = 1;
  for (;;) {
    BATON_ASSIGN_OR_RETURN(const bool have_frame, reader.ensure(kRecordHeaderSize));
    if (!have_frame) return corrupt(path, "truncated: no end chunk");
    uint32_t length = 0;
    std::memcpy(&length, reader.peek(sizeof(length)).data(), sizeof(length));
    if (length > kMaxRecordPayload) return corrupt(path, "implausible chunk length");
    const size_t total = kRecordHeaderSize + length;
    BATON_ASSIGN_OR_RETURN(const bool have_chunk, reader.ensure(total));
    if (!have_chunk) return corrupt(path, "truncated chunk");

    const RecordParseResult parsed = parse_record(reader.peek(total));
    if (parsed.status != RecordParseStatus::kOk) {
      return corrupt(path,
                     std::format("chunk {}: {}", expected_sequence, to_string(parsed.status)));
    }
    if (parsed.record.lsn != expected_sequence) {
      return corrupt(path, std::format("chunk {} is out of sequence", expected_sequence));
    }
    const std::string_view payload = parsed.record.payload;
    const auto type = static_cast<SnapshotChunk>(parsed.record.type);

    if (type == SnapshotChunk::kEnd) {
      ByteReader end(payload);
      const uint64_t chunks = end.varint();
      const uint64_t jobs = end.varint();
      const uint64_t keys = end.varint();
      if (!end.ok() || !end.at_end() || chunks != expected_sequence - 1 ||
          jobs != builder.jobs_added() || keys != builder.idem_added()) {
        return corrupt(path, "end chunk totals do not match the contents");
      }
      reader.consume(total);
      loaded.info.bytes += total;
      BATON_ASSIGN_OR_RETURN(const bool trailing, reader.ensure(1));
      if (trailing) return corrupt(path, "data after the end chunk");
      loaded.info.chunks = expected_sequence;
      loaded.info.jobs = jobs;
      loaded.info.idem_keys = keys;
      BATON_ASSIGN_OR_RETURN(loaded.state, builder.finish());
      return loaded;
    }

    Status added;
    if (type == SnapshotChunk::kMeta) {
      added = builder.add_meta(payload);
    } else if (type == SnapshotChunk::kJobs) {
      added = builder.add_jobs(payload);
    } else if (type == SnapshotChunk::kIdem) {
      added = builder.add_idem(payload);
    } else {
      return corrupt(
          path, std::format("chunk {} has unknown type {}", expected_sequence, parsed.record.type));
    }
    if (!added.ok()) {
      return corrupt(path, std::format("chunk {}: {}", expected_sequence, added.error().message()));
    }
    reader.consume(total);
    loaded.info.bytes += total;
    ++expected_sequence;
  }
}

Result<std::vector<Lsn>> list_snapshots(FileSystem& fs, const std::string& dir) {
  BATON_ASSIGN_OR_RETURN(const std::vector<std::string> names, fs.list_dir(dir));
  std::vector<Lsn> snapshots;
  for (const std::string& name : names) {
    if (const auto lsn = parse_snapshot_file_name(name)) snapshots.push_back(*lsn);
  }
  std::ranges::sort(snapshots, std::greater<>());
  return snapshots;
}

Result<CompactionReport> compact(FileSystem& fs, const std::string& dir) {
  CompactionReport report;
  BATON_ASSIGN_OR_RETURN(const std::vector<Lsn> snapshots, list_snapshots(fs, dir));
  for (size_t i = kSnapshotKeepCount; i < snapshots.size(); ++i) {
    BATON_RETURN_IF_ERROR(fs.remove(join_path(dir, snapshot_file_name(snapshots[i]))));
    ++report.snapshots_removed;
  }

  // Segments are only ever deleted on the strength of the OLDEST retained
  // snapshot, and only when the full complement of snapshots exists, so that
  // falling back from a bad newest snapshot always finds the log it needs.
  if (snapshots.size() >= kSnapshotKeepCount) {
    const Lsn covered = snapshots[kSnapshotKeepCount - 1];
    BATON_ASSIGN_OR_RETURN(const std::vector<std::string> names, fs.list_dir(dir));
    std::vector<Lsn> segments;
    for (const std::string& name : names) {
      if (const auto first = parse_segment_file_name(name)) segments.push_back(*first);
    }
    std::ranges::sort(segments);
    // Segment i holds [first_i, first_{i+1} - 1]. The newest one is still being
    // written to and is never a candidate.
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
      if (segments[i + 1] > covered + 1) break;
      BATON_RETURN_IF_ERROR(fs.remove(join_path(dir, segment_file_name(segments[i]))));
      ++report.segments_removed;
    }
  }

  if (report.snapshots_removed + report.segments_removed > 0) {
    BATON_RETURN_IF_ERROR(fs.sync_dir(dir));
  }
  return report;
}

Status remove_snapshot_temp_files(FileSystem& fs, const std::string& dir) {
  BATON_ASSIGN_OR_RETURN(const std::vector<std::string> names, fs.list_dir(dir));
  bool removed = false;
  for (const std::string& name : names) {
    if (!is_snapshot_temp_name(name)) continue;
    BATON_WARN("snapshot", "removing unfinished snapshot file name={}", name);
    BATON_RETURN_IF_ERROR(fs.remove(join_path(dir, name)));
    removed = true;
  }
  if (removed) BATON_RETURN_IF_ERROR(fs.sync_dir(dir));
  return {};
}

}  // namespace baton
