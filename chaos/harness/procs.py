"""Process control for the chaos harness: the server, and the Python actors."""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "sdk" / "python" / "src"))

import baton  # noqa: E402  (the SDK is part of what is being tested)

ACTORS = Path(__file__).with_name("actors.py")


def find_tool(name: str, env: str) -> Path:
    """A built binary: $BATON_BIN / $BATON_LOGCHECK, or the first build tree that has it."""
    relative = {"baton": "src/server/baton", "baton-logcheck": "tools/baton-logcheck"}[name]
    candidates = [os.environ.get(env, "")]
    candidates += [str(REPO / "build" / preset / relative)
                   for preset in ("release", "asan", "debug", "tsan")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise SystemExit(f"no {name} binary found: build it or set ${env}")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """The baton server under test. Restarts keep the port and the data directory."""

    def __init__(self, data_dir: Path, *args: str, port: Optional[int] = None):
        self.binary = find_tool("baton", "BATON_BIN")
        self.data_dir = data_dir
        self.args = list(args)
        self.port = port or free_port()
        self.log_path = data_dir.parent / f"{data_dir.name}.server.log"
        self.process: Optional[subprocess.Popen] = None
        self.starts: List[Dict[str, float]] = []

    def spawn(self, preexec_fn: Optional[Callable[[], None]] = None,
              restore_signals: bool = True) -> subprocess.Popen:
        log = open(self.log_path, "ab")
        self.process = subprocess.Popen(
            [str(self.binary), "--dir", str(self.data_dir), "--port", str(self.port), *self.args],
            stdout=log, stderr=log, preexec_fn=preexec_fn, restore_signals=restore_signals)
        log.close()
        return self.process

    def start(self, timeout: float = 120.0, **spawn_options) -> Dict[str, float]:
        """Starts the server and waits until it answers. Returns how long that took and
        what the server says about its own recovery."""
        began = time.monotonic()
        self.spawn(**spawn_options)
        while True:
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"the server exited with {self.process.returncode} during startup; "
                    f"see {self.log_path}")
            try:
                with baton.Client(port=self.port, retry_for=0, connect_timeout=0.2) as client:
                    info = client.info("persistence")
                break
            except baton.BatonError:
                if time.monotonic() - began > timeout:
                    raise RuntimeError(f"the server did not come up in {timeout}s") from None
                time.sleep(0.01)
        started = {
            "wall_ms": round((time.monotonic() - began) * 1000, 1),
            "recovery_ms": info["recovery_ms"],
            "recovered_records": info["recovered_records"],
            "from_snapshot_lsn": info["recovered_from_snapshot_lsn"],
            "torn_bytes": info["recovered_torn_bytes"],
            "snapshots_rejected": info["recovery_snapshots_rejected"],
        }
        self.starts.append(started)
        return started

    def wait_exit(self, timeout: float = 60.0) -> int:
        return self.process.wait(timeout=timeout)

    def kill(self) -> None:
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()

    def terminate(self, timeout: float = 120.0) -> int:
        self.process.send_signal(signal.SIGTERM)
        return self.process.wait(timeout=timeout)

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def client(self, **options) -> "baton.Client":
        return baton.Client(port=self.port, **options)

    def log(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def segments(self) -> List[Path]:
        return sorted(self.data_dir.glob("wal-*.log"))

    def snapshots(self) -> List[Path]:
        return sorted(self.data_dir.glob("snapshot-*.snap"))


def spawn_actor(role: str, out_dir: Path, name: str, **options) -> subprocess.Popen:
    """Starts chaos/harness/actors.py as ``role``; its stderr goes to <name>.stderr.log."""
    command = [sys.executable, str(ACTORS), role, "--out", str(out_dir), "--name", name]
    for key, value in options.items():
        command += [f"--{key.replace('_', '-')}", str(value)]
    log = open(out_dir / f"{name}.stderr.log", "ab")
    process = subprocess.Popen(command, stdout=log, stderr=log)
    log.close()
    return process
