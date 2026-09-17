"""The dead-letter queue through redis-py, and leases across a SIGKILL."""

from __future__ import annotations

import time

import pytest
import redis

from helpers import client, fields


@pytest.fixture(params=[2, 3], ids=["resp2", "resp3"])
def r(server, request):
    return client(server, protocol=request.param)


def kill_job(r, queue: str, payload: str, error: str) -> int:
    job_id = r.execute_command("ENQUEUE", queue, payload)
    _, token, *_ = r.execute_command("RESERVE", 0, 1000, queue)
    assert r.execute_command("FAIL", job_id, token, error, "NORETRY")[0] == b"dead"
    return job_id


def test_operating_the_dead_letter_queue(r):
    ids = [kill_job(r, "mail", f"letter-{i}", "smtp down") for i in range(5)]

    listed = [fields(entry) for entry in r.execute_command("DLQ.LIST", "mail")]
    assert [entry["id"] for entry in listed] == ids
    assert {entry["state"] for entry in listed} == {b"dead"}
    assert listed[0]["last_error"] == b"smtp down"
    page = r.execute_command("DLQ.LIST", "mail", 3, 10)
    assert [fields(entry)["id"] for entry in page] == ids[3:]

    assert r.execute_command("DLQ.RETRY", ids[0]) == 1
    lease = r.execute_command("RESERVE", 0, 1000, "mail")
    assert (lease[0], lease[3], lease[4]) == (ids[0], b"letter-0", 1)
    assert r.execute_command("ACK", ids[0], lease[1]) in (b"OK", "OK")

    assert r.execute_command("DLQ.PURGE", ids[1]) == 1
    with pytest.raises(redis.ResponseError, match="^NOTFOUND"):
        r.execute_command("STATUS", ids[1])
    with pytest.raises(redis.ResponseError, match="^STATE"):
        r.execute_command("DLQ.RETRY", ids[0])

    assert r.execute_command("DLQ.RETRY", "mail", "ALL") == 3
    assert r.execute_command("DLQ.LIST", "mail") == []
    assert fields(r.execute_command("STATS", "mail"))["ready"] == 3


def test_lease_survives_sigkill_and_restart(make_server):
    server = make_server("--lease-grace", "2s").start()
    r = client(server)
    job_id = r.execute_command("ENQUEUE", "q", "x", "BACKOFF", 0, 0)
    _, token, *_ = r.execute_command("RESERVE", 0, 300, "q")

    server.kill()
    time.sleep(0.5)  # down for longer than the lease had left
    server.start()

    r = client(server)
    assert fields(r.execute_command("STATUS", job_id))["state"] == b"leased"
    assert r.execute_command("HEARTBEAT", job_id, token, 5000) > 0
    assert r.execute_command("ACK", job_id, token) == b"OK"


def test_unclaimed_lease_is_redelivered_after_the_grace_period(make_server):
    server = make_server("--lease-grace", "500ms").start()
    r = client(server)
    job_id = r.execute_command("ENQUEUE", "q", "x", "BACKOFF", 0, 0)
    _, old_token, *_ = r.execute_command("RESERVE", 0, 300, "q")

    server.kill()
    server.start()

    r = client(server)
    lease = r.execute_command("RESERVE", 10_000, 5000, "q")  # waits out the grace period
    assert lease[0] == job_id and lease[1] > old_token and lease[4] == 2
    with pytest.raises(redis.ResponseError, match="^STALE"):
        r.execute_command("ACK", job_id, old_token)
