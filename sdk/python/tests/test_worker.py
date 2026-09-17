"""baton.Worker against a real server: tasks, failures, heartbeats, lost leases, shutdown."""

from __future__ import annotations

import signal
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path

import baton
from support import wait_until

SCRIPT = Path(__file__).with_name("worker_script.py")


def make_worker(server, queues=("q",), **options) -> baton.Worker:
    options.setdefault("reserve_timeout_ms", 100)
    options.setdefault("lease_ms", 5_000)
    return baton.Worker(list(queues), port=server.port, **options)


@contextmanager
def running(worker: baton.Worker):
    abandoned = []
    thread = threading.Thread(
        target=lambda: abandoned.append(worker.run(install_signal_handlers=False)))
    thread.start()
    try:
        yield
    finally:
        worker.stop()
        thread.join(40)
        assert not thread.is_alive(), "the worker did not shut down"
        assert abandoned == [0], "jobs were still running at shutdown"


def test_tasks_run_with_their_arguments_and_are_acked(server, client):
    worker = make_worker(server, concurrency=3)
    seen = []

    @worker.task()
    def greet(name, punctuation="."):
        job = baton.current_job()
        seen.append((name, punctuation, job.attempt, job.queue))

    @worker.task("math.add")
    def add(a, b):
        seen.append(a + b)

    ids = [client.enqueue_task("q", "greet", ["ada"], {"punctuation": "!"}),
           client.enqueue_task("q", "greet", ["bob"]),
           client.enqueue_task("q", "math.add", [2, 3])]
    with running(worker):
        wait_until(lambda: client.stats("q").succeeded == 3, what="three acks")
    assert sorted(map(str, seen)) == sorted(map(str, [
        ("ada", "!", 1, "q"), ("bob", ".", 1, "q"), 5]))
    assert all(client.status(i).state == "succeeded" for i in ids)
    assert worker.stats == {"succeeded": 3, "failed": 0, "lease_lost": 0}


def test_handlers_run_concurrently(server, client):
    worker = make_worker(server, concurrency=4)
    barrier = threading.Barrier(4, timeout=20)

    @worker.task()
    def meet():
        barrier.wait()  # passes only if four handlers are running at the same time

    for _ in range(4):
        client.enqueue_task("q", "meet")
    with running(worker):
        wait_until(lambda: client.stats("q").succeeded == 4, what="four concurrent jobs")


def test_exceptions_retry_then_dead_letter(server, client):
    worker = make_worker(server)
    attempts = []

    @worker.task()
    def flaky():
        attempts.append(baton.current_job().attempt)
        raise RuntimeError("the database is on fire")

    job_id = client.enqueue_task("q", "flaky", max_attempts=3, backoff_ms=(1, 5))
    with running(worker):
        wait_until(lambda: client.status(job_id).state == "dead", what="dead-lettering")
    assert attempts == [1, 2, 3]
    assert client.status(job_id).last_error == "RuntimeError: the database is on fire"


def test_retry_fatal_and_unknown_tasks(server, client):
    worker = make_worker(server)
    calls = []

    @worker.task()
    def polite():
        calls.append(time.monotonic())
        if len(calls) == 1:
            raise baton.Retry("not yet", in_ms=300)

    @worker.task()
    def hopeless():
        raise baton.Fatal("this payload can never work")

    polite_id = client.enqueue_task("q", "polite")
    hopeless_id = client.enqueue_task("q", "hopeless", max_attempts=10)
    unknown_id = client.enqueue_task("q", "not_registered_here", backoff_ms=(60_000, 60_000))
    garbage_id = client.enqueue("q", "this is not an envelope")
    with running(worker):
        wait_until(lambda: client.status(polite_id).state == "succeeded", what="the retry")
        wait_until(lambda: client.status(hopeless_id).state == "dead", what="Fatal")
        wait_until(lambda: client.status(unknown_id).attempts == 1
                   and client.status(unknown_id).state == "scheduled", what="unknown task")
        wait_until(lambda: client.status(garbage_id).state == "dead", what="garbage payload")

    assert calls[1] - calls[0] >= 0.29, "Retry(in_ms=300) chose the delay"
    assert client.status(hopeless_id).attempts == 1, "Fatal does not use up the attempts"
    assert "Fatal: this payload can never work" in client.status(hopeless_id).last_error
    assert "not_registered_here" in client.status(unknown_id).last_error, "retried, not dead"
    assert "not a task envelope" in client.status(garbage_id).last_error


def test_on_result_reports_every_delivery_once(server, client):
    outcomes = []
    worker = make_worker(server, lease_ms=600,
                         on_result=lambda job, outcome: outcomes.append((job.id, outcome)))

    @worker.task()
    def fine():
        pass

    @worker.task()
    def broken():
        raise baton.Fatal("no")

    @worker.task()
    def cancelled_under_me():
        baton.current_job().wait_lease_lost(20)

    ids = [client.enqueue_task("q", name) for name in ("fine", "broken", "cancelled_under_me")]
    with running(worker):
        wait_until(lambda: client.status(ids[2]).state == "leased", what="the third job")
        client.cancel(ids[2])
        wait_until(lambda: len(outcomes) == 3, what="three outcomes")
    assert sorted(outcomes) == [(ids[0], "acked"), (ids[1], "failed"), (ids[2], "lease_lost")]


def test_raw_handlers_get_the_job_itself(server, client):
    worker = make_worker(server, queues=["blobs"])
    seen = []

    @worker.raw("blobs")
    def handle(job):
        seen.append((job.payload, job.id, job.token > 0))

    job_id = client.enqueue("blobs", b"\x00\x01 not json")
    with running(worker):
        wait_until(lambda: client.status(job_id).state == "succeeded", what="raw job")
    assert seen == [(b"\x00\x01 not json", job_id, True)]


