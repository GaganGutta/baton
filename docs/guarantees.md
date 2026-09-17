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

These are promises of the state machine (M2), checked at the `Engine` API. M3
adds the promise that the wire protocol exposes them unchanged, M4 the rules
for restarts and clock jumps. Tests live in `tests/unit/state/`.

| # | Promise | Checked by |
|---|---|---|
| L1 | **Replay equivalence.** A state rebuilt from the logged records alone — or from a serialized state plus the records after it — is byte-for-byte identical to the live state. `apply` uses nothing that is not in the record: no clock, no RNG, no configuration. | every `EngineTest` (the fixture checks it in `TearDown`), `StateModelTest.RandomTrafficKeepsInvariantsAndReplaysExactly` (12 seeds × 2,500 steps), `StateSerializationTest.*`; mutant "apply reads the clock instead of the record" |
| L2 | **Fencing.** A job has at most one valid lease. Lease tokens increase server-wide and are never reused, including after a dead job is retried. | `EngineTest.TokensIncreaseAcrossJobsAndAttempts`, `StateModelTest`; mutant "lease token counter not advanced" |
| L3 | **Zombies are rejected.** Once a lease has ended — expiry, completion or cancellation — its token can no longer `ACK`, `FAIL` or `HEARTBEAT` the job, whether or not the job has been leased again. The current holder is unaffected. | `EngineTest.ZombieWorkerIsFencedOffAfterItsLeaseExpires`, `.StaleTokenIsRejectedEvenBeforeTheJobIsLeasedAgain`, `.CancelWorksInEveryLiveState`, `StateModelTest`; mutant "engine accepts a stale lease token" |
| L4 | **Heartbeats extend leases**; without one the lease expires on time. An expiry is a logged fact that consumes an attempt and can dead-letter the job. | `EngineTest.HeartbeatKeepsALeaseAlive`, `.LeaseExpiryConsumesAnAttemptAndCanKillTheJob` |
| L5 | **Retries** use exponential backoff with full jitter, capped: the delay is uniform in `[0, min(cap, base·2^(attempt−1))]`, drawn once and logged. After `max_attempts` leases the job moves to the dead-letter queue. `NORETRY` and `RETRY_IN` override. | `EngineTest.JitterIsFullAndStaysWithinBounds`, `.BackoffCeilingDoublesAndIsCapped`, `.ExhaustedAttemptsSendTheJobToTheDeadLetterQueue`, `.NoRetryAndRetryInOverrideTheBackoff`; mutant "attempts never counted" |
| L6 | **Delayed jobs** are never handed out before `run_at`; ready jobs are handed out by priority, then FIFO. | `EngineTest.DelayedJobIsHandedOutOnlyAfterItsDelay`, `.ReserveFollowsPriorityThenFifo`, `.ReserveChecksQueuesInTheOrderGiven`; mutant "ready order ignores priority" |
| L7 | **Idempotent enqueue.** Within the idempotency window one key maps to one job — even after that job has finished and been collected — and a duplicate logs nothing. After the window the key counts as absent, whether or not it has been physically collected. | `EngineTest.SameKeyReturnsTheSameJobWithinTheWindow`, `.KeyStillMatchesAfterTheJobFinishedAndWasCollected`, `.KeyExpiresAfterTheWindow`, `.ExpiredKeyCountsAsAbsentEvenBeforeItIsCollected`, `StateModelTest`; mutants "idempotency window never expires", "idempotency key not recorded" |
| L8 | **A record that does not fit the state is rejected and changes nothing**, so recovery refuses a log that disagrees with the code instead of guessing. | `StateTest.RecordsThatDoNotFitAreRejectedAndChangeNothing`; fuzz target `fuzz_state_apply` |
| L9 | **Limits are errors, not degradation.** An oversized payload or a full `max-memory` budget is refused with a clear error and logs nothing; enqueue works again once memory is freed. | `EngineTest.EnqueueRejectsBadInputWithoutLoggingAnything`, `EngineMemoryLimitTest.EnqueueFailsClearlyAtTheLimitAndRecoversWhenMemoryIsFreed` |
| L10 | **Structural invariants** hold after every operation: each job is in exactly the index its state implies, counts match a recount, the heap property holds, a timer exists exactly when one is needed, the memory estimate equals a recomputation. | `State::check_invariants()` after every step of `StateModelTest` and after every `EngineTest` |

## At the wire

