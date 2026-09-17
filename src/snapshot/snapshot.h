#pragma once

// Snapshot files: writing, loading and the compaction that follows.
// Design and failure analysis: docs/design.md section 8.
//
//   snapshot-<lsn>.snap = header, then chunks framed exactly like log records
//                         (length | crc32c | type | sequence | payload), the
//                         last of which is an end chunk carrying totals.
//
// Nothing here touches a live State: the writer consumes a StateImage (a private
// copy), so it can run on a background thread without any locking.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/fs.h"
#include "common/result.h"
#include "common/types.h"
#include "state/image.h"
#include "state/state.h"

namespace baton {

inline constexpr std::string_view kSnapshotMagic = "BATONSNP";
inline constexpr uint32_t kSnapshotFormatVersion = 1;
inline constexpr size_t kSnapshotHeaderSize = 40;
inline constexpr size_t kSnapshotJobsPerChunk = 4096;
inline constexpr size_t kSnapshotKeepCount = 2;  // snapshots retained (design.md 8.3)

enum class SnapshotChunk : uint8_t { kMeta = 1, kJobs = 2, kIdem = 3, kEnd = 255 };

std::string snapshot_file_name(Lsn lsn);  // "snapshot-<20 digits>.snap"
std::string snapshot_temp_name(Lsn lsn);  // "snapshot-<20 digits>.tmp"
std::optional<Lsn> parse_snapshot_file_name(std::string_view name);
bool is_snapshot_temp_name(std::string_view name);

struct SnapshotInfo {
  Lsn lsn = 0;
  WallTime created_at;
  uint64_t bytes = 0;
  uint64_t chunks = 0;
  uint64_t jobs = 0;
  uint64_t idem_keys = 0;
};

// Writes `image` as the snapshot covering `lsn`: temp file, fsync, rename,
// fsync of the directory. On return the snapshot is durable and visible to
// recovery; on error nothing visible has changed (a stray .tmp may remain and is
// removed at the next startup).
//
// The caller must not call this before the log has committed `lsn`.
Result<SnapshotInfo> write_snapshot(FileSystem& fs, const std::string& dir, const StateImage& image,
                                    Lsn lsn, WallTime created_at);

struct LoadedSnapshot {
  SnapshotInfo info;
  std::unique_ptr<State> state;  // in replay mode: apply the log tail, then end_replay()
};

// Loads and fully validates one snapshot file: header, every chunk checksum,
// chunk sequence, and the end chunk's totals. Any doubt is an error.
Result<LoadedSnapshot> load_snapshot(FileSystem& fs, const std::string& path, StateOptions options);

// Snapshot LSNs present in `dir`, newest first.
Result<std::vector<Lsn>> list_snapshots(FileSystem& fs, const std::string& dir);

struct CompactionReport {
  uint64_t snapshots_removed = 0;
  uint64_t segments_removed = 0;
};

// Deletes what the retained snapshots make unnecessary: snapshots other than
// the newest kSnapshotKeepCount, and log segments lying entirely at or before
// the OLDEST retained snapshot (so falling back to it still works). With fewer
// than kSnapshotKeepCount snapshots no segment is deleted. The newest segment
// is never deleted. Ends with a directory fsync.
Result<CompactionReport> compact(FileSystem& fs, const std::string& dir);

// Removes temp files left behind by a crash during write_snapshot().
Status remove_snapshot_temp_files(FileSystem& fs, const std::string& dir);

}  // namespace baton
