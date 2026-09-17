# Guarantees

This file states exactly what baton promises. The rule of the project is that
**every promise here is checked by a test**, and the README never claims more
than this file does. Each row names the test (or chaos invariant) that would
fail if the promise were broken.

A promise appears here only once the code and the test both exist. Planned
promises live in `PLAN.md` until then.

## Durability

| # | Promise | Checked by |
|---|---|---|
| | *(filled in from M1 onwards)* | |

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
