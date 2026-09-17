#pragma once

// Startup scan of the durable log: validates every segment, repairs a torn
// tail, refuses anything that looks like damage to acknowledged data, and
// feeds the surviving records to the caller in LSN order.
//
// The rules and the reasoning are in docs/design.md section 4.4.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/fs.h"
#include "common/result.h"
#include "common/types.h"
#include "log/format.h"

namespace baton {

struct LogRecoveryOptions {
  // Records with LSN <= replay_after are validated but not delivered: a
  // snapshot already covers them. 0 replays everything.
  Lsn replay_after = 0;
};

struct SegmentInfo {
  std::string name;
  Lsn first_lsn = 0;
  Lsn last_lsn = 0;  // first_lsn - 1 if the segment holds no records
  uint64_t size = 0;
};

struct RecoveredLog {
  // LSN of the last valid record; equals replay_after if the log is empty.
  Lsn last_lsn = 0;
  // Surviving segments in LSN order. New records are appended to the last one.
  std::vector<SegmentInfo> segments;
  uint64_t records_replayed = 0;
  uint64_t torn_bytes_truncated = 0;
  bool removed_torn_segment = false;
};

// Called once per record with LSN > replay_after. The view is only valid during
// the call. Returning an error aborts recovery with that error.
using RecordCallback = std::function<Status(const LogRecordView&)>;

Result<RecoveredLog> recover_log(FileSystem& fs, const std::string& dir,
                                 const LogRecoveryOptions& options,
                                 const RecordCallback& on_record);

}  // namespace baton
