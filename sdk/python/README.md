# baton Python SDK

A client and a worker for [baton](../../README.md). No dependencies, Python 3.9+.
Not on PyPI; install it from the repository:

```bash
pip install "git+https://github.com/GaganGutta/baton#subdirectory=sdk/python"
```

## Producing

```python
import baton

client = baton.Client("127.0.0.1", 7379)

# When enqueue() returns, the job is on disk: it survives kill -9 and power loss.
job_id = client.enqueue_task("emails", "send_welcome", ["ada@example.com"],
                             key="welcome-ada")          # idempotency key
client.enqueue("reports", b"any bytes", delay_ms=60_000, priority=5, max_attempts=3)
```

Give `key=` whenever a duplicate would hurt. With a key, enqueueing is safe to
repeat — by you, and by the SDK, which then retries by itself after a connection
failure. Without one, a connection that dies between request and reply raises
`EnqueueUncertain` instead of guessing.

## Working

```python
import baton

worker = baton.Worker(["emails"], concurrency=8, lease_ms=30_000)

@worker.task()
def send_welcome(address):
    job = baton.current_job()          # id, token, attempt, lease_lost, ...
    ...

worker.run()   # until SIGTERM or SIGINT
```

- **Return** acknowledges the job. **Any exception** fails the attempt: baton
  retries with exponential backoff and jitter, and moves the job to the
  dead-letter queue after `max_attempts`. `raise baton.Retry(in_ms=5000)` picks
  the delay; `raise baton.Fatal("…")` dead-letters immediately.
- **Heartbeats are automatic.** A handler may run for hours on a 30-second
  lease; if the worker dies, its jobs are redelivered 30 seconds later.
- **`job.lease_lost`** turns true if the job was cancelled or the lease could
  not be kept (a long partition). The run's result is then discarded; stop when
  convenient.
- **SIGTERM** stops taking jobs and lets running handlers finish for up to
  `shutdown_timeout` seconds. Whatever is still running then is redelivered
  after its lease expires — exactly as after a crash.
- Threads, so handlers should be I/O-bound. For CPU-bound work run more worker
  processes; the server does not mind.

## Doing something once

Delivery is **at least once**: a worker can die after the side effect and
before the acknowledgement, and then the handler runs again. Exactly-once
effects need the effect and the record of it to commit atomically.
`baton.idempotent.Ledger` is a small fsynced, multi-process-safe set for that:

```python
from baton.idempotent import Ledger

ledger = Ledger("/var/lib/myapp/effects.ledger")

@worker.task()
def charge(order):
    job = baton.current_job()
    # The ledger entry is the effect: exactly once, whoever gets killed when.
    ledger.put_if_absent(f"charge-{job.id}", {"order": order}, token=job.token)

@worker.task()
def send_email(address):
    job = baton.current_job()
    # The effect lives elsewhere: at least once, never again once recorded.
    ledger.once(f"email-{job.id}", lambda: smtp_send(address), token=job.token)
```

`once()` cannot close the window between the effect and its record — nothing
outside the remote system can. If a repeat there is unacceptable, give the
remote system an idempotency key of its own: `job.id` is stable across
redeliveries, and `job.token` is a fencing token that only ever grows.

## Everything else

`client.reserve / heartbeat / ack / fail / cancel / status / stats / dlq_list /
dlq_retry / dlq_purge / snapshot / info` map one-to-one to
[the protocol](../../docs/protocol.md). Server errors are exceptions named after
their codes: `StaleLease`, `NotFound`, `WrongState`, `LimitExceeded`,
`AuthError`, `InvalidRequest`, `Unavailable`.

A `Client` is one connection and is not thread-safe: use one per thread.

## Tests

```bash
cmake --preset release && cmake --build --preset release
pip install pytest
BATON_BIN=build/release/src/server/baton python -m pytest sdk/python/tests
```

They run against the real server binary and include killed workers, a killed
server, replies lost in transit, and racing ledger writers.
