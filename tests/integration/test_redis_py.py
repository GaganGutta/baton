"""baton through redis-py, the stock Python Redis client.

The claim under test: "any Redis client library can connect". Nothing here uses
baton-specific client code; it is all `execute_command`. Every test runs three
times: with redis-py's own defaults (RESP3 since redis-py 8), and pinned to
RESP2 and RESP3.
"""

from __future__ import annotations

import threading
import time

import pytest
import redis

from helpers import client, fields


@pytest.fixture(params=["default", 2, 3], ids=["default-protocol", "resp2", "resp3"])
def options(request) -> dict:
    return {} if request.param == "default" else {"protocol": request.param}


def test_connects_and_pings(server, options):
    r = client(server, **options)
    assert r.ping() is True  # the handshake (HELLO, CLIENT SETINFO ...) succeeded too
    assert r.echo("hello") == b"hello"


def test_job_lifecycle(server, options):
    r = client(server, **options)
    job_id = r.execute_command("ENQUEUE", "emails", b'{"to": "ada"}', "PRIORITY", 3)
    assert job_id == 1

    lease = r.execute_command("RESERVE", 0, 30_000, "emails")
    got_id, token, queue, payload, attempt, max_attempts, expires_at = lease
    assert (got_id, queue, payload, attempt, max_attempts) == (1, b"emails", b'{"to": "ada"}', 1, 10)
    assert expires_at > time.time() * 1000

    status = fields(r.execute_command("STATUS", job_id))
    assert status["state"] == b"leased"
    assert status["priority"] == 3

    assert r.execute_command("HEARTBEAT", job_id, token, 60_000) > expires_at
    assert r.execute_command("ACK", job_id, token) in (b"OK", "OK")
    assert fields(r.execute_command("STATUS", job_id))["state"] == b"succeeded"
    assert r.execute_command("RESERVE", 0, 30_000, "emails") is None


def test_binary_payloads_round_trip(server, options):
    r = client(server, **options)
    payload = bytes(range(256)) * 50
    job_id = r.execute_command("ENQUEUE", "bin", payload)
    lease = r.execute_command("RESERVE", 0, 1000, "bin")
    assert lease[3] == payload
    assert fields(r.execute_command("STATUS", job_id, "PAYLOAD"))["payload"] == payload


def test_errors_carry_machine_readable_codes(server, options):
    r = client(server, **options)
    r.execute_command("ENQUEUE", "q", "x")
    _, token, *_ = r.execute_command("RESERVE", 0, 1000, "q")
    r.execute_command("ACK", 1, token)

    with pytest.raises(redis.ResponseError, match=r"^STALE "):
        r.execute_command("ACK", 1, token)
    with pytest.raises(redis.ResponseError, match=r"^NOTFOUND "):
        r.execute_command("STATUS", 999)
    with pytest.raises(redis.ResponseError, match=r"^STATE "):
        r.execute_command("CANCEL", 1)
    with pytest.raises(redis.ResponseError, match=r"^LIMIT "):
        r.execute_command("ENQUEUE", "q", b"x" * (1024 * 1024 + 1))
    with pytest.raises(redis.ResponseError, match="unknown command"):
        r.execute_command("FLUSHALL")
    assert r.ping(), "the connection survives every one of those"


def test_idempotent_enqueue(server, options):
    r = client(server, **options)
    first = r.execute_command("ENQUEUE", "orders", "charge card", "KEY", "order-42")
    again = r.execute_command("ENQUEUE", "orders", "charge card", "KEY", "order-42")
    other = r.execute_command("ENQUEUE", "orders", "charge card", "KEY", "order-43")
    assert first == again
    assert other != first
    assert fields(r.execute_command("STATS", "orders"))["total_enqueued"] == 2


def test_pipelining_preserves_order(server, options):
    r = client(server, **options)
    pipe = r.pipeline(transaction=False)
    for i in range(500):
        pipe.execute_command("ENQUEUE", "bulk", f"job-{i}")
    assert pipe.execute() == list(range(1, 501))


def test_blocking_reserve_wakes_up(server, options):
    worker = client(server, **options)
    producer = client(server, **options)
    result = {}

    def work():
        result["lease"] = worker.execute_command("RESERVE", 10_000, 1000, "inbox")

    thread = threading.Thread(target=work)
    thread.start()
    time.sleep(0.2)  # let the worker park first
    started = time.monotonic()
    producer.execute_command("ENQUEUE", "inbox", "wake up")
    thread.join(timeout=10)
    assert not thread.is_alive()
    assert result["lease"][3] == b"wake up"
    assert time.monotonic() - started < 2, "the parked worker should be woken at once"


def test_blocking_reserve_times_out_with_none(server, options):
    r = client(server, **options)
    started = time.monotonic()
    assert r.execute_command("RESERVE", 200, 1000, "nothing-here") is None
    assert 0.15 < time.monotonic() - started < 3


def test_fail_retries_then_dead_letters(server, options):
    r = client(server, **options)
    job_id = r.execute_command("ENQUEUE", "flaky", "x", "MAXATTEMPTS", 2)
    _, token, *_ = r.execute_command("RESERVE", 0, 1000, "flaky")
    assert r.execute_command("FAIL", job_id, token, "first", "RETRYIN", 0)[0] == b"retry"
    _, token, *_ = r.execute_command("RESERVE", 1000, 1000, "flaky")
    assert r.execute_command("FAIL", job_id, token, "second") == [b"dead", 0]
    status = fields(r.execute_command("STATUS", job_id))
    assert (status["state"], status["last_error"], status["attempts"]) == (b"dead", b"second", 2)


def test_decode_responses_mode_works_too(server, options):
    r = client(server, decode_responses=True, **options)
    r.execute_command("ENQUEUE", "text", "héllo")
    lease = r.execute_command("RESERVE", 0, 1000, "text")
    assert lease[2:4] == ["text", "héllo"]
    assert fields(r.execute_command("STATUS", 1))["state"] == "leased"
    # baton's INFO is formatted like Redis's, so redis-py's own parser understands it.
    info = r.info("persistence")
    assert info["fsync_policy"] == "always"
    assert info["durable_lsn"] == 2


def test_auth(make_server, options):
    server = make_server("--requirepass", "hunter2").start()
    with pytest.raises(redis.AuthenticationError):
        client(server, **options).ping()
    with pytest.raises(redis.exceptions.RedisError):
        client(server, password="wrong", **options).ping()
    assert client(server, password="hunter2", **options).execute_command("ENQUEUE", "q", "x") == 1
    # redis-py sends the username as well when one is given.
    assert client(server, username="default", password="hunter2", **options).ping()


def test_lease_expiry_redelivers_with_a_new_token(server, options):
    r = client(server, **options)
    job_id = r.execute_command("ENQUEUE", "slow", "x", "BACKOFF", 0, 0)
    _, first_token, *_ = r.execute_command("RESERVE", 0, 150, "slow")
    lease = r.execute_command("RESERVE", 5000, 1000, "slow")  # blocks until the lease expired
    assert lease[0] == job_id
    assert lease[1] > first_token
    assert lease[4] == 2
    with pytest.raises(redis.ResponseError, match=r"^STALE "):
        r.execute_command("ACK", job_id, first_token)
    assert r.execute_command("ACK", job_id, lease[1]) in (b"OK", "OK")
