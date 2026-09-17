"""baton.idempotent.Ledger: once means once, across processes, kills and torn writes."""

from __future__ import annotations

import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

from baton.idempotent import Entry, Ledger, LedgerCorrupt, Superseded

SCRIPT = Path(__file__).with_name("ledger_script.py")


@pytest.fixture
def path(tmp_path):
    return str(tmp_path / "state" / "effects.ledger")


def test_put_if_absent_is_durable_and_happens_once(path):
    ledger = Ledger(path)
    assert ledger.put_if_absent("invoice-1", {"amount": 12}, token=5)
    assert not ledger.put_if_absent("invoice-1", {"amount": 99}, token=6)
    assert "invoice-1" in ledger and "invoice-2" not in ledger

    reopened = Ledger(path)
    assert reopened.get("invoice-1") == Entry("invoice-1", 5, {"amount": 12})
    assert not reopened.put_if_absent("invoice-1")
    assert len(reopened) == 1


def test_once_runs_the_effect_once_and_remembers_its_result(path):
    ledger = Ledger(path)
    calls = []

    def effect():
        calls.append(1)
        return {"message_id": "abc"}

    assert ledger.once("email-7", effect) == {"message_id": "abc"}
    assert ledger.once("email-7", effect) == {"message_id": "abc"}
    assert Ledger(path).once("email-7", effect) == {"message_id": "abc"}, "also after a restart"
    assert calls == [1]


def test_a_failed_effect_is_not_recorded(path):
    ledger = Ledger(path)

    def explode():
        raise RuntimeError("smtp is down")

    with pytest.raises(RuntimeError):
        ledger.once("email-8", explode)
    assert "email-8" not in ledger
    assert ledger.once("email-8", lambda: "sent") == "sent", "the next delivery tries again"


def test_a_zombie_with_an_older_token_is_refused(path):
    successor, zombie = Ledger(path), Ledger(path)
    zombie_ran = []

    def effect_of_the_successor():
        # While the new lease holder (token 9) is at work, the worker it took
        # over from (token 5) wakes up and tries to do the same thing.
        with pytest.raises(Superseded):
            zombie.once("charge-3", lambda: zombie_ran.append(1), token=5)
        return "charged"

    assert successor.once("charge-3", effect_of_the_successor, token=9) == "charged"
    assert zombie_ran == []
    assert zombie.once("charge-3", lambda: zombie_ran.append(1), token=5) == "charged"
    assert zombie_ran == [], "once recorded, nobody runs it again"


def test_two_live_runs_both_happen_and_the_first_result_wins(path):
    # The window the docstring admits to: both hold a claim, both run.
    first, second = Ledger(path), Ledger(path)

    def effect_of_the_second():
        assert first.once("report-1", lambda: "from first", token=7) == "from first"
        return "from second"

    assert second.once("report-1", effect_of_the_second, token=7) == "from first"
    assert second.duplicate_runs == 1


def test_processes_racing_for_the_same_keys_each_win_a_key_exactly_once(path):
    racers = [subprocess.Popen([sys.executable, str(SCRIPT), path, "150", f"racer-{i}"],
                               stdout=subprocess.PIPE, text=True) for i in range(4)]
    wins = []
    for racer in racers:
        out, _ = racer.communicate(timeout=120)
        assert racer.returncode == 0
        wins += [line.split()[1] for line in out.splitlines()]
    assert sorted(wins) == sorted(f"key-{i}" for i in range(150)), "every key won exactly once"
    assert len(Ledger(path)) == 150


def test_a_killed_writer_loses_nothing_it_reported(path):
    for round_number in range(5):
        writer = subprocess.Popen([sys.executable, str(SCRIPT), path, "100000", "doomed"],
                                  stdout=subprocess.PIPE, text=True)
        time.sleep(0.15 + 0.05 * round_number)
        writer.send_signal(signal.SIGKILL)
        out, _ = writer.communicate()
        reported = [line.split()[1] for line in out.splitlines() if line.startswith("won key-")]
        ledger = Ledger(path)  # repairs a torn tail, if the kill left one
        assert all(key in ledger for key in reported)
        assert ledger.put_if_absent(f"after-kill-{round_number}"), "and it accepts new entries"


def test_a_torn_tail_is_repaired(path):
    ledger = Ledger(path)
    ledger.put_if_absent("a", 1)
    ledger.put_if_absent("b", 2)
    intact = Path(path).read_bytes()
    for torn in (intact[: len(intact) - 3], intact + b"\x40\x00\x00\x00garb"):
        Path(path).write_bytes(torn)
        reopened = Ledger(path)
        assert "a" in reopened
        assert reopened.put_if_absent("c"), "appends go after the repaired tail"
        assert [entry.key for entry in Ledger(path).entries()][0] == "a"
        assert "c" in Ledger(path)


def test_damage_in_the_middle_is_not_repaired_silently(path):
    ledger = Ledger(path)
    ledger.put_if_absent("a", 1)
    ledger.put_if_absent("b", 2)
    damaged = bytearray(Path(path).read_bytes())
    damaged[12] ^= 0x01  # inside the first entry
    Path(path).write_bytes(bytes(damaged))
    with pytest.raises(LedgerCorrupt):
        len(Ledger(path))
