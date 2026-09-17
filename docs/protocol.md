# Protocol

baton speaks **RESP**, the Redis serialization protocol, over TCP, with its own
command set: RESP2 by default, RESP3 for connections that ask for it with
`HELLO 3`. Any Redis client library that can send an arbitrary command can talk
to baton:

```python
import redis
r = redis.Redis(port=7379)
job_id = r.execute_command("ENQUEUE", "emails", '{"to": "ada@example.com"}')
```

```console
$ redis-cli -p 7379 ENQUEUE emails '{"to": "ada@example.com"}'
(integer) 1
```

This document is the contract the server is tested against. It was written
before the networking code (milestone M3); commands that arrive with later
milestones are listed at the end.

## Contents

- [Framing](#framing)
- [Replies and errors](#replies-and-errors)
- [Ordering, pipelining and durability](#ordering-pipelining-and-durability)
- [Limits](#limits)
- [Authentication](#authentication)
- [Job commands](#job-commands): ENQUEUE, RESERVE, HEARTBEAT, ACK, FAIL, CANCEL, STATUS, STATS
- [Dead-letter queue](#dead-letter-queue): DLQ.LIST, DLQ.RETRY, DLQ.PURGE
- [Connection and server commands](#connection-and-server-commands): PING, ECHO, AUTH, HELLO, INFO, QUIT, SNAPSHOT, compatibility stubs
- [Commands added by later milestones](#commands-added-by-later-milestones)

## Framing

A request is a RESP array of bulk strings, exactly what Redis clients send:

```
*3\r\n$7\r\nENQUEUE\r\n$6\r\nemails\r\n$5\r\nhello\r\n
```

- Command names and option keywords are case-insensitive. Queue names, keys
  and payloads are case-sensitive bytes.
- Payloads and error messages are binary safe.
- **Inline commands are not supported.** A request that does not start with `*`
  is a protocol error and the connection is closed. This is deliberate: it
  means an HTTP request aimed at baton's port by a browser can never be
  interpreted as a command.
- Integers in arguments are decimal, optionally signed, without whitespace, and
  must fit in 64 bits. Times are **milliseconds**: durations as counts, instants
  as milliseconds since the Unix epoch.
- A malformed frame (bad length, missing CRLF, nested array, a type other than
  bulk string inside the array) gets `-ERR Protocol error: …` and the
  connection is closed, since the stream can no longer be trusted.

## Replies and errors

baton uses the five RESP2 reply types: simple string (`+OK`), error, integer,
bulk string, array. "Nothing" is the null array `*-1` (what `RESERVE` returns
on timeout; clients surface it as `nil`/`None`).

**RESP3.** A connection that sends `HELLO 3` gets exactly two differences:
"nothing" is the RESP3 null `_`, and replies documented as *field–value pairs*
(`HELLO`, `STATUS`, `STATS`, the entries of `DLQ.LIST`) are RESP3 maps instead
of flat arrays, so client libraries hand back a dictionary. Requests are the
same in both versions. RESP3 exists in baton for one reason: some clients —
redis-py 8 among them — open every connection with `HELLO 3` by default and
treat a refusal as fatal. baton does not use RESP3 push messages, doubles,
booleans or attributes.

Errors are `-<CODE> <human readable message>`. Clients should branch on the
code, never on the message:

| Code | Meaning | Typical cause |
|---|---|---|
| `ERR` | The request is malformed. | unknown command, wrong number of arguments, bad option, invalid number, invalid queue name |
| `NOAUTH` | Authentication is required. | any command before `AUTH` when a password is set |
| `WRONGPASS` | The password is wrong. | `AUTH` |
| `NOTFOUND` | The job does not exist (it never did, or it has been collected). | `STATUS`, `ACK`, `CANCEL`, … |
| `STALE` | The lease token is not the job's current lease. The job was completed, cancelled, expired or handed to another worker. **The worker must stop working on the job and must not retry the call.** | `ACK`, `FAIL`, `HEARTBEAT` |
| `STATE` | The job exists but is in the wrong state for this command. | `CANCEL` of a finished job, `DLQ.RETRY` of a job that is not dead |
| `LIMIT` | A configured limit was hit. Nothing was changed. | payload larger than `--max-payload`, `--max-memory` reached, `--max-connections` reached, disk nearly full |
| `UNAVAILABLE` | The server is shutting down. | any command during shutdown |

Queue names must match `[A-Za-z0-9._:-]{1,128}`. Queues are created on first
use.

## Ordering, pipelining and durability

- Replies on a connection are sent in the order of its requests. Clients may
  pipeline: send many requests without waiting.
- **No reply is sent until every log record it depends on is durable**
  (fsynced, under the default `--fsync always`). This includes reads: `STATUS`
  never reveals a job whose `ENQUEUE` is not yet on disk. When a client sees
  `:42` from `ENQUEUE`, job 42 survives `kill -9` and power loss. When a worker
  sees a job from `RESERVE`, both the job and the lease are on disk.
- Requests from many connections are committed together (group commit), so
  waiting for durability costs latency, not throughput.
- A blocking `RESERVE` blocks its connection: requests pipelined behind it are
  not executed until it returns. Use separate connections for producers and
  for each concurrently blocked worker.
- If the connection drops after a request was sent but before the reply
  arrived, the client cannot know whether it was executed. `ENQUEUE` with a
  `KEY` is safe to retry; `ACK`, `FAIL`, `CANCEL` are idempotent in effect (a
  retry gets `STALE`/`STATE`); see `docs/guarantees.md`.

## Limits

| Limit | Default | Behaviour when exceeded |
|---|---|---|
| Payload size (`--max-payload`) | 1 MiB | `-LIMIT`; the oversized argument is discarded as it arrives, the connection stays usable |
| Bulk string size, absolute | 512 MiB | protocol error, connection closed |
| Arguments per request | 1,024 | protocol error, connection closed |
| Memory (`--max-memory`) | unlimited | `ENQUEUE` answers `-LIMIT` until jobs finish and are collected; everything else keeps working |
| Connections (`--max-connections`) | 10,000 | the new connection receives `-LIMIT` and is closed |
| Unsent replies per connection | 64 MiB | the connection is closed (the client is not reading) |
| `RESERVE` timeout | 0 – 3,600,000 ms | `-ERR` |
| Lease length | 100 ms – 24 h | `-ERR` |
| Delay / `AT` distance | ≤ 366 days | `-ERR` |
| `MAXATTEMPTS` | 1 – 1,000 | `-ERR` |
| Idempotency key | ≤ 256 bytes | `-ERR` |
| `FAIL` error message | 1,024 bytes | silently truncated (it is diagnostic text) |

## Authentication

By default baton listens on `127.0.0.1` only and needs no password. With
`--requirepass <password>` (or the `BATON_REQUIREPASS` environment variable),
every command except `AUTH`, `HELLO` and `QUIT` answers `-NOAUTH` until the
connection has authenticated. There is no TLS: the password crosses the wire in
clear text, so use it on trusted networks or through a TLS tunnel.

---

## Job commands

### ENQUEUE

```
ENQUEUE <queue> <payload> [PRIORITY <n>] [DELAY <ms> | AT <unix_ms>]
        [MAXATTEMPTS <n>] [BACKOFF <base_ms> <cap_ms>] [KEY <idempotency-key>]
```

Adds a job. Reply: **integer** — the job id (ids are positive and increase).

| Option | Default | Meaning |
|---|---|---|
| `PRIORITY n` | 0 | 32-bit signed; higher runs first. Within a priority, jobs run in the order they became runnable. |
| `DELAY ms` | 0 | Not before now + ms. |
| `AT unix_ms` | — | Not before this instant. A time in the past means now. Mutually exclusive with `DELAY`. |
| `MAXATTEMPTS n` | 10 | Deliveries before the job is moved to the dead-letter queue. |
| `BACKOFF base cap` | 1000 600000 | Retry delay is uniform in `[0, min(cap, base·2^(attempt−1))]` ms ("full jitter"). |
| `KEY k` | — | Idempotency key. If `k` was used by an `ENQUEUE` in the last 24 h (`--idempotency-window`), nothing is added and the **existing** job id is returned — even if that job has already finished. |

Errors: `ERR`, `LIMIT`.

### RESERVE

```
RESERVE <timeout_ms> <lease_ms> <queue> [<queue> ...]
```

Leases the best ready job from the first listed queue that has one. If none
has, waits up to `timeout_ms` (0 = do not wait) for a job to become ready in
any of them. Waiting connections are served first come, first served.

Reply: the null array on timeout, otherwise an **array of 7**:

| # | Type | Field |
|---|---|---|
| 0 | integer | job id |
| 1 | integer | **lease token** — required by `HEARTBEAT`, `ACK`, `FAIL` |
| 2 | bulk | queue |
| 3 | bulk | payload |
| 4 | integer | attempt (1 for the first delivery) |
| 5 | integer | max attempts |
| 6 | integer | lease expiry (unix ms) |

The worker owns the job until the lease expires. If it needs longer it must
`HEARTBEAT`. If the lease expires the attempt counts as failed, the job is
retried (or dead-lettered) and the token becomes `STALE`.

Tokens increase across the whole server and are never reused. A worker can pass
the token to downstream systems as a fencing token: a resource that remembers
the highest token it has seen for a job can reject writes from a zombie.

Errors: `ERR`.

### HEARTBEAT

```
HEARTBEAT <job_id> <token> [<lease_ms>]
```

Extends the lease to now + `lease_ms` (default 30,000). Reply: **integer**, the
new expiry (unix ms). `STALE` tells the worker it no longer owns the job — this
is also how a worker learns that its job was cancelled.

Errors: `ERR`, `NOTFOUND`, `STALE`.

### ACK

```
ACK <job_id> <token>
```

Marks the job succeeded. Reply: `+OK`. Once the reply is received the job will
never be delivered again.

Errors: `ERR`, `NOTFOUND`, `STALE`.

### FAIL

```
FAIL <job_id> <token> [<error> [RETRYIN <ms> | NORETRY]]
```

Reports a failed attempt. The job is retried after a backoff, or moved to the
dead-letter queue if this was its last attempt or `NORETRY` is given.
`RETRYIN` replaces the computed backoff. An option can only follow an error
message (which may be empty), so a message can never be mistaken for an
option. Reply: **array of 2** — bulk `retry`
or `dead`, and integer retry time (unix ms; 0 when dead).

Errors: `ERR`, `NOTFOUND`, `STALE`.

### CANCEL

```
CANCEL <job_id>
```

Cancels a job that has not finished. A job that is currently leased is
cancelled too; its worker finds out through `STALE` on its next call.
Reply: `+OK`.

Errors: `ERR`, `NOTFOUND`, `STATE` (already succeeded, dead or cancelled).

### STATUS

```
STATUS <job_id> [PAYLOAD]
```

Reply: a flat **array of field–value pairs** (like `HGETALL`):

| Field | Type | |
|---|---|---|
| `id` | integer | |
| `queue` | bulk | |
| `state` | bulk | `scheduled`, `ready`, `leased`, `succeeded`, `dead`, `cancelled` |
| `priority` | integer | |
| `attempts` | integer | deliveries so far |
| `max_attempts` | integer | |
| `run_at` | integer | unix ms |
| `created_at` | integer | unix ms |
| `finished_at` | integer | unix ms, 0 if not finished |
| `lease_expires_at` | integer | unix ms, 0 if not leased |
| `last_error` | bulk | empty if none |
| `key` | bulk | idempotency key, empty if none |
| `payload_size` | integer | bytes |
| `payload` | bulk | only with `PAYLOAD` |

Clients must ignore fields they do not know. Finished jobs stay visible for
`--retain-finished` (default 10 minutes; dead jobs 7 days), then answer
`NOTFOUND`.

Errors: `ERR`, `NOTFOUND`.

### STATS

```
STATS [<queue>]
```

With a queue: a flat array of field–value pairs for that queue (all zeros if it
does not exist). Without: an array with one such array per queue, in name order.

Fields: `queue` (bulk), then integers `scheduled`, `ready`, `leased`,
`succeeded`, `dead`, `cancelled` (jobs currently held in each state) and
`total_enqueued`, `total_succeeded`, `total_failed_attempts`, `total_dead`,
`total_cancelled` (since the data directory was created).

## Dead-letter queue

A job that fails its last attempt becomes `dead` and stays in its queue's
dead-letter queue for 7 days (`--retain-dead`), with its payload and last error.

```
DLQ.LIST  <queue> [<offset> [<count>]]     → array of STATUS-style arrays, oldest first (count ≤ 1000, default 100)
DLQ.RETRY <job_id>                         → :1   the job is ready again with a fresh set of attempts
DLQ.RETRY <queue> ALL                      → :n   jobs retried
DLQ.PURGE <job_id>                         → :1   the job is deleted
DLQ.PURGE <queue> ALL                      → :n   jobs deleted
```

Errors: `ERR`, `NOTFOUND`, `STATE` (the job is not dead).

## Connection and server commands

| Command | Reply | Notes |
|---|---|---|
| `PING [msg]` | `+PONG`, or `msg` as a bulk | |
| `ECHO msg` | bulk | |
| `AUTH [username] password` | `+OK` / `-WRONGPASS` | the username is accepted for compatibility and ignored |
| `HELLO [2\|3 [AUTH user pass] [SETNAME name]]` | field–value pairs describing the server | selects RESP2 or RESP3 for this connection; any other version → `-NOPROTO` |
| `INFO [section]` | bulk | `key:value` lines under `# Section` headers, like Redis; sections `server`, `clients`, `memory`, `persistence`, `jobs` |
| `QUIT` | `+OK`, then the connection is closed | |
| `CLIENT SETNAME\|SETINFO\|GETNAME\|ID …`, `SELECT 0`, `COMMAND …` | harmless replies | sent automatically by redis-cli and client libraries on connect; accepted so that stock clients work |

`INFO persistence` exposes the group-commit behaviour described in the design
doc: `last_lsn`, `durable_lsn`, `log_batches`, `log_records`,
`log_batch_records_avg`, `log_fsync_p99_us`, and so on.

### SNAPSHOT

```
SNAPSHOT        → +OK
```

Asks the server to write a snapshot of its state and then delete the log
segments that are no longer needed (design doc, section 8). The server also
does this by itself after every `--snapshot-every` bytes of log (default 256
MiB), so the command is for operators and tests: before a planned restart, to
make the next startup fast, or with `--snapshot-every 0` to decide when
snapshots happen.

`+OK` means the request was accepted, not that the snapshot is finished: it is
written in the background while the server keeps serving. Progress is visible
in `INFO persistence`:

| Field | Meaning |
|---|---|
| `snapshot_in_progress` | 1 while a snapshot is being captured or written |
| `snapshots_taken`, `snapshots_failed` | since startup; a failed snapshot is logged and costs nothing but the attempt - the log is still complete |
| `last_snapshot_lsn` | the snapshot covers every record up to this LSN |
| `last_snapshot_bytes`, `last_snapshot_jobs` | its size |
| `last_snapshot_pause_us` | how long the event loop stood still to copy the state |
| `last_snapshot_write_ms` | how long the background write took |
| `log_segments_removed` | log segments deleted by compaction since startup |
| `log_bytes_since_snapshot` | what the next automatic snapshot is measured against |
| `recovered_from_snapshot_lsn` | the snapshot this process started from (0: the log alone) |
| `recovery_snapshots_rejected` | snapshots found unreadable at startup and skipped |

If nothing has been logged since the last snapshot, `+OK` is returned and
nothing is written. Errors: `STATE` if a snapshot is already in progress.

## Commands added by later milestones

| Milestone | Commands |
|---|---|
| M9 | `WF.START`, `WF.HISTORY`, `WF.STEP`, `WF.SLEEP`, `WF.WAIT`, `WF.SIGNAL`, `WF.COMPLETE`, `WF.FAIL`, `WF.CANCEL`, `WF.STATUS` |
| M10 | `CRON.ADD`, `CRON.DEL`, `CRON.LIST`, `QUEUE.LIMIT`, `ENQUEUE … UNIQUE` |

Each is specified here before it is implemented.
