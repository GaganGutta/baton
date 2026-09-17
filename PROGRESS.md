# Progress

This file is the hand-off document: a fresh session should be able to resume
from here with no other context. Read `PLAN.md` for the task breakdown and
`docs/design.md` for the reasoning behind decisions.

## Status

- **Current milestone:** M4 (leases, retries, delayed jobs, DLQ) — next
- **Last completed task:** M3 networking: poller, RESP parser, server (reply gating, blocking RESERVE, limits, AUTH), integration tests with redis-cli/redis-py incl. kill -9, RESP3 negotiation
- **Next task:** M4 design section, then DLQ.LIST / DLQ.RETRY / DLQ.PURGE, the wall-clock jump detector, restart lease grace tests through the wire

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Scaffold | **done** | CI: gcc-14 + clang-18 (release, asan), tsan, tidy (LLVM 21), macOS, Docker |
| M1 Durable log | **done** | 140 tests under ASan+UBSan and TSan; 6/6 mutants killed; 2 fuzz targets; guarantees D1–D9 |
| M2 State machine | **done** | 210 tests; model-based test with replay equivalence; 13/13 mutants killed; 5 fuzz targets; guarantees L1–L10. Layers: `Engine` (commands, only reader of clock/RNG) → `State::apply(record)` → `RecordSink` |
| M3 Networking | **done** | 262 unit tests + 50 integration tests (real binary, redis-cli, redis-py under default/RESP2/RESP3, SIGKILL mid-pipeline); 16/16 mutants killed; 6 fuzz targets; guarantees W1–W11 |
| M4 Leases/retries/DLQ | not started | |
| M5 Snapshots | not started | |
| M6 Python SDK | not started | |
| M7 Chaos harness | not started | |
| M8 Benchmarks + v0.1.0 | not started | |
| M9 Workflows | not started | |
| M10 Scheduling | not started | |
| M11 Observability | not started | |
| M12 Example + v0.2.0 | not started | |

## Development environment (this machine)

- Windows 11 host; the working tree lives at `C:\Users\gagan\Desktop\baton`.
- baton is Linux-first, so all builds and tests run inside **WSL2 Ubuntu 26.04**
  (16 cores, 15 GiB RAM): GCC 15.2, Clang 21.1.8, CMake 4.2.3, Ninja 1.13,
  redis-cli 8.0.5, Python 3.14. The toolchain was installed with apt on 2026-09-16.
- The tree is mirrored into WSL ext4 (`~/baton`) before each build with a
  checksum-based rsync, so builds are fast and fsync hits a real Linux
  filesystem instead of the 9p Windows mount. The helper is `.dev/w <command>`
  (gitignored because it is machine-specific; it is a 10-line rsync + exec
  wrapper and can be recreated from this description).
- Build dirs live under `~/baton/build/<preset>` inside WSL.
- Git runs on the Windows side and uses the existing global git identity
  (GitHub noreply address). WSL's separate git identity is deliberately not used,
  so no private email address lands in a public repo.
- Docker Desktop is installed but was not running at project start; it is needed
  for the Dockerfile check (M0) and the Beanstalkd/Faktory comparison (M8).
- Python for integration tests, the SDK and chaos: a venv at `~/baton-venv` in WSL
  with `pytest` and `redis` (`python3 -m venv ~/baton-venv && ~/baton-venv/bin/pip
  install pytest redis`). Run: `BATON_BIN=build/release/src/server/baton
  ~/baton-venv/bin/python -m pytest tests/integration -q`.

## How to verify the current state

```bash
scripts/check.sh            # format, asan, tsan, tidy
scripts/check.sh fuzz       # 30 s per fuzz target
scripts/mutation-check.sh   # durability mutants must all be killed
```

## Key decisions so far

See the "Cross-cutting decisions" table in `PLAN.md`. Each one gets its full
justification in `docs/design.md` when its milestone starts.

## Open issues

- Docker Desktop did not come up when started from the command line on
  2026-09-17 (daemon pipe never appeared). Not blocking: CI builds and smoke
  tests the image. It must be running for the M8 Beanstalkd/Faktory comparison.

## Things that could not be done as specified

- None yet.
