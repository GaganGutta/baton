// Fuzz target: load_snapshot(), the first file recovery reads.
//
// The first input byte picks a mode:
//   even  the rest is the snapshot file, byte for byte. Checksums stop most
//         mutations early, which is exactly what this mode checks.
//   odd   the rest is a list of chunks, [type u8][length u16][payload]. The
//         harness frames them with valid checksums and sequence numbers behind
//         a valid header, so mutations reach the chunk-order rules, the totals
//         in the end chunk and the StateBuilder behind them.
//
// Properties: arbitrary input is either rejected or produces a State whose
// invariants hold, and which - written out as a snapshot again - loads back to
// the identical state. Never a crash, never an oversized allocation.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "common/check.h"
#include "common/codec.h"
#include "common/crc32c.h"
#include "common/logging.h"
#include "log/format.h"
#include "snapshot/snapshot.h"
#include "testing/sim_fs.h"

namespace {

constexpr baton::Lsn kFramedLsn = 7;

std::string framed_file(std::string_view chunks) {
  std::string file;
  baton::ByteWriter w(file);
  w.raw(baton::kSnapshotMagic);
  w.u32(baton::kSnapshotFormatVersion);
  w.u32(0);
  w.u64(kFramedLsn);
  w.u64(1'700'000'000'000);
  w.u32(baton::crc32c(std::string_view(file).substr(0, 32)));
  w.u32(0);

  uint64_t sequence = 0;
  while (chunks.size() >= 3) {
    const auto type = static_cast<uint8_t>(chunks[0]);
    uint16_t length = 0;
    std::memcpy(&length, chunks.data() + 1, sizeof(length));
    chunks.remove_prefix(3);
    const std::string_view payload = chunks.substr(0, length);
    chunks.remove_prefix(payload.size());
    baton::append_record(file, ++sequence, type, payload);
  }
  return file;
}

// The name load_snapshot() insists on: the LSN in the header.
std::string name_for(std::string_view file) {
  baton::Lsn lsn = 0;
  if (file.size() >= 24) std::memcpy(&lsn, file.data() + 16, sizeof(lsn));
  return baton::snapshot_file_name(lsn);
}

std::string serialized(baton::State& state) {
  std::string bytes;
  state.serialize(bytes);
  return bytes;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const bool quiet = [] {
    baton::set_log_level(baton::LogLevel::kOff);
    return true;
  }();
  (void)quiet;
  if (size == 0) return 0;
  const std::string_view body(reinterpret_cast<const char*>(data) + 1, size - 1);
  const std::string file = (data[0] & 1U) != 0 ? framed_file(body) : std::string(body);

  baton::SimFs fs;
  BATON_CHECK(fs.create_dir_if_missing("d").ok(), "mkdir");
  const std::string path = baton::join_path("d", name_for(file));
  fs.write_file(path, file);

  auto loaded = baton::load_snapshot(fs, path, {});
  if (!loaded.ok()) return 0;
  BATON_CHECK(loaded->info.bytes == file.size(), "accepted a file without reading all of it");

  baton::State& state = *loaded->state;
  baton::Status invariants = state.check_invariants();
  BATON_CHECK(invariants.ok(), "after load: {}", invariants.error().to_string());
  state.set_now(baton::WallTime{1'700'000'000'000}, baton::MonoTime{1'000'000});
  state.end_replay(/*lease_grace_ms=*/1'000);
  invariants = state.check_invariants();
  BATON_CHECK(invariants.ok(), "after rebuild: {}", invariants.error().to_string());

  // What was loaded can be snapshotted again, and nothing changes on the way.
  const baton::Lsn again_lsn = loaded->info.lsn + 1;
  const auto written =
      baton::write_snapshot(fs, "d", state.capture_image(), again_lsn, baton::WallTime{1});
  BATON_CHECK(written.ok(), "re-snapshot failed: {}", written.error().to_string());
  auto again =
      baton::load_snapshot(fs, baton::join_path("d", baton::snapshot_file_name(again_lsn)), {});
  BATON_CHECK(again.ok(), "own snapshot does not load: {}", again.error().to_string());
  again->state->set_now(baton::WallTime{1'700'000'000'000}, baton::MonoTime{1'000'000});
  again->state->end_replay(/*lease_grace_ms=*/1'000);
  BATON_CHECK(serialized(*again->state) == serialized(state), "snapshot round trip changed state");
  return 0;
}
