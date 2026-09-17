"""The processes of a chaos round: producers, workers and the rogue.

    actors.py <producer|worker|rogue> --out DIR --name NAME --port N --seed N [...]

Each writes JSON lines that the orchestrator evaluates afterwards
(docs/design.md 10.1). They stop when DIR/stop appears (producers, rogue) or on
SIGTERM (workers).
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sdk" / "python" / "src"))

import baton  # noqa: E402
from baton.idempotent import Ledger  # noqa: E402
from baton.resp import Connection, ServerError  # noqa: E402

QUEUE = "chaos"
SHARED_KEYS = 40  # keys that every producer enqueues, again and again


class JsonLog:
    """One JSON object per line. ``durable`` adds an fsync per line."""

    def __init__(self, path: Path, durable: bool):
        self._file = open(path, "a", encoding="utf-8")
        self._durable = durable

    def write(self, event: str, **fields) -> None:
        self._file.write(json.dumps({"e": event, "t": round(time.time(), 3), **fields}) + "\n")
        self._file.flush()
        if self._durable:
            os.fsync(self._file.fileno())


def produce(args) -> None:
    """Enqueues keyed jobs; journals the intent before and the reply after each one."""
    rng = random.Random(args.seed)
    journal = JsonLog(args.out / f"journal-{args.name}.log", durable=True)
    client = baton.Client(port=args.port, retry_for=120.0, timeout=5.0, name=args.name)
    mine = []
    serial = 0
    while not (args.out / "stop").exists():
        roll = rng.random()
        if roll < 0.10:
            key = f"shared-{rng.randrange(SHARED_KEYS)}"
        elif roll < 0.20 and mine:
            key = rng.choice(mine)  # a caller that retries something it already enqueued
        else:
            kind = rng.choices(["job", "flaky", "poison"], weights=[88, 10, 2])[0]
            key = f"{kind}-{args.name}-{serial}"
            serial += 1
            mine.append(key)
        journal.write("intent", key=key)
        try:
            # Padding makes the log grow like a real one: segments roll, snapshots
            # are taken and the log is compacted many times per round.
            padding = "p" * rng.randrange(200, 1500)
            job_id = client.enqueue_task(QUEUE, "effect", [key], {"pad": padding}, key=key,
                                         max_attempts=1000, backoff_ms=(20, 200))
        except baton.BatonError as error:
            journal.write("error", key=key, error=f"{type(error).__name__}: {error}")
            time.sleep(0.2)
            continue
        journal.write("ok", key=key, id=job_id)
        time.sleep(rng.uniform(0, 2 * args.pace_ms / 1000.0))


def work(args) -> None:
    """A baton.Worker whose side effect is a ledger entry; every run is logged."""
    logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                        format="%(asctime)s %(levelname)s %(message)s")
    rng = random.Random(args.seed)
    runs = JsonLog(args.out / f"runs-{args.name}.log", durable=False)
    ledger = Ledger(str(args.out / "effects.ledger"))

    def on_result(job, outcome):
        runs.write("result", job=job.id, token=job.token, outcome=outcome)

    worker = baton.Worker([QUEUE], port=args.port, concurrency=args.concurrency,
                          lease_ms=args.lease_ms, reserve_timeout_ms=300, shutdown_timeout=20,
                          name=args.name, on_result=on_result)

    @worker.task()
    def effect(key, pad=""):
        job = baton.current_job()
        runs.write("start", job=job.id, token=job.token, attempt=job.attempt, key=key)
        if key.startswith("poison-"):
            raise baton.Fatal("poison")
        if key.startswith("flaky-") and job.attempt == 1:
            raise RuntimeError("flaky on the first attempt")
        time.sleep(rng.uniform(0, args.work_ms / 1000.0))
        landed = ledger.put_if_absent(key, {"job": job.id, "token": job.token}, token=job.token)
        runs.write("effect", job=job.id, token=job.token, key=key, landed=landed)

    abandoned = worker.run()
    sys.exit(0 if abandoned == 0 else 3)


def go_rogue(args) -> None:
    """A zombie on purpose: lets its lease die, waits until a successor holds the job
    (or the job has ended), and then uses the dead token - ACK, HEARTBEAT or FAIL.

    Every such request must be answered STALE. It is sent once, on a bare
    connection, so that what is logged is the server's answer to exactly that
    request and not the outcome of a client's retries. The well-behaved workers
    never get this far: the SDK notices a lost lease and sends nothing.
    """
    rng = random.Random(args.seed)
    log = JsonLog(args.out / f"rogue-{args.name}.log", durable=False)
    client = baton.Client(port=args.port, retry_for=60.0, timeout=5.0, name=args.name)
    while not (args.out / "stop").exists():
        try:
            job = client.reserve(QUEUE, timeout_ms=300, lease_ms=300)
            if job is None:
                continue
            log.write("reserved", job=job.id, token=job.token, attempt=job.attempt)
            # The dangerous moment for a zombie's token is not when its lease has
            # just expired (the job is waiting out its backoff, nobody holds it) but
            # when its successor is at work. Wait for that, or for the job to end.
            state, deadline = "leased", time.monotonic() + 20
            while time.monotonic() < deadline:
                time.sleep(0.01)
                try:
                    status = client.status(job.id)
                except baton.NotFound:
                    state = "collected"
                    break
                state = status.state
                if status.attempts > job.attempt and state == "leased":
                    state = "held by a successor"
                    break
                if state in ("succeeded", "dead", "cancelled"):
                    break
            if state == "leased":
                log.write("gave_up", job=job.id, token=job.token)
                continue

            command = rng.choices(["ACK", "HEARTBEAT", "FAIL"], weights=[6, 2, 2])[0]
            raw = Connection(port=args.port, timeout=5.0)
            try:
                raw.connect()
                reply = raw.call(command, job.id, job.token)
            finally:
                raw.close()
            accepted = not isinstance(reply, ServerError)
            log.write("stale_attempt", job=job.id, token=job.token, command=command, when=state,
                      accepted=accepted, code=None if accepted else reply.code)
        except baton.BatonError as error:
            log.write("error", error=f"{type(error).__name__}: {error}")
            time.sleep(0.2)
        time.sleep(rng.uniform(0.02, 0.2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=["producer", "worker", "rogue"])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--pace-ms", type=float, default=4.0)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--lease-ms", type=int, default=1500)
    parser.add_argument("--work-ms", type=float, default=100.0)
    args = parser.parse_args()
    {"producer": produce, "worker": work, "rogue": go_rogue}[args.role](args)


if __name__ == "__main__":
    main()
