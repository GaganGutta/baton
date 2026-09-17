#!/usr/bin/env bash
# Formats all C++ sources in place, or with --check fails if anything would change.
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
mapfile -t files < <(find src tests fuzz bench -type f \( -name '*.h' -o -name '*.cpp' \) 2>/dev/null | sort)
if [ "${#files[@]}" -eq 0 ]; then
  echo "format.sh: no sources found" >&2
  exit 1
fi

if [ "${1:-}" = "--check" ]; then
  "$CLANG_FORMAT" --version
  "$CLANG_FORMAT" --dry-run --Werror "${files[@]}"
  echo "format.sh: ${#files[@]} files are clean"
else
  "$CLANG_FORMAT" -i "${files[@]}"
  echo "format.sh: formatted ${#files[@]} files"
fi
