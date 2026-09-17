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

## Snapshots and compaction

Snapshots must never weaken D1–D9: they are an optimization of recovery, and
the oracle in every test below is "the state that replaying the complete log
gives".

| # | Promise | Checked by |
|---|---|---|
| S1 | **Snapshot + log tail = full replay.** Recovering from a snapshot and the records after it gives exactly the state that replaying the whole log gives, wherever the snapshot was taken. | `StateRecoveryTest.SnapshotPlusTailEqualsFullReplay`, `.SnapshotAtTheVeryEndNeedsNoReplay`, `.CompactedLogRecoversFromTheSnapshots`; `StateModelTest` (snapshot at random points); through the wire: `ServerSnapshotTest.RestartRecoversFromTheSnapshotPlusTheLogAfterIt`; real binary, real file system, SIGKILL: `test_snapshots.py::test_snapshot_command_then_sigkill_recovers_from_the_snapshot` |
| S2 | **A crash at any point of a snapshot cycle loses nothing.** A crash image taken after *every* file-system operation of three consecutive snapshot-and-compact cycles — with un-synced data lost, kept, or torn, and with an arbitrary subset of un-synced directory operations surviving — recovers to the same state. | `SnapshotCrashTest.EveryCrashPointRecoversTheSameState` (3 crash modes), `StateRecoveryTest.TornCrashesWithManySeeds`, `RecoveryTest.LeftoverSegmentsCoveredByTheSnapshotAreIgnored`; mutants "snapshot renamed into place without fsync", "snapshot rename not made durable", "leftover segments behind the snapshot are validated" |
| S3 | **Log segments are deleted only when two durable snapshots make them dispensable**: never with a single snapshot, never the segment being written, never a segment that holds a record after the *older* retained snapshot. | `CompactionTest.*` (6 tests); mutant "log compacted on the strength of the newest snapshot" |
| S4 | **An unreadable snapshot is never trusted and never fatal by itself.** Every truncation, every single-bit flip, trailing bytes, a cut at a chunk boundary, reordered, duplicated, missing or foreign chunks and a wrong file name are all rejected; recovery then falls back to the older snapshot, or to the whole log if it is still there. | `SnapshotDamageTest.*` (8 tests), `StateRecoveryTest.UnreadableNewestSnapshotFallsBackToTheOlderOne`, `.TruncatedNewestSnapshotFallsBackToo`, `.NoReadableSnapshotFallsBackToTheWholeLogIfItIsStillThere`, `test_snapshots.py::test_unreadable_newest_snapshot_falls_back_to_the_older_one`; `fuzz_snapshot_load`; mutants "snapshot end-chunk totals not checked", "snapshot chunk sequence not checked" |
| S5 | **A snapshot never hides log damage and never invents state.** Damage in the log after the snapshot still refuses to start; if neither snapshot nor log can cover a gap, baton refuses rather than start with jobs missing. | `StateRecoveryTest.LogDamageIsNeverPaperedOverByASnapshot`, `.RefusesWhenNeitherSnapshotNorLogCanCoverTheGap` |
| S6 | **A snapshot is never ahead of the durable log.** It is written only once every record it contains has been fsynced. | `ServerSnapshotTest.SnapshotIsNeverAheadOfTheDurableLog`; mutant "snapshot started before the log is durable up to it" |
| S7 | **Snapshots do not stop the server.** Clients are served while a snapshot is being written, even if its fsync stalls; a failed snapshot is reported and changes nothing; automatic snapshots compact the log under load and a power failure afterwards loses nothing. | `ServerSnapshotTest.TrafficContinuesWhileASnapshotIsBeingWritten`, `ServerAutoSnapshotTest.LogIsCompactedInTheBackgroundAndNothingIsLost`, `StateRecoveryTest.SnapshotterReportsFailuresInsteadOfCrashing`, `SnapshotTest.FailedWriteLeavesNothingVisible`, `test_snapshots.py::test_the_log_is_compacted_while_serving_and_sigkill_loses_nothing`; TSan over the unit tests |

Not promised: how long the event loop pauses to copy the state. That is
measured, not guaranteed (`docs/benchmarks.md`, design doc 8.8).

