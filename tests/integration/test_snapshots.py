"""Snapshots and compaction with the real binary on the real file system.

The unit tests prove the write protocol against a simulated file system; these
prove that the POSIX implementation underneath behaves the same way: rename,
directory fsync, streaming reads, unlinking segments the log thread has closed.
"""

from __future__ import annotations

import time

from helpers import client, fields


def wait_for(r, field: str, predicate, timeout: float = 30.0):
    deadline = time.monotonic() + timeout
    while True:
        value = r.info("persistence")[field]
        if predicate(value):
            return value
        assert time.monotonic() < deadline, f"{field} stayed at {value!r}"
        time.sleep(0.02)


def test_snapshot_command_then_sigkill_recovers_from_the_snapshot(server):
    r = client(server)
    ids = [r.execute_command("ENQUEUE", "q", f"job-{i}", "KEY", f"key-{i}") for i in range(300)]
    _, token, *_ = r.execute_command("RESERVE", 0, 60_000, "q")
    r.execute_command("ACK", ids[0], token)

    assert r.execute_command("SNAPSHOT") == b"OK"
    wait_for(r, "snapshots_taken", lambda n: n == 1)
    info = r.info("persistence")
    assert info["last_snapshot_lsn"] == 302
    assert info["last_snapshot_jobs"] == 300
    snapshots = sorted(p.name for p in server.data_dir.glob("snapshot-*"))
    assert snapshots == ["snapshot-00000000000000000302.snap"], "and no temp file is left"

    # Work that only the log has.
    _, held_token, *_ = r.execute_command("RESERVE", 0, 60_000, "q")
    tail = [r.execute_command("ENQUEUE", "q", f"late-{i}") for i in range(10)]

    server.kill()
    server.start()

    r = client(server)
    info = r.info("persistence")
    assert info["recovered_from_snapshot_lsn"] == 302
    assert info["recovered_records"] == 11, "only the tail is replayed"
    assert fields(r.execute_command("STATUS", ids[0]))["state"] == b"succeeded"
    assert fields(r.execute_command("STATUS", ids[1]))["state"] == b"leased"
    assert fields(r.execute_command("STATUS", tail[-1], "PAYLOAD"))["payload"] == b"late-9"
    assert r.execute_command("ENQUEUE", "q", "again", "KEY", "key-7") == ids[7]
    assert r.execute_command("ACK", ids[1], held_token) == b"OK"
    assert r.execute_command("ENQUEUE", "q", "new") == tail[-1] + 1


def test_the_log_is_compacted_while_serving_and_sigkill_loses_nothing(make_server):
    server = make_server("--segment-size", "64k", "--snapshot-every", "256k").start()
    r = client(server)
    padding = "p" * 2_000
    total = 1_500  # about 3 MB of log: a dozen snapshot cycles
    done = 0
    for i in range(total):
        r.execute_command("ENQUEUE", "q", f"{padding}{i}")
        if i % 3 == 2:
            job_id, token, *_ = r.execute_command("RESERVE", 0, 60_000, "q")
            r.execute_command("ACK", job_id, token)
            done += 1

    wait_for(r, "snapshot_in_progress", lambda n: n == 0)
    info = r.info("persistence")
    assert info["snapshots_taken"] >= 3
    assert info["snapshots_failed"] == 0
    assert info["log_segments_removed"] > 0
    assert len(list(server.data_dir.glob("snapshot-*.snap"))) <= 2
    assert not list(server.data_dir.glob("snapshot-*.tmp"))
    first_segment = server.segments()[0].name
    assert first_segment != "wal-00000000000000000001.log", "the beginning of the log is gone"
    log_on_disk = sum(p.stat().st_size for p in server.segments())
    assert log_on_disk < info["log_bytes"], "compaction freed log space"

    server.kill()
    server.start()

    r = client(server)
    info = r.info("persistence")
    assert info["recovered_from_snapshot_lsn"] > 0
    assert info["recovered_records"] < total
    jobs = r.info("jobs")
    assert jobs["jobs_succeeded"] == done
    assert jobs["jobs_ready"] == total - done
    last = fields(r.execute_command("STATUS", total, "PAYLOAD"))
    assert last["payload"] == f"{padding}{total - 1}".encode()
    assert r.execute_command("ENQUEUE", "q", "next") == total + 1


def test_unreadable_newest_snapshot_falls_back_to_the_older_one(make_server):
    server = make_server("--snapshot-every", "0").start()
    r = client(server)
    for round_number in range(3):
        for i in range(50):
            r.execute_command("ENQUEUE", "q", f"round-{round_number}-{i}")
        assert r.execute_command("SNAPSHOT") == b"OK"
        wait_for(r, "snapshots_taken", lambda n, want=round_number + 1: n == want)
    server.terminate()

    snapshots = sorted(server.data_dir.glob("snapshot-*.snap"))
    assert [p.name for p in snapshots] == [
        "snapshot-00000000000000000100.snap",
        "snapshot-00000000000000000150.snap",
    ], "the newest two are kept"
    damaged = bytearray(snapshots[-1].read_bytes())
    damaged[len(damaged) // 2] ^= 0x10  # bit rot
    snapshots[-1].write_bytes(bytes(damaged))

    server.start()
    r = client(server)
    info = r.info("persistence")
    assert info["recovery_snapshots_rejected"] == 1
    assert info["recovered_from_snapshot_lsn"] == 100
    assert info["recovered_records"] == 50
    assert r.info("jobs")["jobs_ready"] == 150
    assert "skipping unreadable snapshot" in server.log()
