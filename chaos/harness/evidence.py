"""Reading what a round left behind: journals, run logs, the rogue's log, the ledger.

The ledger is parsed here from its raw bytes, on purpose without using
baton.idempotent: the Ledger class keeps a dict by key, which would hide a
duplicate entry - the one thing invariant 3 is looking for.
"""

from __future__ import annotations

import json
import struct
import zlib
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterator, List, Set


def json_lines(path: Path) -> Iterator[dict]:
    with open(path, encoding="utf-8") as lines:
        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except ValueError:
                # The last line of a process that was SIGKILLed in the middle of a
                # write. Nothing was promised about it.
                continue


class Journals:
    """What the producers were told."""

    def __init__(self, out_dir: Path):
        self.intents: Set[str] = set()
        self.ids_by_key: Dict[str, Set[int]] = defaultdict(set)
        self.errors: List[dict] = []
        for path in sorted(out_dir.glob("journal-*.log")):
            for line in json_lines(path):
                if line["e"] == "intent":
                    self.intents.add(line["key"])
                elif line["e"] == "ok":
                    self.ids_by_key[line["key"]].add(line["id"])
                elif line["e"] == "error":
                    self.errors.append(line)

    @property
    def acknowledged(self) -> Dict[int, str]:
        """job id -> key, for every enqueue that received OK."""
        return {job_id: key for key, ids in self.ids_by_key.items() for job_id in ids}


class RunLogs:
    """What the workers and the rogue saw."""

    def __init__(self, out_dir: Path):
        self.starts: List[dict] = []
        self.effects: List[dict] = []
        self.results: List[dict] = []
        self.tokens_seen: Dict[int, Set[int]] = defaultdict(set)
        for path in sorted(out_dir.glob("runs-*.log")):
            for line in json_lines(path):
                if line["e"] == "start":
                    self.starts.append(line)
                    self.tokens_seen[line["job"]].add(line["token"])
                elif line["e"] == "effect":
                    self.effects.append(line)
                elif line["e"] == "result":
                    self.results.append(line)

        self.rogue_reserved = 0
        self.rogue_attempts: Counter = Counter()  # "ACK while held by a successor" -> n
        self.rogue_accepted: List[dict] = []
        for path in sorted(out_dir.glob("rogue-*.log")):
            for line in json_lines(path):
                if line["e"] == "reserved":
                    self.rogue_reserved += 1
                    self.tokens_seen[line["job"]].add(line["token"])
                elif line["e"] == "stale_attempt":
                    self.rogue_attempts[f"{line['command']} while {line['when']}"] += 1
                    if line["accepted"]:
                        self.rogue_accepted.append(line)


_HEADER = struct.Struct("<II")


def ledger_entries(path: Path) -> Counter:
    """key -> number of 'done' records in the ledger file. A torn final record (a
    writer killed mid-append) is ignored; damage anywhere else raises."""
    done: Counter = Counter()
    if not path.exists():
        return done
    data = path.read_bytes()
    position = 0
    while position < len(data):
        if position + _HEADER.size > len(data):
            break  # torn header at the tail
        length, checksum = _HEADER.unpack_from(data, position)
        start = position + _HEADER.size
        payload = data[start:start + length]
        if len(payload) < length:
            break  # torn payload at the tail
        if zlib.crc32(payload) != checksum:
            if start + length == len(data):
                break  # torn final record
            raise ValueError(f"{path}: checksum mismatch at byte {position}, not at the tail")
        record = json.loads(payload)
        if record["op"] == "done":
            done[record["key"]] += 1
        position = start + length
    return done
