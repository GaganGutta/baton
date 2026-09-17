// Fuzz target: arbitrary bytes as the newest log segment.
//
// Properties checked on every input:
//   - recovery never crashes, over-reads or trips a sanitizer;
//   - records are delivered with contiguous LSNs starting at the segment's first;
//   - if recovery repaired the segment, a second recovery finds nothing left to
//     repair and sees the same log (the repair is idempotent).

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/check.h"
#include "log/format.h"
#include "log/recovery.h"
#include "testing/sim_fs.h"

namespace {

constexpr const char* kDir = "d";

baton::Result<baton::RecoveredLog> recover(baton::SimFs& fs) {
  baton::Lsn expected = 1;
  return baton::recover_log(fs, kDir, {}, [&](const baton::LogRecordView& r) -> baton::Status {
    BATON_CHECK(r.lsn == expected, "delivered LSN {} but expected {}", r.lsn, expected);
    ++expected;
    return {};
  });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string input(reinterpret_cast<const char*>(data), size);

  // The record parser on its own, at the start of the input.
  (void)baton::parse_record(input);
  (void)baton::parse_segment_header(input);

  baton::SimFs fs;
  BATON_CHECK(fs.create_dir_if_missing(kDir).ok());
  fs.write_file(baton::join_path(kDir, baton::segment_file_name(1)), input);

  const auto first = recover(fs);
  if (!first.ok()) return 0;  // refusing to start is a valid outcome

  const auto second = recover(fs);
  BATON_CHECK(second.ok(), "second recovery failed: {}", second.error().to_string());
  BATON_CHECK(second->last_lsn == first->last_lsn);
  BATON_CHECK(second->torn_bytes_truncated == 0);
  BATON_CHECK(!second->removed_torn_segment);
  return 0;
}