## Delivery and leases

These are promises of the state machine (M2), checked at the `Engine` API. M3
adds the promise that the wire protocol exposes them unchanged, M4 the rules
for restarts and clock jumps. Tests live in `tests/unit/state/`.

| # | Promise | Checked by |
|---|---|---|
| L1 | **Replay equivalence.** A state rebuilt from the logged records alone — or from a serialized state plus the records after it — is byte-for-byte identical to the live state. `apply` uses nothing that is not in the record: no clock, no RNG, no configuration. | every `EngineTest` (the fixture checks it in `TearDown`), `StateModelTest.RandomTrafficKeepsInvariantsAndReplaysExactly` (12 seeds × 2,500 steps), `StateSerializationTest.*`; mutant "apply reads the clock instead of the record" |
| L2 | **Fencing.** A job has at most one valid lease. Lease tokens increase server-wide and are never reused, including after a dead job is retried. | `EngineTest.TokensIncreaseAcrossJobsAndAttempts`, `StateModelTest`; mutant "lease token counter not advanced" |
| L3 | **Zombies are rejected.** Once a lease has ended — expiry, completion or cancellation — its token can no longer change anything: `FAIL` and `HEARTBEAT` answer `STALE`, and so does `ACK` unless it repeats the very `ACK` that completed the job (L14) — whether or not the job has been leased again. The current holder is unaffected. | `EngineTest.ZombieWorkerIsFencedOffAfterItsLeaseExpires`, `.StaleTokenIsRejectedEvenBeforeTheJobIsLeasedAgain`, `.CancelWorksInEveryLiveState`, `StateModelTest`; mutant "engine accepts a stale lease token" |
| L4 | **Heartbeats extend leases**; without one the lease expires on time — **never before** the wall-clock expiry the worker was given, even when the wall clock lags the monotonic clock that drives the timers. An expiry is a logged fact that consumes an attempt and can dead-letter the job. | `EngineTest.HeartbeatKeepsALeaseAlive`, `.LeaseExpiryConsumesAnAttemptAndCanKillTheJob`, `ClockJumpTest.ALeaseIsNeverExpiredBeforeItsWallClockExpiry`; every expiry in every chaos round's log (`baton-logcheck`); mutant "lease expired before its wall-clock expiry" |
| L5 | **Retries** use exponential backoff with full jitter, capped: the delay is uniform in `[0, min(cap, base·2^(attempt−1))]`, drawn once and logged. After `max_attempts` leases the job moves to the dead-letter queue. `NORETRY` and `RETRY_IN` override. | `EngineTest.JitterIsFullAndStaysWithinBounds`, `.BackoffCeilingDoublesAndIsCapped`, `.ExhaustedAttemptsSendTheJobToTheDeadLetterQueue`, `.NoRetryAndRetryInOverrideTheBackoff`; mutant "attempts never counted" |
| L6 | **Delayed jobs** are never handed out before `run_at`; ready jobs are handed out by priority, then FIFO. | `EngineTest.DelayedJobIsHandedOutOnlyAfterItsDelay`, `.ReserveFollowsPriorityThenFifo`, `.ReserveChecksQueuesInTheOrderGiven`; mutant "ready order ignores priority" |
| L7 | **Idempotent enqueue.** Within the idempotency window one key maps to one job — even after that job has finished and been collected — and a duplicate logs nothing. After the window the key counts as absent, whether or not it has been physically collected. | `EngineTest.SameKeyReturnsTheSameJobWithinTheWindow`, `.KeyStillMatchesAfterTheJobFinishedAndWasCollected`, `.KeyExpiresAfterTheWindow`, `.ExpiredKeyCountsAsAbsentEvenBeforeItIsCollected`, `StateModelTest`; mutants "idempotency window never expires", "idempotency key not recorded" |
| L8 | **A record that does not fit the state is rejected and changes nothing**, so recovery refuses a log that disagrees with the code instead of guessing. | `StateTest.RecordsThatDoNotFitAreRejectedAndChangeNothing`; fuzz target `fuzz_state_apply` |
| L9 | **Limits are errors, not degradation.** An oversized payload or a full `max-memory` budget is refused with a clear error and logs nothing; enqueue works again once memory is freed. | `EngineTest.EnqueueRejectsBadInputWithoutLoggingAnything`, `EngineMemoryLimitTest.EnqueueFailsClearlyAtTheLimitAndRecoversWhenMemoryIsFreed` |
| L10 | **Structural invariants** hold after every operation: each job is in exactly the index its state implies, counts match a recount, the heap property holds, a timer exists exactly when one is needed, the memory estimate equals a recomputation. | `State::check_invariants()` after every step of `StateModelTest` and after every `EngineTest` |
| L11 | **Leases survive a restart** under the same token, and every lease gets at least `--lease-grace` after a restart before it can expire, so workers that could not heartbeat while the server was down are not punished. Without a heartbeat the lease still expires after the grace period and the job is redelivered under a new token. | `ServerLeaseGraceTest.WorkerThatOutlivedTheServerCanStillFinishWithinTheGracePeriod`, `.SilenceAfterTheGracePeriodStillExpiresTheLease`, `test_dlq.py::test_lease_survives_sigkill_and_restart`, `::test_unclaimed_lease_is_redelivered_after_the_grace_period`; mutant "restart expires leases without a grace period" |
| L12 | **Wall-clock steps are handled like a restart.** A step of more than 1 s is detected within one loop iteration and all timers are re-derived from their persisted deadlines: after a forward step, jobs whose time has come run at once while leases get the grace period instead of expiring en masse; after a backward step delayed jobs wait for the wall clock and no lease is shortened. Slew and jitter never trigger it. | `ClockJumpTest.*` (6 tests), `StateModelTest` (random forward steps); mutants "wall-clock jumps go unnoticed", "clock jump expires leases without a grace period" |
| L13 | **Dead-letter queue.** A job that exhausts its attempts keeps its payload and last error and can be listed (paged, oldest first), retried (ready now, attempts reset, a new and larger token) or purged, one at a time or per queue. These operations are logged and survive restarts. | `DlqTest.*` (7 tests), `ServerTest.DeadLetterQueueOverTheWire`, `.DeadLetterOperationsSurviveARestart`, `test_dlq.py::test_operating_the_dead_letter_queue` (RESP2 and RESP3), `StateModelTest`; mutant "DLQ retry keeps the spent attempts" |
| L14 | **`ACK` is idempotent for the token that completed the job, and only for it.** Repeating that `ACK` answers `OK` again and logs nothing, for as long as the finished job is retained; every other token gets `STALE`. A client whose connection died mid-`ACK` therefore learns exactly whether its `ACK` counted. | `EngineTest.AckIsIdempotentForTheTokenThatCompletedTheJobAndOnlyForIt`, `.OnlySucceededJobsRememberAToken`, `StateModelTest` (repeated ACKs must log nothing), `ServerTest.JobLifecycleOverTheWire`; mutant "repeated ACK accepted for any token" |

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

