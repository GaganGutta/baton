# baton — implementation plan

baton is a durable job and workflow engine in C++20: one binary, no external
services, Redis wire protocol (RESP2). This file breaks every milestone into
concrete tasks. `PROGRESS.md` tracks where we are; `docs/design.md` holds the
reasoning behind each decision.

Conventions used below:

- A milestone is **done** only when its tests pass locally under ASan+UBSan
  (and TSan where threads are involved) **and** CI is green.
- Every milestone starts by writing its `docs/design.md` section (data
  structures, formats, failure cases, alternatives, test plan) and ends by
  updating that section with known limitations.
- Every promise added to `docs/guarantees.md` names the test that checks it.

## Cross-cutting decisions (made up front, justified in docs/design.md)

| Topic | Decision |
|---|---|
| Threads | Event loop thread owns all state (no locks). Log thread does write+fsync. A transient snapshot writer thread only ever sees a private copy. |
| Core invariant | Every reply is held until the LSN of the last record appended before the reply was generated is durable. Durability is prefix-ordered, so one number per reply covers all dependencies (e.g. RESERVE's lease record implies the enqueue record). Reads are gated the same way, so they never reveal undurable state. |
| Single apply | Handlers validate, resolve all nondeterminism (ids, clocks, jitter, tokens) into a record, then call `apply(record)`. Recovery calls the same `apply`. `apply` never reads a clock or RNG. |
| Errors | `Result<T>` for expected failures; `BATON_CHECK` aborts on invariant violations; fsync failure aborts. No exceptions thrown by baton code. |
| Logging | Small in-house leveled `key=value` logger to stderr. |
| Time | Wall-clock deadlines are persisted; the monotonic clock drives the timing wheel; a jump detector re-anchors timers using the same code path as restart. |
| Fencing | Lease tokens come from one server-wide counter that only increases (survives DLQ.RETRY resetting attempts). |
| Lease validity | A lease is valid until a `LeaseExpired` record is applied — it is state, not a clock comparison. |
| SDK transport | The Python SDK ships its own ~200-line RESP2 client (zero dependencies, exact control over retries/timeouts). Compatibility with stock clients is proven separately with redis-cli and redis-py integration tests. |
| Dependencies | GoogleTest, Google Benchmark, libFuzzer only. |

## Phase 1 — MVP

### M0. Scaffold
- [x] Repo hygiene: `.gitignore`, `.gitattributes` (LF everywhere), `.editorconfig`, MIT `LICENSE`.
- [x] `CMakeLists.txt` + `CMakePresets.json`: presets `debug`, `release`, `asan` (ASan+UBSan), `tsan`, `fuzz` (clang + libFuzzer), `tidy`. C++20, `-Wall -Wextra -Werror`, warnings applied only to baton targets (not FetchContent deps).
- [x] FetchContent: GoogleTest, Google Benchmark (pinned tags). Fuzz targets use the compiler's libFuzzer.
- [x] `.clang-format`, `.clang-tidy`; `scripts/format.sh`, `scripts/check.sh` (what CI runs, runnable locally).
- [x] `src/common`: `Result<T>`/`Error`, `BATON_CHECK`, logger, RAII `Fd`, clock interface — with unit tests.
- [x] `src/server/main.cpp`: `baton --version` so the binary, Dockerfile, and CI have something real to build.
- [x] GitHub Actions: GCC + Clang builds, ASan+UBSan tests, TSan job, macOS build+test, clang-format check, clang-tidy, Docker image build. The fuzz job is added with the first fuzz target (M1) and the chaos job with the harness (M7) — a CI job that runs nothing would only pretend to cover something.
- [x] Multi-stage `Dockerfile` (build stage → minimal runtime, non-root user, volume for data dir).
- [x] Skeletons: `README.md`, `docs/design.md` (M0 section: error handling + logging decisions), `docs/guarantees.md`, `docs/protocol.md`, `docs/testing.md`, `docs/roadmap.md`.
- [x] CI green, push.

### M1. Durable log
- [x] Design section: segment + record format, recovery rules, group commit, fsync policies, fsync-failure policy (crash; "fsyncgate"), directory fsync rules.
- [x] `common/crc32c`: software slicing-by-8 + hardware (SSE4.2 / ARMv8 CRC) with runtime dispatch; tests against known vectors and hw==sw on random data.
- [x] `common/codec`: little-endian fixed ints, varints, length-prefixed bytes; bounds-checked reader.
- [x] `common/fs`: thin `FileSystem` interface (open/append/sync/rename/remove/list/sync-dir) with a POSIX implementation and a fault-injecting in-memory fake for tests (torn writes, fsync failure, ENOSPC).
- [x] `log/format`: record header `length | crc32c | type | lsn`, segment header (magic, version, first LSN, CRC).
- [x] `log/segment_writer`: append, roll at size threshold (fsync old → create new → header → fsync file → fsync dir).
- [x] `log/reader` + recovery: scan segments in LSN order, verify CRC and LSN continuity; torn tail in final segment → truncate; anything else (bad record followed by a valid one, bad record in a non-final segment, LSN gap, missing segment) → refuse to start with a precise error.
- [x] `log/log_thread`: group commit. Event loop hands over batches; log thread writes, fsyncs per policy (`always` = fdatasync before acknowledging; `interval` = acknowledge after write, fsync on a timer), publishes `durable_lsn`, wakes the loop. Backpressure when the un-durable backlog exceeds a bound.
- [x] macOS: `F_FULLFSYNC`; Linux: `fdatasync`.
- [x] Tests: round-trip; roll; truncation at **every byte offset** of the final record; bit flip at every byte of a mid-log record → refuse; LSN gap; missing middle segment; empty/partial final segment header; fsync failure → process aborts (death test); group commit ordering + batch accounting; TSan run of the log thread.
- [x] Fuzz targets: `fuzz_log_segment` (raw bytes) and `fuzz_log_damage` (structure-aware damage to a valid log).
- [x] `scripts/mutation-check.sh`: six durability mutants, all must be killed.
- [x] Update design (limitations), guarantees, PROGRESS; CI green; push.

### M2. State machine
- [x] Design section: job model, records (the "facts" vocabulary), apply rules, queue ordering `(priority desc, run_at asc, id asc)`, idempotency index semantics, invisible GC rules (key expiry, finished-job retention), memory accounting, timing wheel.
- [x] `sched/timing_wheel`: hierarchical (6 levels × 64 slots, 1 ms tick), slab-backed intrusive lists, O(1) insert/cancel, occupancy bitmaps for `next_expiry()` so the loop sleeps exactly as long as needed; cascading; far-future clamp.
- [x] `state/job`, `state/job_store`, `state/ready_queue` (indexed binary heap with O(log n) arbitrary removal), `state/idempotency_index`.
- [x] `state/records`: encode/decode for every record type, versioned.
- [x] `state/state.apply(record)`: the single mutation path. Pure with respect to clock/RNG.
- [x] `state/backoff`: exponential backoff with full jitter and cap (RNG injected; result goes in the record).
- [x] Invariant checker (`State::check_invariants`) used by tests: every job in exactly one index, counters match, heap property, timers match states.
- [x] Tests: per-transition unit tests; timing wheel vs. a naive reference model (randomized); **model-based test**: random command sequences, after every step check invariants; **replay equivalence**: canonical dump of live state == state rebuilt by replaying the records.
- [x] Microbenchmarks wired up (timing wheel, heap) — numbers reported in M8.
- [x] `state/engine`: command logic (validate → resolve nondeterminism → apply → log) behind a `RecordSink`; fuzz targets `fuzz_record_decode`, `fuzz_state_apply`, `fuzz_state_deserialize`; 7 state-machine mutants added to `scripts/mutation-check.sh`.
- [x] Update docs; CI green; push.

### M3. Networking
- [x] **Write `docs/protocol.md` first**: framing, every command's syntax/replies/errors, error-code prefixes (`ERR`, `NOAUTH`, `NOTFOUND`, `STALE`, `STATE`, `LIMIT`, `BADARG`), pipelining and ordering rules, limits.
- [x] Design section: event loop, connection lifecycle, reply gating, blocking RESERVE, backpressure, limits.
- [x] `net/poller`: epoll (Linux) and kqueue (macOS) behind one interface; self-pipe wakeup.
- [x] `net/resp`: incremental zero-copy RESP2 request parser with hard limits (array length, bulk length, total request size); reply writer.
- [x] Fuzz target: RESP parser (incl. split-at-every-offset equivalence check); corpus seeds.
- [x] `net/connection`: non-blocking read/write buffers, ordered reply queue of `(bytes, required_lsn)`, output-buffer limit, max connections.
- [x] `server/`: wiring of loop + log thread + state; command table; handlers for PING, INFO, AUTH, ENQUEUE, RESERVE (blocking, multi-queue, timeout via timing wheel, fair waiter wakeup), HEARTBEAT, ACK, FAIL, CANCEL, STATUS, STATS; client-compat stubs (HELLO, CLIENT, COMMAND, SELECT, QUIT).
- [x] Recovery on startup (replay log → state), graceful shutdown on SIGTERM/SIGINT.
- [x] Config/flags: dir, bind (default 127.0.0.1), port, fsync policy, requirepass, limits.
- [x] Tests: parser unit tests; handler tests against an in-process server with a **controllable log** proving the reply-after-durable invariant (hold the fsync → no reply bytes; release → replies, in pipeline order); limits return clear errors; AUTH; blocking RESERVE semantics (timeout, multi-queue order, waiter fairness, disconnect while blocked).
- [x] Integration tests (pytest): redis-cli and redis-py against the real binary; kill -9 + restart keeps acknowledged jobs.
- [x] RESP3 negotiation (`HELLO 3`): not planned, but required — redis-py 8 defaults to it and treats a refusal as fatal (found by the integration tests).
- [x] Update docs; CI green; push.

### M4. Leases, retries, delayed jobs, DLQ
- [x] Design section: lease lifecycle, token rules, expiry as a logged record, retry/backoff, DLQ, **restart policy for live leases** (leases survive; expiry timers get a restart grace so workers that could not heartbeat during downtime are not punished), clock-jump handling (= re-anchor path).
- [x] Lease expiry timer → `LeaseExpired` record (counts as a failed attempt; backoff or dead).
- [x] HEARTBEAT reschedules the expiry timer; stale token → `STALE`.
- [x] FAIL → retry with backoff or dead; client-supplied `RETRY_IN`/`NORETRY`.
- [x] Delayed jobs (`DELAY`/`AT`), promotion scheduled→ready by the wheel.
- [x] DLQ.LIST (paged), DLQ.RETRY (one / all), DLQ.PURGE.
- [x] Recovery: rebuild timers from persisted wall-clock deadlines; jump detector shares the path.
- [x] Tests with a fake clock: zombie worker ACK rejected after re-lease; heartbeat keeps a lease alive; expiry → retry → dead; backoff bounds (jitter within `[0, min(cap, base·2^n)]`); restart with live leases (worker ACK after restart accepted; expired-during-downtime handled with grace); clock jump forward/backward.
- [x] Integration tests for the same through the wire.
- [ ] Update docs; CI green; push.

### M5. Snapshots and compaction
- [ ] Design section: evaluate fork COW (incl. multithreaded-fork hazards), copy-then-write, incremental/delta snapshots; pick one; file format; retention (keep 2); recovery order; interaction with log durability (snapshot at LSN X is finalized only after the log is durable through X).
- [ ] Shared immutable payload blobs so the consistent copy is cheap (metadata copy + refcount bump, no payload memcpy).
- [ ] `snapshot/writer`: temp file → fsync → rename → fsync dir → delete covered segments (governed by the older retained snapshot).
- [ ] `snapshot/reader`: validate magic/version/section CRCs/footer; torn or corrupt newest snapshot → fall back to the older snapshot + log; otherwise refuse.
- [ ] Triggers: log bytes since last snapshot, `SNAPSHOT` command. Startup cleans `*.tmp`.
- [ ] Tests: snapshot+tail == full replay (canonical dump); crash at each step of the write protocol (fault-injecting FS); torn snapshot fallback; segments never deleted before the covering snapshot is durable; TSan on snapshot thread handoff.
- [ ] Fuzz target: snapshot loader.
- [ ] Measure: event-loop pause during snapshot vs. live-job count; recovery time vs. log size with/without snapshot (`bench/` scripts; results recorded in docs/benchmarks.md).
- [ ] Update docs; CI green; push.

### M6. Python SDK
- [ ] `sdk/python/pyproject.toml` (installable with `pip install "git+https://github.com/GaganGutta/baton#subdirectory=sdk/python"`), package `baton`.
- [ ] `baton.resp`: minimal RESP2 codec + connection (timeouts, reconnect).
- [ ] `baton.Client`: enqueue, reserve, heartbeat, ack, fail, cancel, status, stats, dlq_*; typed results and exceptions mapped from error prefixes.
- [ ] `baton.Worker`: `@worker.task("name")` decorator, JSON envelope convention `{task, args, kwargs}`, configurable concurrency (threads), background heartbeats, graceful SIGTERM shutdown (stop reserving, finish in-flight, bounded wait), retry-on-exception via FAIL.
- [ ] `baton.idempotent`: helper that makes a side effect happen once across duplicate deliveries (pluggable store; file-backed fsynced store included; documented contract).
- [ ] Tests (pytest) against a real server binary: end-to-end enqueue→work→ack, heartbeat keeps long jobs alive, SIGTERM drains, worker kill → job re-delivered, idempotent helper under duplicate delivery.
- [ ] CI job for SDK tests; docs; push.

### M7. Chaos harness
- [ ] Design section + `docs/testing.md`: harness architecture, seeds, invariants, fault model, how to reproduce a failure from a seed.
- [ ] `chaos/`: seeded orchestrator (Python): starts server + N worker processes + producers; SIGKILLs server/workers at seeded random moments; restarts; drains; checks.
- [ ] Producers log every `OK` (job id, idempotency key) to an fsynced journal; handlers write side effects through the idempotency helper to an fsynced ledger and log every run.
- [ ] Invariant checks 1–5 from the spec (no lost jobs; no two valid leases — verified from the server's own log via an offline log checker **and** from worker-observed tokens; exactly-once ledger effects with duplicate-run count reported; one key → one job; recovery after every kill with recovery time recorded).
- [ ] Fault injection: truncated log tail (must recover), flipped bit mid-log (must refuse), torn snapshot (must fall back or refuse — never load garbage), full disk (clear error / clean crash, no acknowledged loss, recovers when space returns).
- [ ] CI short run (fixed seeds, ~2 min); long local run (many seeds, ≥ 30 min), results recorded in docs/testing.md.
- [ ] Fix every bug found (each gets a regression test); push.

### M8. Benchmarks and launch
- [ ] `bench/loadgen` (C++): pipelined multi-connection load generator, log-bucket latency histogram (p50/p99/p999), modes: enqueue-only, end-to-end with N workers, pickup latency.
- [ ] Server-side metrics for group-commit batch sizes; memory per job measurement; recovery-time benchmark (reuse M5).
- [ ] Google Benchmark microbenchmarks: RESP parser, record encode/decode + CRC32C, timing wheel.
- [ ] Comparison: Beanstalkd (`-b`, fsync every write and interval variants) and Faktory via Docker with matched durability; loadgen speaks their protocols; every setting documented. If Docker is unavailable, record that in PROGRESS.md rather than faking numbers.
- [ ] `bench/run_all.sh` emits JSON + markdown tables; README numbers are pasted only from those outputs, with hardware specs and exact commands.
- [ ] README: pitch, quickstart (docker run → Python worker in under a minute), command reference, guarantees, mermaid architecture diagram, results, honest comparison (Beanstalkd, Faktory, Temporal, River), "when not to use baton".
- [ ] Tag `v0.1.0`.

## Phase 2 — workflows and depth

### M9. Durable workflows
- [ ] Design section: a workflow is a replayable job; history model; records (`WfStarted`, `WfStepCompleted`, `WfSleepStarted`, `WfSignalWaitStarted`, `WfSignaled`, `WfWoken`, `WfCompleted`, `WfFailed`, `WfCancelled`); fencing of history appends by the task job's lease token; determinism check (step name at sequence position must match).
- [ ] Server: WF.START (optional caller-supplied id for idempotent start), WF.HISTORY, WF.STEP, WF.SLEEP, WF.WAIT, WF.SIGNAL (buffered if nobody is waiting yet), WF.COMPLETE, WF.FAIL, WF.CANCEL, WF.STATUS.
- [ ] SDK: `@worker.workflow`, `ctx.step(name, fn)`, `ctx.sleep(name, seconds)`, `ctx.wait_signal(name, timeout)`, `NonDeterminismError`, cancellation.
- [ ] Tests: replay returns stored results without re-running; nondeterminism fails loudly; durable sleep survives restart; signal before/after wait, timeout; cancel while sleeping/waiting/running; stale worker cannot append history.
- [ ] Chaos extension: ledger proves no step executes after its result became durable.
- [ ] Snapshot support for workflow state; replay-equivalence tests extended.

### M10. Scheduling
- [ ] Cron: 5-field parser (lists, ranges, steps; UTC), fuzzed; `CRON.ADD/DEL/LIST`; firing is one atomic record (enqueue + advance), so it cannot double-fire; misfire policy (`fire_once` default / `skip`), documented and tested across simulated downtime.
- [ ] Per-queue concurrency limits (`QUEUE.LIMIT`), persisted; RESERVE honours them; waiters wake when a slot frees.
- [ ] Unique jobs (`UNIQUE key`): at most one non-terminal job per key.
- [ ] Tests incl. restart in the middle of each feature; chaos run with cron enabled checks no double fire.

### M11. Observability
- [ ] Minimal HTTP/1.1 listener on the same event loop (separate port, localhost by default, read-only, GET only).
- [ ] `/metrics` (Prometheus text format): queue depths by state, throughput counters, latency histograms, group-commit batch sizes, fsync latency, snapshot stats, connections, memory.
- [ ] Dashboard: single embedded HTML page + JSON endpoints for queues, jobs, DLQ, workflow step history.
- [ ] `batonctl` admin CLI: stats, queues, job, dlq list/retry, wf status, snapshot, and offline `log verify` / `log dump`.
- [ ] Tests: metrics format parse test, HTTP parser fuzz target, endpoint integration tests.

### M12. examples/pipeline
- [ ] Realistic workflow: fetch a web page → analyze → durable sleep (politeness delay/re-check) → build report; retries on fetch; README explaining how to copy it.
- [ ] Runs in CI against a local HTTP fixture (no external network dependency in tests).
- [ ] Tag `v0.2.0`.

## Final deliverables
- [ ] `docs/interview-prep.md`: 20 hardest design questions with answers pointing at code.
- [ ] Three resume bullets using only measured numbers from `bench/`.
- [ ] Final summary in `PROGRESS.md`: what was built, benchmark results, chaos results, known gaps.
