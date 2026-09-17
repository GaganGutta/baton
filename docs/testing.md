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

Last run: 2026-09-17, all 27 killed.

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

## Rules

- A failing test is fixed in the code, never skipped, disabled or weakened.
- Every bug found by fuzzing or chaos gets a regression test before the fix.
- Tests that involve job timers use `FakeClock`. No test relies on a fixed
  sleep; the few that wait for another thread (the log thread's commit, the
  background fsync of the `interval` policy) block on a condition with a
  generous timeout.
