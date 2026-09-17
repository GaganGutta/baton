#!/usr/bin/env bash
# Builds the release preset and runs every benchmark that the README and
# docs/benchmarks.md quote. Results land in bench/out/ (results.json, results.md).
#
#   bench/run_all.sh                     # about ten minutes
#   bench/run_all.sh --quick             # a smoke test of the benchmark suite itself
#   bench/run_all.sh --only enqueue,e2e
#
# The comparison with Beanstalkd and Faktory is separate, because it needs
# Docker: bench/compare/run_compare.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release >/dev/null
cmake --build --preset release --target baton baton-loadgen baton_microbench >/dev/null
exec "${PYTHON:-python3}" bench/run_all.py "$@"
