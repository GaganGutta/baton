# Guarantees

This file states exactly what baton promises. The rule of the project is that
**every promise here is checked by a test**, and the README never claims more
than this file does. Each row names the test (or chaos invariant) that would
fail if the promise were broken.

A promise appears here only once the code and the test both exist. Planned
promises live in `PLAN.md` until then.

## Durability

These are promises of the durable log (M1). The server-level promise built on
them — "no reply before its records are durable" — is added with M3.

"Committed" means the log has announced an LSN as durable under the configured
fsync policy. Tests live in `tests/unit/log/` unless noted.

| # | Promise | Checked by |
|---|---|---|
| D1 | With `fsync=always`, a record is committed only after an `fdatasync` covering it has returned: at the instant a commit is announced, a power failure cannot lose it. | `LogWriterTest.AlwaysPolicyCommitsOnlyWhatSurvivesPowerLoss`; mutant "commit announced before fsync" in `scripts/mutation-check.sh` |
| D2 | Every committed record survives a crash at any point, including torn and garbled final writes and lost directory updates. `always`: power loss and process crashes. `interval`: process crashes only (see D8). | `LogCrashTest.CommittedRecordsSurviveACrashAtAnyPoint` — 6 scenarios × 150 seeds of randomized crash images |
| D3 | A torn final write is repaired by truncating it, wherever the tear falls; a torn segment creation is removed. The repair is itself durable and idempotent. | `RecoveryTest.TruncationAtEveryByteOfTheFinalRecordIsRepaired`, `.GarbageAfterTheLastRecordIsATornTail`, `.ZeroFilledTailIsATornTail`, `.TornSegmentCreationIsRemoved`; fuzz target `fuzz_log_segment` |
| D4 | Damage anywhere but the tail makes baton **refuse to start** instead of dropping data: a flipped bit in any mid-log record, damage in an older segment, an LSN gap or duplicate, a missing or overlapping segment, a header that contradicts its file name. Refusing modifies nothing on disk. | `RecoveryTest.BitFlipAnywhereInAMidLogRecordIsRefused` (every bit of a record), `.DamageInANonFinalSegmentIsRefusedEvenAtItsTail`, `.ValidRecordAfterGarbageIsRefused`, `.LsnGapInsideASegmentIsRefused`, `.MissingMiddleSegmentIsRefused`, `.OverlappingSegmentsAreRefused`, …; fuzz target `fuzz_log_damage` |
| D5 | Recovery only ever delivers records exactly as written, in LSN order, with no gaps — or nothing at all. | `fuzz_log_damage` (oracle: delivered records are an intact prefix of what was written), `LogCrashTest` |
| D6 | A new segment is durable — contents *and* directory entry — before any record is written to it, and the previous segment is durable before the log moves on. | `LogWriterTest.FirstSegmentIsDurableBeforeAnyRecord`, `.RollsSegmentsAndRecoversAcrossThem`, `LogCrashTest`; mutants "segment created without directory fsync" and "segment rolled before the old one is durable" |
| D7 | If `fsync` or `write` fails, baton aborts. It never retries an fsync and never acknowledges the affected records. | `LogWriterDeathTest.FsyncFailureAborts`, `.WriteFailureAborts`, `.FullDiskAborts` |
| D8 | With `fsync=interval`, a committed record survives a process crash immediately and a power failure once the next background `fdatasync` has run. **It may be lost to a power failure before that** — this is the documented trade-off, and it is tested as such. | `LogWriterTest.IntervalPolicyCommitsBeforeSyncAndStopMakesItDurable`, `.IntervalPolicySyncsInTheBackground` |
| D9 | A segment written by a newer, unknown format version is refused, never mistaken for garbage and deleted. | `RecoveryTest.SegmentFromANewerFormatIsRefusedAndKept` |

Limits of these promises are listed in `docs/design.md` section 4.7; the one
that matters most: damage confined to the very last record is indistinguishable
from a torn write and is repaired by truncation.

## Delivery and leases

| # | Promise | Checked by |
|---|---|---|
| | *(filled in from M3/M4 onwards)* | |

## Workflows

| # | Promise | Checked by |
|---|---|---|
| | *(filled in from M9 onwards)* | |

## What baton does **not** promise

- **Exactly-once delivery.** Delivery is at-least-once. A worker can finish a
  job and crash before its ACK is durable; the job will run again. baton gives
  handlers the tools (idempotency keys, fencing tokens, the SDK's idempotency
  helper) to make each *side effect* happen once. `docs/design.md` explains why
  no job system can do better.
- **Availability.** One node, no replication. When baton is down, the queue is
  down.
- **Protection from disk loss.** Durability means "survives process and OS
  crashes and power loss on a disk that honours fsync". It does not mean
  "survives losing the disk".
