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
)

rsync -a --exclude build --exclude .git ./ "$work/src/"
cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null || exit 2

build_and_test() {  # $1 = test binary; returns 0 if the tests pass
  cmake --build "$work/build" -j "$jobs" --target "$(basename "$1")" >/dev/null 2>&1 || return 2
  "$work/build/$1" --gtest_brief=1 >/dev/null 2>&1
}

echo "baseline:"
if build_and_test tests/unit/log_test; then
  echo "  ok: tests pass on the unmodified tree"
else
  echo "  ERROR: tests do not pass on the unmodified tree"
  exit 2
fi

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
