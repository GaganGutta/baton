#!/usr/bin/env bash
# Runs locally what CI runs: format check, then build + test under each preset.
#
#   scripts/check.sh                 # format, asan, tsan, tidy
#   scripts/check.sh asan            # just one preset
#   scripts/check.sh format asan     # any combination, in order
set -euo pipefail
cd "$(dirname "$0")/.."

steps=("$@")
if [ "${#steps[@]}" -eq 0 ]; then
  steps=(format asan tsan tidy)
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
