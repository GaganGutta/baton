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
4. [M1 — The durable log](#4-m1--the-durable-log)
5. [M2 — The state machine](#5-m2--the-state-machine)
6. [M3 — Networking](#6-m3--networking)
7. [M4 — Leases, retries, time and the dead-letter queue](#7-m4--leases-retries-time-and-the-dead-letter-queue)
8. [M5 — Snapshots and log compaction](#8-m5--snapshots-and-log-compaction)
9. [M6 — The Python SDK](#9-m6--the-python-sdk)
10. [M7 — The chaos harness](#10-m7--the-chaos-harness)
11. [M8 — Measuring](#11-m8--measuring)

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

## 4. M1 — The durable log

The log is baton's system of record: in-memory state is a cache of it. This
section defines the on-disk format, how records get durable (group commit), and
what recovery accepts, repairs and refuses.

### 4.1 Files

Everything lives flat in one data directory, so there is exactly one directory
to fsync:

```
<data-dir>/
  wal-00000000000000000001.log     segment; the number is the LSN of its first record
  wal-00000000000000104858.log
  ...
```

A **segment** is an append-only file. When the active segment reaches the
segment size (default 64 MiB) the log thread rolls to a new one. Segments are
the unit of deletion after a snapshot (M5).

**Segment header** (32 bytes, little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `BATONLOG` |
| 8 | 4 | format version (1) |
| 12 | 4 | reserved (0) |
| 16 | 8 | LSN of the first record in this segment |
| 24 | 4 | CRC32C of bytes 0..23 |
| 28 | 4 | reserved (0) |

**Record** (17-byte header + payload):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | payload length `n` |
| 4 | 4 | CRC32C over bytes 0..3 and 8..(17+n) — everything except the CRC itself |
| 8 | 1 | record type (opaque to the log; defined by the state machine in M2) |
| 9 | 8 | LSN |
| 17 | n | payload |

LSNs start at 1 and increase by exactly 1 per record, across segments. The
length is covered by the CRC so that a corrupted length cannot redirect the
reader to a region that happens to checksum. Records larger than 64 MiB + 64 KiB
are rejected as invalid, which bounds what a corrupted length can make the
reader do.

**CRC32C** (Castagnoli) rather than CRC32: better error detection for the same
cost and hardware support on x86 (SSE4.2) and ARMv8. baton ships a
slicing-by-8 software implementation and selects the hardware one at runtime
when available; tests check both against the RFC 3720 vectors and against each
other on random inputs.

### 4.2 Group commit

```
event loop thread                         log thread
─────────────────                         ──────────
append(type, payload) → LSN   ┐
append(...)                   │ encode into the
append(...)                   ┘ pending batch
flush()  ── batch (bytes, first/last LSN) ──►  write(batch)
   keeps accumulating the next batch           fdatasync()        (policy: always)
                                               durable_lsn = last LSN
◄── wakeup: "durable up to LSN n" ─────────    notify
release replies whose LSN ≤ n
```

The event loop assigns LSNs (it needs them to tag replies) and encodes records
into a pending buffer. Once per loop iteration it hands the buffer to the log
thread by swapping it into a mutex-protected inbox — the lock is held for a
pointer swap, never for I/O. While the log thread is inside `fdatasync`, the
loop keeps serving clients and the next batch grows. The batch size therefore
adapts to the disk: a slow fsync produces big batches, a fast one small batches,
and no tuning knob is needed. This is classic group commit.

Two fsync policies:

| Policy | A record is acknowledged when… | Survives process crash (SIGKILL, OOM) | Survives OS crash / power loss |
|---|---|---|---|
| `always` (default) | `fdatasync` covering it has returned | yes | yes |
| `interval` | `write` covering it has returned; `fdatasync` runs every *N* ms (default 100) | yes — the page cache outlives the process | up to the last *N* ms of acknowledged operations may be lost |

`interval` is the same trade Redis offers with `appendfsync everysec` and
Beanstalkd with `-f <ms>`. It is opt-in and the README says exactly what it
gives up.

**Backpressure.** The front end exposes the number of bytes appended but not
yet acknowledged. The server (M3) stops reading from client sockets when that
backlog passes a bound, so a disk that cannot keep up slows clients down
through TCP instead of growing memory without limit.

### 4.3 What happens when the disk misbehaves

**`fdatasync` fails → log the error and abort.** After a failed fsync the
kernel may already have marked the dirty pages clean and dropped them; a retry
can then return success without the data ever reaching the disk (the 2018
PostgreSQL "fsyncgate" finding). Once fsync has failed, the process can no
longer know what is durable, so the only honest move is to stop and let
recovery read back what actually reached the disk. Nothing that was
acknowledged is affected — acknowledgement only ever follows a *successful*
fsync.

**`write` fails (ENOSPC, EIO) → log the error and abort.** baton applies a
record to memory before it is durable (that is what makes pipelining and group
commit possible), so when a record cannot be written, memory is ahead of the
log and there is no undo. Crashing and replaying the log is the undo. To keep a
full disk from being a surprise, the server (M3) checks free space and rejects
new work with a clear `LIMIT` error before the disk is actually full.

**Directories are fsynced** after every create, rename and delete. `fsync` on a
file does not make its directory entry durable; without the directory fsync a
freshly rolled segment can vanish in a crash even though its contents were
synced. macOS uses `fcntl(F_FULLFSYNC)`, because plain `fsync` there does not
flush the drive cache.

### 4.4 Recovery: repair a torn tail, refuse everything else

On startup the log is scanned from the oldest segment. Every record must have a
valid CRC and the next expected LSN. The interesting question is what to do
when one does not.

A crash in the middle of writing can only damage **the end of the last
segment**: everything before the last successful fsync is intact, and only one
un-acknowledged batch can be in flight. So:

| Finding | Meaning | Action |
|---|---|---|
| Invalid record in the last segment, and **no valid record anywhere after it** | torn final write; nothing after it was ever acknowledged | truncate the segment at the record's offset, fsync, continue |
| Last segment shorter than a header, or header invalid, and no valid record in the file | crash while creating the segment | delete it, fsync the directory, continue |
| Invalid record in the last segment **followed by a valid record** | damage in the middle of the log (bit rot, bad sector, operator error); acknowledged data may be affected | **refuse to start** |
| Invalid record or header in any segment but the last | same | **refuse to start** |
| LSN gap between or within segments, a missing middle segment, first LSN in header ≠ file name | same | **refuse to start** |

"No valid record after it" is decided by scanning the rest of the segment at
every byte offset for a header that is plausible (length within bounds, LSN
within the range that could follow the last good record) and whose CRC matches.
A false positive needs a 32-bit CRC collision on top of a plausible header, and
the failure mode of a false positive is the safe one (refusing to start).

There is one known case where this rule is stricter than necessary: a torn
multi-block write where a later block reached the disk but an earlier one did
not leaves a valid un-acknowledged record after an invalid one. baton cannot
distinguish that from mid-log corruption without knowing the durable boundary,
so it refuses, and the operator decides. Refusing loudly is the right default
for a system whose one job is not losing acknowledged work; silently truncating
at the first bad record (what many logs do) would turn a flipped bit into
quietly dropped jobs.

After a successful scan the writer reopens the last segment and appends after
the last valid record.

### 4.5 Testing the log against crashes: `SimFs`

All file access goes through a small `FileSystem` interface with two
implementations: `PosixFs` and `SimFs`, an in-memory file system built for
crash testing. `SimFs` models exactly the two things that matter:

- File contents are **volatile until `sync()`**. On a simulated crash a file
  keeps its synced bytes plus an arbitrary, possibly garbled, prefix of the
  unsynced tail.
- Directory entries (create, rename, remove) are **volatile until
  `sync_dir()`**. A file that was written and synced but whose directory was
  never synced does not exist after a crash.

`SimFs` can capture a "crash image" after the *n*-th file-system operation
while the system under test keeps running. A test then recovers from that image
and checks it against what had been acknowledged at the moment of capture. This
turns "did we fsync the directory?" and "is every acknowledged record
recoverable no matter where the crash lands?" into deterministic unit tests
instead of hopes. It can also inject `sync` and `write` failures to test the
abort paths.

Test plan: CRC vectors; codec round trips and bounds; segment round trip and
roll; truncation of the final record at **every** byte offset → recovers all
earlier records; a bit flip at **every** byte of a mid-log record → refuses; a
flipped bit in the final record with nothing after it → treated as torn; LSN
gap, missing middle segment, wrong header LSN → refuse; torn segment creation →
repaired; fsync failure and write failure → abort (death tests); group commit
ordering, batch accounting and both policies; randomized `SimFs` crash-image
tests (every acknowledged LSN is recovered, recovered LSNs are contiguous, the
log accepts appends after recovery); ThreadSanitizer over all of it; a libFuzzer
target for the segment reader.

### 4.6 Alternatives considered

- **Fixed-size blocks with record fragments (LevelDB/RocksDB log format).**
  Allows resynchronizing after corruption at the next block boundary. baton
  does not want to resynchronize — it refuses to start — so the simpler
  length-prefixed format is enough.
- **Preallocating segments** (`fallocate`) makes appends cheaper on some file
  systems and lets etcd detect torn writes by looking for zeroed sectors. It
  also makes "where does the log end?" depend on recognizing zeros. Plain
  appends keep the file size meaningful; revisit if benchmarks show allocation
  cost matters.
- **`O_DIRECT` / `O_DSYNC`.** `O_DSYNC` makes every write synchronous and gives
  up group commit's batching of the flush. `O_DIRECT` needs aligned buffers and
  bypasses the page cache that the `interval` policy relies on.
- **One file instead of segments.** Compaction would need rewriting the whole
  file; with segments it is `unlink`.
- **io_uring.** One sequential writer issuing write+fsync gains little from it
  and it would cost the macOS port.

### 4.7 Known limitations (recorded after implementation)

- **The end of the log cannot be proven.** Damage confined to the *final*
  record, a newest segment that has been deleted, or a log cut exactly at a
  record boundary all look like a log that simply ended earlier, so recovery
  accepts them (`RecoveryTest.DamageConfinedToTheFinalRecordIsTreatedAsTorn`
  pins this down). A checksum can prove that bytes are intact, not that there
  were no more of them. Closing this gap needs a separately persisted "log ends
  at LSN n" marker, i.e. a second fsync per commit; baton does not pay that.
  Snapshots (M5) narrow the window: a log that ends before the snapshot's LSN is
  refused.
- **Stricter than necessary in one torn-write pattern** (section 4.4): an
  out-of-order multi-block torn write is refused rather than repaired.
- **Recovery reads one whole segment into memory at a time** (64 MiB by
  default). Simple, and it makes the scan-forward check trivial.
- **`flush()` copies when the log thread has not yet picked up the previous
  batch.** It is a memcpy of bytes that are about to be written to disk anyway;
  M8 measures whether it matters.
- **Little-endian hosts only** (x86-64, arm64), enforced by a `static_assert`.
- **A full disk aborts the process** (section 4.3). M3 adds the free-space check
  that turns an approaching full disk into a clear client-visible error first.
- **The `interval` fsync timer runs on the real steady clock**, not the
  injectable `Clock`: it lives entirely inside the log thread and nothing
  observable depends on its exact timing.

## 5. M2 — The state machine

Everything between the wire protocol and the log: what a job is, which facts
get logged, how those facts change state, and the data structures that make it
fast. No networking and no threads — this layer is a deterministic,
single-threaded library, which is what makes it testable to exhaustion.

### 5.1 Layers

```
Engine   command logic: validate → resolve nondeterminism → record → log → apply
  │      (the only code that reads the clock or the RNG)
  ▼
State    jobs, queues, idempotency index, timers; mutated ONLY by apply(record)
  │
  ▼
RecordSink   "append this record, give me its LSN" (the LogWriter in production,
             a vector in tests)
```

`Engine` exposes one typed method per command (`enqueue`, `reserve`,
`heartbeat`, `ack`, `fail`, `cancel`, `status`, …). The network layer (M3) only
translates RESP to these calls and gates replies on the LSN they return.

### 5.2 Jobs

| Field | Notes |
|---|---|
| `id` | u64, server-generated, strictly increasing from 1 |
| `queue` | name, `[A-Za-z0-9._:-]{1,128}`; queues are created on first use |
| `payload` | opaque bytes, size-limited (M3); immutable and reference-counted so a snapshot can share it instead of copying it (M5) |
| `priority` | i32, higher first, default 0 |
| `run_at` | wall-clock ms; the job is not handed out before this |
| `attempts` / `max_attempts` | attempts = leases granted so far |
| `backoff_base_ms` / `backoff_cap_ms` | per job, defaults 1 s / 5 min |
| `state` | `scheduled`, `ready`, `leased`, `succeeded`, `dead`, `cancelled` |
| `lease_token`, `lease_expires_at` | valid while `leased` |
| `last_error`, `created_at`, `finished_at` | for `STATUS` and the DLQ |
| `idem_key` | optional, see 5.5 |

```
             ENQUEUE                    RESERVE                 ACK
 (run_at > now) ──► scheduled ──due──► ready ─────► leased ─────────► succeeded
 (run_at <= now) ──────────────────────▲              │
                                       │              │ FAIL / lease expired
                        backoff elapsed│              ▼
                                  scheduled ◄── attempts < max ──┤
                                                                 └─ attempts == max ──► dead
 CANCEL: scheduled | ready | leased ──► cancelled        DLQ.RETRY: dead ──► ready
```

`scheduled` vs `ready` is *derived* from `run_at` and the clock, not logged:
promotion is an index move, and replay re-derives it. Everything else is a
logged fact.

**Ready order** within a queue is `(priority desc, run_at asc, id asc)`: FIFO by
the time a job became runnable, so a retried job queues behind work that was
already waiting, and ties break deterministically.

### 5.3 Records: the vocabulary of facts

Handlers resolve every nondeterministic input *before* logging, so a record is
a complete fact and `apply` is a pure function of `(state, record)`:

| Type | Record | Resolved by the handler |
|---|---|---|
| 1 | `JobEnqueued {id, queue, payload, priority, run_at, max_attempts, backoff, idem_key, idem_expires_at, at}` | id, `at` (clock), `run_at` from `DELAY` |
| 2 | `JobLeased {id, token, lease_expires_at, at}` | token, clock |
| 3 | `LeaseExtended {id, token, lease_expires_at, at}` | clock |
| 4 | `JobSucceeded {id, token, at}` | clock |
| 5 | `AttemptFailed {id, token, reason, error, at, outcome: retry(run_at) \| dead}` | clock, **backoff jitter (RNG)**, the retry-or-dead decision |
| 6 | `JobCancelled {id, at}` | clock |
| 7 | `DeadJobRetried {id, run_at, at}` | clock |
| 8 | `JobsPurged {ids}` | — |

`AttemptFailed` covers both a worker's `FAIL` and a lease expiry
(`reason = lease_expired`): a lease expiring is a state change — it can consume
the last attempt and send the job to the dead-letter queue — so it must be a
logged fact, not something recovery guesses from timestamps.

Payloads are encoded with varints and length-prefixed byte strings
(`common/codec.h`). Each payload starts with a version byte so fields can be
added later. Decoding is strict: trailing bytes, unknown enum values or
truncated fields fail recovery with a corruption error rather than being
skipped.

`apply` has preconditions (the job exists, is in the right state, the token
matches). Live, the handler has already verified them. During replay a violated
precondition means the log and the code disagree, and `apply` returns an error
that stops recovery: better to refuse than to guess.

### 5.4 Leases and fencing

`RESERVE` pops the best ready job, takes the next value of a **server-wide
token counter**, and logs `JobLeased`. `HEARTBEAT`, `ACK` and `FAIL` must
present the token. After a lease expires and the job is leased again, the old
worker's token no longer matches and its `ACK` is rejected with `STALE` — a
zombie cannot complete a job someone else now owns.

The counter is server-wide rather than per job for two reasons: tokens stay
unique even after `DLQ.RETRY` resets a job's attempts, and a globally
increasing token is what a downstream resource needs if it wants to fence
writes itself ("reject any write carrying a token lower than one I have seen").

**A lease is valid until an `AttemptFailed{lease_expired}` record says
otherwise** — validity is state, not a clock comparison. An `ACK` that arrives
after the nominal expiry but before the expiry was processed is accepted:
nobody else has been given the job, so accepting it is both safe and the most
useful answer. This also removes a whole class of clock-skew arguments. The
other direction is a hard rule: a lease is never expired *before* its recorded
wall-clock expiry, even if the monotonic timer fires early (added in M7, when
the chaos harness's log checker found expiries dated a millisecond too soon).

**`ACK` is idempotent for the token that completed the job** (added in M7). A
succeeded job keeps the token of the lease that completed it; dead and
cancelled jobs keep none. Repeating that `ACK` answers `OK` and logs nothing;
any other token is `STALE`. The reason is the one failure a client cannot
resolve by itself: the connection dies after `ACK` was sent. On retry, `STALE`
used to mean either "your first `ACK` worked" or "you lost the job, and
whoever got it may have finished it" — and `STATUS` says `succeeded` in both
cases. With this rule the retry's answer is exact. It costs nothing in the log
and eight bytes that the job had anyway.

### 5.5 Idempotent enqueue

`ENQUEUE … KEY k` looks `k` up in the idempotency index. If an entry exists and
`expires_at > now`, the existing job id is returned and nothing is logged.
Otherwise a `JobEnqueued` carrying the key and its expiry is logged, and
`apply` (over)writes the index entry. The decision uses the clock, so it is
made in the handler; `apply` just records the outcome.

### 5.6 Invisible garbage collection

Two things disappear purely because time passes: idempotency keys (after their
window) and finished jobs (after the retention period: succeeded/cancelled
default 10 minutes, dead 7 days). Neither is logged. The rule that keeps this
sound:

> Visibility is defined by timestamps, and physical removal is invisible. A
> handler treats an expired key as absent whether or not it has been removed
> yet; `apply` never consults the clock. Replay applies records without
> collecting anything, then collects once at the end using the current time.

Live and replayed states can therefore differ physically (what lingers) but
never in anything a client can observe.

### 5.7 Timers: a hierarchical timing wheel

Delayed jobs, retry backoff, lease expiry, key expiry and retention all need
"call me at time T" for potentially millions of pending entries, with frequent
cancellation (every `HEARTBEAT` moves a lease deadline). A binary heap costs
O(log n) per operation and lazy deletion bloats it under heartbeat churn; a
hashed hierarchical timing wheel (Varghese & Lauck, 1987) gives O(1) insert and
cancel.

- 6 levels × 64 slots, 1 ms resolution. Level *k* slots are 64^k ms wide, so the
  wheel spans 64^6 ms ≈ 2.2 years; later deadlines are clamped and re-inserted
  when they come up.
- The level for a deadline is found from the highest bit in which it differs
  from the current time; one 64-bit occupancy bitmap per level makes "when is
  the next non-empty slot?" a few bit operations, so the event loop can sleep
  exactly until the next deadline instead of ticking every millisecond.
- Entries live in a slab (`std::vector`) and are linked by index, with a free
  list: no allocation per timer, no raw pointers, and handles carry a generation
  counter so a stale handle can never cancel someone else's timer.
- When the clock reaches a higher-level slot, its entries cascade down a level.

The wheel runs on **monotonic** milliseconds. Deadlines are stored in jobs as
**wall-clock** times (they must survive restarts). The conversion happens at
insertion: `mono_deadline = mono_now + max(0, wall_deadline − wall_now)`.
Timers are derived state: replay does not touch the wheel, and after replay —
or when a wall-clock jump is detected (M4) — all timers are rebuilt from the
jobs. One rebuild path serves restart and clock jumps alike.

### 5.8 Indexes and memory

- **Job table:** `std::unordered_map<JobId, Job>`; node-based, so `Job*` is
  stable for the heap and the timers. (Revisit with measurements in M8.)
- **Ready queue per named queue:** an indexed binary heap of `Job*`. Each job
  stores its heap position, so `CANCEL` of a ready job is O(log n) instead of a
  scan or a tombstone.
- **Dead set per queue:** ordered by job id for stable `DLQ.LIST` paging.
- **Memory accounting:** `apply` maintains a running estimate (job struct, map
  node, payload, strings, timer node, index entries). M3's `max memory` limit
  compares against it; M8 compares it with RSS to see how honest it is.

### 5.9 Test plan

- Timing wheel against a naive reference model under randomized
  insert/cancel/advance, including cascades, far-future clamping, stale handles
  and `next_deadline` never being later than the true next expiry.
- Indexed heap: ordering, arbitrary removal, randomized against `std::sort`.
- Record encode/decode round trips; strict decoding rejects truncation at every
  length, trailing bytes and unknown enum values; a fuzz target for the decoder.
- One test per state transition and per rejected transition.
- **Model-based test:** long random sequences of commands and clock advances.
  After every step `State::check_invariants()` must pass (each job in exactly
  the index its state implies, counters equal a recount, heap property, timer
  present exactly when required, memory estimate equals a recomputation).
- **Replay equivalence:** at random points, rebuild a fresh `State` by replaying
  the records logged so far and compare canonical serializations. This is the
  test that fails if `apply` ever reads a clock, an RNG, or anything not in the
  record.
- Serialization round trip: `deserialize(serialize(s))` is equivalent to `s`
  (the basis of M5 snapshots).

### 5.10 Alternatives considered

- **Logging commands instead of facts** (log `FAIL job 7`, recompute the backoff
  on replay). Smaller records, but replay then depends on RNG state, the clock
  and configuration at replay time; any change to the backoff code would
  silently rewrite history. Facts make old logs mean the same thing forever.
- **Per-job attempt numbers as fencing tokens.** Simpler, but reused after
  `DLQ.RETRY`.
- **`std::set` for the ready queue.** Trivially correct, but a node allocation
  per job and poor locality; the indexed heap stores one pointer per job.
- **A binary heap for timers.** See 5.7: O(log n) and heartbeat churn.
- **Logging GC.** Deterministic physical state, but one more record per job
  for something no client can observe.

### 5.11 Known limitations (recorded after implementation)

- **Handlers validate, `apply` re-validates.** Every `apply_record` checks its
  preconditions before mutating, so a rejected record leaves the state untouched
  (`StateTest.RecordsThatDoNotFitAreRejectedAndChangeNothing`, `fuzz_state_apply`).
  The engine applies a record *before* appending it to the log: if a handler
  ever produced a record that does not fit, the process stops without the bad
  record reaching disk, instead of bricking the data directory.
- **The engine's clock is sampled once per `tick()`**, i.e. once per event-loop
  iteration. All commands handled in one iteration see the same timestamp.
- **Collection can lag, visibility cannot.** Garbage collection pops from the
  front of insertion-ordered queues and stops at the first entry that is not
  yet due. If the idempotency window or a retention period is changed between
  restarts, or the wall clock is stepped, an older long-lived entry can delay
  the physical removal of expired ones behind it. Handlers compare timestamps,
  so nothing expired is ever *visible*
  (`EngineTest.ExpiredKeyCountsAsAbsentEvenBeforeItIsCollected` — a test that
  exists because the mutation check showed the comparison was otherwise
  untested).
- **The idempotency window is server-wide**, not per job. The expiry is stored
  per entry, so changing the setting never rewrites history.
- **Ties in the ready order go to the lower job id.** A job retried with
  `RETRY_IN 0` re-enters the queue with the same millisecond `run_at` as work
  enqueued in that millisecond and sorts before higher ids.
- **Timestamps outside `[0, 2^50]` ms are rejected by the decoders**, so a
  damaged but checksummed record can never cause signed overflow in time
  arithmetic.
- **The memory estimate is an estimate** (fixed per-node overheads). M8 measures
  it against RSS.
- **Jitter values are not portable across standard libraries**
  (`std::uniform_int_distribution` is implementation-defined). Irrelevant to
  correctness: the drawn value is logged, never re-drawn.
- `DeadJobRetried` and `JobsPurged` are applied and tested here; the commands
  that produce them (`DLQ.RETRY`, `DLQ.PURGE`) arrive with M4.

## 6. M3 — Networking

The wire contract is `docs/protocol.md`, written first. This section is about
how the server honours it: one event loop, a parser that survives hostile
input, and the mechanism behind "no reply before its records are durable".

### 6.1 One loop iteration

```
wait for events (epoll/kqueue), at most until the next timer deadline
 1. engine.tick()            sample clocks; promote due jobs; expire leases (records)
 2. accept new connections
 3. for each readable connection: read → parse → execute commands
                                  (each reply is queued with the LSN it depends on)
 4. serve blocked RESERVEs for queues that gained ready jobs; time out the rest
 5. log.flush()              hand this iteration's records to the log thread: ONE batch
 6. release replies whose LSN <= log.committed_lsn(); write to sockets
```

Everything a loop iteration logs goes to the log thread as one batch, however
many connections contributed to it; that is where group commit comes from. The
log thread's commit callback writes a byte to a self-pipe, which wakes the loop
to run step 6 for the replies that were waiting.

### 6.2 Reply gating: the core invariant, mechanically

Each connection has a FIFO of `(reply bytes, required LSN)`. `required LSN` is
`engine.last_lsn()` at the moment the reply is produced — the LSN of the last
record appended by *anyone* so far. Because the log becomes durable strictly in
LSN order, "LSN n is durable" implies every record the reply could depend on
is durable: its own, and every earlier one that shaped the state it reports.

- A mutating command's reply waits for its own record.
- A read-only command's reply waits for whatever was logged before it, so
  `STATUS` cannot reveal a job whose `ENQUEUE` has not reached the disk.
- When nothing is in flight, `required LSN <= committed LSN` already holds and
  the reply goes out in the same iteration: reads are not slowed down by
  durability unless they race with writes.
- A reply is only ever released from the *front* of its connection's FIFO, so
  pipelined replies cannot overtake each other.

The server keeps one global deque of `(LSN, connection)` in LSN order; when the
committed LSN advances it pops the front while `LSN <= committed`. The cost of
releasing replies is proportional to the replies released, not to the number of
connections.

### 6.3 The RESP2 request parser

Incremental, zero-copy and bounded:

- It consumes bytes from the connection's read buffer and yields a request as a
  vector of `string_view`s into that buffer; nothing is copied until a handler
  decides to keep a payload.
- It can be fed one byte at a time and produces the same requests as when fed
  everything at once (the fuzz target checks this equivalence at every split).
- **Every length is validated before it is used**: argument count ≤ 1,024, bulk
  length ≤ 512 MiB, and no allocation is ever sized by an unvalidated number.
- An argument larger than `--max-payload` is **skipped as it streams in** rather
  than buffered: the request is then answered with `-LIMIT`, and the connection
  stays in sync. Without this, a too-large payload would either have to be
  buffered in full (a memory DoS) or cost the client its connection.
- Anything that is not an array of bulk strings is a protocol error: reply and
  close. Inline commands are rejected on purpose (see protocol.md).

### 6.4 Blocking RESERVE

A `RESERVE` that finds nothing parks the connection: it is appended to the
waiter list of every queue it named and gets a timeout timer (a second timing
wheel owned by the server; the state machine's wheel stays free of connection
concerns). While parked, the connection's later pipelined requests stay in its
read buffer, unparsed.

When `State` reports that a queue gained a ready job, the loop wakes that
queue's waiters in arrival order; each re-runs its `RESERVE` over its own queue
list. A waiter that gets a job, times out or disconnects is removed from all
its lists. Fairness is first come, first served per queue.

### 6.5 Backpressure and limits

- **Slow disk:** when the log's uncommitted backlog exceeds 64 MiB the loop stops
  reading from client sockets until it drains; TCP pushes back on producers.
- **Slow reader:** a connection whose unsent replies exceed 64 MiB is closed.
- **Too many connections:** the new socket gets `-LIMIT` and is closed.
- **Memory / payload:** `-LIMIT` from the engine (M2) or the parser (6.3).
- **Disk nearly full:** free space is checked (statvfs) every second; below a
  reserve of twice the segment size, `ENQUEUE` answers `-LIMIT` while `ACK`,
  `FAIL` and reads keep working so the backlog can drain. If the disk fills
  anyway, the log aborts (4.3) and recovery takes over.

### 6.6 Startup, shutdown, single instance

Startup: lock the data directory (`flock` on `LOCK`; a second instance fails
fast), recover the log, replay it into `State`, `end_replay()`, open the
`LogWriter`, listen. Shutdown on SIGTERM/SIGINT: stop accepting and reading,
answer parked `RESERVE`s with the null array, flush and fsync the log, release
the replies that were waiting on it, close. A signal handler only sets a flag
and writes to the self-pipe.

### 6.7 Security posture

Binds to `127.0.0.1` unless told otherwise. Optional `--requirepass`, compared
in constant time. No TLS (non-goal). The parser refuses inline commands, so
cross-protocol requests from browsers cannot execute anything.

### 6.8 Test plan

- Parser: unit tests for every frame shape and every limit; oversize skipping;
  fuzz target with the split-anywhere equivalence oracle.
- **Reply-after-durable, end to end:** an in-process server on loopback over a
  `SimFs` whose `sync()` can be held. While it is held, clients that enqueue,
  reserve or read must receive *no bytes*; when it is released, the replies
  arrive, in pipeline order. A mutant that releases replies early must fail it.
- Blocking `RESERVE`: timeout, wake on enqueue, wake on delayed job and on
  retry, multi-queue order, FIFO fairness, disconnect while parked, pipelining
  behind a parked `RESERVE`.
- Limits and auth: every row of the limits table in protocol.md.
- Integration (pytest, real binary): redis-cli and redis-py sessions; `kill -9`
  at arbitrary points followed by restart keeps every acknowledged job.

### 6.9 Alternatives considered

- **Thread per connection / a thread pool.** Would need locking around all
  state, the opposite of the design's core simplification. One loop handles far
  more requests than the disk can make durable anyway.
- **io_uring.** Faster than epoll for many small operations, but Linux-only and
  the bottleneck is fsync, not syscalls.
- **Gating only mutating replies.** Simpler, but a read could then reveal state
  that a crash takes back, which is exactly what the invariant forbids.
- **Inline commands** for telnet-friendliness: rejected for the cross-protocol
  reason above; `redis-cli` and `batonctl` cover manual use.

### 6.10 What building it changed, and known limitations

- **RESP3 had to be added.** The plan said RESP2 only. The first integration
  test against redis-py 8 failed: it opens every connection with `HELLO 3` by
  default and treats `-NOPROTO` as fatal, so the most popular Python client
  could not connect out of the box. Telling users to pass `protocol=2` would
  have hollowed out "any Redis client library can connect", so baton
  negotiates RESP3 per connection. The cost was small because every reply type
  baton uses is valid RESP3 already; only "nothing" (`_` instead of `*-1`) and
  field–value replies (maps instead of flat arrays) differ. The integration
  suite runs every test under redis-py's defaults, RESP2 and RESP3.
- **Everything is gated, even `PING`.** The rule is uniform — a reply waits for
  the last LSN logged before it — rather than per command. During an fsync
  stall a health check stalls too, which is an honest answer: the server cannot
  acknowledge anything at that moment. A per-command exemption list would be
  one more thing to get wrong.
- **A lease can be granted to a connection that is already gone.** If a client
  disconnects after its `RESERVE` was executed but before the reply could be
  written, the lease record is durable and the job waits out its lease before
  it is retried. Correct (at-least-once), just slow for that one job; releasing
  such leases early is on the roadmap. A parked `RESERVE` whose client
  disconnects is cleaned up immediately and never leases anything
  (`ServerTest.DisconnectedWaiterDoesNotSwallowAJob`).
- **Input is parsed from a contiguous buffer.** A request that arrives in many
  small reads is re-examined from its first byte on each read; the work is
  bounded by the argument count (≤ 1,024), not by the bytes, because the
  declared lengths let the parser skip. Consumed input is compacted lazily.
- **One `write()` per connection per loop iteration**, not per reply: replies
  accumulate in the connection's output buffer and are flushed once.
- **Backpressure is coarse:** when the log backlog exceeds 64 MiB all client
  reads pause, not just those of the heaviest producer.
- **The disk-space check is a poll** (once per second, `statvfs`). A disk that
  fills faster than that still ends in the abort-and-recover path of 4.3.
- **Host names are not resolved for `--bind`**, only address literals, so what
  baton listens on never depends on DNS.

## 7. M4 — Leases, retries, time and the dead-letter queue

The mechanisms (lease records, fencing tokens, backoff, the timing wheel) were
built in M2. This section settles the policies around them: why delivery is
at-least-once, what a restart does to leases, what a clock jump does to timers,
and how the dead-letter queue is operated.

### 7.1 Why delivery is at-least-once, and what to do about it

A worker finishes a job and sends `ACK`. One of three things happens:

1. The `ACK` is logged and the worker sees `+OK`. Done, exactly once.
2. The worker (or the network) dies **before** the `ACK` reaches baton. baton
   cannot tell "finished but could not say so" from "died halfway": both look
   like silence. The lease expires and the job runs again.
3. The `ACK` is logged but the reply is lost. The worker does not know whether
   it succeeded; if it retries it gets `STALE`, which is safe.

Case 2 is not a baton limitation. No system can make "perform a side effect in
the outside world" and "record that it was performed" one atomic step when they
live in different failure domains: the process can always die between the two.
Every job system must therefore choose: redeliver when unsure (at-least-once,
possible duplicates) or never redeliver (at-most-once, possible loss). For
background jobs, loss is worse than duplication, so baton redelivers.

What is achievable is that each **side effect happens exactly once**, by making
the effect idempotent. baton supplies three tools:

- **Idempotency keys on `ENQUEUE`**, so a producer that retries after a timeout
  does not create a second job.
- **A stable job id and attempt number** in every delivery. A handler can use
  `(job id)` as the deduplication key for its effect: "INSERT … ON CONFLICT DO
  NOTHING", a payment provider's idempotency key, a marker file. The SDK's
  idempotency helper (M6) packages this pattern.
- **Fencing tokens.** A zombie — a worker that stalled past its lease and then
  resumed — is refused by baton (`STALE`), and the token lets a downstream
  resource refuse it as well: tokens only increase, so "reject writes carrying
  a token lower than one I have already seen for this job" closes the last
  window, where a zombie's write races the new worker's.

### 7.2 The lease lifecycle

```
RESERVE ──► JobLeased{token, expires_at}            (logged; the reply waits for it)
HEARTBEAT ► LeaseExtended{token, new expires_at}    (logged; resets the expiry timer)
ACK ──────► JobSucceeded{token}
FAIL ─────► AttemptFailed{worker_failed, retry_at | dead}
(silence) ► AttemptFailed{lease_expired, retry_at | dead}   written by the server's timer
CANCEL ───► JobCancelled                            the holder learns via STALE
```

A lease ends only when one of these records is applied. In particular expiry is
a *record*, produced when the expiry timer fires, not a comparison against the
clock made by whoever happens to look. Two consequences: `ACK` is judged
against state alone (if no expiry record exists yet, the lease is still the
current one and the `ACK` wins), and replay never has to guess whether a lease
"would have" expired.

Heartbeats are logged, so they cost a log record each. That is deliberate: an
extended lease that was not durable would shrink back after a crash, and a
worker that did everything right would see its job handed to someone else.

### 7.3 Restarts: leases survive, with a grace period

**Decision:** when the server restarts, leases that were active stay active
under the same tokens. A worker that outlived the server reconnects and
`ACK`s, `FAIL`s or heartbeats as if nothing had happened.

The subtlety is downtime. While the server is down, workers *cannot*
heartbeat; a lease that was perfectly healthy may be past its expiry when the
server comes back. Expiring those leases at startup would re-run every
in-flight job after every restart — a guaranteed duplicate-execution storm —
to punish workers for the server's outage. So at startup each lease's expiry
timer is set to `max(its persisted expiry, now + --lease-grace)` (default 5 s):
every worker gets at least one grace period to reconnect and heartbeat. The
persisted expiry itself is not rewritten; the grace only affects when the timer
fires.

Alternatives considered:

- *Void all leases on restart.* Simple, and no worker could ever hold a lease
  the server has forgotten, but every restart duplicates all in-flight work,
  and zero-downtime upgrades become impossible.
- *Expire by persisted deadline, no grace.* Punishes workers for downtime they
  did not cause (see above).
- *Extend every lease by the measured downtime.* More precise, but needs a
  trustworthy "when did I stop" (a crash leaves none) and a wall clock that did
  not move meanwhile.

What is lost in a crash is only what was never acknowledged: a `RESERVE` whose
reply never left the server leaves a lease nobody knows they hold. It expires
(after the grace) and the job is redelivered. Correct, merely slow for that job.

### 7.4 Wall-clock jumps

Deadlines are persisted as wall-clock times (nothing else survives a restart),
but timers run on the monotonic clock (nothing else is safe from jumps). The
two are reconciled at two moments only:

1. **When a timer is set:** `mono_deadline = mono_now + max(0, wall_deadline −
   wall_now)`. From then on the timer is immune to wall-clock changes: a lease
   of 30 s lasts 30 s of real time even if NTP steps the clock meanwhile.
2. **When the offset between the clocks changes abruptly.** Each loop iteration
   the engine computes `wall_now − mono_now`. Slewing moves this by at most
   0.05 %, far below one millisecond per iteration; a change of more than
   `1 s` between two iterations can only be a step (an NTP correction, an
   operator setting the date, a VM or laptop resuming from suspend — Linux's
   monotonic clock does not count suspended time). On a step baton logs a
   warning and **re-derives every timer from its persisted wall-clock deadline,
   through the same code path as a restart**, lease grace included.

So a clock jump is treated exactly like a restart, and there is only one
rebuild path to test. After a forward jump, delayed jobs whose wall-clock time
has now arrived run at once (their `AT 09:00` really is in the past), while
leases get the grace period instead of expiring en masse. After a backward
jump, delayed jobs wait until the wall clock reaches their time again; leases,
whose persisted expiry is now "further away", keep at least their remaining
time.

Steps smaller than the threshold are ignored; their only effect is that timers
set before the step fire up to that much early or late relative to the new
wall clock. Across a restart the same rule applies implicitly: timers are
derived from wall-clock deadlines, so a clock that moved while baton was down
shifts them by that amount. Run NTP in slew mode on production hosts.

### 7.5 The dead-letter queue

A job whose last attempt fails becomes `dead`: it keeps its payload, last error
and attempt count, leaves the ready structures, and is indexed per queue by job
id. It stays for `--retain-dead` (7 days) and then is collected like any
finished job.

- `DLQ.LIST queue [offset [count]]` pages through it, oldest first.
- `DLQ.RETRY id` / `DLQ.RETRY queue ALL` logs `DeadJobRetried`: the job is
  ready again *now*, with attempts reset to 0 (a fresh budget — the operator
  presumably fixed something) and its last error kept for context. Fencing is
  unaffected because tokens are server-wide, not per attempt.
- `DLQ.PURGE id` / `DLQ.PURGE queue ALL` logs `JobsPurged` (in chunks of 10,000
  ids) and deletes the jobs. Purging is logged, unlike retention-based
  collection, because it is an operator's decision rather than a function of
  time.

### 7.6 Test plan

Engine level, with `FakeClock`: every DLQ operation and its errors; the model
test gains DLQ retry/purge so replay equivalence covers their records; forward
and backward clock jumps (delayed jobs, leases and the grace period); slow
drift never triggers a rebuild. Through the wire, in real time: a worker
`ACK`s after a restart; a lease that "expired" during downtime is still
honoured within the grace period and redelivered with a new token after it;
DLQ commands under RESP2 and RESP3. Mutants: restart grace ignored; clock jumps
not detected.

### 7.7 Known limitations (recorded after implementation)

- **A backward clock step plus a restart can make recently expired things
  visible again.** Visibility of idempotency keys and finished jobs is defined
  by timestamps, and collection is not logged (5.6). Live, something collected
  at 12:00 stays gone; but if the wall clock is then stepped back to 11:50 and
  the server restarts, replay rebuilds the entry and, at 11:50, it has not
  expired yet. The effects are benign — a key deduplicates a little longer, a
  finished job answers `STATUS` again for a while — and they need a backward
  step *and* a restart inside the retention window. The model-based test
  therefore simulates forward steps only; backward steps are covered by
  targeted tests.
- **A backward step lengthens leases** by up to the size of the step, because
  the persisted expiry is now further away and baton never shortens a lease
  behind a worker's back. A forward step cannot shorten them either (grace).
- **Re-deriving timers is O(jobs).** A clock step or a restart walks every job
  once. At a million jobs that is a pause of a few hundred milliseconds, for an
  event that should be rare; M5 measures the same walk as part of recovery.
- **Steps below the threshold (1 s) are not corrected.** Timers set before such
  a step fire that much early or late relative to the new wall clock.
- **`DLQ.RETRY queue ALL` logs one record per job** and `DLQ.LIST` walks an
  ordered set to reach its offset; both are operator commands on what should
  be a small set, not hot paths.
- **The grace period is one number for all leases.** A worker whose lease was
  hours long and a worker whose lease was 100 ms get the same grace after a
  restart.

## 8. M5 — Snapshots and log compaction

Without compaction the log grows forever and recovery replays all of it. A
snapshot is the durable state as of some LSN; once it is safely on disk, the
log before that LSN can go, and recovery becomes "load the snapshot, replay the
tail".

### 8.1 The hard part: a consistent copy without stopping the world

A snapshot must describe one instant (everything up to LSN *X*, nothing after),
but writing it takes seconds and the event loop must keep mutating state
meanwhile. Three ways to get a consistent view were evaluated:

**fork() and copy-on-write** (what Redis does). The child sees memory frozen at
the fork and writes it out at leisure. The pause is only the fork itself,
which copies page tables: roughly 10–20 ms per GB.
Rejected because:

- *baton is multithreaded.* After `fork()` only the calling thread exists in
  the child. If the log thread held a lock at that instant — the allocator's,
  the logger's, the stats mutex — it is held forever in the child. POSIX
  permits only async-signal-safe calls there; serializing a state machine is
  not that. glibc happens to reset its malloc locks in the child, which is why
  this works for Redis in practice, but it is an implementation detail, not a
  contract, and it does not extend to our own mutexes.
- *It cannot be tested the way baton tests durability.* The whole crash-testing
  approach rests on `SimFs`, an in-process file system; a forked child's writes
  to it are invisible to the parent. Snapshot writing would be the one
  durability path without crash-image tests, and ThreadSanitizer does not
  support forking a threaded process either.
- Copy-on-write can double memory under write load, invisibly, and the fork
  pause still grows with the heap.

**Incremental snapshots** (persist only what changed since the last one). The
smallest pause and the least I/O, but recovery then needs a chain of deltas,
each a new way for one bad file to make everything after it unusable, plus a
merge step to keep the chain short. Too much machinery — and too many new
failure modes — for the problem at hand. On the roadmap.

**Copy, then write in the background** — chosen. The event loop makes a
consistent in-memory copy of the durable state (`State::capture_image()`), and
a background thread serializes that private copy, fsyncs it and swaps it in.
The thread shares nothing mutable with the loop, so there are no locks and
nothing for TSan to find; it runs on `SimFs` like everything else, so every
step of the write protocol gets crash-image tests.

The cost is that the pause is the time to make the copy, which grows with the
number of live jobs. Two things keep it small: payloads are immutable and
reference-counted (`SharedBytes`, put in place in M2 for exactly this), so
copying a job copies ~100 bytes of metadata and bumps a counter, however large
the payload; and only durable fields are copied, never indexes or timers. The
pause is measured (section 8.6), not assumed.

### 8.2 File format

```
snapshot-00000000000001048576.snap     the number is the LSN the snapshot covers
```

A header (magic `BATONSNP`, version, LSN, creation time, CRC) followed by
**chunks framed exactly like log records** (`length | crc32c | type | sequence
| payload`, reusing the log's encoder and parser) and terminated by an end
chunk carrying totals:

| Chunk | Contents |
|---|---|
| meta | id and token counters, the queue table with per-queue totals |
| jobs | up to 4,096 jobs each; as many chunks as needed |
| idempotency | up to 4,096 keys each |
| end | chunk, job and key counts — a file without it is incomplete |

Chunking means neither writing nor loading ever holds the whole serialized
state in memory, and a checksum failure names the chunk. Sequence numbers must
be contiguous and the end chunk's totals must match, so truncation at a chunk
boundary cannot pass for a complete snapshot.

### 8.3 Write protocol

1. The loop captures the image and notes `X = last LSN`. It hands the image to
   the snapshot thread **only once the log has committed `X`**: a snapshot must
   never be ahead of the durable log, or a crash would leave a state whose own
   history is missing.
2. The thread writes `snapshot-X.tmp`, fsyncs it, renames it to
   `snapshot-X.snap`, and fsyncs the directory. Only now does the snapshot
   exist as far as recovery is concerned.
3. The thread deletes what is no longer needed and fsyncs the directory again:
   snapshots other than the newest two, and log segments that lie entirely at
   or before the **older** retained snapshot.

**Two snapshots are kept**, and the log needed by the older one, so that a
newest snapshot that turns out to be unreadable (bit rot; the write protocol
itself cannot produce a torn `.snap`) still leaves a way to recover: the older
snapshot plus a longer tail. With a single snapshot, no segment is deleted.
The price is disk space: up to two snapshots plus the log since the older one.

A crash at any point leaves either the old world (a stray `.tmp`, deleted at
startup) or the new one. Deletions happen strictly after the snapshot that
makes them safe is durable.

### 8.4 Recovery

Newest snapshot first: validate everything (header, every chunk's CRC,
sequence, totals) while loading into a fresh `State`; on any failure log a
warning and try the older snapshot; if none loads, fall back to replaying the
log from the beginning. Then replay the log after the snapshot's LSN through
the usual `apply`, and `end_replay()`. If the segments needed by whichever
snapshot was chosen are missing, the log layer refuses (section 4.4), exactly
as it would for any other gap.

Log recovery starts at the last segment that begins at or before the first LSN
it needs. Segments entirely behind the snapshot are leftovers of a compaction
that a crash interrupted; they are ignored, not validated. This was a bug
found by the crash tests, not foresight: compaction unlinks several segments
and then fsyncs the directory once, and a power failure in between may persist
*some* of the unlinks — say, segment 2 gone and segment 1 still there. The
first version of recovery checked continuity across every segment it found and
refused to start on that hole, although nothing it needed was missing.
(`SimFs` had to learn to keep an arbitrary subset of un-synced directory
operations to produce this; section 8.7.)

### 8.5 When

After every `--snapshot-every` bytes of log (default 256 MiB), at most one at a
time; and on the `SNAPSHOT` command. The snapshot thread is started per
snapshot and joined by the loop when it reports back through the wake pipe.

### 8.6 Measurements

`bench/storage_bench` measures, on a real file system: the event-loop pause
(`capture_image`) and the total snapshot time against the number of live jobs,
and recovery time against log length with and without a snapshot. Results and
the exact commands are in `docs/benchmarks.md`. In short, on the development
laptop: the pause is 0.15 ms at 10,000 live jobs, 3.5 ms at 100,000 and 126 ms
at a million; a snapshot makes recovery faster only once the log is longer
than the live state (69 ms instead of 579 ms for a million-job history of
which a tenth is still live), and no faster when everything is still live.

### 8.7 Test plan

Image round trip equals `State::serialize` equivalence (snapshot + tail ==
full replay, already part of the model test, now through real files); a
`SimFs` crash image after *every* file-system operation of a snapshot cycle
must recover to the same state as the log alone; torn, truncated-at-a-chunk-
boundary and bit-flipped snapshots fall back to the older one; segments are
never deleted before the covering snapshot is durable, and never with only one
snapshot; a snapshot is never finalized ahead of the committed LSN; TSan over
the hand-off; a fuzz target for the loader; through the wire, `SNAPSHOT`
followed by restart and by a power-loss image.

The crash-image test needed a harsher `SimFs`. Until M5 a crash image kept
either all or none of a directory's un-synced operations, which is kinder than
real file systems: they may persist some creates, renames and unlinks and not
others. In torn mode `SimFs` now keeps a seeded random subset of them (in
their original order), and the snapshot protocol is run against many seeds at
every crash point. That model is what found the recovery bug described in 8.4.
It still assumes what POSIX promises: a rename is atomic, and an fsynced
directory has everything before the fsync.

### 8.8 Limitations

- **The pause grows with the number of live jobs.** It is the time to copy
  ~170 bytes per job; measured in `docs/benchmarks.md`. Fine for hundreds of
  thousands of jobs, a visible stall at a million. The fix is an incremental
  capture (copy a slice per loop iteration, with copy-on-write for jobs touched
  meanwhile); it is on the roadmap because it is exactly the kind of cleverness
  that needs its own test campaign.
- **Memory while a snapshot is written**: the image (~170 bytes per job) plus
  every payload that finishes during the write, which the image keeps alive
  until the write ends.
- **A snapshot does not make recovery faster unless the log is longer than the
  state** (measured: loading a job costs about as much as replaying the three
  records of its life). What snapshots buy is a bound: recovery time and disk
  use stop growing with uptime.
- **Replay keeps every job of the replayed tail in memory** until it ends,
  because retention is only enforced once derived state exists. The tail is at
  most `--snapshot-every` of log, unless snapshots are turned off.
- **Disk use** is up to two snapshots plus the log since the older one: about
  twice the state plus twice `--snapshot-every` in the worst case.
- **Triggers are log growth and the command only**: no time-based trigger, no
  snapshot at shutdown (a restart replays at most `--snapshot-every` of log).
  A failed snapshot (disk full) is logged, counted in `INFO`, and retried at
  the next trigger, not sooner.
- **Snapshots are not compressed**, and are written at full speed: no I/O
  throttling to protect the log's fsync latency on a shared disk.
- If both retained snapshots are unreadable *and* the log before them has been
  compacted away, the server refuses to start. There is no partial recovery,
  deliberately: baton would rather stop than silently forget jobs.

## 9. M6 — The Python SDK

The server's guarantees end at the socket. The SDK's job is to carry them the
rest of the way — into a worker process that gets killed, loses its network or
runs a handler for an hour — without promising anything the protocol cannot
back. It lives in `sdk/python`, is installed from the repository (no PyPI), has
no dependencies, and supports Python 3.9+.

### 9.1 Its own RESP client

`baton.resp` is ~200 lines: an encoder, an incremental reply parser and a
blocking socket connection. redis-py works against baton (the integration
tests prove it and keep proving it), but the SDK does not use it:

- **Timeouts are part of the semantics.** A blocking `RESERVE` must wait
  `timeout_ms` plus a margin, everything else should fail fast. With a
  general-purpose client that is a per-call fight with a connection pool.
- **Retries are part of the semantics too** (next section). A library that
  transparently reconnects and resends — as Redis clients reasonably do for
  Redis — would turn one `ENQUEUE` into two jobs.
- Zero dependencies means a worker image needs nothing but Python.

A `Connection` is one socket and is not thread-safe; nothing in the SDK shares
one between threads. RESP2 only: the SDK never sends `HELLO 3`.

### 9.2 What is retried, and what is not

When a connection dies between sending a request and reading its reply, the
client cannot know whether the command ran. What the SDK does next depends on
the command, and the rule is: **retry only when a duplicate is impossible.**

| Command | After an ambiguous failure |
|---|---|
| `ENQUEUE` with a `key` | Retried on a fresh connection. The idempotency key makes the second attempt return the first one's job id. |
| `ENQUEUE` without a key | **Not retried.** `EnqueueUncertain` is raised: the job may or may not exist, and only the caller knows whether a duplicate or a loss is worse. The message says so and points at `key=`. |
| `ACK` | Retried. The server answers `OK` again to the one token whose `ACK` completed the job and `STALE` to any other, so the retry's answer is exact: `OK` means this worker's `ACK` counted. (The first version inferred that from `STATUS` instead, and the chaos harness caught it being wrong: section 10.6.) |
| `FAIL`, `HEARTBEAT` | Retried. A `HEARTBEAT` that ran twice is harmless; a `FAIL` that already ran makes the retry answer `STALE`, which the worker treats like any lost lease: the attempt is over either way. |
| `RESERVE` | Retried by the worker loop. A job leased to a connection that died before the reply arrived is delivered again when its lease expires — at-least-once absorbs it (the server-side fix is on the roadmap). |
| reads | Retried. |

Failures *before* anything was sent (connect refused, the server restarting)
are always retried, with capped exponential backoff, for up to
`connect_timeout` overall. The SDK does not generate idempotency keys behind
the caller's back: every key costs server memory for the 24-hour window, and a
random key would make the retry safe while defeating the purpose of keys
(deduplicating the *caller's* retries, which a fresh random key per call does
not).

Errors map to exceptions by code, never by message: `StaleLease`, `NotFound`,
`WrongState`, `LimitExceeded`, `AuthError`, `Unavailable`, `ProtocolError`,
all under `BatonError`.

### 9.3 The worker

```python
worker = baton.Worker(queues=["emails"], concurrency=8, lease_ms=30_000)

@worker.task("send_email")
def send_email(to, subject): ...

worker.run()          # until SIGTERM / SIGINT
```

**Task envelope.** A task job's payload is JSON:
`{"task": "send_email", "args": [...], "kwargs": {...}}`, produced by
`client.enqueue_task(queue, name, args, kwargs, **options)` — the task's
arguments are passed as a list and a dict, not splatted, so that they can never
collide with enqueue options such as `key=` or `priority=`. JSON because any language
can produce it and a human can read it in `STATUS … PAYLOAD`; pickle would tie
producers to Python and execute whatever the queue contains. A worker can also
register a raw handler per queue for payloads that are not envelopes.

**Threads, one connection each.** `concurrency` worker threads each own a
connection and loop: blocking `RESERVE` (short timeout, so shutdown is
prompt) → run the handler → `ACK` or `FAIL`. Threads rather than asyncio
because handlers are arbitrary user code that blocks; rather than processes
because the jobs of a queue system are mostly I/O-bound and one process is
simpler to supervise. CPU-bound work scales by running more worker processes —
the server does not care how many connections come from where.

**Heartbeats.** One more thread, with its own connection, extends the lease of
every in-flight job every `lease_ms / 3`. A handler may therefore run for
hours while a crashed worker's jobs come back after `lease_ms`. Two details
matter:

- If a heartbeat answers `STALE` or `NOTFOUND`, the lease is lost (expired
  during a long GC pause or a network partition, or the job was cancelled).
  The job's context is flagged — `job.lease_lost` is visible to the handler,
  which should stop at the next convenient point — and the SDK will not `ACK`
  or `FAIL` it: the result of a run that lost its lease is discarded, because
  another worker may already own the job.
- If heartbeats cannot reach the server at all, the SDK keeps trying; once the
  lease's known expiry passes without a successful extension the job is flagged
  the same way. The worker never assumes it still holds a lease it could not
  confirm.

This is fencing at the SDK level, not a guarantee of mutual exclusion: a
handler that ignores `lease_lost`, or is stuck in a system call, keeps running
while the job is redelivered. Handlers that write to systems which can compare
numbers should pass `job.token` along as a fencing token (section 5.5).

**Outcomes.** Return → `ACK`. Exception → `FAIL` with the exception's type and
message (the server backs off and retries up to `max_attempts`, then
dead-letters). `raise baton.Retry(in_ms=…)` chooses the delay;
`raise baton.Fatal(…)` dead-letters at once (`NORETRY`). A job for a task name
this worker does not know fails *with* retry: during a rolling deploy the next
attempt may land on a worker that knows it.

**Graceful shutdown.** SIGTERM or SIGINT: stop reserving; let running handlers
finish for up to `shutdown_timeout` (default 30 s) while heartbeats continue;
then stop. Jobs still running at the deadline are abandoned, not failed: the
process is about to die, their leases expire, and they are redelivered —
exactly what would happen after `kill -9`, which is the case the system is
built for anyway. A second signal skips the wait. A job handed over by a
`RESERVE` that was already in flight when the signal arrived is run like any
other rather than dropped, because dropping it would cost one of its attempts.

### 9.4 Doing something once: `baton.idempotent`

At-least-once delivery means a handler can run twice: the worker dies after the
side effect and before the `ACK`, or loses its lease mid-run. The honest
position is that **exactly-once effects cannot be bolted on from outside**: the
effect and the record of having done it must commit atomically, or there is a
window where a crash repeats the effect (record after) or loses it (record
before). The helper is built around that fact instead of hiding it:

- `Ledger` is a small durable set: an append-only, checksummed, fsynced file
  with `put_if_absent(key, value)` under an inter-process file lock. When the
  side effect *is* the ledger entry (or lives in a database that can do the
  same thing in a transaction), duplicates are impossible: the second run sees
  the key and skips. The chaos harness (M7) uses exactly this to check
  exactly-once effects under kills.
- `ledger.once(key, fn)` is for effects that live elsewhere (send the email,
  call the API): run `fn`, then record the key with its result; a later run
  with the same key returns the recorded result without calling `fn`. The
  docstring states the contract without decoration: *at least once, and at
  most once after it has been recorded*. A crash between `fn` and the record
  repeats `fn`. If that is not acceptable, the remote system needs an
  idempotency key of its own — pass it `job.id`.
- Entries carry the fencing token; an entry from a lower token than one
  already recorded for the key is rejected, so a zombie cannot overwrite the
  result of the run that superseded it.

The natural key is the job id (`job.id`), which is stable across redeliveries.

### 9.5 Test plan

Against the real server binary (pytest, no mocks of the server): task round
trip; every error code maps to its exception; a handler that outlives several
lease periods is delivered once (heartbeats); a worker process killed with
SIGKILL mid-job → the job is redelivered and completes elsewhere; SIGTERM lets
the running job finish, acks it, and exits 0 without taking new jobs;
cancelling a running job flips `lease_lost` and suppresses the `ACK`; server
restart mid-run (reconnect, keyed enqueue retried, unkeyed enqueue raises
`EnqueueUncertain`); `Retry` / `Fatal` / unknown task; the ledger under
concurrent processes and after a torn final write. The RESP codec gets plain
unit tests including byte-at-a-time feeding.

"The connection died after the request was sent" is produced on demand by a
small TCP proxy in the tests that forwards a request, waits until the server
has answered it, and then drops the reply and the connection. That makes the
ambiguous case deterministic: the tests can show both that a keyed `ENQUEUE`
ends up as exactly one job and that an unkeyed one really did execute when the
SDK refuses to guess.

### 9.6 Limitations

- **Threads and the GIL.** Handlers share one interpreter: fine for I/O-bound
  work, no speedup for CPU-bound work (run more processes). There is no asyncio
  API.
- **`lease_lost` is cooperative.** A handler that never looks, or is stuck in a
  system call, keeps running after its job has been given to someone else. The
  SDK discards its result; it cannot stop it. Use the fencing token downstream.
- **Lease bookkeeping is local and slightly optimistic**: the worker counts a
  new lease from the moment the `RESERVE` reply arrived, which is later than
  the moment the server granted it by one network latency. Heartbeats, counted
  from when they were *sent*, are conservative.
- **One heartbeat thread, serial.** With very many long jobs and a slow server
  the round can take longer than planned; the `lease / 3` interval is the
  slack.
- **A lease granted by a `RESERVE` whose reply was lost** is not released; the
  job waits out the lease (roadmap: server-side release on disconnect).
- **The `Ledger` is for one machine**: it relies on `flock`, which is not
  dependable on network file systems, and is POSIX-only. It never compacts —
  one small entry per key forever, all keys in memory — so long-lived users
  should rotate it (for instance one ledger per month, keyed by job id).
- **Shutdown while the server is unreachable** can take up to the reserve
  timeout plus the client's retry window before the worker threads notice.

## 10. M7 — The chaos harness

Unit tests prove rules one at a time, against a simulated disk. The chaos
harness asks the question the README's headline rests on, end to end, with
real processes, real sockets, a real file system and `kill -9`: *does the whole
system keep its promises while things die?* It lives in `chaos/`, is written in
Python on top of the SDK (so the SDK is under test too), and has two parts:
randomized **kill rounds** checked against five invariants, and four
deterministic **fault scenarios**.

### 10.1 A kill round

One round runs one seed for a fixed time:

- the **server**, with small segments and frequent snapshots
  (`--segment-size 64k --snapshot-every 256k`) so that rolling, snapshotting
  and compaction all happen many times per round, under fire;
- **producers**: processes that enqueue jobs with idempotency keys. Before each
  attempt they journal `intent <key>`, after each reply `ok <key> <job id>`,
  one fsynced line each. They deliberately re-enqueue old keys (their own and
  other producers') to give the deduplication something to do;
- **workers**: processes running `baton.Worker`. The handler's side effect is a
  `Ledger.put_if_absent(key)` — an fsynced ledger shared by all workers — and
  every run is logged (job, token, attempt, whether it was the run that landed
  the effect). Some jobs fail on their first attempt, some are poison
  (`Fatal` → dead-letter), most take a few milliseconds;
- a **rogue**: a client that reserves jobs with a short lease, never
  heartbeats, waits until `STATUS` proves the lease is gone (the job is pending
  again or on a later attempt), and then sends `ACK` with the old token. Every
  one of those must be answered `STALE`;
- the **orchestrator**, which follows a schedule derived from the seed:
  SIGKILL the server and restart it after a short outage; SIGKILL a worker and
  start a new one; SIGSTOP a worker for longer than its leases and SIGCONT it
  (a zombie: its jobs are redelivered while it still believes it owns them);
  ask for a snapshot.

Then producers stop, the killing stops, and the round **drains**: every
journaled job must reach a terminal state within a deadline. Workers and
server are stopped gracefully, and the checks run.

### 10.2 The invariants, and what each check actually reads

| # | Invariant | Evidence |
|---|---|---|
| 1 | Every enqueue that got `OK` ends up succeeded or dead | producer journals vs. `STATUS` of every journaled id after the drain (`--retain-finished 24h` keeps them visible): poison keys must be `dead`, all others `succeeded` |
| 2 | Never two valid leases on a job; stale-token ACKs always rejected | **the server's own log**, read offline by `baton-logcheck` (below); plus the rogue's tally (stale ACKs accepted must be 0); plus the workers' run logs: at most one successful ACK per job, and it carries the highest token any worker ever saw for that job |
| 3 | Handlers may run twice, each effect lands once | the ledger file is parsed *raw*, record by record, and must hold exactly one `done` entry per succeeded job's key — the `Ledger` class is not trusted to report on itself, since its in-memory map would hide a duplicate entry. The number of extra handler runs is reported |
| 4 | One key never creates two jobs | every `ok` line for a key, across all producers and retries, names the same job id; and the server's `total_enqueued` lies between the number of keys acknowledged and the number attempted |
| 5 | The server recovers after every kill | every restart must reach "accepting connections"; the wall time to get there and the server's own `recovery_ms` are recorded per kill |

`baton-logcheck` (`tools/logcheck.cpp`) is a second, deliberately tiny
implementation of the lease rules, independent of `State`: it walks the log —
starting from the oldest retained snapshot if compaction has removed the
beginning — and keeps only a map from job to its current token. It fails if
tokens are ever reused or decrease, if a lease is granted while another is
current, if a heartbeat, ack or failure is recorded for anything but the
current token, or if a lease-expiry record is dated before the lease's
recorded expiry. It opens the directory read-only: a log that would need
repair is reported, not repaired.

### 10.3 Reproducibility, honestly

The seed fixes the schedule (what is killed, when, for how long), the workload
mix and every process's own random choices. It cannot fix the operating
system's scheduling, so a seed reproduces a *scenario*, not an interleaving: a
failure may need several runs of its seed to show up again. To make up for
that, a failing round keeps everything — data directory, every process's
stderr, journals, run logs, the ledger, the log checker's output — and the
report names the first violated invariant with the job ids involved.

### 10.4 Fault scenarios

Deterministic, one server each, checked against what was acknowledged before
the fault:

| Fault | Injected how | Required behaviour |
|---|---|---|
| Torn log tail | SIGKILL, then append half a record to the newest segment (and separately: chop bytes off it) | starts; with garbage appended every acknowledged job is there and `recovered_torn_bytes > 0`; with bytes chopped — which destroys acknowledged data, something a real torn write cannot do — it still starts, and what it has is a gap-free prefix |
| Flipped bit mid-log | SIGKILL, flip one bit inside an early record | refuses to start, non-zero exit, names the segment and offset, leaves every file byte-for-byte untouched; with the bit restored it starts with everything |
| Torn snapshot | truncate the newest `.snap`; leave a stray `.tmp` | starts from the older snapshot, reports one rejected snapshot, loses nothing, removes the `.tmp` |
| Full disk | (a) `RLIMIT_FSIZE` below the segment size: the log write fails; (b) where a small file system is provided (`BATON_CHAOS_SMALL_FS`, a tmpfs in CI): fill it with ballast | (a) the server dies instead of acknowledging what it could not write, and after a restart without the limit every acknowledged job is there; (b) `ENQUEUE` is refused with `LIMIT` before the disk is full, everything else keeps working, and enqueueing resumes when space returns |

Fsync failure, a stalled disk and clock jumps are not injected here: they need
a file system or clock that lies on command, which is what `SimFs` and
`FakeClock` are for (guarantees D7, W9, L12).

### 10.5 Short and long runs

CI runs two seeds for 20 seconds each plus the fault scenarios on every push.
The long run (40 seeds × 45 s, more processes) is run locally before a release
and its results are recorded in `docs/testing.md`.

### 10.6 What it found

Details and regression tests are in `docs/testing.md`; the short version is that
the harness paid for itself within its first minutes, and that the most useful
part was the one that shares no code with the server.

1. **Leases expired up to a millisecond early.** Only `baton-logcheck` could
   see this: it compares each expiry record with the expiry the log had
   promised. Timers run on the monotonic clock, expiries are wall-clock
   instants, and the two tick over at different moments. `State` now re-arms a
   lease timer that fires before its wall-clock time, as it already did for
   delayed jobs.
2. **An SDK inference that could not be made sound.** A resent `ACK` that got
   `STALE` was resolved by asking `STATUS`; a zombie whose successor had
   completed the job concluded that its own `ACK` had counted. The server had
   done nothing wrong — but no client can tell those cases apart, so the
   protocol changed instead: `ACK` is idempotent for the completing token
   (section 5 — a succeeded job keeps that token), and the SDK infers nothing.
3. **A blind spot in the harness itself**, found by `chaos/selftest.sh`: the
   first rogue client used its dead token at a moment when even a server
   without token checks answers `STALE`. It now waits until a successor holds
   the job.

### 10.7 Limitations

- **A seed is a scenario, not an interleaving** (10.3). There is no
  deterministic simulation of the whole system; that would need the server's
  I/O and threads behind an abstraction it does not have.
- **Process kills, not power failures.** SIGKILL never tears a write or loses
  un-synced data, because the kernel survives. Power-loss behaviour is covered
  by `SimFs` crash images and, coarsely, by the torn-tail and torn-snapshot
  scenarios.
- **One machine, loopback network.** No partitions, no delays, no packet loss;
  a paused process is the closest thing to a partition here.
- **The full-disk scenario's second half needs a small file system** and is
  skipped (and says so) without one.
- **Invariant 3 is about the `Ledger`.** Effects that live elsewhere get
  at-least-once, as section 9.4 says; the harness reports how often handlers
  really ran more than once so that the number is not abstract.

## 11. M8 — Measuring

The rule is that every number in the README comes from a script in `bench/` and
that unflattering numbers stay in. This section is about making the numbers
mean something.

### 11.1 What is measured, and with what

`bench/loadgen` is a load generator written for this purpose, because the
questions are specific: what does *waiting for the disk* cost, what does group
commit buy back, and how long does a job wait for a worker. It speaks baton's
protocol and — for the comparison — Beanstalkd's and Faktory's, so all three
are driven by the same code, threads, clocks and histograms.

- **Closed loop** (each connection keeps `--depth` requests in flight) finds
  capacity. It says nothing trustworthy about latency: when the server slows
  down, a closed-loop generator politely sends less.
- **Open loop** (`--rate`) sends on a fixed schedule and measures each request
  from the moment it was *due*. A server that stalls for 100 ms is charged for
  every request that should have been sent meanwhile — the correction for what
  Gil Tene named coordinated omission. The generator waits with nanosecond
  timeouts (`ppoll`) rather than spinning, so that it does not compete with the
  server for the CPU it is measuring.
- **Pickup latency** travels in the payload: the producer stamps the due time,
  the worker subtracts it on receipt. Both ends are one process, one clock.
- **Histograms** are log-linear (`common/histogram.h`, at most 3% high, tested
  against sorted arrays), one per thread, merged at the end; nothing is recorded
  during warm-up.
- **Server-side numbers ride along**: the group-commit batch sizes and the
  `fdatasync` p50/p99/max that the server itself observed during each run are
  printed next to the client-side figures. Under `--fsync always` throughput at
  low connection counts *is* the disk's fsync latency, and tail latency *is* its
  fsync tail; the tables are meant to show that, not leave it to faith.

Every experiment starts a fresh server on an empty data directory on a real
file system; the machine, kernel, file system, compiler, commit and date are
printed with the results.

### 11.2 A fair comparison

"Jobs per second" without "and what survives a power cut" compares nothing, so
the comparison is grouped by what an acknowledgement means:

| Group | baton | Beanstalkd | Faktory |
|---|---|---|---|
| fsync before every acknowledgement | `--fsync always` | `-b … -f 0` | not available |
| fsync every 50 ms | `--fsync interval --fsync-interval 50ms` | `-b … -f 50` (its default) | not available |
| snapshots only | not available | — | its defaults: embedded Redis, RDB `save 30 5` / `save 120 1`, no AOF |

All three run the same way — in a container, on the host's network (no
port-mapping proxy in the path), data on a fresh named volume on the same disk
— and are driven from the host. Faktory has no setting that matches either of
the first two groups, so it stands alone, labelled with what its
acknowledgement means: a crash can lose up to 30 seconds of acknowledged jobs.
It is in the table because people will compare anyway, and it is *expected* to
win on raw throughput: it does no disk I/O per job and runs on many cores.

What "end to end" costs differs too, and the docs say so: baton makes three
things durable per job (enqueue, lease, acknowledgement); Beanstalkd logs the
put and the delete but not the reserve; Faktory none of them.

### 11.3 Limitations

- **One machine, a laptop, under WSL2.** Load generator and server share 16
  hardware threads; the disk is a virtual disk whose `fdatasync` takes 2–4 ms
  and occasionally 50–300 ms. Absolute numbers will differ elsewhere — faster
  fsync moves every `always` row — but what grows with what should carry over.
- **Short runs** (10 s after 2 s of warm-up) on a fresh server: no long-term
  effects such as snapshots of a large state (section 8 measures those
  separately), fragmentation or page-cache pressure.
- **A closed-loop generator with one thread per connection** tops out at a few
  hundred connections; baton's 10,000-connection limit is tested for
  correctness, not measured for throughput.
- **Payloads are 256 bytes** unless stated. Larger payloads shift the cost from
  fsync count to bytes written.
- **The competitors run with the settings in `bench/compare/run_compare.py`
  and nothing else was tuned** — for any of the three, baton included.

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
- Aborting on a failed `fsync` instead of retrying it follows the lesson
  PostgreSQL drew from "fsyncgate" (section 4.3). Hierarchical timing wheels are
  from Varghese & Lauck (section 5.7). Fencing tokens as described by Martin
  Kleppmann (section 5.4). Snapshot by fork is what **Redis** does and the
  reason section 8.1 discusses it; the two-snapshots-then-compact rule is
  baton's own.
- Log-linear latency histograms follow Gil Tene's **HdrHistogram**, and
  measuring open-loop latency from the intended send time is his correction for
  "coordinated omission" (section 11.1).
- The chaos harness's approach — real processes, kill them, check invariants
  from the evidence, and test the checker with planted bugs — owes its attitude
  to **Jepsen**.
