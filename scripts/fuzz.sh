#!/usr/bin/env bash
# Runs every libFuzzer target for a bounded time. Any crash, sanitizer report or
# failed BATON_CHECK fails the script and leaves the reproducer in the corpus dir.
#
#   cmake --preset fuzz && cmake --build --preset fuzz
#   scripts/fuzz.sh 60            # seconds per target (default 30)
#   scripts/fuzz.sh 600 fuzz_resp # only targets whose name contains "fuzz_resp"
set -euo pipefail
cd "$(dirname "$0")/.."

seconds="${1:-30}"
filter="${2:-}"
bin_dir="build/fuzz/fuzz"
corpus_root="build/fuzz/corpus"

if [ ! -x "$bin_dir/fuzz_make_seeds" ]; then
  echo "fuzz.sh: build the fuzz preset first (cmake --preset fuzz && cmake --build --preset fuzz)" >&2
  exit 2
fi
"$bin_dir/fuzz_make_seeds" "$corpus_root"

status=0
for target in "$bin_dir"/fuzz_*; do
  name="$(basename "$target")"
  [ "$name" = "fuzz_make_seeds" ] && continue
  [ -x "$target" ] || continue
  case "$name" in *"$filter"*) ;; *) continue ;; esac

  mkdir -p "$corpus_root/$name"
  echo "=== $name (${seconds}s) ==="
  if ! "$target" "$corpus_root/$name" \
      -max_total_time="$seconds" -timeout=20 -rss_limit_mb=4096 \
      -use_value_profile=1 -print_final_stats=1 \
      -artifact_prefix="$corpus_root/$name/" 2>&1 | tail -n 12; then
    echo "fuzz.sh: $name FAILED" >&2
    status=1
  fi
done
exit "$status"