Promises of the server (M3). Unit tests are in `tests/unit/server/` and run an
in-process server over loopback on `SimFs`; integration tests are in
`tests/integration/` and run the real binary on a real file system.

| # | Promise | Checked by |
|---|---|---|
| W1 | **No reply before its records are durable.** With fsync frozen, a client that enqueues, reserves or reads receives *no bytes at all*; a power-loss image taken at that moment contains none of the unacknowledged work; when fsync completes the replies arrive, in pipeline order. This covers reads too: `STATUS` cannot reveal a job that a crash could still take back. | `ServerTest.NoReplyBeforeItsRecordsAreDurable`; mutant "replies sent before their records are durable" |
| W2 | **Everything a client was told survives.** After a power-loss image (unit) or `SIGKILL` (integration), every job whose id was returned exists, acknowledged jobs stay succeeded, idempotency keys still deduplicate, and job ids are never reused. | `ServerTest.EverythingAcknowledgedSurvivesPowerLoss`, `test_crash_recovery.py::test_acknowledged_work_survives_sigkill`, `::test_sigkill_in_the_middle_of_a_pipeline` (5 seeds × 4 kills mid-pipeline) |
| W3 | **The server restarts by itself after a crash**, repairing a torn log tail on a real file system, and **refuses to start** on a log with mid-log damage or one whose records do not fit the state machine, without modifying it. | `test_crash_recovery.py::test_torn_tail_is_repaired_on_a_real_file_system`, `::test_mid_log_damage_makes_the_server_refuse_to_start`, `ServerStartupTest.RefusesToStartOnADamagedLog`, `.RefusesALogThatDoesNotFitTheStateMachine` |
| W4 | **Stock Redis clients work unmodified**: redis-cli, and redis-py with its default settings, with RESP2 and with RESP3. | `tests/integration/test_redis_py.py` (every test × 3 protocol settings), `test_redis_cli.py`, `ServerTest.Resp3IsNegotiatedPerConnection` |
| W5 | **Blocking `RESERVE`** wakes as soon as a job becomes ready (new, delayed or retried), serves parked connections first come first served, times out with nil, keeps pipelined requests behind it in order, and never leases a job to a parked connection that has disconnected. | `ServerTest.BlockingReserve*`, `.ParkedWorkersAreServedFirstComeFirstServed`, `.RequestsPipelinedBehindAParkedReserveRunAfterIt`, `.DisconnectedWaiterDoesNotSwallowAJob`; mutant "parked RESERVEs served newest first" |
| W6 | **Limits answer with `-LIMIT` and change nothing**: a payload over `--max-payload` (skipped as it streams in, never buffered; the connection stays usable), `--max-memory`, `--max-connections`. A client that never reads its replies is disconnected instead of growing the server's memory. | `ServerLimitsTest.*`, `RespParserTest.OversizedArgumentIsSkippedWithoutBuffering`; fuzz target `fuzz_resp_parser` (bounded buffering on every input) |
| W7 | **Hostile bytes cannot hurt.** Malformed frames, inline commands and HTTP requests get an error and a closed connection; other connections are unaffected. Any chunking of a byte stream parses to the same requests. | `ServerTest.ProtocolErrorsGetAnErrorReplyAndAClosedConnection`, `RespParserTest.*`; `fuzz_resp_parser` |
| W8 | **Authentication**: with `--requirepass`, nothing but `AUTH`, `HELLO` and `QUIT` works before the right password is given. | `ServerAuthTest.EverythingNeedsAuthUntilThePasswordIsGiven`, `test_redis_py.py::test_auth`; mutant "commands allowed without AUTH" |
| W9 | **One server per data directory.** A second instance fails fast and leaves the first untouched. | `ServerTest.SecondServerOnTheSameDirectoryIsRefused`, `test_crash_recovery.py::test_second_instance_on_the_same_directory_is_refused` |
| W10 | **A stalled disk slows clients down instead of growing memory**: reads pause when the log backlog passes its bound and resume when it drains; nothing is lost or deadlocked. | `ServerBackpressureTest.StalledDiskPausesReadsAndEverythingCompletesAfterwards` |
| W11 | **Graceful shutdown** (SIGTERM) flushes and syncs the log, sends every reply that was waiting on it, answers parked `RESERVE`s with nil, and exits 0. | `ServerTest.ShutdownAnswersParkedReservesAndFlushesAcknowledgements`, `test_crash_recovery.py::test_sigterm_is_a_clean_shutdown` |

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
