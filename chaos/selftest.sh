#!/usr/bin/env bash
# Proves that the chaos harness can fail.
#
# A harness that has only ever printed "ok" has proven nothing about itself. This
# script plants a bug in a scratch copy of the tree, builds the server from it,
# and runs a short kill round against it. Every round must FAIL, and for the
# right invariant:
#
#   stale tokens     the engine and apply() stop comparing lease tokens, so a
#                    zombie can acknowledge a job that someone else holds now
#                    -> invariant 2 (the rogue's tally and baton-logcheck)
#   reused tokens    the lease-token counter stops advancing. The server's own
#                    recovery checks notice and it refuses to restart
#                    -> invariant 5 (recovers after every kill)
#   forgotten keys   idempotency keys are no longer recorded, so a retried
#                    enqueue creates a second job
#                    -> invariant 4 (one key, one job)
#
#   chaos/selftest.sh            # takes a few minutes: three release builds
set -uo pipefail
cd "$(dirname "$0")/.."

python="${PYTHON:-python3}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="${BATON_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# Replaces one line of a file in the scratch tree; fails if nothing changed.
plant() {  # $1 = file, $2 = sed expression
  cp "$work/src/$1" "$work/before"
  sed -i.bak "$2" "$work/src/$1"
  if cmp -s "$work/src/$1" "$work/before"; then
    echo "  ERROR: '$2' did not change $1 (the source moved on; update this script)"
    return 1
  fi
}

plant_stale_tokens() {
  plant src/state/engine.cpp 's,  if (job->lease_token != token) {,  if (false) {,' &&
    plant src/state/state.cpp 's,  if (job->state != JobState::kLeased || job->lease_token != token) {,  if (job->state != JobState::kLeased) {,'
}
plant_reused_tokens() {
  plant src/state/state.cpp 's,  next_token_ = r.token + 1;,  // planted bug,'
}
plant_forgotten_keys() {
  plant src/state/state.cpp 's,  if (!r.idem_key.empty()) upsert_idem(r.idem_key\, r.id\, r.idem_expires_at);,  // planted bug,'
}

# plant function | what the failing round must print
cases=(
  "plant_stale_tokens|VIOLATED 2 one valid lease"
  "plant_reused_tokens|VIOLATED 5 recovers after every kill"
  "plant_forgotten_keys|VIOLATED 4 one key"
)

failures=0
for case in "${cases[@]}"; do
  IFS='|' read -r planter expected <<<"$case"
  rsync -a --delete --exclude build --exclude .git --exclude 'chaos/out' ./ "$work/src/"
  if ! "$planter"; then
    failures=$((failures + 1))
    continue
  fi
  if ! { cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
           -DBATON_BUILD_TESTS=OFF >/dev/null &&
         cmake --build "$work/build" -j "$jobs" --target baton baton-logcheck >/dev/null 2>&1; }; then
    echo "  ERROR: $planter does not compile"
    failures=$((failures + 1))
    continue
  fi
  BATON_BIN="$work/build/src/server/baton" BATON_LOGCHECK="$work/build/tools/baton-logcheck" \
    "$python" chaos/run.py --seeds 1 --duration 12 --out "$work/out" >"$work/round.log" 2>&1
  if [ "$?" -eq 0 ]; then
    echo "  MISSED: the harness passed a server with $planter"
    failures=$((failures + 1))
  elif ! grep -q "$expected" "$work/round.log"; then
    echo "  WRONG REASON: $planter failed the round, but not with '$expected':"
    grep -E "VIOLATED|Error" "$work/round.log" | head -5 | cut -c1-220 | sed 's/^/    /'
    failures=$((failures + 1))
  else
    echo "  caught: $planter"
    grep "$expected" "$work/round.log" | head -3 | cut -c1-220 | sed 's/^/    /'
  fi
done

if [ "$failures" -ne 0 ]; then
  echo "chaos self-test FAILED: $failures problem(s)"
  exit 1
fi
echo "chaos self-test passed: every planted bug failed its round for the right reason"