## Python SDK

What the SDK adds on the client's side of the socket. Tests live in
`sdk/python/tests/` and run against the real server binary.

| # | Promise | Checked by |
|---|---|---|
| P1 | **A keyed enqueue is exactly one job, even if the reply is lost.** The SDK resends it on a new connection and gets the first attempt's job id. | `test_client.py::test_keyed_enqueue_is_retried_and_does_not_duplicate` (a proxy drops the reply after the server has executed the request) |
| P2 | **An unkeyed enqueue is never silently duplicated.** If the connection dies after the request was sent, `EnqueueUncertain` is raised instead of retrying. | `test_client.py::test_unkeyed_enqueue_refuses_to_guess` |
| P3 | **An `ACK` whose reply was lost still counts, and nobody else's does**: the SDK resends it and relies on L14 — `OK` means its own `ACK` completed the job, `StaleLease` means it did not, even if the job has meanwhile succeeded under another worker. | `test_client.py::test_ack_whose_reply_was_lost_still_counts`, `::test_stale_ack_after_a_resend_is_still_stale_if_the_job_did_not_succeed`, `::test_a_resent_ack_never_claims_another_workers_success` (found by the chaos harness) |
| P4 | **A killed worker loses nothing**: its job is delivered again once the lease expires, and completes. A restarted *server* does not take the worker down either. | `test_worker.py::test_a_killed_worker_loses_nothing` (SIGKILL mid-job), `::test_survives_a_server_restart` |
| P5 | **Heartbeats keep a long job alive**: a handler that runs for several lease periods is delivered once. | `test_worker.py::test_heartbeats_keep_a_long_job_alive` |
| P6 | **A lost lease is noticed and its result discarded**: after a cancellation the handler sees `lease_lost`, and the worker does not acknowledge over it. | `test_worker.py::test_a_cancelled_job_loses_its_lease_and_is_not_acked` |
| P7 | **Graceful shutdown**: on SIGTERM the running handler finishes and is acknowledged, no new job is taken, and the process exits 0. | `test_worker.py::test_sigterm_lets_the_running_job_finish_and_exits_zero` (a real process and signal), `::test_graceful_stop_finishes_the_running_job_and_takes_no_new_one` |
| P8 | **Handler outcomes map to the protocol**: return → `ACK`; exception → `FAIL` with backoff until dead-lettered; `Retry` chooses the delay; `Fatal` and unparseable payloads dead-letter at once; an unknown task is retried, not dead-lettered. | `test_worker.py::test_exceptions_retry_then_dead_letter`, `::test_retry_fatal_and_unknown_tasks` |
| P9 | **`Ledger.put_if_absent` happens once per key**: among racing processes exactly one wins each key; nothing reported as recorded is lost to SIGKILL; a torn tail is repaired; damage elsewhere is refused, not truncated away. | `test_idempotent.py::test_processes_racing_for_the_same_keys_each_win_a_key_exactly_once`, `::test_a_killed_writer_loses_nothing_it_reported`, `::test_a_torn_tail_is_repaired`, `::test_damage_in_the_middle_is_not_repaired_silently` |
| P10 | **A duplicate delivery does not duplicate a ledger effect.** A worker that dies after its side effect and before the `ACK` causes a second run; the effect recorded through the ledger exists once. | `test_worker.py::test_a_duplicate_delivery_does_not_duplicate_a_ledger_effect` |

