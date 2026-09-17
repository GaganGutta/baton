"""The four fault scenarios (docs/design.md 10.4).

Each builds a server with acknowledged work, injects one fault into its data
directory (or its environment), and checks what the server does about it
against what had been acknowledged before. Deterministic: no seeds involved.
"""

from __future__ import annotations

import hashlib
import os
import resource
import struct
import time
from pathlib import Path
from typing import Callable, Dict, List

from .procs import Server, baton

QUEUE = "faults"


class Failed(Exception):
    """A scenario's requirement was not met."""


def require(condition: bool, what: str) -> None:
    if not condition:
        raise Failed(what)


def enqueue_many(client, count: int, prefix: str, size: int = 200) -> Dict[int, str]:
    """Enqueues and returns {job id: payload} - only what was acknowledged."""
    acknowledged = {}
    for i in range(count):
        payload = f"{prefix}-{i}-" + "x" * size
        acknowledged[client.enqueue(QUEUE, payload, key=f"{prefix}-{i}")] = payload
    return acknowledged


def require_all_present(server: Server, acknowledged: Dict[int, str]) -> None:
    with server.client() as client:
        for job_id, payload in acknowledged.items():
            try:
                status = client.status(job_id, payload=True)
            except baton.NotFound:
                raise Failed(f"acknowledged job {job_id} is gone") from None
            require(status.payload == payload.encode(), f"job {job_id} came back with another "
                                                         "payload")


def fingerprint(directory: Path) -> Dict[str, str]:
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.iterdir()) if p.is_file() and p.name != "LOCK"}


# --- 1. a torn log tail -------------------------------------------------------------------


def torn_log_tail(out: Path) -> str:
    server = Server(out / "data", "--snapshot-every", "0")
    try:
        server.start()
        with server.client() as client:
            acknowledged = enqueue_many(client, 300, "torn")
        server.kill()

        # (a) What a power failure during a write leaves behind: part of a record
        # that nobody was told about. Everything acknowledged must survive it.
        newest = server.segments()[-1]
        intact = newest.read_bytes()
        half_a_record = struct.pack("<II", 4096, 0xDEADBEEF) + b"\x01" + b"half a record"
        newest.write_bytes(intact + half_a_record)
        started = server.start()
        require(started["torn_bytes"] == len(half_a_record),
                f"expected {len(half_a_record)} torn bytes to be reported, got "
                f"{started['torn_bytes']}")
        require_all_present(server, acknowledged)
        with server.client() as client:
            more = enqueue_many(client, 20, "after-repair")
        server.kill()

        # (b) Chop off the end of the log. This destroys acknowledged records - a
        # real torn write cannot - so the requirement is weaker: the server starts,
        # and what it has is a gap-free prefix of what was acknowledged.
        newest = server.segments()[-1]
        chopped = 777
        newest.write_bytes(newest.read_bytes()[:-chopped])
        server.start()
        everything = {**acknowledged, **more}
        with server.client() as client:
            present = []
            for job_id in sorted(everything):
                try:
                    client.status(job_id)
                    present.append(job_id)
                except baton.NotFound:
                    pass
            require(present == list(range(1, len(present) + 1)),
                    "after truncation the surviving jobs are not a gap-free prefix")
            require(len(present) < len(everything), "chopping 777 bytes lost nothing?")
            new_id = client.enqueue(QUEUE, "life goes on")
            require(new_id == len(present) + 1, "ids do not continue after the surviving prefix")
        return (f"garbage tail: all {len(acknowledged)} jobs kept, {len(half_a_record)} torn "
                f"bytes cut; {chopped} bytes chopped: a gap-free prefix of {len(present)}/"
                f"{len(everything)} jobs, and the log accepts writes")
    finally:
        server.stop()


# --- 2. a flipped bit in the middle of the log --------------------------------------------


