"""Fixtures for integration tests: the real baton binary, real sockets, real files.

The binary is taken from $BATON_BIN, or the first build directory that has one.
"""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def find_binary() -> Path:
    candidates = [os.environ.get("BATON_BIN", "")]
    candidates += [str(REPO / "build" / preset / "src" / "server" / "baton")
                   for preset in ("release", "asan", "debug", "tsan")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise RuntimeError("no baton binary found: build one or set BATON_BIN")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class BatonServer:
    """A baton process on a private port and data directory."""

    def __init__(self, data_dir: Path, *extra_args: str):
        self.binary = find_binary()
        self.data_dir = data_dir
        self.extra_args = list(extra_args)
        self.port = free_port()
        self.process: subprocess.Popen | None = None
        self.log_path = data_dir.parent / f"{data_dir.name}.stderr.log"

    def start(self, expect_failure: bool = False) -> "BatonServer":
        log = open(self.log_path, "ab")
        self.process = subprocess.Popen(
            [str(self.binary), "--dir", str(self.data_dir), "--port", str(self.port),
             *self.extra_args],
            stdout=log, stderr=log)
        if expect_failure:
            return self
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"baton exited with {self.process.returncode}:\n{self.log()}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.02)
        raise RuntimeError(f"baton did not start listening:\n{self.log()}")

    def kill(self) -> None:
        """SIGKILL: no chance to flush anything."""
        assert self.process is not None
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()

    def terminate(self) -> int:
        """SIGTERM: graceful shutdown. Returns the exit code."""
        assert self.process is not None
        self.process.send_signal(signal.SIGTERM)
        return self.process.wait(timeout=30)

    def wait(self, timeout: float = 30) -> int:
        assert self.process is not None
        return self.process.wait(timeout=timeout)

    def log(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def segments(self) -> list[Path]:
        return sorted(self.data_dir.glob("wal-*.log"))

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait()


@pytest.fixture
def server(tmp_path):
    s = BatonServer(tmp_path / "data").start()
    yield s
    s.stop()


@pytest.fixture
def make_server(tmp_path):
    """Factory for tests that need special flags or several servers."""
    created: list[BatonServer] = []

    def factory(*extra_args: str, name: str = "data") -> BatonServer:
        s = BatonServer(tmp_path / name, *extra_args)
        created.append(s)
        return s

    yield factory
    for s in created:
        s.stop()
