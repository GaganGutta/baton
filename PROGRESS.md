# Progress

This file is the hand-off document: a fresh session should be able to resume
from here with no other context. Read `PLAN.md` for the task breakdown and
`docs/design.md` for the reasoning behind decisions.

## Status

- **Current milestone:** M1 (durable log) — in progress
- **Last completed task:** M0 done (CI green on first push, run 35181138012); M1 design section written
- **Next task:** M1 code: crc32c → codec → FileSystem (PosixFs + SimFs) → log format/reader/recovery → group-commit log thread

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Scaffold | **done** | CI: gcc-14 + clang-18 (release, asan), tsan, tidy (LLVM 21), macOS, Docker |
| M1 Durable log | in progress | design in docs/design.md §4 |
| M2 State machine | not started | |
| M3 Networking | not started | |
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

## Key decisions so far

See the "Cross-cutting decisions" table in `PLAN.md`. Each one gets its full
justification in `docs/design.md` when its milestone starts.

## Open issues

- Docker Desktop did not come up when started from the command line on
  2026-09-17 (daemon pipe never appeared). Not blocking: CI builds and smoke
  tests the image. It must be running for the M8 Beanstalkd/Faktory comparison.

## Things that could not be done as specified

- None yet.