def flipped_bit_mid_log(out: Path) -> str:
    server = Server(out / "data", "--snapshot-every", "0", "--segment-size", "64k")
    try:
        server.start()
        with server.client() as client:
            acknowledged = enqueue_many(client, 600, "flip")  # several segments
        require(server.terminate() == 0, "clean shutdown failed")
        segments = server.segments()
        require(len(segments) >= 3, f"expected several segments, got {len(segments)}")

        victim = segments[0]
        pristine = victim.read_bytes()
        offset = len(pristine) // 2
        damaged = bytearray(pristine)
        damaged[offset] ^= 0x08
        victim.write_bytes(bytes(damaged))
        before = fingerprint(server.data_dir)

        process = server.spawn()
        code = process.wait(timeout=60)
        require(code != 0, "the server started on a log with a flipped bit in its middle")
        log = server.log()
        require(victim.name in log, "the refusal does not name the damaged segment")
        require(fingerprint(server.data_dir) == before, "refusing to start modified the files")

        victim.write_bytes(pristine)
        server.start()
        require_all_present(server, acknowledged)
        return (f"bit {offset * 8 + 3} of {victim.name} flipped: refused to start (exit {code}), "
                f"named the segment, touched nothing; restored: all {len(acknowledged)} jobs back")
    finally:
        server.stop()


# --- 3. a torn snapshot -------------------------------------------------------------------


def torn_snapshot(out: Path) -> str:
    server = Server(out / "data", "--snapshot-every", "0", "--segment-size", "64k")
    try:
        server.start()
        acknowledged: Dict[int, str] = {}
        with server.client() as client:
            for round_number in range(3):
                acknowledged.update(enqueue_many(client, 250, f"snap-{round_number}"))
                client.snapshot()
                deadline = time.monotonic() + 60
                while client.info("persistence")["snapshots_taken"] <= round_number:
                    require(time.monotonic() < deadline, "the snapshot never finished")
                    time.sleep(0.02)
            acknowledged.update(enqueue_many(client, 50, "tail"))
        server.kill()

        snapshots = server.snapshots()
        require(len(snapshots) == 2, f"expected the newest two snapshots, found {len(snapshots)}")
        newest = snapshots[-1]
        newest.write_bytes(newest.read_bytes()[: newest.stat().st_size * 2 // 3])
        stray = server.data_dir / "snapshot-00000000000000999999.tmp"
        stray.write_bytes(b"a snapshot that was being written when the power failed")

        started = server.start()
        require(started["snapshots_rejected"] == 1, "the torn snapshot was not rejected")
        older_lsn = int(snapshots[0].name.split("-")[1].split(".")[0])
        require(started["from_snapshot_lsn"] == older_lsn,
                f"recovered from LSN {started['from_snapshot_lsn']}, expected the older "
                f"snapshot at {older_lsn}")
        require(not stray.exists(), "the stray .tmp file was not removed")
        require_all_present(server, acknowledged)
        return (f"newest snapshot torn at 2/3: rejected, recovered from the older one (LSN "
                f"{older_lsn}) plus {started['recovered_records']} log records; all "
                f"{len(acknowledged)} jobs present; stray .tmp removed")
    finally:
        server.stop()


# --- 4. a full disk -----------------------------------------------------------------------


def full_disk(out: Path) -> str:
    first = _write_fails(out / "rlimit")
    small_fs = os.environ.get("BATON_CHAOS_SMALL_FS")
    if not small_fs:
        return first + "; low-space guard: SKIPPED (no BATON_CHAOS_SMALL_FS)"
    try:
        return first + "; " + _disk_fills_up(Path(small_fs))
    except Exception as error:
        raise Failed(f"{first}; but the low-space guard scenario failed: "
                     f"{type(error).__name__}: {error}") from error


def _write_fails(out: Path) -> str:
    """The log cannot be written (RLIMIT_FSIZE makes write() fail with EFBIG, as a full
    disk makes it fail with ENOSPC). The server must die rather than acknowledge."""
    out.mkdir(parents=True)
    limit = 256 * 1024
    server = Server(out / "data", "--snapshot-every", "0")

    def cap_file_size() -> None:
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))

    try:
        # restore_signals=False keeps SIGXFSZ ignored (as Python has it), so the
        # write fails with an error code instead of killing the process outright.
        server.start(preexec_fn=cap_file_size, restore_signals=False)
        acknowledged: Dict[int, str] = {}
        with server.client(retry_for=0, timeout=5.0) as client:
            try:
                for i in range(10_000):
                    payload = f"full-{i}-" + "x" * 2_000
                    acknowledged[client.enqueue(QUEUE, payload, key=f"full-{i}")] = payload
                raise Failed("10,000 jobs fitted into a 256 KiB log")
            except baton.BatonError:
                pass  # the connection died with the server
        code = server.wait_exit(timeout=60)
        require(code != 0, "the server exited cleanly although it could not write its log")
        require("aborting so that recovery can establish what is durable" in server.log(),
                "the server did not say why it stopped")
        written = sum(s.stat().st_size for s in server.segments())
        require(written <= limit, "the log grew past the limit")

        server.start()  # space is back
        require_all_present(server, acknowledged)
        with server.client() as client:
            client.enqueue(QUEUE, "and it accepts work again")
        return (f"write failure at {limit // 1024} KiB: aborted (exit {code}) without "
                f"acknowledging; after restart all {len(acknowledged)} acknowledged jobs present")
    finally:
        server.stop()


