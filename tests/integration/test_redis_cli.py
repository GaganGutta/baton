"""baton through redis-cli: what a person poking at the server actually types."""

from __future__ import annotations

import shutil
import subprocess

import pytest

pytestmark = pytest.mark.skipif(shutil.which("redis-cli") is None, reason="redis-cli not installed")


def cli(server, *args: str, password: str | None = None) -> str:
    command = ["redis-cli", "-p", str(server.port)]
    if password:
        command += ["-a", password, "--no-auth-warning"]
    done = subprocess.run(command + list(args), capture_output=True, text=True, timeout=30)
    return done.stdout.strip()


def test_basic_session(server):
    assert cli(server, "PING") == "PONG"
    assert cli(server, "ENQUEUE", "emails", "hello world") == "1"
    assert cli(server, "ENQUEUE", "emails", "later", "DELAY", "60000") == "2"

    lease = cli(server, "RESERVE", "0", "30000", "emails").splitlines()
    assert lease[:4] == ["1", "1", "emails", "hello world"]

    status = cli(server, "STATUS", "2").splitlines()
    assert status[status.index("state") + 1] == "scheduled"

    assert cli(server, "ACK", "1", "1") == "OK"
    assert cli(server, "ACK", "1", "1").startswith("STALE")
    assert cli(server, "RESERVE", "0", "30000", "emails") == ""  # nil


def test_errors_are_readable(server):
    assert "unknown command" in cli(server, "HGETALL", "x")
    assert "wrong number of arguments" in cli(server, "ENQUEUE", "q")
    assert cli(server, "STATUS", "12345").startswith("NOTFOUND")
    assert "queue name must match" in cli(server, "ENQUEUE", "no spaces allowed", "x")


def test_info_looks_like_redis_info(server):
    cli(server, "ENQUEUE", "q", "x")
    info = cli(server, "INFO")
    assert "# Server" in info and "# Persistence" in info and "# Jobs" in info
    assert "durable_lsn:1" in info
    assert "jobs_ready:1" in info


def test_auth(make_server):
    server = make_server("--requirepass", "open sesame").start()
    assert "NOAUTH" in cli(server, "PING")
    assert cli(server, "PING", password="open sesame") == "PONG"
