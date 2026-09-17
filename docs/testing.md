# Testing

baton's bar is that every guarantee in `docs/guarantees.md` is proven by a test.
This document describes the layers of testing, how to run them, and (from M7)
the chaos harness and its recorded results.

## Layers

| Layer | What it proves | Where | Status |
|---|---|---|---|
| Unit tests (GoogleTest) | each module against its contract, incl. death tests for aborts | `tests/unit/` | from M0 |
| Crash-image tests | every committed record survives a crash at any point; torn tails are repaired; damage is refused | `tests/unit/log/` on top of `SimFs` | M1 |
| Mutation check | the durability tests fail when a durability rule is broken | `scripts/mutation-check.sh` | M1 |
| Model-based tests | random operation sequences keep state invariants; live state == replayed state | `tests/unit/state/` | M2 |
| Fuzzing (libFuzzer) | parsers and decoders never crash or over-read; recovery never delivers damaged data | `fuzz/` | from M1 |
| Integration tests (pytest) | the real binary with redis-cli, redis-py and the SDK | `tests/integration/`, `sdk/python/tests/` | M3, M6 |
| Chaos harness | invariants hold under repeated SIGKILL and injected disk faults | `chaos/` | M7 |

## Running

```bash
scripts/check.sh              # format check, ASan+UBSan, TSan, clang-tidy: what CI runs
scripts/check.sh asan         # one step only
scripts/check.sh fuzz         # build the fuzzers and run each for 30 s
scripts/mutation-check.sh     # a few minutes; run after touching src/log or its tests
ctest --preset asan -R Recovery   # a subset (after configuring and building the preset)
```

All unit tests run under AddressSanitizer + UndefinedBehaviorSanitizer and again
under ThreadSanitizer, locally and in CI. UBSan is configured with
`-fno-sanitize-recover=all`, so undefined behaviour fails the run instead of
printing a warning.

## SimFs: deterministic crash testing

Durability bugs hide in the gap between "written" and "on disk". `SimFs`
(`src/testing/sim_fs.h`) is an in-memory file system that models exactly that
gap: appended bytes are volatile until `sync()`, and directory changes are
volatile until `sync_dir()`. It can produce the file system as it would look
after three kinds of crash:

| Mode | Models | What survives |
|---|---|---|
| `kKeepUnsynced` | process crash (SIGKILL, OOM) | everything written |
| `kLoseUnsynced` | clean power loss | only synced bytes and synced directory entries |
| `kTorn` | power loss mid-writeback | synced bytes, plus a random prefix of each unsynced tail whose end may be garbage; of each directory's unsynced creates, renames and unlinks, a random subset (applied in their original order) |

The directory half of `kTorn` was added in M5 and is deliberately harsher than
ordered-mode ext4: an application that fsyncs a directory once after several
unlinks has no promise about which of them a crash keeps. It immediately found
a recovery bug (design doc, 8.4). What `SimFs` still assumes is what POSIX
promises: `rename` is atomic, and `fsync` on a directory persists everything
done to it before.

A crash image can be captured after the *n*-th file-system operation while the
code under test keeps running, together with what had been acknowledged at that
instant. `tests/unit/log/crash_test.cpp` does this for 6 scenarios × 150 seeds
and checks that recovery succeeds, every acknowledged record is back intact,
LSNs are contiguous, and the repaired log accepts new writes. `SimFs` has its
own tests (`tests/unit/testing/`), because a simulator that is too forgiving
would make everything built on it meaningless.

## Model-based testing of the state machine

`tests/unit/state/model_test.cpp` drives the `Engine` with long random command
sequences shaped like real traffic — several queues, priorities, delays,
duplicate idempotency keys, workers that ack, fail, heartbeat, go silent, and
come back as zombies — for 12 seeds × 2,500 steps. Three layers of checking:

1. **After every step**, `State::check_invariants()`.
2. **A tiny client-side model** asserts the contract: tokens only increase; only
   the current token can act on a job; a key maps to one job for its window.
3. **Periodically, replay equivalence**: a fresh `State` rebuilt from the logged
   records must serialize to the same bytes as the live one — and so must a
   state deserialized from a mid-run "snapshot" plus the records after it.

Every `EngineTest` also runs checks 1 and 3 in its `TearDown`, so each
behavioural test doubles as a recovery test.

## Mutation check: testing the tests

A durability test that cannot fail proves nothing. `scripts/mutation-check.sh`
breaks one rule at a time in a scratch copy of the tree and requires the test
suite to fail each time:

| Mutant | Rule it breaks |
|---|---|
| segment created without directory fsync | D6 |
| commit announced before fsync | D1 |
| segment rolled before the old one is durable | D6 |
| torn tail not truncated | D3 |
| mid-log damage treated as a torn tail | D4 |
| record checksum not verified | D4, D5 |
| engine accepts a stale lease token | L3 |
| apply reads the clock instead of the record | L1 |
| lease token counter not advanced | L2 |
| attempts never counted | L5 |
| ready order ignores priority | L6 |
| idempotency window never expires | L7 |
| idempotency key not recorded | L7 |
| replies sent before their records are durable | W1 |
| parked RESERVEs served newest first | W5 |
| commands allowed without AUTH | W8 |
| restart expires leases without a grace period | L11 |
| wall-clock jumps go unnoticed | L12 |
| clock jump expires leases without a grace period | L12 |
| DLQ retry keeps the spent attempts | L13 |
| snapshot renamed into place without fsync | S2 |
| snapshot rename not made durable | S2 |
| log compacted on the strength of the newest snapshot | S3 |
| snapshot end-chunk totals not checked | S4 |
| snapshot chunk sequence not checked | S4 |
| snapshot started before the log is durable up to it | S6 |
| leftover segments behind the snapshot are validated | S2 |
| repeated ACK accepted for any token | L14 |
| lease expired before its wall-clock expiry | L4 |

Last run: 2026-09-17, all 29 killed.

The check earns its keep: on its first run against the state machine, "idempotency
window never expires" **survived**. The expiry comparison in `ENQUEUE` was
correct but untested, because garbage collection always removed expired keys
before the handler could see them. The comparison only matters when collection
lags (section 5.11 of the design doc), so a test for exactly that case was added.
It happened again in M5: "snapshot end-chunk totals not checked" survived,
because the only test with wrong totals also had a wrong chunk count, which a
different check caught first. The totals exist to catch a chunk spliced in
from another snapshot (valid checksum, valid sequence number), so that is now
what `SnapshotDamageTest.ChunkSplicedInFromAnotherSnapshotIsRejected` does.

## Fuzz targets

| Target | Input | Oracle |
|---|---|---|
| `fuzz_log_segment` | arbitrary bytes as the newest log segment | no crash or sanitizer report; delivered LSNs are contiguous; a repair is idempotent |
| `fuzz_log_damage` | a program of damage operations (bit flips, truncation, garbage, deleted segments) applied to a valid multi-segment log | recovery either refuses or delivers an intact prefix of the original records — never a damaged record, never one out of order |
| `fuzz_resp_parser` | a chunk size and a byte stream from an untrusted client | no crash or over-read; buffering stays bounded (oversized arguments are skipped as they stream in); fed in chunks or all at once, the same requests or the same error come out |
| `fuzz_record_decode` | a type byte and a record payload | decoding never crashes or over-allocates; whatever decodes re-encodes to a stable form |
| `fuzz_state_apply` | a sequence of records, as recovery would read them from a checksummed but hostile log | `apply` accepts a record or rejects it *and changes nothing*; afterwards all invariants hold and the state survives a serialize/deserialize round trip |
| `fuzz_state_deserialize` | arbitrary bytes as a serialized state (the body of a snapshot) | rejected, or loads into a state whose invariants hold and that re-serializes |
| `fuzz_snapshot_load` | a snapshot file, either raw bytes or a list of chunks that the harness frames with valid checksums and sequence numbers | rejected, or every byte was read, the loaded state's invariants hold, and a snapshot written from it loads back to the identical state |

`fuzz_log_damage` is structure-aware because raw-byte fuzzing cannot get past
the record checksums. Its garbage comes from a PRNG seeded by the input rather
than from the input itself: libFuzzer's compare tracing can learn checksum
values, and with raw control it could forge a record with a valid CRC, which is
not a recovery bug (a checksum is not a MAC).

CI runs every target for 60 seconds per push; reproducers are uploaded as
artifacts on failure. Longer local runs are recorded here as they happen.

## Chaos harness

`chaos/` runs the real server, producers, `baton.Worker` processes and
deliberately misbehaving clients against each other while an orchestrator kills
things, and then checks five invariants from the evidence they left behind.
Design doc section 10 explains the architecture; this section is how to run it,
what it has found, and what the long run looked like.

```bash
chaos/run.py --seeds 1-3 --duration 20       # kill rounds (CI runs seeds 1,2 plus --faults)
chaos/run.py --faults                        # torn log tail, flipped bit, torn snapshot, full disk
chaos/run.py --seeds 17 --duration 45 --keep # reproduce one seed, keep every file
chaos/selftest.sh                            # prove that the harness can fail
```

