"""Doing something once under at-least-once delivery (docs/design.md 9.4).

baton may run a handler twice: the worker dies after the side effect and before
the ACK, or loses its lease mid-run. Exactly-once *effects* cannot be added from
outside - the effect and the record of it must commit atomically. ``Ledger`` is
a small durable set that makes that possible when the effect can *be* the
record, and is honest about the window that remains when it cannot.

    ledger = Ledger("/var/lib/myapp/effects.ledger")

    # The entry is the effect: exactly once, whatever gets killed.
    ledger.put_if_absent(f"invoice-{job.id}", {"amount": 12})

    # The effect lives elsewhere: at least once, and never again once recorded.
    ledger.once(f"email-{job.id}", lambda: send_email(...), token=job.token)

The file is append-only, checksummed and fsynced per entry; processes on the
same machine share it safely through a lock file. POSIX only (``fcntl``).
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any, Callable, Dict, Iterator, List, Optional

from .errors import BatonError

_HEADER = struct.Struct("<II")  # payload length, crc32 of the payload
_MAX_RECORD = 16 * 1024 * 1024


class LedgerCorrupt(BatonError):
    """The ledger is damaged somewhere other than its tail. It is not repaired
    automatically: entries after the damage would be silently forgotten."""


class Superseded(BatonError):
    """A run with a higher fencing token has already claimed this key: the caller
    is a zombie whose lease was taken over, and must not perform the effect."""


@dataclass(frozen=True)
class Entry:
    key: str
    token: int
    value: Any


class Ledger:
    def __init__(self, path: str) -> None:
        try:
            import fcntl  # noqa: F401
        except ImportError as error:  # pragma: no cover - Windows
            raise BatonError("baton.idempotent.Ledger needs POSIX file locks (fcntl)") from error
        self.path = os.path.abspath(path)
        self._lock_path = self.path + ".lock"
        self._done: Dict[str, Entry] = {}
        self._claims: Dict[str, int] = {}
        self._offset = 0  # everything before this has been read into the maps
        self.duplicate_runs = 0  # once(): runs whose result lost the race to be recorded
        directory = os.path.dirname(self.path)
        os.makedirs(directory, exist_ok=True)
        if not os.path.exists(self.path):
            with self._locked():
                pass  # creates the file durably

    # --- public API ---------------------------------------------------------------------------

    def put_if_absent(self, key: str, value: Any = None, *, token: int = 0) -> bool:
        """Records ``key`` durably unless it is already there. True if this call added it.

        When the entry itself is the side effect, this is exactly-once: of any
        number of runs, concurrent or not, exactly one gets True."""
        with self._locked() as append:
            if key in self._done:
                return False
            append({"op": "done", "key": key, "token": token, "value": value})
            return True

    def once(self, key: str, effect: Callable[[], Any], *, token: int = 0) -> Any:
        """Runs ``effect`` unless a run for ``key`` has already been recorded, and returns
        the recorded result (JSON-serializable).

        The contract, without decoration: **at least once, and never again once
        recorded.** A crash between the effect and the record repeats the effect
        on the next delivery; two workers that both hold the job (a zombie and
        its successor) may both run it. ``token`` (the job's lease token) narrows
        the second window: a run is refused with Superseded once a higher token
        has claimed the key. If a repeat is unacceptable, the system the effect
        lives in needs an idempotency key of its own - give it the job id.
        """
        with self._locked() as append:
            if key in self._done:
                return self._done[key].value
            claimed = self._claims.get(key, 0)
            if token < claimed:
                raise Superseded(f"{key}: token {token} is older than {claimed}")
            if token > claimed:
                append({"op": "claim", "key": key, "token": token})

        value = effect()  # not under the lock: it may take long

        with self._locked() as append:
            if key in self._done:
                self.duplicate_runs += 1
                return self._done[key].value
            append({"op": "done", "key": key, "token": token, "value": value})
            return value

    def get(self, key: str) -> Optional[Entry]:
        with self._locked():
            return self._done.get(key)

    def entries(self) -> List[Entry]:
        with self._locked():
            return list(self._done.values())

    def __contains__(self, key: str) -> bool:
        return self.get(key) is not None

    def __len__(self) -> int:
        with self._locked():
            return len(self._done)

    # --- the file -----------------------------------------------------------------------------

    @contextmanager
    def _locked(self) -> Iterator[Callable[[Dict[str, Any]], None]]:
        """Holds the inter-process lock, brings the maps up to date with what other
        processes appended, and yields an ``append(record)`` that is durable on return."""
        import fcntl

        with open(self._lock_path, "a+b") as lock_file:
            fcntl.flock(lock_file, fcntl.LOCK_EX)
            try:
                created = not os.path.exists(self.path)
                with open(self.path, "a+b") as ledger:
                    if created:
                        os.fsync(ledger.fileno())
                        _fsync_directory(os.path.dirname(self.path))
                    self._catch_up(ledger)

                    def append(record: Dict[str, Any]) -> None:
                        payload = json.dumps(record, separators=(",", ":")).encode("utf-8")
                        ledger.seek(0, os.SEEK_END)
                        ledger.write(_HEADER.pack(len(payload), zlib.crc32(payload)) + payload)
                        ledger.flush()
                        os.fsync(ledger.fileno())
                        self._offset = ledger.tell()
                        self._apply(record)

                    yield append
            finally:
                fcntl.flock(lock_file, fcntl.LOCK_UN)

    def _apply(self, record: Dict[str, Any]) -> None:
        key, token = record["key"], int(record.get("token", 0))
        if record["op"] == "done":
            self._done.setdefault(key, Entry(key, token, record.get("value")))
        self._claims[key] = max(self._claims.get(key, 0), token)

    def _catch_up(self, ledger: Any) -> None:
        ledger.seek(0, os.SEEK_END)
        size = ledger.tell()
        if size < self._offset:
            raise LedgerCorrupt(f"{self.path} shrank from {self._offset} to {size} bytes")
        ledger.seek(self._offset)
        data = ledger.read(size - self._offset)
        position = 0
        while position < len(data):
            record = _parse(data, position)
            if record is None:
                break
            payload, position = record
            self._apply(payload)
        if position < len(data):
            # We hold the lock, so nobody is in the middle of an append: this is
            # what a writer that crashed left behind - if it is at the very end.
            if any(_parse(data, later) for later in range(position + 1, len(data))):
                raise LedgerCorrupt(
                    f"{self.path}: damaged at byte {self._offset + position}, with valid "
                    "entries after it"
                )
            ledger.truncate(self._offset + position)
            ledger.flush()
            os.fsync(ledger.fileno())
        self._offset += position


def _parse(data: bytes, position: int):
    """The record starting at ``position`` as (payload, end), or None if there is
    no intact record there."""
    if position + _HEADER.size > len(data):
        return None
    length, checksum = _HEADER.unpack_from(data, position)
    start = position + _HEADER.size
    if length > _MAX_RECORD or start + length > len(data):
        return None
    payload = data[start : start + length]
    if zlib.crc32(payload) != checksum:
        return None
    try:
        record = json.loads(payload)
    except ValueError:
        return None
    if not isinstance(record, dict) or "key" not in record or "op" not in record:
        return None
    return record, start + length


def _fsync_directory(directory: str) -> None:
    fd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
