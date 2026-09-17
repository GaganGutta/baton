# Benchmarks

Rules for this file:

- Every number comes from a script in `bench/`, quoted verbatim together with
  the command, the commit, the date and the machine. Nothing is estimated,
  rounded favourably or remembered.
- A result that is unflattering stays in.
- The machine is a laptop running WSL2. Absolute numbers will differ elsewhere;
  what should carry over is the shape (what grows with what).

Contents: [the server under load](#the-server-under-load-m8) ·
[next to Beanstalkd and Faktory](#next-to-beanstalkd-and-faktory-m8) ·
[snapshots and recovery](#storage-what-snapshots-cost-and-what-they-buy-m5).
How the measurements are made, and what they cannot tell, is in
[design.md, section 11](design.md#11-m8--measuring) and
[bench/README.md](../bench/README.md).

## The server under load (M8)

```bash
bench/run_all.sh          # about fifteen minutes; writes bench/out/results.{json,md}
```

A fresh server per experiment, 256-byte payloads, 10-second runs after 2 seconds
of warm-up, the load generator on the same machine. What follows is
`bench/out/results.md`, unedited.

```
date:       2026-09-17T13:58Z
commit:     4f45de4
kernel:     Linux 6.18.33.2-microsoft-standard-WSL2
cpu:        AMD Ryzen 9 8945HS w/ Radeon 780M Graphics
threads:    16
memory_gb:  15.3
filesystem: ext4 on /dev/sdd
compiler:   c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0
build:      CMAKE_BUILD_TYPE=Release
```

### Enqueue throughput (closed loop)

| fsync | connections | depth | jobs/s | latency p50 / p99 / p99.9 ms | records per fsync batch avg / max | fsync p50 / p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| always | 1 | 1 | 451 | 2.16 / 3.93 / 4.85 | 1.0 / 1 | 1.98 / 3.65 |
| always | 8 | 1 | 1,849 | 4.33 / 7.86 / 8.91 | 4.0 / 7 | 2.11 / 4.09 |
| always | 64 | 1 | 14,393 | 4.46 / 8.26 / 9.70 | 32.0 / 64 | 2.17 / 4.03 |
| always | 256 | 1 | 30,305 | 8.39 / 10.49 / 14.94 | 126.7 / 194 | 2.30 / 4.22 |
| interval | 1 | 1 | 7,088 | 0.13 / 0.22 / 2.62 | 1.0 / 1 | 2.75 / 81.92 |
| interval | 8 | 1 | 28,924 | 0.27 / 0.42 / 3.28 | 3.5 / 8 | 3.07 / 4.86 |
| interval | 64 | 1 | 34,259 | 1.80 / 4.13 / 6.16 | 31.7 / 64 | 3.26 / 5.76 |
| interval | 256 | 1 | 30,933 | 8.26 / 11.01 / 15.20 | 128.5 / 256 | 3.20 / 4.99 |
| always | 8 | 32 | 59,260 | 4.19 / 8.00 / 11.53 | 128.0 / 224 | 2.05 / 3.77 |
| interval | 8 | 32 | 303,879 | 0.51 / 1.25 / 25.69 | 96.5 / 256 | 10.75 / 147.46 |

### Enqueue latency at a fixed rate (open loop, 16 connections)

| fsync | target jobs/s | achieved jobs/s | latency p50 / p99 / p99.9 ms | max ms | server's fsync p50 / p99 / max ms |
|---|---:|---:|---:|---:|---:|
| always | 1,000 | 1,000 | 3.47 / 7.08 / 10.49 | 11.8 | 2.05 / 3.65 / 9.8 |
| always | 10,000 | 10,000 | 3.74 / 8.39 / 23.59 | 31.2 | 2.11 / 4.22 / 71.1 |
| interval | 1,000 | 1,000 | 0.26 / 12.32 / 15.73 | 17.1 | 9.73 / 18.94 / 19.0 |
| interval | 10,000 | 10,000 | 0.23 / 14.16 / 18.35 | 22.7 | 11.52 / 19.45 / 22.5 |
| interval | 50,000 | 50,000 | 0.98 / 180.36 / 268.44 | 276.4 | 14.08 / 23.55 / 273.4 |

### End to end: 8 pipelining producers, N workers (closed loop)

Each job is enqueued, reserved and acknowledged: three durable operations.
Where completed/s is below enqueued/s the workers are the bottleneck and the
queue is growing, so no latency is quoted here; see the next table.

| fsync | workers | enqueued/s | completed/s |
|---|---:|---:|---:|
| always | 1 | 14,893 | 116 |
| always | 8 | 13,151 | 822 |
| always | 64 | 12,926 | 6,464 |
| always | 256 | 9,760 | 9,761 |
| interval | 1 | 144,874 | 1,132 |
| interval | 8 | 96,125 | 6,009 |
| interval | 64 | 27,264 | 13,633 |
| interval | 256 | 13,553 | 13,553 |

### Pickup latency at a fixed rate, 64 workers waiting (open loop)

From the moment the ENQUEUE was due to be sent to the moment a worker holds the
job: the enqueue's fsync, the hand-off to a parked RESERVE, and the lease's fsync.

| fsync | jobs/s | completed/s | enqueue p50 / p99 / p99.9 ms | pickup p50 / p99 / p99.9 ms | server's fsync p99 / max ms |
|---|---:|---:|---:|---:|---:|
| always | 1,000 | 1,000 | 3.54 / 7.34 / 9.44 | 3.54 / 7.47 / 9.70 | 4.09 / 8.0 |
| always | 5,000 | 5,000 | 3.60 / 7.47 / 23.59 | 4.00 / 26.21 / 36.70 | 4.03 / 17.7 |
| interval | 1,000 | 1,000 | 0.22 / 14.94 / 18.87 | 0.25 / 14.94 / 18.87 | 20.99 / 22.0 |
| interval | 5,000 | 5,003 | 0.21 / 15.20 / 18.87 | 0.24 / 15.73 / 18.87 | 20.99 / 21.0 |

### Memory per queued job

| payload bytes | jobs | RSS bytes per job | server's own estimate per job |
|---:|---:|---:|---:|
| 100 | 1,471,040 | 392.5 | 396.0 |
| 1,000 | 1,147,264 | 1,284.5 | 1,296.0 |

### Restart after kill -9

| recovering from | jobs | log MB | records replayed | server recovery ms | exec to first reply ms |
|---|---:|---:|---:|---:|---:|
| log only | 1,241,088 | 379.8 | 1,241,088 | 815 | 819.9 |
| snapshot | 1,241,088 | 379.8 | 0 | 709 | 713.1 |

### Microbenchmarks

| benchmark | ns/op | throughput |
|---|---:|---:|
| `BM_Crc32cSoftware/64` | 22.0 | 2,907.3 MB/s |
| `BM_Crc32cSoftware/1024` | 401.9 | 2,547.6 MB/s |
| `BM_Crc32cSoftware/65536` | 25,427.1 | 2,577.4 MB/s |
| `BM_Crc32cHardware/64` | 2.7 | 23,378.4 MB/s |
| `BM_Crc32cHardware/1024` | 69.2 | 14,798.3 MB/s |
| `BM_Crc32cHardware/65536` | 4,984.6 | 13,147.7 MB/s |
| `BM_ReadyHeapPushPop/100` | 29.1 | 34,399,637/s |
| `BM_ReadyHeapPushPop/100000` | 89.8 | 11,140,700/s |
| `BM_ReadyHeapPushPop/1000000` | 108.7 | 9,198,173/s |
| `BM_ReadyHeapRemoveArbitrary/100000` | 210.6 | 4,748,093/s |
| `BM_EncodeEnqueueIntoLogRecord/100` | 84.9 | 2,156.3 MB/s |
| `BM_EncodeEnqueueIntoLogRecord/1024` | 179.3 | 6,178.1 MB/s |
| `BM_EncodeEnqueueIntoLogRecord/65536` | 6,896.8 | 9,514.8 MB/s |
| `BM_DecodeEnqueueFromLogRecord/100` | 58.2 | 3,143.5 MB/s |
| `BM_DecodeEnqueueFromLogRecord/1024` | 126.2 | 8,777.8 MB/s |
| `BM_DecodeEnqueueFromLogRecord/65536` | 5,000.0 | 13,124.2 MB/s |
| `BM_RespParsePipelinedEnqueue/100` | 2,550.7 | 25,091,309/s |
| `BM_RespParsePipelinedEnqueue/1024` | 2,600.4 | 24,611,163/s |
| `BM_RespParsePipelinedEnqueue/65536` | 2,619.7 | 24,429,971/s |
| `BM_RespParseFragmentedEnqueue/16` | 1,882.9 | 531,098/s |
| `BM_RespParseFragmentedEnqueue/256` | 148.7 | 6,724,441/s |
| `BM_TimingWheelScheduleCancel/0` | 9.0 | 110,913,403/s |
| `BM_TimingWheelScheduleCancel/10000` | 9.2 | 108,215,683/s |
| `BM_TimingWheelScheduleCancel/1000000` | 8.6 | 115,878,252/s |
| `BM_TimingWheelSteadyState/1000` | 9.7 | 103,221,888/s |
| `BM_TimingWheelSteadyState/100000` | 281.2 | 3,555,996/s |
| `BM_TimingWheelNextWakeup/10` | 6.5 |  |
| `BM_TimingWheelNextWakeup/1000000` | 8.5 |  |

(`RespParsePipelinedEnqueue`'s ns/op is for a pipeline of 64 requests, about 40
ns each whatever the payload size, because arguments are views into the read
buffer and payload bytes are never touched. `TimingWheelSteadyState/100000` is
slower per timer because every tick fires and re-arms 100 timers.)

### Reading the numbers

**With `--fsync always`, one connection gets one fsync per job, and group commit
gives the disk to everyone else for free.** A single connection manages 451
jobs/s because each job waits for its own `fdatasync` (about 2 ms on this disk).
With 64 connections the same fsyncs carry 32 records each and throughput is
14,393 jobs/s — 32 times as much — for twice the latency (p50 4.5 ms instead of
2.2: a request now waits for the fsync in progress and then for its own); with
256, 127 records per fsync and 30,305 jobs/s. Eight connections pipelining 32
deep reach 59,260 jobs/s, every one of them on disk before its reply. The fsync
latency itself does not change (p50 2.0–2.3 ms in every `always` row): durable
throughput here is *batch size × fsyncs per second*, and the batch grows by
itself with load.

**Without waiting for the disk the ceiling is the event loop:** about 30,000
jobs/s unpipelined (one read, one write and one wake-up per request) and about
300,000 jobs/s with pipelining, on one core. Past 64 connections `always` and
`interval` converge (30,305 vs 30,933 jobs/s at 256), because by then the
batches are so large that the fsync is amortized away.

**Tail latency is the disk's fsync tail, in both modes.** The server's own
fsync figures are printed next to each open-loop row for that reason (they cover
the whole run including warm-up, so a slow fsync there does not always fall into
the measured window: the 71 ms one in the 10,000/s row did not). Under `always`,
p99 (7–8 ms) is about two fsyncs: a request that just misses a batch waits for
that fsync and then for its own. Under `interval` the median is
a quarter of a millisecond, but the periodic background fsync runs on the log
thread, so every request that arrives during one waits for it: p99 ≈ the fsync
duration (12–15 ms in a phase where this disk took 10–20 ms per background
fsync), and the 50,000/s row shows what a single 273 ms fsync does to 50,000
requests per second — p99 180 ms. This virtual disk produces such an fsync every
few runs; which row it lands in changes from run to run (an earlier run had it
in the 10,000/s row). A disk with a tighter fsync distribution would tighten
every tail here. Moving the background fsync off the log thread is on the
roadmap.

**A waiting worker gets the job in the same fsync as the producer's
acknowledgement.** At 1,000 jobs/s under `always`, pickup latency (p50 3.54 ms,
p99 7.47 ms) is indistinguishable from enqueue latency (3.54 / 7.34): the
enqueue is applied, a parked `RESERVE` is served in the same loop iteration, and
the lease record joins the enqueue record in one group commit; both replies are
released by the same fsync. At 5,000 jobs/s some jobs find all 64 workers busy
in their own round trips and wait for one (p99 26 ms).

**End to end, each job is three durable operations** (enqueue, lease,
acknowledgement), and a worker is a closed loop of two of them. One worker under
`always` completes 116 jobs/s — two fsyncs and two round trips per job, no
sharing; 256 workers complete 9,761 jobs/s, by which point they keep up with the
producers. Under `interval`, 13,600 jobs/s end to end with 64–256 workers is the
event loop's limit for three commands, their replies and their records per job.

**A queued job costs about 290 bytes plus its payload**: 392 bytes of resident
memory per job with 100-byte payloads, 1,284 with 1,000-byte payloads. The
server's own estimate, which drives `--max-memory`, is within 1% of the measured
RSS (396 and 1,296).

**Restart after `kill -9` with 1.24 million queued jobs takes 0.8 s** from a 380
MB log and 0.7 s from a snapshot; as in the storage section below, a snapshot
buys little when the state is as large as the history, and a lot when it is not.

## Next to Beanstalkd and Faktory (M8)

```bash
bench/compare/run_compare.sh     # needs Docker; writes bench/out/compare.{json,md}
```

All three servers run in a container on the host's network with their data on a
fresh named volume on the same disk; the same load generator drives all three
from the host, 256-byte payloads, 10-second runs after 2 seconds of warm-up.
Every setting is in `bench/compare/run_compare.py`; nothing else was tuned, for
any of them. Same machine and commit as above; `bench/out/compare.md`, unedited:

```
docker:     29.1.3
beanstalkd: beanstalkd 1.13
faktory:    contribsys/faktory:1.10.0
baton:      baton 0.1.0
```

| system | what an acknowledgement means |
|---|---|
| baton --fsync always | fdatasync before any reply that depends on the record; group commit |
| beanstalkd -f 0 | binlog, fsync after every write |
| baton --fsync interval (50 ms) | reply after write(); fdatasync every 50 ms: a power cut can lose 50 ms |
| beanstalkd -f 50 | binlog, fsync at most every 50 ms (its default): a power cut can lose 50 ms |
| faktory (defaults) | embedded Redis with RDB snapshots (save 30 5 / save 120 1), no AOF: a crash can lose up to 30 s of acknowledged jobs |

| durability | system | scenario | enqueued/s | enqueue p50 / p99 ms | completed/s | pickup p50 / p99 ms | errors |
|---|---|---|---:|---:|---:|---:|---:|
| fsync before every acknowledgement | baton --fsync always | enqueue, 1 connections | 470 | 2.06 / 3.80 |  |  | 0 |
| fsync before every acknowledgement | baton --fsync always | enqueue, 8 connections | 1,741 | 4.46 / 8.26 |  |  | 0 |
| fsync before every acknowledgement | baton --fsync always | enqueue, 64 connections | 14,148 | 4.33 / 8.13 |  |  | 0 |
| fsync before every acknowledgement | baton --fsync always | end to end, 8 producers, 32 workers | 1,772 | 4.33 / 8.26 | 1,772 | 4.33 / 8.39 | 0 |
| fsync before every acknowledgement | beanstalkd -f 0 | enqueue, 1 connections | 754 | 0.97 / 2.69 |  |  | 0 |
| fsync before every acknowledgement | beanstalkd -f 0 | enqueue, 8 connections | 745 | 10.75 / 18.35 |  |  | 0 |
| fsync before every acknowledgement | beanstalkd -f 0 | enqueue, 64 connections | 743 | 83.89 / 146.80 |  |  | 0 |
| fsync before every acknowledgement | beanstalkd -f 0 | end to end, 8 producers, 32 workers | 367 | 20.97 / 46.14 | 367 | 20.97 / 46.14 | 0 |
| fsync every 50 ms | baton --fsync interval (50 ms) | enqueue, 1 connections | 6,296 | 0.12 / 0.21 |  |  | 0 |
| fsync every 50 ms | baton --fsync interval (50 ms) | enqueue, 8 connections | 24,678 | 0.27 / 0.44 |  |  | 0 |
| fsync every 50 ms | baton --fsync interval (50 ms) | enqueue, 64 connections | 28,695 | 1.83 / 15.47 |  |  | 0 |
| fsync every 50 ms | baton --fsync interval (50 ms) | end to end, 8 producers, 32 workers | 9,452 | 0.70 / 4.98 | 9,452 | 0.72 / 4.98 | 0 |
| fsync every 50 ms | beanstalkd -f 50 | enqueue, 1 connections | 10,028 | 0.08 / 0.14 |  |  | 0 |
| fsync every 50 ms | beanstalkd -f 50 | enqueue, 8 connections | 22,119 | 0.28 / 0.48 |  |  | 0 |
| fsync every 50 ms | beanstalkd -f 50 | enqueue, 64 connections | 19,505 | 2.42 / 21.50 |  |  | 0 |
| fsync every 50 ms | beanstalkd -f 50 | end to end, 8 producers, 32 workers | 7,455 | 0.80 / 12.58 | 7,454 | 0.84 / 12.58 | 0 |
| snapshots only | faktory (defaults) | enqueue, 1 connections | 5,364 | 0.18 / 0.29 |  |  | 0 |
| snapshots only | faktory (defaults) | enqueue, 8 connections | 22,972 | 0.29 / 1.18 |  |  | 0 |
| snapshots only | faktory (defaults) | enqueue, 64 connections | 78,165 | 0.72 / 3.47 |  |  | 0 |
| snapshots only | faktory (defaults) | end to end, 8 producers, 32 workers | 13,060 | 0.50 / 2.62 | 11,971 | 587.20 / 989.86 | 0 |

### Reading the comparison

**Where baton is slower.**

- *One connection, fsync per job:* Beanstalkd does 754 jobs/s, baton 470.
  Beanstalkd writes and fsyncs inline on its one thread; baton hands the record
  to a log thread and waits to be woken when it is durable — two thread hand-offs
  per request that buy nothing when there is nobody to share the fsync with.
- *One connection, no fsync per job:* Beanstalkd 10,028 jobs/s, baton 6,296, for
  the same reason, and because baton does more per job (timestamps, backoff
  settings, an idempotency lookup, a checksummed record).
- *Faktory at high concurrency:* 78,165 jobs/s with 64 connections against
  baton's 28,695 (and 11,971 vs 9,452 completed end to end). Faktory is a
  multi-threaded Go
  server in front of an in-memory Redis and writes nothing to disk per job;
  baton runs every command on one thread and `write()`s every record to its log
  before replying. That is a different trade, not a tuning gap: baton's
  `interval` mode still survives `kill -9` with nothing lost and a power cut with
  at most 50 ms lost, where Faktory's snapshot schedule can lose up to 30
  seconds of acknowledged jobs either way. If that is acceptable for a workload,
  Faktory's throughput is simply higher.

**Where baton is faster.**

- *fsync per job with more than one client:* Beanstalkd stays at about 745
  jobs/s whether 1, 8 or 64 connections are waiting, because each `put` pays its
  own fsync and they run one after another; latency grows with the queue (p50 84
  ms at 64 connections). baton shares each fsync among everyone who is waiting:
  1,741 jobs/s at 8 connections and 14,148 at 64 — 19 times Beanstalkd's — at a
  p50 of 4.3 ms. This is the design decision the project is built around, and the
  only comparison here in which both systems make the same promise.
- *End to end with fsync per job:* 1,772 jobs/s against 367, although baton makes
  three operations durable per job (enqueue, lease, acknowledgement) and
  Beanstalkd two: a reservation is not written to its binlog (checked: after
  `kill -9` a reserved job comes back as `ready` with `reserves: 0`).
- *fsync every 50 ms under load:* 28,695 vs 19,505 jobs/s at 64 connections and
  9,452 vs 7,455 end to end; at 8 connections the two are within 12%.

**What is not comparable.** Faktory's end-to-end pickup latency (p50 587 ms)
reflects a queue that was growing during the run (13,060 jobs/s in, 11,971 out),
not its dispatch speed. And none of this measures what the three do differently
on purpose — Beanstalkd's tiny footprint and years of production use, Faktory's
UI and ecosystem, baton's lease tokens, idempotency keys and crash testing.

## Storage: what snapshots cost and what they buy (M5)

```
bench/run_storage_bench.sh
```

builds the `release` preset and runs `bench/storage/storage_bench.cpp` against
a real ext4 file system. The source file's header describes the workloads
exactly. In short:

- **Snapshot cost** for *N* live jobs (256-byte payloads, 8 queues, 5% leased,
  10% delayed): the event-loop **pause** (`State::capture_image()`, sampled 9
  times), the background **write** (serialize, write, fsync, rename, fsync the
  directory) and the **load** at startup (read, verify, rebuild indexes and
  timers).
- **Recovery time** for a log in which each of *N* jobs was enqueued, leased and
  acknowledged (3*N* records, written by the real log writer): from the log
  alone, and from a snapshot taken at the end of that log. In the *retained*
  history all *N* finished jobs are still inside their 10-minute retention, so
  the state is as large as the history. In the *churned* history the same jobs
  are spread over 100 minutes, so nine tenths of them have been forgotten — the
  normal condition of a server that has been up for a while.

Every number except the pause is the median of 5 runs. Files had just been
written, so reads were served from the page cache: these are CPU costs, not
cold-disk costs (a cold disk adds the time to read "log MB" or "snapshot MB").

```
date:       2026-09-17T08:04Z
commit:     365023a
kernel:     Linux 6.18.33.2-microsoft-standard-WSL2
cpu:        AMD Ryzen 9 8945HS w/ Radeon 780M Graphics (16 threads)
filesystem: ext4 on /dev/sdd
compiler:   c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0
build:      CMAKE_BUILD_TYPE=Release

Payload 256 bytes per job.
```

| live jobs | pause p50 ms | pause max ms | write ms | load ms | snapshot MB | state MB |
|---:|---:|---:|---:|---:|---:|---:|
|     10000 |     0.15 |     1.06 |     10.7 |      1.6 |      2.9 |      5.5 |
|    100000 |     3.53 |    17.87 |     44.7 |     25.9 |     28.8 |     55.2 |
|   1000000 |   125.58 |   187.27 |    380.7 |    316.1 |    288.3 |    552.0 |

| history | jobs | log records | log MB | jobs in memory | snapshot MB | recover from log ms | recover from snapshot ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| retained |     10000 |     30000 |      3.7 |     10000 |      3.0 |       3.6 |       3.3 |
| churned  |     10000 |     30000 |      3.7 |       999 |      0.3 |       3.6 |       1.1 |
| retained |    100000 |    300000 |     37.3 |    100000 |     29.8 |      55.8 |      61.6 |
| churned  |    100000 |    300000 |     37.3 |      9990 |      3.0 |      59.9 |      14.9 |
| retained |   1000000 |   3000000 |    373.9 |   1000000 |    298.0 |     508.8 |     593.4 |
| churned  |   1000000 |   3000000 |    373.9 |     99900 |     29.8 |     578.8 |      69.0 |

### Reading the numbers

**The pause is small until the live set is large, and then it is not.** Copying
the state stops the event loop for 0.15 ms at 10,000 live jobs and 3.5 ms at
100,000, but 126 ms (187 ms worst of 9) at a million. It grows faster than the
job count — 35× for 10× the jobs — because at a million jobs the copy no longer
fits the CPU caches: every job costs a hash-table node and a reference-count
update on its payload, each a cache miss. A quarter-second stall once per 256
MiB of log is tolerable for a job queue and would not be for a cache; it is
listed as a limitation (design doc 8.8) with the fix on the roadmap. Payload
size does not enter into it: payloads are shared, not copied.

**Writing is off the event loop.** 381 ms for a 288 MB snapshot runs on the
snapshot thread while clients are served
(`ServerSnapshotTest.TrafficContinuesWhileASnapshotIsBeingWritten`).

**A snapshot does not make recovery faster by itself.** Replay runs at roughly
5 million records per second here, and loading a job from a snapshot costs
about as much as replaying the three records of its life: with everything
retained, recovery from the snapshot (593 ms) is no faster than from the log
(509 ms) — slightly slower, since it also has to scan the newest log segment
for the end of the log.

**What a snapshot buys is a bound.** Once history outgrows the live state — the
churned rows — recovery from the snapshot takes 69 ms instead of 579 ms at a
million jobs, reads 30 MB instead of 374 MB, and, unlike the log, neither
number grows with uptime. Log-only recovery time is proportional to everything
that ever happened; snapshot recovery time is proportional to what is still
live plus at most `--snapshot-every` of log.

### Variance

Across six runs on this machine while developing the benchmark, the
million-job pause median ranged from 122 to 187 ms, snapshot writes from 381 to
726 ms, and recoveries by up to ±30%. It is a laptop under WSL2 with a desktop
session running. The table above is one run, unedited; treat differences
smaller than that spread as noise.
