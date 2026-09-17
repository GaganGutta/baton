"""One kill round: run a seeded schedule of kills against a live system, drain, check."""

from __future__ import annotations

import json
import os
import random
import re
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

from . import invariants
from .evidence import Journals, RunLogs, ledger_entries
from .procs import Server, baton, find_tool, spawn_actor  # procs puts the SDK on sys.path

SERVER_ARGS = ["--segment-size", "64k", "--snapshot-every", "96k", "--retain-finished", "24h",
               "--log-level", "info"]
LEASE_MS = 1500
DRAIN_TIMEOUT = 180.0
FATAL_MARKERS = ("CHECK failed", "AddressSanitizer", "ThreadSanitizer", "runtime error:",
                 "fatal:")


class SegmentArchiver(threading.Thread):
    """Keeps every log segment that ever existed, so that the lease check can read
    the whole history although the server compacts its log many times per round.

    A hard link, not a copy: it costs nothing, it keeps following the segment
    while the server appends to it (same inode), and it survives the server's
    unlink. A segment is only deleted two snapshots after it was closed, so
    polling cannot miss one. If a name reappears with a new inode, the server
    removed a torn segment creation after a kill and started the segment again;
    the archive follows.
    """

    def __init__(self, data_dir: Path, archive: Path):
        super().__init__(daemon=True)
        self.data_dir = data_dir
        self.archive = archive
        self._done = threading.Event()

    def run(self) -> None:
        self.archive.mkdir(parents=True, exist_ok=True)
        while not self._done.is_set():
            self.sweep()
            self._done.wait(0.02)
        self.sweep()

    def sweep(self) -> None:
        for segment in self.data_dir.glob("wal-*.log"):
            link = self.archive / segment.name
            try:
                if link.exists() and link.stat().st_ino != segment.stat().st_ino:
                    link.unlink()
                if not link.exists():
                    os.link(segment, link)
            except FileNotFoundError:
                continue  # deleted between glob() and link(): it was archived long ago

    def stop(self) -> None:
        self._done.set()
        if self.is_alive():
            self.join()


@dataclass
class Scale:
    producers: int = 2
    workers: int = 3
    concurrency: int = 4
    pace_ms: float = 2.0
    work_ms: float = 100.0
    rogues: int = 2


