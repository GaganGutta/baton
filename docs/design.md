# baton design

This document explains how baton works and, more importantly, why. Each
milestone adds a section **before** its code is written (data structures,
formats, failure cases, alternatives, test plan) and revisits it afterwards to
record known limitations. `docs/guarantees.md` lists the promises that fall out
of this design together with the tests that check them.

Contents:

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [Architecture in one page](#2-architecture-in-one-page)
3. [M0 — Foundations](#3-m0--foundations)

---

## 1. Goals and non-goals

baton is a job server for small teams and side projects that want reliable
background jobs and multi-step workflows **without operating Postgres, Redis or
Temporal**. It is one binary with no runtime dependencies. Clients speak the
Redis wire protocol (RESP2) with baton's own commands, so every language that
has a Redis client library already has a baton client.

Goals, in priority order:

1. **Correctness.** No acknowledged job is ever lost, even if the server or the
   workers are killed at any instant. Every guarantee is proven by a test.
2. **Simplicity.** A design that one person can hold in their head and an
   operator can reason about: one data directory, one log, one state machine.
3. **Performance**, as far as 1 and 2 allow — measured, never estimated.

Non-goals (stated honestly so nobody is surprised):

- **No replication or clustering.** baton is a single node. If the disk dies,
  the data is gone; if the machine is down, the queue is down. Back up the data
  directory or run something else if that is unacceptable.
- **No TLS.** baton binds to localhost by default. Put it behind a private
  network, a tunnel or a TLS-terminating proxy.
- **The dataset must fit in RAM.** All live jobs are held in memory; the disk
  holds the log and snapshots needed to rebuild it.

## 2. Architecture in one page

```
            clients (any Redis client library)
                 │  RESP2 over TCP
                 ▼
┌──────────────────────────────────────────────┐
│ event loop thread (epoll / kqueue)           │
│  parse → validate → build record → apply()   │──► replies are parked until
│  owns ALL in-memory state, so no locks       │    their records are durable
└──────────────┬───────────────────────▲───────┘
   batches of  │                       │ "durable up to LSN n"
   records     ▼                       │ (wakes the loop)
┌──────────────────────────────────────┴───────┐
│ log thread: write → fsync (group commit)     │
└──────────────────────────────────────────────┘
                 │
                 ▼
      data dir: log segments + snapshots
```

Three rules carry most of the design:

**One owner.** A single event loop thread owns every piece of in-memory state:
jobs, queues, timers, connections. There is nothing to lock and no ordering
between threads to get wrong. The only other long-lived thread writes and
fsyncs the log; the two communicate through a small mutex-protected hand-off
buffer and a wakeup pipe.

**Reply after durable.** No reply that confirms or reveals a state change is
sent until every log record it depends on is fsynced. Records get increasing
log sequence numbers (LSNs) and become durable strictly in order, so "everything
this reply depends on" collapses to a single number: the LSN of the last record
appended before the reply was generated. Each connection keeps its replies in a
FIFO tagged with that LSN and releases them as the durable LSN advances. Reads
are tagged the same way, so a `STATUS` can never reveal a job whose enqueue is
not yet on disk. For example, `RESERVE` replies only once the lease record is
durable — which implies the enqueue record before it is durable too.

**One apply function.** Command handlers never mutate state directly. They
validate the request against current state, resolve every source of
nondeterminism (job ids, wall-clock timestamps, random backoff jitter, lease
tokens) into a log record, and hand that record to `apply()`. Recovery reads
records from disk and hands them to the same `apply()`. Because `apply()` never
reads a clock or a random number generator, replaying the log reproduces the
live state exactly; there is no second "recovery" code path that could drift
from the live one.

The sections below fill in each layer as it is built.

## 3. M0 — Foundations

### 3.1 Error handling: `Result<T>`, not exceptions

baton has three kinds of failure and treats each differently:

| Kind | Examples | Mechanism |
|---|---|---|
| Expected runtime failures | malformed command, unknown job id, stale lease token, disk full, corrupt log segment | `Result<T>` / `Status` return values |
| Broken invariants (baton bugs) | a job in two queues, LSN going backwards | `BATON_CHECK` → print location, `abort()` |
| Failures that make durability unknowable | `fsync` returning an error | log and `abort()` (see M1) |

**Why values instead of exceptions.**

- *Failure is normal control flow here.* A server spends its life rejecting bad
  input and hitting limits. `Result<T>` is `[[nodiscard]]`, so the compiler
  refuses to let a call site ignore a failure, and a reviewer can see every
  error path in the diff. With exceptions the error paths are invisible at the
  call site.
- *Apply must be all-or-nothing.* baton's core invariant is that in-memory state
  mirrors the log. An exception unwinding through the middle of a state change
  would leave state half-applied with no record of it. With explicit returns,
  validation happens first and mutation happens in code that cannot fail.
- *Crash-only recovery is the real safety net.* For anything unexpected the
  right response is not to unwind but to stop: the durable log brings a
  restarted process back to a known-good state, which no `catch` block can
  promise.

What this costs: call sites are noisier (`BATON_RETURN_IF_ERROR`,
`BATON_ASSIGN_OR_RETURN` keep it tolerable), and constructors cannot fail, so
fallible construction goes through static factory functions that return
`Result<T>`.

Exceptions are still *enabled* in the build: the standard library may throw
(`std::bad_alloc`), and GoogleTest needs them. baton code itself does not throw
and does not catch. Allocation failure therefore terminates the process, which
is consistent with the crash-only stance; the `max memory` limit (M3) exists so
that baton refuses work with a clear error long before the allocator fails.

`std::expected` would be the natural choice but is C++23; `Result<T>` is a
60-line subset of it over `std::variant`.

**Alternatives considered.** *Exceptions everywhere*: less typing, but hides
error paths and makes partial state changes possible. *Error codes + out
parameters*: no allocation, but easy to ignore and awkward to compose.
*`std::error_code`*: fits OS errors well but cannot carry the context message
("segment 0000042, offset 1337: bad CRC") that makes a corruption report
actionable.

### 3.2 Logging: a small in-house logger to stderr

Diagnostic logging (distinct from the durable log) is one line per event:

```
2026-09-17T08:15:30.123Z INFO  server: listening addr=127.0.0.1:7379
```

- **Destination: stderr only.** Supervisors (Docker, systemd, a terminal)
  already capture, rotate and ship stderr. baton managing its own log files
  would duplicate that and add failure modes (disk full from logs, rotation
  races).
- **Format: timestamp, level, component, message with `key=value` pairs.**
  Greppable by humans, trivially parsed by log shippers, no JSON escaping cost.
- **Levels: debug, info, warn, error.** The level is an atomic, so the check on
  a disabled level is one load and the message arguments are not evaluated.
- **No logging on hot paths at info or above.** Per-command events are counted
  in metrics (M11), not logged.
- **Thread safety:** a mutex serializes line output. Only two threads log, and
  rarely, so contention is irrelevant; whole lines never interleave.
- **Tests** can install a sink to assert on log output.

**Why not spdlog or glog?** The project's rule is no dependencies beyond the
test, benchmark and fuzzing frameworks unless clearly justified. baton's logging
needs are about 100 lines of code on top of `std::format`; a dependency would
add more build surface than it removes code. Asynchronous logging — the main
thing spdlog would add — is unnecessary when hot paths do not log.

### 3.3 Time: two clocks, two types

`WallTime` (milliseconds since the Unix epoch) and `MonoTime` (milliseconds on
the monotonic clock) are distinct types with no implicit conversion. Deadlines
are persisted as `WallTime` because only the wall clock means anything after a
restart; timers are scheduled with `MonoTime` because only the monotonic clock
is guaranteed not to jump. Making them different types turns "compared a wall
deadline with a monotonic now" from a latent bug into a compile error. All
code takes a `Clock&`, and tests use `FakeClock` to move time and to simulate
wall-clock jumps deterministically. The reconciliation rules are in M4.

### 3.4 Build, tooling and dependencies

- **C++20, CMake presets, `-Wall -Wextra -Werror`** on baton's own targets only
  (an `INTERFACE` options target), so third-party code is not held to our
  warning set. Sanitizer flags, by contrast, are global: mixing instrumented and
  uninstrumented code produces false TSan reports.
- **Presets:** `debug`, `release`, `asan` (ASan+UBSan, `-fno-sanitize-recover`
  so UB fails the test run), `tsan`, `fuzz` (Clang + libFuzzer), `tidy`.
- **One static library per module**, with link dependencies mirroring the
  allowed layering, so an accidental upward include fails to link:
  `common ← log, sched ← state ← snapshot ← server`, and `net` depends only on
  `common`.
- **Dependencies:** GoogleTest, Google Benchmark (both via FetchContent, pinned
  tags) and the compiler's libFuzzer. The `baton` binary links only the C++
  standard library. The Docker build disables tests, so it fetches nothing.
- **clang-format and clang-tidy are pinned to LLVM 21 in CI** so local and CI
  results agree. Every disabled clang-tidy check is listed in `.clang-tidy`
  with its reason.
- **Conventions:** types `CamelCase`, functions and variables `snake_case`,
  members `trailing_`, constants `kCamelCase`, macros `BATON_UPPER`. No raw
  `new`/`delete`; every resource has an RAII owner (`Fd` for descriptors).

### 3.5 Platform notes

Linux is the primary target (epoll, `fdatasync`). macOS builds and runs for
local development (kqueue, `F_FULLFSYNC`); CI builds and runs the unit tests on
macOS to keep that true. Windows is not supported; on Windows, use WSL2 or
Docker.

---

## Credits

Ideas (never code) borrowed from prior work are credited in the section that
uses them. So far:

- The reply-after-durable rule and group commit are standard database practice
  (see Gray & Reuter, *Transaction Processing*).
- The overall shape — a job server speaking a simple text protocol, reserve /
  delete / release / bury, time-to-run — follows **Beanstalkd**; the idea of a
  language-agnostic job server with a dead set follows **Faktory**; durable
  workflow replay follows **Temporal**.