**What a round does.** The seed fixes a schedule of actions, one every 0.3–1.5
seconds: SIGKILL the server and restart it after up to a second of outage (40%
of actions); SIGKILL a worker and start a fresh one (30%); SIGSTOP a worker for
1.5–2.5 lease periods and SIGCONT it, which makes it a zombie whose jobs were
handed to others (20%); ask for a snapshot (10%). The server runs with 64 KiB
segments and a snapshot every 96 KiB of log, so segment rolls, snapshots and
compaction happen dozens of times per round, under fire. Afterwards producers
stop, the queue must drain, everything is shut down gracefully, and the checks
run.

| # | Invariant | How it is checked |
|---|---|---|
| 1 | No enqueue that got `OK` is lost | every job id in the producers' fsynced journals must be `succeeded` (or `dead`, for the poison jobs) |
| 2 | Never two valid leases; stale tokens always rejected | `baton-logcheck` replays the **whole** log of the round (the orchestrator hard-links every segment into an archive before compaction can delete it) through an independent model of the lease rules; two rogue clients let their leases die, wait until a successor holds the job, and then try `ACK`, `HEARTBEAT` or `FAIL` with the dead token — all must get `STALE`; and the workers' logs must show at most one acknowledged run per job, under the highest token anyone saw |
| 3 | Each effect lands once, however often the handler runs | the ledger file is parsed raw — exactly one entry per succeeded job's key, none for poison jobs. Extra handler runs are counted and reported |
| 4 | One key, one job | every `OK` for a key, across producers and retries, names the same id; the server's job count lies between the keys acknowledged and the keys attempted |
| 5 | The server recovers after every kill | every restart must answer; wall time to the first reply and the server's own `recovery_ms` are recorded |

Plus process health: the server and every worker exit 0 on SIGTERM, and the
server's log contains no failed check, sanitizer report or fatal error.

**Reproducing a failure.** `--seeds N --keep` replays the same schedule and
workload. It cannot replay the kernel's scheduling, so a failure may need a few
runs to reappear; to make up for that, a failing round keeps everything
(`chaos/README.md` lists the files) and the report names the violated invariant
and the job ids involved.

### Results

**CI** runs seeds 1 and 2 for 20 seconds each plus the fault scenarios on every
push (about two minutes).

**The long run**, 2026-09-17, commit `a1cbb7f`, on the development laptop (AMD
Ryzen 9 8945HS, WSL2, ext4): 40 seeds × 45 s with 3 producers, 4 worker
processes × 4 threads and 2 rogues, plus the fault scenarios; 33.6 minutes.

```bash
BATON_CHAOS_SMALL_FS=/mnt/baton-small chaos/run.py --seeds 1-40 --duration 45 \
    --producers 3 --workers 4 --faults
```

| | |
|---|---:|
| rounds / rounds with a violated invariant | 40 / **0** |
| server SIGKILLs, each followed by a restart | 652 |
| worker SIGKILLs / worker SIGSTOPs of 1.5–2.5 lease periods | 456 / 296 |
| enqueues acknowledged to producers | 232,682 |
| … of those `succeeded` / `dead` (poison jobs) / anything else | 228,049 / 4,633 / **0** |
| idempotency keys attempted / acknowledged / jobs the server counts | 232,682 / 232,682 / 232,682 |
| handler runs | 257,955 |
| … extra runs (failed first attempts, redeliveries after kills and pauses) | 25,273 |
| … runs that found their effect already in the ledger | 922 |
| effects in the ledger / keys with more than one entry | 228,049 / **0** |
| most runs of a single job | 4 |
| deliveries whose result the SDK discarded because the lease was lost | 865 |
| dead-token requests sent by the rogues / while a successor held the job / accepted | 1,157 / 1,115 / **0** |
| log records replayed by `baton-logcheck` / leases / lease expiries / violations | 778,275 / 259,774 / 3,973 / **0** |
| snapshots written / log segments compacted away, under fire | 2,741 / 3,638 |
| restarts that recovered from a snapshot plus the log tail | 652 of 652 |
| restart, exec to first reply: median of round medians / worst | 11.4 ms / 76 ms |
| server-reported recovery time, worst / most records replayed in one recovery | 18 ms / 370 |
| longest drain after the killing stopped | 4.8 s |
| fault scenarios passed | 4 of 4 |

