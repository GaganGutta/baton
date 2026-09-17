#!/usr/bin/env bash
# Mutation check: proves that the durability tests fail for the right reasons.
#
# Each mutant breaks exactly one durability rule in a scratch copy of the tree.
# The check passes only if the test suite FAILS for every mutant (and passes on
# the unmodified copy). A surviving mutant means a rule is not actually tested.
#
#   scripts/mutation-check.sh            # all mutants
#
# Takes a few minutes; run it after touching src/log or its tests.
set -uo pipefail
cd "$(dirname "$0")/.."

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="${BATON_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# name | file | sed expression | test binary (relative to the build dir)
mutants=(
  "segment created without directory fsync|src/log/log_writer.cpp|s,  BATON_RETURN_IF_ERROR(fs_.sync_dir(options_.dir));,  // mutant,|tests/unit/log_test"
  "commit announced before fsync|src/log/log_writer.cpp|s,  if (options_.fsync_policy == FsyncPolicy::kAlways) sync_active();,  // mutant,|tests/unit/log_test"
  "segment rolled before the old one is durable|src/log/log_writer.cpp|s,    if (dirty_) sync_active();,    // mutant,|tests/unit/log_test"
  "torn tail not truncated|src/log/recovery.cpp|s,        BATON_RETURN_IF_ERROR(fs.truncate(path\, offset));,        // mutant,|tests/unit/log_test"
  "mid-log damage treated as a torn tail|src/log/recovery.cpp|s,        if (valid_record_follows(data\, offset + 1\, expected_lsn)) {,        if (false) {,|tests/unit/log_test"
  "record checksum not verified|src/log/format.cpp|s,  if (load_u32(encoded\, 4) != record_crc(encoded.substr(0\, 4)\, encoded.substr(8))) {,  if (false) {,|tests/unit/log_test"
  "engine accepts a stale lease token|src/state/engine.cpp|s,  if (job->lease_token != token) {,  if (false) {,|tests/unit/state_test"
  "apply reads the clock instead of the record|src/state/state.cpp|s,  finish(\*job\, JobState::kSucceeded\, r.at);,  finish(*job\, JobState::kSucceeded\, wall_now_);,|tests/unit/state_test"
  "lease token counter not advanced|src/state/state.cpp|s,  next_token_ = r.token + 1;,  // mutant,|tests/unit/state_test"
  "attempts never counted|src/state/state.cpp|s,  ++job->attempts;,  // mutant,|tests/unit/state_test"
  "ready order ignores priority|src/state/ready_heap.cpp|s,  if (a.priority != b.priority) return a.priority > b.priority;,  // mutant,|tests/unit/state_test"
  "idempotency window never expires|src/state/engine.cpp|s,    if (entry != nullptr && entry->expires_at > now) {,    if (entry != nullptr) {,|tests/unit/state_test"
  "idempotency key not recorded|src/state/state.cpp|s,  if (!r.idem_key.empty()) upsert_idem(r.idem_key\, r.id\, r.idem_expires_at);,  // mutant,|tests/unit/state_test"
  "replies sent before their records are durable|src/server/server.cpp|s,  if (c.marks.empty() && lsn <= committed_lsn_) {,  if (true) {,|tests/unit/server_test"
  "parked RESERVEs served newest first|src/server/server.cpp|s,      Connection\* c = find(it->second.front());,      Connection* c = find(it->second.back());,|tests/unit/server_test"
  "commands allowed without AUTH|src/server/server.cpp|s,    } else if (command->needs_auth && !c.authenticated) {,    } else if (false) {,|tests/unit/server_test"
  "restart expires leases without a grace period|src/server/server.cpp|s,  state_->end_replay(config_.engine.lease_grace_ms);,  state_->end_replay(0);,|tests/unit/server_test"
  "wall-clock jumps go unnoticed|src/state/engine.cpp|s,  if (std::abs(change) <= options_.clock_jump_threshold_ms) return;,  if (true) return;,|tests/unit/state_test"
  "clock jump expires leases without a grace period|src/state/engine.cpp|s,  state_.rebuild_derived(options_.lease_grace_ms);,  state_.rebuild_derived(0);,|tests/unit/state_test"
  "DLQ retry keeps the spent attempts|src/state/state.cpp|s,  job->attempts = 0;,  // mutant,|tests/unit/state_test"
)

rsync -a --exclude build --exclude .git ./ "$work/src/"
cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null || exit 2

build_and_test() {  # $1 = test binary; returns 0 if the tests pass
  cmake --build "$work/build" -j "$jobs" --target "$(basename "$1")" >/dev/null 2>&1 || return 2
  # A mutant may hang a test instead of failing it; a timeout counts as killed.
  if command -v timeout >/dev/null; then
    timeout 900 "$work/build/$1" --gtest_brief=1 >/dev/null 2>&1
  else
    "$work/build/$1" --gtest_brief=1 >/dev/null 2>&1
  fi
}

echo "baseline:"
for binary in tests/unit/log_test tests/unit/state_test tests/unit/server_test; do
  if build_and_test "$binary"; then
    echo "  ok: $(basename "$binary") passes on the unmodified tree"
  else
    echo "  ERROR: $(basename "$binary") does not pass on the unmodified tree"
    exit 2
  fi
done

survivors=0
for mutant in "${mutants[@]}"; do
  IFS='|' read -r name file expr binary <<<"$mutant"
  cp "$work/src/$file" "$work/original"
  sed -i.bak "$expr" "$work/src/$file"
  if cmp -s "$work/src/$file" "$work/original"; then
    echo "  ERROR: mutant '$name' did not change $file (the source moved on; update this script)"
    survivors=$((survivors + 1))
  else
    build_and_test "$binary"
    case $? in
      0) echo "  SURVIVED: $name"; survivors=$((survivors + 1)) ;;
      2) echo "  ERROR: mutant '$name' does not compile"; survivors=$((survivors + 1)) ;;
      *) echo "  killed:   $name" ;;
    esac
  fi
  cp "$work/original" "$work/src/$file"
done

if [ "$survivors" -ne 0 ]; then
  echo "mutation check FAILED: $survivors problem(s)"
  exit 1
fi
echo "mutation check passed: every mutant was killed"
