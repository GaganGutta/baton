#include "snapshot/recovery.h"

#include <format>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "state/records.h"

namespace baton {

Result<StateRecovery> recover_state(FileSystem& fs, const std::string& dir, StateOptions options,
                                    WallTime wall_now, MonoTime mono_now,
                                    DurationMs lease_grace_ms) {
  BATON_RETURN_IF_ERROR(remove_snapshot_temp_files(fs, dir));

  StateRecovery recovery;
  BATON_ASSIGN_OR_RETURN(const std::vector<Lsn> snapshots, list_snapshots(fs, dir));
  for (const Lsn lsn : snapshots) {
    const std::string path = join_path(dir, snapshot_file_name(lsn));
    Result<LoadedSnapshot> loaded = load_snapshot(fs, path, options);
    if (!loaded.ok()) {
      BATON_WARN("snapshot", "skipping unreadable snapshot: {}", loaded.error().to_string());
      ++recovery.snapshots_rejected;
      continue;
    }
    recovery.snapshot = loaded->info;
    recovery.state = std::move(loaded->state);
    break;
  }
  if (!recovery.state) {
    recovery.state = std::make_unique<State>(options);
    recovery.state->begin_replay();
  }

  State& state = *recovery.state;
  const auto replay = [&state](const LogRecordView& view) -> Status {
    auto record = decode_record(view.type, view.payload);
    if (!record.ok()) {
      return Error{ErrorCode::kCorruption,
                   std::format("LSN {}: {}", view.lsn, record.error().message())};
    }
    if (const Status applied = state.apply(*record); !applied.ok()) {
      return Error{ErrorCode::kCorruption,
                   std::format("LSN {} does not fit the state rebuilt so far: {}", view.lsn,
                               applied.error().message())};
    }
    return {};
  };
  BATON_ASSIGN_OR_RETURN(
      recovery.log,
      recover_log(fs, dir, LogRecoveryOptions{.replay_after = recovery.snapshot.lsn}, replay));

  state.set_now(wall_now, mono_now);
  state.end_replay(lease_grace_ms);
  return recovery;
}

}  // namespace baton