Restarts are fast here because frequent snapshots keep the replayed tail tiny
(at most 370 records); `docs/benchmarks.md` has recovery times for a million
jobs. The first attempt at this run passed all 40 rounds too and then crashed
in the last fault scenario because the tmpfs it needs had disappeared with a WSL
reboot — before the summary was written. The harness now saves its summary
after every round and reports a scenario that throws as a failure instead of
dying with it.

### The harness tests itself

A harness that has only ever printed "ok" has proven nothing. `chaos/selftest.sh`
plants a bug in a scratch copy of the tree, builds the server, and requires the
round to fail for the right reason:

| Planted bug | Caught by |
|---|---|
| engine and `apply` stop comparing lease tokens | invariant 2: the rogues' stale `ACK`s are accepted, and `baton-logcheck` reports "ack … with stale token … was accepted" |
| the lease-token counter stops advancing | invariant 5: recovery's own consistency checks refuse the log, so the server does not come back |
| idempotency keys are not recorded | invariant 4: one key acknowledged with several job ids |

The first version of the rogue **missed** the first of these. It fired its stale
`ACK` the moment its lease was gone — when the job is usually waiting out its
retry backoff and nobody holds it, a state in which even the broken server
answered `STALE`. A zombie is dangerous when its successor is at work, so that is
now the moment the rogue waits for. Without the self-test, invariant 2 would
have been checked by a client that could not see it fail.

### Bugs found by the harness

Both were found within the first minutes, both by evidence the unit tests did
not have, and both have regression tests.

1. **Leases were expired up to a millisecond early.** `baton-logcheck` reported
   `the lease of job 78 (token 82) was expired at …117 ms, before its recorded
   expiry at …118 ms`. The expiry timer runs on the monotonic clock, the expiry
   is a wall-clock instant, and the two clocks tick over at different moments,
   so a timer could fire while the wall clock still said "one millisecond to
   go". A heartbeat arriving in that millisecond would have been refused.
   Delayed jobs already re-armed their timer in this case; leases now do too
   (`ClockJumpTest.ALeaseIsNeverExpiredBeforeItsWallClockExpiry`; mutant "lease
   expired before its wall-clock expiry").
2. **Two workers both believed they had acknowledged the same job.** The server
   was right (its log showed one accepted `ACK`); the SDK was wrong. After a
   connection failure it resent the `ACK`, got `STALE`, asked `STATUS`, saw
   `succeeded` and concluded "my first `ACK` went through" — but a zombie whose
   job had been completed *by its successor* sees exactly the same thing. No
   client-side inference can tell the two apart, so the fix went into the
   protocol: `ACK` is now idempotent for the one token that completed the job
   and `STALE` for every other, and the SDK infers nothing
   (`EngineTest.AckIsIdempotentForTheTokenThatCompletedTheJobAndOnlyForIt`,
   `test_client.py::test_a_resent_ack_never_claims_another_workers_success`,
   the model test; mutant "repeated ACK accepted for any token").

### Fault scenarios

| Fault | Result (from `chaos/run.py --faults`) |
|---|---|
| Torn log tail | half a record appended after SIGKILL: starts, reports the 22 torn bytes, all 300 acknowledged jobs present. 777 bytes chopped off (which destroys acknowledged records, as no real torn write can): starts with a gap-free prefix, 317 of 320 jobs, ids continue after it |
| Flipped bit mid-log | refuses to start (exit 1), names the segment, leaves every file byte-for-byte untouched; with the bit restored, all 600 jobs are back |
| Torn snapshot | newest snapshot cut at 2/3 plus a stray `.tmp`: rejects it, recovers from the older snapshot plus 300 log records, all 800 jobs present, `.tmp` removed |
| Full disk | log write fails (`RLIMIT_FSIZE`, as `ENOSPC` would): the server aborts instead of acknowledging, and after a restart every acknowledged job is there. On a 24 MiB tmpfs filled with ballast: `ENQUEUE` is refused with `LIMIT` while `RESERVE` and `ACK` keep working, and enqueueing resumes when space returns |

Not injected here: fsync errors, a stalled disk, clock jumps. They need a file
system or a clock that lies on command, which is what `SimFs` and `FakeClock`
are for (guarantees D7, W9, L12).

## Rules

- A failing test is fixed in the code, never skipped, disabled or weakened.
- Every bug found by fuzzing or chaos gets a regression test before the fix.
- Tests that involve job timers use `FakeClock`. No test relies on a fixed
  sleep; the few that wait for another thread (the log thread's commit, the
  background fsync of the `interval` policy) block on a condition with a
  generous timeout.
