# Roadmap

Things that are tempting but out of scope for the current plan. They are written
down here instead of being built, so the core stays small and provable.

## Out of scope by design (non-goals)

- Replication, clustering, leader election.
- TLS termination inside baton.
- Datasets larger than RAM.

## Ideas for later

- **RESP3** support (`HELLO 3`), so clients get maps instead of flat arrays.
- **Time zones for cron schedules.** Schedules are UTC-only in the plan.
- **Windows-native build.** WSL2 and Docker cover Windows users today.
- **Per-job result retention policies** beyond a single server-wide setting.
- **Rate limiting** per queue (tokens per second), beyond concurrency limits.
- **Payload compression** in the log and snapshots.

Items get added here whenever a milestone surfaces something worth doing that
is not worth doing now.
