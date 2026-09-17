"""The real binary, the real file system, and SIGKILL.

What a client has been told must survive; what it has not been told may or may
not. After each kill the server must come back by itself.
"""

from __future__ import annotations

import random
import socket

import pytest
import redis

from helpers import client, fields


def encode(*args) -> bytes:
    out = b"*%d\r\n" % len(args)
    for arg in args:
        data = arg if isinstance(arg, bytes) else str(arg).encode()
        out += b"$%d\r\n%s\r\n" % (len(data), data)
    return out


def test_acknowledged_work_survives_sigkill(server):
    r = client(server)
    ids = [r.execute_command("ENQUEUE", "q", f"job-{i}", "KEY", f"key-{i}") for i in range(200)]
    _, token, *_ = r.execute_command("RESERVE", 0, 60_000, "q")
    r.execute_command("ACK", ids[0], token)
    _, held_token, *_ = r.execute_command("RESERVE", 0, 60_000, "q")

    server.kill()
    server.start()

    r = client(server)
    assert fields(r.execute_command("STATUS", ids[0]))["state"] == b"succeeded"
    assert fields(r.execute_command("STATUS", ids[1]))["state"] == b"leased"
    assert fields(r.execute_command("STATUS", ids[-1], "PAYLOAD"))["payload"] == b"job-199"
    # Idempotency keys survive too: no duplicate job after a crash.
    assert r.execute_command("ENQUEUE", "q", "job-5", "KEY", "key-5") == ids[5]
    # A worker that outlived the server can still finish its job.
    assert r.execute_command("ACK", ids[1], held_token) == b"OK"
    assert r.execute_command("ENQUEUE", "q", "new") == ids[-1] + 1, "ids are never reused"


@pytest.mark.parametrize("seed", range(5))
def test_sigkill_in_the_middle_of_a_pipeline(server, seed):
    """Kill the server while a pipeline is in flight, repeatedly.

    Every reply that was read before the kill names a job that must exist
    afterwards. Replies that never arrived promise nothing.
    """
    rng = random.Random(seed)
    confirmed: list[int] = []
    for _ in range(4):
        sock = socket.create_connection(("127.0.0.1", server.port))
        burst = rng.randint(200, 2000)
        sock.sendall(b"".join(encode("ENQUEUE", "burst", f"payload-{i}") for i in range(burst)))

        # Read some of the replies, then pull the plug mid-stream.
        wanted = rng.randint(1, burst)
        buffer = b""
        sock.settimeout(10)
        while buffer.count(b"\r\n") < wanted:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
        server.kill()
        sock.close()

        lines = buffer.split(b"\r\n")[:-1]  # drop a possibly incomplete last line
        assert all(line.startswith(b":") for line in lines), lines[:3]
        confirmed += [int(line[1:]) for line in lines]

        server.start()
        r = client(server)
        missing = [job for job in confirmed if fields(r.execute_command("STATUS", job))["id"] != job]
        assert not missing
    assert len(confirmed) == len(set(confirmed)), "a job id was handed out twice"


def test_sigterm_is_a_clean_shutdown(server):
    r = client(server)
    r.execute_command("ENQUEUE", "q", "x")
    assert server.terminate() == 0
    assert "stopped last_lsn=1" in server.log()
    server.start()
    assert fields(client(server).execute_command("STATUS", 1))["state"] == b"ready"


def test_second_instance_on_the_same_directory_is_refused(server, make_server):
    twin = make_server()
    twin.data_dir = server.data_dir
    twin.start(expect_failure=True)
    assert twin.wait() == 1
    assert "in use by another baton process" in twin.log()
    assert client(server).ping(), "the first instance is unaffected"


def test_torn_tail_is_repaired_on_a_real_file_system(server):
    r = client(server)
    for i in range(20):
        r.execute_command("ENQUEUE", "q", f"job-{i}")
    server.kill()

    # The last record was torn by the crash.
    segment = server.segments()[-1]
    size = segment.stat().st_size
    with open(segment, "r+b") as f:
        f.truncate(size - 7)

    server.start()
    assert "truncating torn tail" in server.log()
    r = client(server)
    assert fields(r.execute_command("STATUS", 19))["state"] == b"ready"
    with pytest.raises(redis.ResponseError, match="^NOTFOUND"):
        r.execute_command("STATUS", 20)
    assert r.execute_command("ENQUEUE", "q", "after repair") == 20


def test_mid_log_damage_makes_the_server_refuse_to_start(server):
    r = client(server)
    for i in range(20):
        r.execute_command("ENQUEUE", "q", f"job-{i}")
    server.kill()

    segment = server.segments()[0]
    data = bytearray(segment.read_bytes())
    data[len(data) // 2] ^= 0x10  # one flipped bit, in the middle
    segment.write_bytes(data)

    server.start(expect_failure=True)
    assert server.wait() == 1
    assert "refusing to drop acknowledged data" in server.log()
    assert segment.read_bytes() == bytes(data), "refusing must not modify the log"
