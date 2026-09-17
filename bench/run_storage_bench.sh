#!/usr/bin/env bash
# Builds the release preset and runs storage_bench on a real file system,
# printing the environment first. docs/benchmarks.md quotes this output.
#
#   bench/run_storage_bench.sh                      # 10k, 100k and 1M jobs
#   bench/run_storage_bench.sh --jobs 10000         # anything storage_bench takes
#   BATON_BENCH_DIR=/mnt/fast bench/run_storage_bench.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release >/dev/null
cmake --build --preset release --target storage_bench >/dev/null

dir="${BATON_BENCH_DIR:-build/release/storage-bench-data}"
mkdir -p "$dir"

echo "date:       $(date -u +%Y-%m-%dT%H:%MZ)"
echo "commit:     $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "kernel:     $(uname -sr)"
if [ -r /proc/cpuinfo ]; then
  echo "cpu:        $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs) ($(nproc) threads)"
else
  echo "cpu:        $(sysctl -n machdep.cpu.brand_string) ($(sysctl -n hw.ncpu) threads)"
fi
echo "filesystem: $(df -PT "$dir" 2>/dev/null | awk 'NR==2 {print $2 " on " $1}' || echo unknown)"
echo "compiler:   $(grep -m1 CMAKE_CXX_COMPILER: build/release/CMakeCache.txt | cut -d= -f2)"
echo

build/release/bench/storage_bench --dir "$dir" "$@"
