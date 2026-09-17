#pragma once

// Startup recovery of the state machine: newest loadable snapshot, then the log
// after it, through the same State::apply() the live path uses
// (docs/design.md 8.4).

#include <cstdint>
#include <memory>
#include <string>

#include "common/clock.h"
#include "common/fs.h"
#include "common/result.h"
#include "log/recovery.h"
#include "snapshot/snapshot.h"
#include "state/state.h"

namespace baton {

struct StateRecovery {
  std::unique_ptr<State> state;  // ready to use: derived state has been built
  RecoveredLog log;
  SnapshotInfo snapshot;            // snapshot.lsn == 0: recovered from the log alone
  uint64_t snapshots_rejected = 0;  // unreadable snapshots that were skipped
};

// An unreadable snapshot is skipped in favour of an older one (or of the whole
// log); an unreadable or gappy LOG is never skipped: that is an error, exactly
// as in recover_log().
Result<StateRecovery> recover_state(FileSystem& fs, const std::string& dir, StateOptions options,
                                    WallTime wall_now, MonoTime mono_now,
                                    DurationMs lease_grace_ms);

}  // namespace baton
