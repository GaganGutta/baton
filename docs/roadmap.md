# Roadmap

Things that are tempting but out of scope for the current plan. They are written
down here instead of being built, so the core stays small and provable.

## Out of scope by design (non-goals)

- Replication, clustering, leader election.
- TLS termination inside baton.
- Datasets larger than RAM.

## Ideas for later

- **More of RESP3.** `HELLO 3`, maps and the RESP3 null are supported (M3,
  because redis-py 8 requires them); push messages, e.g. to tell a worker that
  its job was cancelled without waiting for the next heartbeat, are not.
- **Release a lease whose RESERVE reply could not be delivered.** Today a job
  leased to a connection that died before reading the reply waits out its
  lease.
- **Time zones for cron schedules.** Schedules are UTC-only in the plan.
- **Windows-native build.** WSL2 and Docker cover Windows users today.
- **Per-job result retention policies** beyond a single server-wide setting.
- **Rate limiting** per queue (tokens per second), beyond concurrency limits.
- **Payload compression** in the log and snapshots.
- **Incremental snapshot capture.** The event-loop pause of a snapshot is a
  copy of every job's metadata, so it grows with the live set (measured in
  `docs/benchmarks.md`). Copying a slice per loop iteration, with copy-on-write
  for jobs touched meanwhile, would make it constant. Incremental snapshot
  *files* (deltas) are a separate, larger idea (design doc, 8.1).
- **More snapshot triggers**: time-based, at shutdown, and a retry soon after a
  failed attempt; plus I/O throttling so a snapshot cannot hurt the log's fsync
  latency on a shared disk.
- **Retention during replay.** Recovery keeps every job of the replayed log tail
  in memory until replay ends. Enforcing retention while replaying would bound
  recovery memory even with snapshots turned off.

Items get added here whenever a milestone surfaces something worth doing that
is not worth doing now.
