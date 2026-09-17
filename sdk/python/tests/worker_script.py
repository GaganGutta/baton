"""A worker process for the tests that need one to signal or kill.

usage: worker_script.py <port> <queue> <marker-dir> <lease-ms>
"""

import logging
import os
import sys
import time
from pathlib import Path

try:
    import baton
except ImportError:  # not installed: use the source tree
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    import baton

port, queue, marker_dir, lease_ms = sys.argv[1], sys.argv[2], Path(sys.argv[3]), int(sys.argv[4])
logging.basicConfig(level=logging.INFO, stream=sys.stderr)

worker = baton.Worker([queue], port=int(port), concurrency=1, lease_ms=lease_ms,
                      reserve_timeout_ms=200, shutdown_timeout=20)


@worker.task()
def slow(name, seconds):
    job = baton.current_job()
    (marker_dir / f"{name}.started-{job.attempt}").write_text(str(os.getpid()))
    time.sleep(seconds)
    (marker_dir / f"{name}.finished-{job.attempt}").write_text(str(os.getpid()))


@worker.task()
def charge_then_crash(ledger_path, runs_path):
    """Performs its side effect through a Ledger; the first delivery dies before the ACK."""
    from baton.idempotent import Ledger

    job = baton.current_job()
    charged = Ledger(ledger_path).put_if_absent(f"charge-{job.id}", {"amount": 12},
                                                token=job.token)
    with open(runs_path, "a") as runs:
        runs.write(f"attempt={job.attempt} charged={charged}\n")
        runs.flush()
        os.fsync(runs.fileno())
    if job.attempt == 1:
        os.kill(os.getpid(), 9)  # after the effect, before the ACK: the worst moment


(marker_dir / "ready").write_text(str(os.getpid()))
abandoned = worker.run()
sys.exit(0 if abandoned == 0 else 3)