def _disk_fills_up(small_fs: Path) -> str:
    """On a small file system: ENQUEUE is refused before the disk is full, everything
    else keeps working, and enqueueing resumes when space returns."""
    require(small_fs.is_dir() and os.access(small_fs, os.W_OK),
            f"BATON_CHAOS_SMALL_FS={small_fs} is not a writable directory (is it mounted?)")
    root = small_fs / f"baton-chaos-{os.getpid()}"
    root.mkdir(parents=True)
    segment = 1 << 20
    server = Server(root / "data", "--snapshot-every", "0", "--segment-size", "1m")
    ballast = root / "ballast"
    try:
        server.start()
        free = os.statvfs(root).f_bavail * os.statvfs(root).f_frsize
        require(free > 6 * segment, f"{small_fs} has only {free} bytes free")
        with open(ballast, "wb") as filler:  # leave about 3.5 segments of space
            filler.write(b"\0" * (free - 3 * segment - segment // 2))
            filler.flush()
            os.fsync(filler.fileno())

        acknowledged: Dict[int, str] = {}
        refused = None
        with server.client() as client:
            first = client.enqueue(QUEUE, "to be worked on while the disk is full")
            for i in range(400):
                payload = f"small-{i}-" + "x" * 16_000
                try:
                    acknowledged[client.enqueue(QUEUE, payload, key=f"small-{i}")] = payload
                except baton.LimitExceeded as error:
                    refused = str(error)
                    break
                time.sleep(0.02)  # the guard samples free space once a second
            require(refused is not None, "ENQUEUE was never refused on a nearly full disk")
            require(client.info("persistence")["disk_low"] == 1, "INFO does not report disk_low")
            # Everything but ENQUEUE still works, so that the queue can drain.
            job = client.reserve(QUEUE)
            require(job is not None and job.id == first, "RESERVE failed on a full disk")
            client.ack(job.id, job.token)

            ballast.unlink()
            deadline = time.monotonic() + 10
            while True:
                try:
                    client.enqueue(QUEUE, "space is back")
                    break
                except baton.LimitExceeded:
                    require(time.monotonic() < deadline, "ENQUEUE stayed refused after space "
                                                         "returned")
                    time.sleep(0.1)
        require(server.process.poll() is None, "the server died")
        require_all_present(server, acknowledged)
        return (f"low-space guard on {small_fs}: ENQUEUE refused with LIMIT after "
                f"{len(acknowledged)} jobs ('{refused}'), RESERVE/ACK kept working, enqueueing "
                "resumed once space returned")
    finally:
        server.stop()
        for leftover in sorted(root.rglob("*"), reverse=True):
            leftover.unlink() if leftover.is_file() else leftover.rmdir()
        root.rmdir()


SCENARIOS: List[Callable[[Path], str]] = [torn_log_tail, flipped_bit_mid_log, torn_snapshot,
                                          full_disk]


def run(scenario: Callable[[Path], str], out: Path) -> dict:
    try:
        return {"name": scenario.__name__, "ok": True, "detail": scenario(out)}
    except Exception as error:  # a scenario that blows up has failed; the run goes on
        return {"name": scenario.__name__, "ok": False,
                "detail": f"{type(error).__name__}: {error}"}