class Round:
    def __init__(self, seed: int, duration: float, out_dir: Path, scale: Scale):
        self.seed = seed
        self.duration = duration
        self.out = out_dir
        self.scale = scale
        self.rng = random.Random(seed)
        self.server = Server(out_dir / "data", *SERVER_ARGS)
        self.archiver = SegmentArchiver(out_dir / "data", out_dir / "log-archive")
        self.workers: List[subprocess.Popen] = []
        self.paused: Dict[int, float] = {}  # pid -> when to SIGCONT (monotonic)
        self.spawned = 0
        self.restarts: List[dict] = []
        self.actions: List[dict] = []
        self.failed_restart = ""

    # --- the actors -----------------------------------------------------------------------

    def spawn_worker(self) -> None:
        self.spawned += 1
        self.workers.append(spawn_actor(
            "worker", self.out, f"worker-{self.spawned}", port=self.server.port,
            seed=self.seed * 1000 + self.spawned, concurrency=self.scale.concurrency,
            lease_ms=LEASE_MS, work_ms=self.scale.work_ms))

    def resume_due(self, everyone: bool = False) -> None:
        now = time.monotonic()
        for worker in self.workers:
            due = self.paused.get(worker.pid)
            if due is not None and (everyone or now >= due):
                del self.paused[worker.pid]
                if worker.poll() is None:
                    worker.send_signal(signal.SIGCONT)

    # --- the schedule ---------------------------------------------------------------------

    def act(self, action: str) -> None:
        entry = {"at": round(time.monotonic() - self.began, 2), "action": action}
        if action == "kill_server":
            self.server.kill()
            outage = self.rng.uniform(0.0, 1.0)
            time.sleep(outage)
            try:
                entry.update(self.server.start(), outage_s=round(outage, 2))
                self.restarts.append(entry)
            except RuntimeError as error:
                self.failed_restart = f"after kill #{len(self.restarts) + 1}: {error}"
        elif action == "kill_worker":
            victim = self.rng.randrange(len(self.workers))
            entry["pid"] = self.workers[victim].pid
            self.workers[victim].send_signal(signal.SIGKILL)
            self.workers[victim].wait()
            self.paused.pop(self.workers[victim].pid, None)
            del self.workers[victim]
            self.spawn_worker()
        elif action == "pause_worker":
            # Longer than a lease: its jobs are handed to others while it still
            # believes it owns them. A zombie, once it wakes up.
            candidates = [w for w in self.workers if w.pid not in self.paused]
            if candidates:
                victim = self.rng.choice(candidates)
                pause = self.rng.uniform(1.5, 2.5) * LEASE_MS / 1000.0
                victim.send_signal(signal.SIGSTOP)
                self.paused[victim.pid] = time.monotonic() + pause
                entry.update(pid=victim.pid, pause_s=round(pause, 2))
        elif action == "snapshot":
            try:
                with self.server.client(retry_for=0) as client:
                    client.snapshot()
            except baton.BatonError as error:  # one is already running: fine
                entry["note"] = type(error).__name__
        self.actions.append(entry)

    def run(self) -> dict:
        self.out.mkdir(parents=True, exist_ok=True)
        self.began = time.monotonic()
        self.archiver.start()
        self.server.start()
        self.sources = [
            spawn_actor("producer", self.out, f"producer-{i}", port=self.server.port,
                        seed=self.seed * 100 + i, pace_ms=self.scale.pace_ms)
            for i in range(self.scale.producers)]
        for _ in range(self.scale.workers):
            self.spawn_worker()
        self.sources += [
            spawn_actor("rogue", self.out, f"rogue-{i}", port=self.server.port,
                        seed=self.seed * 10 + i)
            for i in range(self.scale.rogues)]

        next_action = self.began + self.rng.uniform(1.0, 2.0)
        while time.monotonic() - self.began < self.duration and not self.failed_restart:
            self.resume_due()
            if time.monotonic() >= next_action:
                self.act(self.rng.choices(
                    ["kill_server", "kill_worker", "pause_worker", "snapshot"],
                    weights=[4, 3, 2, 1])[0])
                next_action = time.monotonic() + self.rng.uniform(0.3, 1.5)
            time.sleep(0.02)

        # The killing stops. Producers and the rogue finish what they are doing and
        # exit; then everything that was acknowledged must complete.
        (self.out / "stop").touch()
        self.resume_due(everyone=True)
        for source in self.sources:
            source.wait(timeout=200)
        report = {"seed": self.seed, "duration_s": self.duration, "scale": vars(self.scale),
                  "actions": dict(_count(a["action"] for a in self.actions))}
        violations = self.drain_and_check(report) if not self.failed_restart else {}
        violations["5 recovers after every kill"] = invariants.recovers_every_time(
            self.restarts, self.failed_restart, report)

        report["violations"] = {name: found for name, found in violations.items() if found}
        report["ok"] = not report["violations"]
        (self.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (self.out / "actions.json").write_text(json.dumps(self.actions, indent=2) + "\n")
        return report

    # --- after the storm --------------------------------------------------------------------

    def drain_and_check(self, report: dict) -> Dict[str, List[str]]:
        drained_in = self.drain()
        report["drain_s"] = drained_in

        worker_exits = []
        for worker in self.workers:
            worker.send_signal(signal.SIGTERM)
        for worker in self.workers:
            worker_exits.append(worker.wait(timeout=60))

        journals = Journals(self.out)
        with self.server.client() as client:
            states = {}
            for job_id in journals.acknowledged:
                try:
                    states[job_id] = client.status(job_id).state
                except baton.NotFound:
                    states[job_id] = "missing"
            total_enqueued = client.stats("chaos").total_enqueued
            persistence = client.info("persistence")
        report["snapshots_taken_by_last_server"] = persistence["snapshots_taken"]
        report["log_segments_removed_by_last_server"] = persistence["log_segments_removed"]

        server_exit = self.server.terminate()
        log = self.server.log()
        report["snapshots_written"] = log.count(" snapshot: done ")
        report["log_segments_compacted"] = sum(
            int(n) for n in re.findall(r" snapshot: done .*segments_removed=(\d+)", log))
        self.archiver.stop()
        # The whole history, from the archive of every segment that ever existed;
        # and the compacted directory, where the check has to start from a snapshot.
        logcheck = self.run_logcheck(self.archiver.archive, "logcheck.json")
        compacted = self.run_logcheck(self.server.data_dir, "logcheck-compacted.json")
        report["logcheck_compacted"] = {k: compacted.get(k) for k in (
            "ok", "started_after_lsn", "records", "leases_granted", "error")}
        if not compacted.get("ok"):
            logcheck = {"ok": False, "violations": (
                [f"compacted directory: {v}" for v in compacted.get("violations", [])]
                or [f"compacted directory: {compacted.get('error')}"])
                + logcheck.get("violations", [])}
        runs = RunLogs(self.out)
        ledger = ledger_entries(self.out / "effects.ledger")

        violations = {
            "1 no lost jobs": invariants.no_lost_jobs(journals, states, report),
            "2 one valid lease": invariants.one_valid_lease(runs, logcheck, report),
            "3 effects land once": invariants.effects_land_once(journals, runs, states, ledger,
                                                                report),
            "4 one key, one job": invariants.one_key_one_job(journals, total_enqueued, report),
        }
        if drained_in is None:
            violations["1 no lost jobs"].insert(
                0, f"the queue did not drain within {DRAIN_TIMEOUT:.0f}s")

        health = []
        if server_exit != 0:
            health.append(f"the server exited with {server_exit} on SIGTERM")
        if any(code != 0 for code in worker_exits):
            health.append(f"worker exit codes on SIGTERM: {worker_exits}")
        health += [f"the server log contains '{marker}'" for marker in FATAL_MARKERS
                   if marker in log]
        violations["process health"] = health
        return violations

    def drain(self) -> Optional[float]:
        """Waits until no job is pending or leased. Seconds it took, or None on timeout."""
        began = time.monotonic()
        with self.server.client(retry_for=30.0) as client:
            while time.monotonic() - began < DRAIN_TIMEOUT:
                stats = client.stats("chaos")
                if stats.ready + stats.scheduled + stats.leased == 0:
                    return round(time.monotonic() - began, 1)
                time.sleep(0.25)
        return None

    def run_logcheck(self, directory: Path, save_as: str) -> dict:
        tool = find_tool("baton-logcheck", "BATON_LOGCHECK")
        done = subprocess.run([str(tool), str(directory)], capture_output=True, text=True,
                              timeout=300)
        (self.out / save_as).write_text(done.stdout)
        try:
            return json.loads(done.stdout)
        except ValueError:
            return {"ok": False, "error": f"exit {done.returncode}: {done.stderr[-500:]}"}

    def cleanup(self) -> None:
        self.archiver.stop()
        (self.out / "stop").touch()
        for process in self.workers + getattr(self, "sources", []):
            if process.poll() is None:
                process.kill()
                process.wait()
        self.server.stop()


def _count(items) -> Dict[str, int]:
    counts: Dict[str, int] = {}
    for item in items:
        counts[item] = counts.get(item, 0) + 1
    return counts
