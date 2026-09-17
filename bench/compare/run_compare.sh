#!/usr/bin/env bash
# Builds the three images and the load generator, then runs the comparison.
# Needs Docker. Results: bench/out/compare.json and compare.md.
#
#   bench/compare/run_compare.sh [--quick]
set -euo pipefail
cd "$(dirname "$0")/../.."

cmake --preset release >/dev/null
cmake --build --preset release --target baton-loadgen >/dev/null
docker build -q -t baton:bench . >/dev/null
docker build -q -t beanstalkd:bench -f bench/compare/beanstalkd.Dockerfile bench/compare >/dev/null
docker pull -q contribsys/faktory:1.10.0 >/dev/null
exec "${PYTHON:-python3}" bench/compare/run_compare.py "$@"