def test_heartbeats_keep_a_long_job_alive(server, client):
    worker = make_worker(server, lease_ms=600)
    runs = []

    @worker.task()
    def slow():
        runs.append(baton.current_job().attempt)
        time.sleep(2.5)  # more than four lease periods
        assert not baton.current_job().lease_lost

    job_id = client.enqueue_task("q", "slow")
    with running(worker):
        wait_until(lambda: client.status(job_id).state == "succeeded", what="the long job")
    assert runs == [1], "delivered exactly once: the lease never expired"
    assert client.stats("q").total_failed_attempts == 0


def test_a_cancelled_job_loses_its_lease_and_is_not_acked(server, client):
    worker = make_worker(server, lease_ms=600)
    observed = []

    @worker.task()
    def patient():
        observed.append(baton.current_job().wait_lease_lost(20))

    job_id = client.enqueue_task("q", "patient")
    with running(worker):
        wait_until(lambda: client.status(job_id).state == "leased", what="the job to start")
        client.cancel(job_id)
        wait_until(lambda: observed, what="the handler to notice")
    assert observed == [True], "the next heartbeat answered STALE"
    assert client.status(job_id).state == "cancelled", "and the result was not acked over it"
    assert worker.stats["lease_lost"] == 1


def test_graceful_stop_finishes_the_running_job_and_takes_no_new_one(server, client):
    worker = make_worker(server, concurrency=1)
    started = threading.Event()

    @worker.task()
    def slow():
        started.set()
        time.sleep(1.0)

    first = client.enqueue_task("q", "slow")
    with running(worker):
        assert started.wait(10)
        second = client.enqueue_task("q", "slow")
        worker.stop()
    assert client.status(first).state == "succeeded", "finished and acked during shutdown"
    assert client.status(second).state == "ready", "not taken after stop()"


def test_survives_a_server_restart(server, client):
    worker = make_worker(server)
    done = []

    @worker.task()
    def note(value):
        done.append(value)

    with running(worker):
        client.enqueue_task("q", "note", ["before"], key="before")
        wait_until(lambda: "before" in done, what="the first job")
        server.kill()
        time.sleep(0.3)
        server.start()
        client.enqueue_task("q", "note", ["after"], key="after")
        wait_until(lambda: "after" in done, timeout=30, what="the job after the restart")


# --- a real worker process, real signals ----------------------------------------------------


def spawn_worker(server, markers: Path, lease_ms: int) -> subprocess.Popen:
    markers.mkdir(exist_ok=True)
    (markers / "ready").unlink(missing_ok=True)
    process = subprocess.Popen(
        [sys.executable, str(SCRIPT), str(server.port), "q", str(markers), str(lease_ms)],
        stderr=open(markers / "worker.log", "ab"))
    wait_until(lambda: (markers / "ready").exists(), what="the worker process")
    return process


def test_sigterm_lets_the_running_job_finish_and_exits_zero(server, client, tmp_path):
    markers = tmp_path / "markers"
    process = spawn_worker(server, markers, lease_ms=5_000)
    try:
        first = client.enqueue_task("q", "slow", ["first", 1.5])
        wait_until(lambda: (markers / "first.started-1").exists(), what="the job to start")
        second = client.enqueue_task("q", "slow", ["second", 0])
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=30) == 0
    finally:
        process.kill()
    assert (markers / "first.finished-1").exists(), "the handler was not interrupted"
    assert client.status(first).state == "succeeded", "and its result was acked"
    assert client.status(second).state == "ready", "no new job after SIGTERM"


def test_a_duplicate_delivery_does_not_duplicate_a_ledger_effect(server, client, tmp_path):
    """The worker dies after the side effect and before the ACK, so the handler
    runs twice. With the effect recorded through a Ledger it still happens once."""
    from baton.idempotent import Ledger

    markers = tmp_path / "markers"
    ledger_path, runs_path = str(tmp_path / "effects.ledger"), str(tmp_path / "runs.log")
    first = spawn_worker(server, markers, lease_ms=1_000)
    job_id = client.enqueue_task("q", "charge_then_crash", [ledger_path, runs_path])
    assert first.wait(timeout=30) == -signal.SIGKILL, "the handler killed its own process"

    second = spawn_worker(server, markers, lease_ms=1_000)
    try:
        wait_until(lambda: client.status(job_id).state == "succeeded", timeout=30,
                   what="the redelivery")
    finally:
        second.send_signal(signal.SIGTERM)
        second.wait(timeout=30)

    assert Path(runs_path).read_text().splitlines() == [
        "attempt=1 charged=True", "attempt=2 charged=False"], "the handler really ran twice"
    assert [entry.key for entry in Ledger(ledger_path).entries()] == [f"charge-{job_id}"]


def test_a_killed_worker_loses_nothing(server, client, tmp_path):
    markers = tmp_path / "markers"
    doomed = spawn_worker(server, markers, lease_ms=1_000)
    job_id = client.enqueue_task("q", "slow", ["job", 0.5])
    wait_until(lambda: (markers / "job.started-1").exists(), what="the job to start")
    doomed.send_signal(signal.SIGKILL)
    doomed.wait()
    assert client.status(job_id).state == "leased", "the server cannot know yet"

    survivor = spawn_worker(server, markers, lease_ms=1_000)
    try:
        wait_until(lambda: client.status(job_id).state == "succeeded", timeout=30,
                   what="redelivery after the lease expired")
    finally:
        survivor.send_signal(signal.SIGTERM)
        survivor.wait(timeout=30)
    assert not (markers / "job.finished-1").exists()
    assert (markers / "job.finished-2").exists(), "the second delivery completed it"
    assert client.status(job_id).attempts == 2
