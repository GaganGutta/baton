// baton-logcheck: an offline, independent check of the lease rules.
//
//   baton-logcheck <data-dir>        prints a JSON report; exit 0 = no violations,
//                                    1 = violations, 2 = the log could not be read
//
// The server's own log is the best witness of what the server treated as valid.
// This tool walks it with LeaseModel (src/check/lease_model.h), a second,
// deliberately tiny implementation of the lease rules that shares no code with
// State::apply.
//
// If compaction has removed the beginning of the log, the walk starts from the
// oldest snapshot that the remaining log connects to. The directory is opened
// read-only: a log that needs repair (a torn tail) is reported, not repaired -
// start the server once, or run this after a clean shutdown.
//
// The chaos harness runs it after every round (docs/design.md 10.2).

#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check/lease_model.h"
#include "common/logging.h"
#include "common/posix_fs.h"
#include "log/format.h"
#include "log/recovery.h"
#include "snapshot/snapshot.h"

namespace baton {
namespace {

// Reads pass through; anything that would change the directory is refused.
class ReadOnlyFs final : public FileSystem {
 public:
  explicit ReadOnlyFs(FileSystem& inner) : inner_(inner) {}

  Status create_dir_if_missing(const std::string& /*dir*/) override { return refused(); }
  Result<std::vector<std::string>> list_dir(const std::string& dir) override {
    return inner_.list_dir(dir);
  }
  Result<std::unique_ptr<WritableFile>> open_append(const std::string& /*path*/,
                                                    OpenMode /*mode*/) override {
    return refused();
  }
  Result<std::string> read_file(const std::string& path) override {
    return inner_.read_file(path);
  }
  Result<std::unique_ptr<ReadableFile>> open_read(const std::string& path) override {
    return inner_.open_read(path);
  }
  Result<uint64_t> file_size(const std::string& path) override { return inner_.file_size(path); }
  Status truncate(const std::string& /*path*/, uint64_t /*size*/) override { return refused(); }
  Status rename(const std::string& /*from*/, const std::string& /*to*/) override {
    return refused();
  }
  Status remove(const std::string& /*path*/) override { return refused(); }
  Status sync_dir(const std::string& /*dir*/) override { return refused(); }
  Result<std::unique_ptr<DirLock>> lock_dir(const std::string& /*dir*/) override {
    return refused();
  }
  Result<uint64_t> available_bytes(const std::string& dir) override {
    return inner_.available_bytes(dir);
  }

 private:
  static Error refused() {
    return Error{ErrorCode::kFailedPrecondition,
                 "the log needs repair and baton-logcheck is read-only: start the server once "
                 "(it repairs a torn tail), stop it, and check again"};
  }

  FileSystem& inner_;
};

std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(c));
    } else {
      out += c;
    }
  }
  return out + "\"";
}

int unreadable(std::string_view message) {
  std::cout << std::format("{{\"ok\": false, \"error\": {}}}\n", json_string(message));
  return 2;
}

int run(const std::string& dir) {
  set_log_level(LogLevel::kError);
  PosixFs posix;
  ReadOnlyFs fs(posix);

  // Where does the remaining log begin?
  const auto names = fs.list_dir(dir);
  if (!names.ok()) return unreadable(names.error().to_string());
  std::optional<Lsn> first_segment;
  for (const std::string& name : *names) {
    if (const auto lsn = parse_segment_file_name(name)) {
      first_segment = std::min(first_segment.value_or(*lsn), *lsn);
    }
  }

  LeaseModel model;
  Lsn start_after = 0;
  if (first_segment.value_or(1) > 1) {
    // Compaction removed the beginning: start from the oldest snapshot that the
    // log still connects to (older means more history to check).
    auto snapshots = list_snapshots(fs, dir);
    if (!snapshots.ok()) return unreadable(snapshots.error().to_string());
    std::ranges::sort(*snapshots);
    bool loaded = false;
    for (const Lsn lsn : *snapshots) {
      if (lsn + 1 < *first_segment) continue;
      auto snapshot = load_snapshot(fs, join_path(dir, snapshot_file_name(lsn)), StateOptions{});
      if (!snapshot.ok()) continue;  // unreadable: the server would skip it too
      model.start_from(snapshot->state->capture_image());
      start_after = lsn;
      loaded = true;
      break;
    }
    if (!loaded) {
      return unreadable(std::format("the log starts at LSN {} and no readable snapshot covers "
                                    "what came before",
                                    *first_segment));
    }
  }

  uint64_t records = 0;
  const auto recovered =
      recover_log(fs, dir, LogRecoveryOptions{.replay_after = start_after},
                  [&](const LogRecordView& view) -> Status {
                    auto record = decode_record(view.type, view.payload);
                    if (!record.ok()) return record.error();
                    ++records;
                    model.on(view.lsn, *record);
                    return {};
                  });
  if (!recovered.ok()) return unreadable(recovered.error().to_string());

  std::string violations;
  for (const std::string& v : model.violations()) {
    violations += (violations.empty() ? "" : ", ") + json_string(v);
  }
  const bool ok = model.violation_count() == 0;
  const LeaseModel::Counters& c = model.counters();
  std::cout << std::format(
      "{{\"ok\": {}, \"started_after_lsn\": {}, \"last_lsn\": {}, \"records\": {}, "
      "\"enqueued\": {}, \"leases_granted\": {}, \"heartbeats\": {}, \"acks\": {}, "
      "\"worker_failures\": {}, \"lease_expiries\": {}, \"cancellations\": {}, "
      "\"max_token\": {}, \"open_leases\": {}, \"violation_count\": {}, \"violations\": [{}]}}\n",
      ok ? "true" : "false", start_after, recovered->last_lsn, records, c.enqueued,
      c.leases_granted, c.heartbeats, c.acks, c.worker_failures, c.lease_expiries, c.cancellations,
      model.max_token(), model.open_leases(), model.violation_count(), violations);
  return ok ? 0 : 1;
}

}  // namespace
}  // namespace baton

int main(int argc, char** argv) {
  try {
    if (argc != 2 || std::string_view(argv[1]).starts_with("-")) {
      std::cerr << "usage: baton-logcheck <data-dir>\n";
      return 2;
    }
    return baton::run(argv[1]);
  } catch (const std::exception& e) {
    std::cerr << "baton-logcheck: " << e.what() << "\n";
    return 2;
  }
}
