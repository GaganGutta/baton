"""The baton client: one method per protocol command (docs/protocol.md).

Retry policy (docs/design.md 9.2): a request is repeated after a connection
failure only when a duplicate is impossible. Failures before anything was sent
are always retried, for up to ``retry_for`` seconds.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

from .errors import ConnectionLost, EnqueueUncertain, ProtocolError, error_from_reply
from .resp import Connection, ServerError

Payload = Union[bytes, str]


@dataclass(frozen=True)
class Job:
    """A leased job, as returned by RESERVE."""

    id: int
    token: int  # the lease (fencing) token: needed to heartbeat, ack or fail
    queue: str
    payload: bytes
    attempt: int  # 1 for the first delivery
    max_attempts: int
    lease_expires_at: int  # unix ms, by the server's clock


@dataclass(frozen=True)
class JobStatus:
    id: int
    queue: str
    state: str  # scheduled, ready, leased, succeeded, dead, cancelled
    priority: int
    attempts: int
    max_attempts: int
    run_at: int
    created_at: int
    finished_at: int
    lease_expires_at: int
    last_error: str
    key: str
    payload_size: int
    payload: Optional[bytes] = None
    raw: Dict[str, Any] = field(default_factory=dict, compare=False, repr=False)


@dataclass(frozen=True)
class FailResult:
    outcome: str  # "retry" or "dead"
    retry_at: int  # unix ms; 0 when dead


@dataclass(frozen=True)
class QueueStats:
    queue: str
    scheduled: int = 0
    ready: int = 0
    leased: int = 0
    succeeded: int = 0
    dead: int = 0
    cancelled: int = 0
    total_enqueued: int = 0
    total_succeeded: int = 0
    total_failed_attempts: int = 0
    total_dead: int = 0
    total_cancelled: int = 0


def _text(value: Any) -> str:
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else str(value)


def _pairs(reply: Any) -> Dict[str, Any]:
    if not isinstance(reply, list) or len(reply) % 2 != 0:
        raise ProtocolError(f"expected field-value pairs, got {reply!r}")
    return {_text(reply[i]): reply[i + 1] for i in range(0, len(reply), 2)}


def _status(reply: Any) -> JobStatus:
    raw = _pairs(reply)
    try:
        return JobStatus(
            id=raw["id"],
            queue=_text(raw["queue"]),
            state=_text(raw["state"]),
            priority=raw["priority"],
            attempts=raw["attempts"],
            max_attempts=raw["max_attempts"],
            run_at=raw["run_at"],
            created_at=raw["created_at"],
            finished_at=raw["finished_at"],
            lease_expires_at=raw["lease_expires_at"],
            last_error=_text(raw["last_error"]),
            key=_text(raw["key"]),
            payload_size=raw["payload_size"],
            payload=raw.get("payload"),
            raw=raw,
        )
    except KeyError as missing:
        raise ProtocolError(f"STATUS reply lacks the field {missing}") from None


def _queue_stats(reply: Any) -> QueueStats:
    raw = _pairs(reply)
    known = {name: raw[name] for name in QueueStats.__dataclass_fields__ if name in raw}
    known["queue"] = _text(raw.get("queue", b""))
    return QueueStats(**known)


def task_payload(task: str, args: Sequence[Any] = (), kwargs: Optional[Mapping[str, Any]] = None):
    """The JSON envelope that ``Worker`` tasks are enqueued as."""
    return json.dumps(
        {"task": task, "args": list(args), "kwargs": dict(kwargs or {})}, separators=(",", ":")
    )


class Client:
    """A connection to a baton server. Not thread-safe: use one per thread."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 7379,
        *,
        password: Optional[str] = None,
        name: Optional[str] = None,
        timeout: float = 10.0,
        connect_timeout: float = 5.0,
        retry_for: float = 15.0,
    ) -> None:
        self._connection = Connection(
            host, port, password=password, name=name, connect_timeout=connect_timeout,
            timeout=timeout,
        )
        self._timeout = timeout
        self._retry_for = retry_for

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc_info: Any) -> None:
        self.close()

    def close(self) -> None:
        self._connection.close()

    # --- plumbing ---------------------------------------------------------------------

    def _call(
        self, *args: Union[str, bytes, int], repeatable: bool, timeout: Optional[float] = None
    ) -> Any:
        """Sends a request and returns its reply, raising server errors as exceptions.

        ``repeatable``: the request may be sent again after an attempt that the
        server may have executed. Attempts that certainly were not executed are
        always repeated, for up to ``retry_for`` seconds."""
        deadline = time.monotonic() + self._retry_for
        delay = 0.05
        while True:
            try:
                if not self._connection.connected:
                    self._connection.connect()
                reply = self._connection.call(*args, timeout=timeout)
            except ConnectionLost as error:
                if error.maybe_executed and not repeatable:
                    raise
                if time.monotonic() + delay > deadline:
                    raise
            else:
                if not isinstance(reply, ServerError):
                    return reply
                # A server that is shutting down has executed nothing: come back later.
                if reply.code != "UNAVAILABLE" or time.monotonic() + delay > deadline:
                    raise error_from_reply(reply.code, reply.message)
                self._connection.close()
            time.sleep(delay)
            delay = min(delay * 2, 1.0)

    # --- producing --------------------------------------------------------------------

    def enqueue(
        self,
        queue: str,
        payload: Payload,
        *,
        priority: int = 0,
        delay_ms: Optional[int] = None,
        at_ms: Optional[int] = None,
        max_attempts: Optional[int] = None,
        backoff_ms: Optional[Tuple[int, int]] = None,
        key: Optional[str] = None,
    ) -> int:
        """Adds a job and returns its id. When this returns, the job is on disk.

        With ``key`` (an idempotency key) the call is safe to repeat, and the SDK
        repeats it by itself after a connection failure. Without one it raises
        EnqueueUncertain instead of risking a duplicate.
        """
        args: List[Union[str, bytes, int]] = ["ENQUEUE", queue, payload]
        if priority:
            args += ["PRIORITY", priority]
        if delay_ms is not None:
            args += ["DELAY", delay_ms]
        if at_ms is not None:
            args += ["AT", at_ms]
        if max_attempts is not None:
            args += ["MAXATTEMPTS", max_attempts]
        if backoff_ms is not None:
            args += ["BACKOFF", backoff_ms[0], backoff_ms[1]]
        if key is not None:
            args += ["KEY", key]
        try:
            reply = self._call(*args, repeatable=key is not None)
        except ConnectionLost as error:
            if error.maybe_executed and key is None:
                raise EnqueueUncertain(
                    f"the connection failed after ENQUEUE was sent ({error}); the job may or may "
                    "not exist. Pass key= to make enqueueing safe to retry."
                ) from error
            raise
        return int(reply)

    def enqueue_task(
        self,
        queue: str,
        task: str,
        args: Sequence[Any] = (),
        kwargs: Optional[Mapping[str, Any]] = None,
        **options: Any,
    ) -> int:
        """Enqueues a call of a ``@worker.task``. ``options`` are those of enqueue()."""
        return self.enqueue(queue, task_payload(task, args, kwargs), **options)

    # --- consuming --------------------------------------------------------------------

    def reserve(
        self, queues: Union[str, Iterable[str]], *, timeout_ms: int = 0, lease_ms: int = 30_000
    ) -> Optional[Job]:
        """Leases the next job, waiting up to ``timeout_ms`` for one. None on timeout."""
        names = [queues] if isinstance(queues, str) else list(queues)
        reply = self._call(
            "RESERVE", timeout_ms, lease_ms, *names,
            repeatable=True,  # a lease lost with its reply simply expires (at-least-once)
            timeout=self._timeout + timeout_ms / 1000.0,
        )
        if reply is None:
            return None
        if not isinstance(reply, list) or len(reply) < 7:
            raise ProtocolError(f"unexpected RESERVE reply: {reply!r}")
        return Job(
            id=reply[0], token=reply[1], queue=_text(reply[2]), payload=reply[3],
            attempt=reply[4], max_attempts=reply[5], lease_expires_at=reply[6],
        )

    def heartbeat(self, job_id: int, token: int, lease_ms: Optional[int] = None) -> int:
        """Extends the lease; returns the new expiry (unix ms). StaleLease: stop working."""
        args: List[Union[str, int]] = ["HEARTBEAT", job_id, token]
        if lease_ms is not None:
            args.append(lease_ms)
        reply = self._call(*args, repeatable=True)
        return int(reply)

    def ack(self, job_id: int, token: int) -> None:
        """Marks the job succeeded. Once this returns it is never delivered again."""
        # Safe to repeat after a connection failure: the server answers OK again to
        # the one token whose ACK completed the job, and STALE to any other.
        self._call("ACK", job_id, token, repeatable=True)

    def fail(
        self,
        job_id: int,
        token: int,
        error: str = "",
        *,
        retry_in_ms: Optional[int] = None,
        no_retry: bool = False,
    ) -> FailResult:
        """Reports a failed attempt: retried after a backoff, or dead-lettered."""
        if retry_in_ms is not None and no_retry:
            raise ValueError("retry_in_ms and no_retry exclude each other")
        args: List[Union[str, int]] = ["FAIL", job_id, token, error]
        if retry_in_ms is not None:
            args += ["RETRYIN", retry_in_ms]
        if no_retry:
            args.append("NORETRY")
        reply = self._call(*args, repeatable=True)
        return FailResult(outcome=_text(reply[0]), retry_at=int(reply[1]))

    # --- inspecting and operating -----------------------------------------------------

    def cancel(self, job_id: int) -> None:
        self._call("CANCEL", job_id, repeatable=True)

    def status(self, job_id: int, *, payload: bool = False) -> JobStatus:
        args: List[Union[str, int]] = ["STATUS", job_id]
        if payload:
            args.append("PAYLOAD")
        reply = self._call(*args, repeatable=True)
        return _status(reply)

    def stats(self, queue: str) -> QueueStats:
        reply = self._call("STATS", queue, repeatable=True)
        return _queue_stats(reply)

    def all_stats(self) -> List[QueueStats]:
        reply = self._call("STATS", repeatable=True)
        return [_queue_stats(entry) for entry in reply]

    def dlq_list(self, queue: str, offset: int = 0, count: int = 100) -> List[JobStatus]:
        reply = self._call("DLQ.LIST", queue, offset, count, repeatable=True)
        return [_status(entry) for entry in reply]

    def dlq_retry(self, job_id: int) -> None:
        self._call("DLQ.RETRY", job_id, repeatable=True)

    def dlq_retry_all(self, queue: str) -> int:
        reply = self._call("DLQ.RETRY", queue, "ALL", repeatable=True)
        return int(reply)

    def dlq_purge(self, job_id: int) -> None:
        self._call("DLQ.PURGE", job_id, repeatable=True)

    def dlq_purge_all(self, queue: str) -> int:
        reply = self._call("DLQ.PURGE", queue, "ALL", repeatable=True)
        return int(reply)

    def snapshot(self) -> None:
        self._call("SNAPSHOT", repeatable=True)

    def ping(self) -> bool:
        reply = self._call("PING", repeatable=True)
        return reply == "PONG"

    def info(self, section: Optional[str] = None) -> Dict[str, Union[int, str]]:
        args = ["INFO"] + ([section] if section else [])
        reply = self._call(*args, repeatable=True)
        result: Dict[str, Union[int, str]] = {}
        for line in _text(reply).splitlines():
            if not line or line.startswith("#"):
                continue
            name, _, value = line.partition(":")
            result[name] = int(value) if value.lstrip("-").isdigit() else value
        return result
