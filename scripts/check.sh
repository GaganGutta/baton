#!/usr/bin/env bash
# Runs locally what CI runs: format check, then build + test under each preset.
#
#   scripts/check.sh                 # format, asan, tsan, tidy, python
#   scripts/check.sh asan            # just one preset
#   scripts/check.sh format asan     # any combination, in order
#
# The `python` step runs the suites that talk to the real binary (tests/integration
# with redis-cli and redis-py, sdk/python/tests) against a release build. They
# are the only tests that see the wire protocol the way clients do, so a protocol
# change is not checked until they have run. It needs pytest and redis in
# $PYTHON (default python3), and redis-cli.
set -euo pipefail
cd "$(dirname "$0")/.."

steps=("$@")
if [ "${#steps[@]}" -eq 0 ]; then
  steps=(format asan tsan tidy python)
fi

jobs="${BATON_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

for step in "${steps[@]}"; do
  echo "=== ${step} ==="
  case "$step" in
    format)
      scripts/format.sh --check
      ;;
    debug | release | asan | tsan | tidy)
      cmake --preset "$step" >/dev/null
      cmake --build --preset "$step" -j "$jobs"
      ctest --preset "$step" -j "$jobs"
      ;;
    python)
      cmake --preset release >/dev/null
      cmake --build --preset release -j "$jobs" --target baton
      export BATON_BIN="$PWD/build/release/src/server/baton"
      "${PYTHON:-python3}" -m pytest tests/integration -q
      "${PYTHON:-python3}" -m pytest sdk/python/tests -q
      ;;
    fuzz)
      cmake --preset fuzz >/dev/null
      cmake --build --preset fuzz -j "$jobs"
      scripts/fuzz.sh "${BATON_FUZZ_SECONDS:-30}"
      ;;
    *)
      echo "check.sh: unknown step '$step'" >&2
      exit 2
      ;;
  esac
done
echo "=== all checks passed: ${steps[*]} ==="
