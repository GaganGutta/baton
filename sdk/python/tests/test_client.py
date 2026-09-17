"""baton.Client against a real server, including connections that die at the worst moment."""

from __future__ import annotations

import json
import socket
import threading
import time

import pytest

import baton
from baton.resp import Parser


def test_job_lifecycle(client):
    job_id = client.enqueue("emails", b"hello \x00 bytes", priority=5, max_attempts=3,
                            backoff_ms=(10, 100), key="k1")
    assert client.enqueue("emails", "again", key="k1") == job_id, "same key, same job"

    status = client.status(job_id, payload=True)
    assert (status.state, status.queue, status.priority, status.max_attempts) == (
        "ready", "emails", 5, 3)
    assert status.key == "k1" and status.payload == b"hello \x00 bytes"
    assert client.status(job_id).payload is None, "payload only on request"

    job = client.reserve("emails", lease_ms=5_000)
    assert (job.id, job.queue, job.payload, job.attempt, job.max_attempts) == (
        job_id, "emails", b"hello \x00 bytes", 1, 3)
    assert client.reserve(["emails", "other"]) is None, "nothing else is ready"

    now_ms = int(time.time() * 1000)
    assert client.heartbeat(job.id, job.token, 60_000) > now_ms + 50_000
    client.ack(job.id, job.token)
    assert client.status(job_id).state == "succeeded"

    stats = client.stats("emails")
    assert (stats.succeeded, stats.total_enqueued, stats.ready) == (1, 1, 0)
    assert [s.queue for s in client.all_stats()] == ["emails"]
    assert client.ping()
    assert client.info("persistence")["fsync_policy"] == "always"
    assert client.info()["durable_lsn"] >= 3


def test_delays_and_the_task_envelope(client):
    client.enqueue("q", "later", delay_ms=60_000)
    client.enqueue("q", "much later", at_ms=int(time.time() * 1000) + 3_600_000)
    assert client.stats("q").scheduled == 2
    assert client.reserve("q") is None

    client.enqueue_task("q", "send_email", ["ada@example.com"], {"subject": "hi"}, priority=1)
    job = client.reserve("q")
    assert json.loads(job.payload) == {
        "task": "send_email", "args": ["ada@example.com"], "kwargs": {"subject": "hi"}}


def test_failures_retries_and_the_dead_letter_queue(client):
    job_id = client.enqueue("q", "fragile", max_attempts=2)
    first = client.reserve("q")
    result = client.fail(first.id, first.token, "boom", retry_in_ms=0)
    assert result.outcome == "retry" and result.retry_at > 0

    second = client.reserve("q", timeout_ms=2_000)
    assert (second.id, second.attempt) == (job_id, 2)
    assert second.token > first.token, "fencing tokens only grow"
    assert client.fail(second.id, second.token, "boom again") == baton.FailResult("dead", 0)

    (dead,) = client.dlq_list("q")
    assert (dead.id, dead.state, dead.last_error) == (job_id, "dead", "boom again")
    client.dlq_retry(job_id)
    assert client.status(job_id).state == "ready"

    third = client.reserve("q")
    client.fail(third.id, third.token, "for good", no_retry=True)
    assert client.dlq_purge_all("q") == 1
    with pytest.raises(baton.NotFound):
        client.status(job_id)
    with pytest.raises(ValueError):
        client.fail(1, 1, "x", retry_in_ms=5, no_retry=True)


def test_every_error_code_has_its_exception(client, make_server):
    with pytest.raises(baton.NotFound):
        client.status(999)
    with pytest.raises(baton.InvalidRequest):
        client.enqueue("bad queue name", "x")

    job_id = client.enqueue("q", "x")
    job = client.reserve("q")
    with pytest.raises(baton.StaleLease) as stale:
        client.ack(job.id, job.token + 1)
    assert stale.value.code == "STALE"
    client.cancel(job_id)
    with pytest.raises(baton.WrongState):
        client.cancel(job_id)
    with pytest.raises(baton.StaleLease):
        client.heartbeat(job.id, job.token)  # how a worker learns about a cancellation

    small = make_server("--max-payload", "1k")
    with small.client() as c:
        with pytest.raises(baton.LimitExceeded):
            c.enqueue("q", "x" * 2_000)
        assert c.enqueue("q", "fits") == 1, "the connection survives an oversized payload"

    locked = make_server("--requirepass", "open sesame")
    with locked.client() as anonymous:
        with pytest.raises(baton.AuthError):
            anonymous.ping()
    with locked.client(password="wrong") as intruder:
        with pytest.raises(baton.AuthError):
            intruder.ping()
    with locked.client(password="open sesame") as friend:
        assert friend.ping()


def test_blocking_reserve(server, client):
    started = time.monotonic()
    assert client.reserve("q", timeout_ms=300) is None
    assert 0.25 < time.monotonic() - started < 5

    def produce():
        time.sleep(0.2)
        with server.client() as producer:
            producer.enqueue("q", "wake up")

    thread = threading.Thread(target=produce)
    thread.start()
    job = client.reserve("q", timeout_ms=20_000, lease_ms=1_000)
    thread.join()
    assert job.payload == b"wake up"


