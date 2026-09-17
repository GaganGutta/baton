# Benchmarks

Every number in the README and in [docs/benchmarks.md](../docs/benchmarks.md)
comes from a script in this directory, together with the machine, the commit and
the exact command. Nothing is estimated.

| Script | What it measures |
|---|---|
| `run_all.sh` | enqueue throughput and latency under both fsync policies (closed and open loop), group-commit batch sizes, end-to-end throughput with N workers, pickup latency, memory per job, restart time after `kill -9`, and the Google Benchmark microbenchmarks. Writes `out/results.json` and `out/results.md` |
| `compare/run_compare.sh` | the same load against baton, Beanstalkd and Faktory, all in containers on the host network with data on a fresh volume (needs Docker). Writes `out/compare.json` and `out/compare.md` |
| `run_storage_bench.sh` | snapshot pause, write and load time; recovery from the log and from a snapshot, without the server around it |

```bash
bench/run_all.sh                 # about ten minutes
bench/run_all.sh --quick         # 3-second runs: checks the suite, not the server
bench/run_all.sh --only enqueue,e2e
bench/compare/run_compare.sh
```

## The load generator

`loadgen/` builds `baton-loadgen`, which speaks three protocols (`--protocol
baton | beanstalkd | faktory`) and has two modes:

- `--mode enqueue`: producers only. **Closed loop** by default — each of
  `--connections` keeps `--depth` requests in flight — which finds the maximum
  throughput. With `--rate N` it is **open loop**: requests go out on a fixed
  schedule and latency is measured from the moment a request was *due*, so a
  server that stalls cannot hide the stall by slowing the generator down
  (coordinated omission).
- `--mode e2e`: `--producers` enqueue while `--workers` reserve and acknowledge.
  Reports completed jobs per second and the pickup latency — from the enqueue
  being sent (or due) to a worker holding the job — using a timestamp carried in
  the payload.

One thread per connection; latencies go into per-thread log-linear histograms
(`src/common/histogram.h`: at most 3% error, tested against sorted arrays) that are merged at the end;
nothing is recorded during `--warmup`. The result is one JSON object on stdout.

## Reading the results

- A closed-loop generator measures capacity, not latency under a given load:
  when the server is slow, the generator sends less. Quote latency from the
  open-loop runs.
- `--fsync always` numbers are bounded by the disk's `fdatasync` latency, which
  the results report next to them (`fsync p50 / p99`). On another disk, expect
  the throughput at low connection counts to scale with it.
- Microbenchmarks live in `micro/` and run as part of `run_all.sh`.
