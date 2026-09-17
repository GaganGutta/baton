# Benchmarks

Rules for this file:

- Every number comes from a script in `bench/`, quoted verbatim together with
  the command, the commit, the date and the machine. Nothing is estimated,
  rounded favourably or remembered.
- A result that is unflattering stays in.
- The machine is a laptop running WSL2. Absolute numbers will differ elsewhere;
  what should carry over is the shape (what grows with what).

Throughput and latency of the server as a whole, and the comparison with
Beanstalkd and Faktory, are added in M8.

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