def test_reconnects_after_a_server_restart(server, client):
    job_id = client.enqueue("q", "before", key="a")
    server.kill()
    server.start()
    assert client.status(job_id).state == "ready", "acknowledged work survived, and so did we"
    assert client.enqueue("q", "before", key="a") == job_id


def test_gives_up_after_retry_for(make_server):
    server = make_server()
    c = server.client(retry_for=0.5, connect_timeout=0.2)
    assert c.ping()
    server.kill()
    started = time.monotonic()
    with pytest.raises(baton.ConnectionLost):
        c.ping()
    assert time.monotonic() - started < 5


# --- the connection dies after the request was sent -----------------------------------------


class ReplyDroppingProxy:
    """Forwards to the server. While ``drop`` is positive, a request is forwarded
    and executed, but its reply is swallowed and the client's connection closed:
    the client cannot know whether the command ran."""

    def __init__(self, upstream_port: int):
        self.upstream_port = upstream_port
        self.drop = 0
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                downstream, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(downstream,), daemon=True).start()

    def _serve(self, downstream: socket.socket):
        upstream = socket.create_connection(("127.0.0.1", self.upstream_port))
        try:
            while True:
                request = downstream.recv(65536)
                if not request:
                    return
                upstream.sendall(request)
                parser, raw = Parser(), b""
                while True:
                    data = upstream.recv(65536)
                    raw += data
                    parser.feed(data)
                    if not data or parser.next_reply() is not Parser.INCOMPLETE:
                        break
                if self.drop > 0:
                    self.drop -= 1
                    return  # the server has executed it; the client will never hear
                downstream.sendall(raw)
        finally:
            downstream.close()
            upstream.close()

    def close(self):
        self.listener.close()


@pytest.fixture
def proxy(server):
    p = ReplyDroppingProxy(server.port)
    yield p
    p.close()


def test_keyed_enqueue_is_retried_and_does_not_duplicate(proxy, client):
    with baton.Client(port=proxy.port) as flaky:
        proxy.drop = 1
        job_id = flaky.enqueue("q", "exactly one of me", key="order-42")
    assert proxy.drop == 0, "the first reply really was lost"
    assert client.stats("q").total_enqueued == 1
    assert client.status(job_id).key == "order-42"


def test_unkeyed_enqueue_refuses_to_guess(proxy, client):
    with baton.Client(port=proxy.port) as flaky:
        proxy.drop = 1
        with pytest.raises(baton.EnqueueUncertain) as uncertain:
            flaky.enqueue("q", "did I happen?")
        assert "key=" in str(uncertain.value)
        # This time it did happen; a blind retry would have made it two.
        assert client.stats("q").total_enqueued == 1
        assert flaky.enqueue("q", "the connection is usable again") == 2


def test_ack_whose_reply_was_lost_still_counts(proxy, client):
    job_id = client.enqueue("q", "x")
    with baton.Client(port=proxy.port) as flaky:
        job = flaky.reserve("q")
        proxy.drop = 1
        flaky.ack(job.id, job.token)  # resent; the token that completed the job gets OK again
    assert client.status(job_id).state == "succeeded"


def test_a_resent_ack_never_claims_another_workers_success(proxy, client):
    """Found by the chaos harness. The SDK used to resolve a resent ACK that
    answered STALE by asking STATUS, and took `succeeded` to mean "my ACK counted".
    When a zombie did that for a job that someone else had completed, two workers
    believed they had acknowledged it. The server now answers a repeated ACK
    exactly, so there is nothing left to infer."""
    job_id = client.enqueue("q", "x", backoff_ms=(1, 1))
    with baton.Client(port=proxy.port) as zombie:
        stale = zombie.reserve("q", lease_ms=300)
        fresh = client.reserve("q", timeout_ms=10_000)  # after the zombie's lease expired
        assert (fresh.id, fresh.attempt) == (job_id, 2)
        client.ack(fresh.id, fresh.token)

        proxy.drop = 1  # the zombie's ACK is sent twice, like any ACK whose reply is lost
        with pytest.raises(baton.StaleLease):
            zombie.ack(stale.id, stale.token)
    client.ack(fresh.id, fresh.token)  # while the ACK that did count can be repeated
    assert client.status(job_id).state == "succeeded"


def test_stale_ack_after_a_resend_is_still_stale_if_the_job_did_not_succeed(proxy, client):
    job_id = client.enqueue("q", "x")
    with baton.Client(port=proxy.port) as flaky:
        job = flaky.reserve("q")
        client.cancel(job_id)  # the lease is gone before the ack
        proxy.drop = 1
        with pytest.raises(baton.StaleLease):
            flaky.ack(job.id, job.token)
    assert client.status(job_id).state == "cancelled"