Not promised: exactly-once for effects outside a ledger. `Ledger.once()` is "at
least once, and never again once recorded", and says so
(`test_idempotent.py::test_two_live_runs_both_happen_and_the_first_result_wins`
pins the window down).

## Under chaos

The promises above, checked together, end to end, with real processes being
killed (`chaos/`, design doc section 10, results in `docs/testing.md`). CI runs
two seeded kill rounds and the fault scenarios on every push.

| # | Promise | Checked by |
|---|---|---|
| C1 | **No acknowledged enqueue is lost** while the server and workers are SIGKILLed, paused and restarted at random: every job id a producer was given ends up `succeeded` (or `dead`, for jobs that always fail). | chaos invariant 1 |
| C2 | **The server never treats two leases on a job as valid, and always rejects dead tokens.** Verified from the server's own complete log by an independent model (`baton-logcheck`, `LeaseModelTest.*`, `LeaseModelHistoryTest.*`), by rogue clients that use dead tokens while a successor holds the job, and from the workers' run logs. | chaos invariant 2; `chaos/selftest.sh` proves a server without token checks fails it |
| C3 | **Handlers may run more than once; an effect recorded through the `Ledger` exists exactly once.** The ledger file is parsed raw. | chaos invariant 3 |
| C4 | **One idempotency key never creates two jobs**, across producers, client retries and server restarts. | chaos invariant 4; `chaos/selftest.sh` proves a server that forgets keys fails it |
| C5 | **The server recovers after every kill**, from a snapshot plus the log tail, while snapshots and compaction are running. Recovery times are recorded. | chaos invariant 5 |
| C6 | **Injected storage faults**: a torn log tail is repaired and loses nothing acknowledged; a flipped bit mid-log is refused without touching a file; a torn snapshot falls back to the older one; a failing log write kills the server before it can acknowledge; a nearly full disk refuses `ENQUEUE` with `LIMIT` while the queue keeps draining. | `chaos/run.py --faults` (`harness/faults.py`) |

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
