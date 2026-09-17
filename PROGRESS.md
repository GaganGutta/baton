# Progress

This file is the hand-off document: a fresh session should be able to resume
from here with no other context. Read `PLAN.md` for the task breakdown and
`docs/design.md` for the reasoning behind decisions.

## Status

- **Current milestone:** M8 (benchmarks, README, v0.1.0). M7 is done (the tmpfs for the chaos full-disk scenario must be re-mounted as root after every WSL reboot: see `chaos/README.md`)
- **Last completed task:** M8 measurements. `bench/loadgen` (baton/beanstalkd/faktory protocols, open and closed loop), `bench/run_all.sh`, `bench/compare/run_compare.sh` (Docker Engine installed inside WSL; Docker Desktop does not start on this machine) ran on commit `4f45de4`; results are quoted verbatim in `docs/benchmarks.md`, and the README's performance section quotes a subset. Design section 11 describes the methodology and its limits
- **Next task:** finish M8. Run the quickstart end to end from a clean checkout (docker build from the GitHub URL, then `examples/quickstart`) and fix anything that does not work as written; run `scripts/check.sh` (includes the Python suites); confirm CI green; tick M8 in PLAN.md; tag `v0.1.0`. Then M9 (workflows): design notes are drafted in `.dev/m9-notes.md` and `.dev/design-m9.md` (gitignored scratch)

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Scaffold | **done** | CI: gcc-14 + clang-18 (release, asan), tsan, tidy (LLVM 21), macOS, Docker |
| M1 Durable log | **done** | 140 tests under ASan+UBSan and TSan; 6/6 mutants killed; 2 fuzz targets; guarantees D1–D9 |
| M2 State machine | **done** | 210 tests; model-based test with replay equivalence; 13/13 mutants killed; 5 fuzz targets; guarantees L1–L10. Layers: `Engine` (commands, only reader of clock/RNG) → `State::apply(record)` → `RecordSink` |
| M3 Networking | **done** | 262 unit tests + 50 integration tests (real binary, redis-cli, redis-py under default/RESP2/RESP3, SIGKILL mid-pipeline); 16/16 mutants killed; 6 fuzz targets; guarantees W1–W11 |
| M4 Leases/retries/DLQ | **done** | 279 unit + 54 integration tests; 20/20 mutants killed; guarantees L11–L13. Decisions: leases survive restarts with `--lease-grace` (5 s); a wall-clock step > 1 s is handled exactly like a restart |
| M5 Snapshots | **done** | 325 unit + 57 integration tests; 27/27 mutants killed; 7 fuzz targets; guarantees S1–S7. SimFs torn crashes now keep an arbitrary subset of unsynced directory operations, which found a real recovery bug (leftover segments behind the snapshot, design.md 8.4). Measured: pause 0.15 ms / 3.5 ms / 126 ms at 10k / 100k / 1M live jobs |
| M6 Python SDK | **done** | 51 tests (`sdk/python/tests`, real binary): replies lost in transit via a dropping proxy, SIGKILLed and SIGTERMed worker processes, server restart under a running worker, racing and killed ledger writers; guarantees P1–P10. Run: `BATON_BIN=build/release/src/server/baton ~/baton-venv/bin/python -m pytest sdk/python/tests -q` |
| M7 Chaos harness | **done** | `chaos/` (kill rounds, 5 invariants, 4 fault scenarios, `selftest.sh` with planted bugs), `baton-logcheck` + `LeaseModel` (independent offline lease check of the server's whole log); 346 unit tests, 29/29 mutants, 53 SDK tests; guarantees L14, C1–C6. Long run 2026-09-17: 40 rounds, 652 server kills, 232,682 acknowledged jobs, 0 violations (docs/testing.md). Found and fixed: leases expired ≤ 1 ms early; unsound SDK inference on resent ACKs → `ACK` idempotent for the completing token |
| M8 Benchmarks + v0.1.0 | in progress | benchmarks and comparison measured and documented; README written; remaining: quickstart verification, tag |
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
scripts/mutation-check.sh   # durability mutants must all be killed (optional name filter)
bench/run_storage_bench.sh  # snapshot pause / write / load and recovery time (docs/benchmarks.md)
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
