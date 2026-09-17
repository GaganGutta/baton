"""SDK tests run against the real baton binary: $BATON_BIN, or a build directory."""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
try:
    import baton  # the installed package, as in CI
except ImportError:  # or straight from the source tree
    sys.path.insert(0, str(REPO / "sdk" / "python" / "src"))
    import baton


def find_binary() -> Path:
    candidates = [os.environ.get("BATON_BIN", "")]
    candidates += [str(REPO / "build" / preset / "src" / "server" / "baton")
                   for preset in ("release", "asan", "debug", "tsan")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise RuntimeError("no baton binary found: build one or set BATON_BIN")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class BatonServer:
    """A baton process on a private port and data directory. Restarts keep both."""

    def __init__(self, data_dir: Path, *extra_args: str):
        self.binary = find_binary()
        self.data_dir = data_dir
        self.extra_args = list(extra_args)
        self.port = free_port()
        self.process = None
        self.log_path = data_dir.parent / f"{data_dir.name}.stderr.log"

    def start(self) -> "BatonServer":
        log = open(self.log_path, "ab")
        self.process = subprocess.Popen(
            [str(self.binary), "--dir", str(self.data_dir), "--port", str(self.port),
             *self.extra_args],
            stdout=log, stderr=log)
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
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()

    def log(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def client(self, **kwargs) -> "baton.Client":
        return baton.Client(port=self.port, **kwargs)


@pytest.fixture
def make_server(tmp_path):
    created = []

    def factory(*extra_args: str) -> BatonServer:
        server = BatonServer(tmp_path / f"data-{len(created)}", *extra_args).start()
        created.append(server)
        return server

    yield factory
    for server in created:
        server.stop()


@pytest.fixture
def server(make_server):
    return make_server()


@pytest.fixture
def client(server):
    with server.client() as c:
        yield c
