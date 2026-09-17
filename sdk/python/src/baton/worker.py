"""The worker: reserve, run, heartbeat, ack - and stop gracefully.

docs/design.md 9.3 explains the choices. In short: ``concurrency`` threads with
one connection each; one heartbeat thread that keeps the leases of running jobs
alive and notices when one is lost; SIGTERM stops reserving and lets running
handlers finish.
"""

from __future__ import annotations

import json
import logging
import signal
import threading
import time
from typing import Any, Callable, Dict, Iterable, List, Optional

from .client import Client, Job
from .errors import BatonError, ConnectionLost, NotFound, StaleLease

_current = threading.local()


class Retry(Exception):
    """Raise from a handler to fail the attempt and choose when the next one runs."""

    def __init__(self, message: str = "", *, in_ms: int):
        super().__init__(message)
        self.in_ms = in_ms


class Fatal(Exception):
    """Raise from a handler to dead-letter the job now, whatever attempts remain."""


class JobContext:
    """What a running handler can know about its job: ``baton.current_job()``."""

    def __init__(self, job: Job, lease_ms: int):
        self.job = job
        self.id = job.id
        self.token = job.token  # pass it downstream as a fencing token
        self.queue = job.queue
        self.payload = job.payload
        self.attempt = job.attempt
        self.max_attempts = job.max_attempts
        self._lost = threading.Event()
        # Monotonic time until which the lease is known to be ours. Heartbeats
        # push it forward; if they stop succeeding, it runs out.
        self.confirmed_until = time.monotonic() + lease_ms / 1000.0

    @property
    def lease_lost(self) -> bool:
        """True once the worker no longer holds the lease: it expired, the job was
        cancelled, or the server could not be reached for a whole lease period.
        The result of this run will be discarded; stop when convenient."""
        return self._lost.is_set() or time.monotonic() > self.confirmed_until

    def wait_lease_lost(self, timeout: float) -> bool:
        """Sleeps up to ``timeout`` seconds; returns early (True) if the lease is lost."""
        deadline = time.monotonic() + timeout
        while not self.lease_lost:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self._lost.wait(min(remaining, 0.2))
        return True


def current_job() -> JobContext:
    """The job the calling handler thread is running."""
    context = getattr(_current, "context", None)
    if context is None:
        raise RuntimeError("current_job() was called outside a job handler")
    return context


class Worker:
    def __init__(
        self,
        queues: Iterable[str],
        *,
        host: str = "127.0.0.1",
        port: int = 7379,
        password: Optional[str] = None,
        concurrency: int = 4,
        lease_ms: int = 30_000,
        reserve_timeout_ms: int = 2_000,
        shutdown_timeout: float = 30.0,
        name: str = "baton-worker",
        logger: Optional[logging.Logger] = None,
        on_result: Optional[Callable[["JobContext", str], None]] = None,
    ) -> None:
        """``on_result(job, outcome)`` is called once per delivery, on the worker thread,
        after the server has been told (or could not be): ``"acked"``, ``"failed"``,
        ``"lease_lost"`` (the result was discarded) or ``"unreported"`` (the server
        could not be reached; the lease will expire). For metrics and audit logs."""
        self.queues = [queues] if isinstance(queues, str) else list(queues)
        if not self.queues:
            raise ValueError("a worker needs at least one queue")
        if concurrency < 1:
            raise ValueError("concurrency must be at least 1")
        if lease_ms < 300:
            raise ValueError("lease_ms must be at least 300: heartbeats run every lease_ms / 3")
        self._connect = dict(host=host, port=port, password=password)
        self.concurrency = concurrency
        self.lease_ms = lease_ms
        self.reserve_timeout_ms = reserve_timeout_ms
        self.shutdown_timeout = shutdown_timeout
        self.name = name
        self.log = logger or logging.getLogger("baton.worker")
        self._on_result = on_result

        self._tasks: Dict[str, Callable[..., Any]] = {}
        self._raw_handlers: Dict[str, Callable[[JobContext], Any]] = {}
        self._stopping = threading.Event()  # no more reserving
        self._abandon = threading.Event()  # do not even wait for running handlers
        self._heartbeats_done = threading.Event()
        self._lock = threading.Lock()
        self._running: Dict[int, JobContext] = {}  # guarded by _lock
        self.stats = {"succeeded": 0, "failed": 0, "lease_lost": 0}  # guarded by _lock

    # --- registration ---------------------------------------------------------------------

    def task(self, name: Optional[str] = None) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """Registers a function as the handler of task ``name`` (default: its own name).

        Jobs for it are enqueued with ``Client.enqueue_task(queue, name, args, kwargs)``.
        """

        def register(function: Callable[..., Any]) -> Callable[..., Any]:
            task_name = name or function.__name__
            if task_name in self._tasks:
                raise ValueError(f"task {task_name!r} is already registered")
            self._tasks[task_name] = function
            return function

        return register

    def raw(self, queue: str) -> Callable[[Callable[[JobContext], Any]], Callable[[JobContext], Any]]:
        """Registers ``handler(job)`` for every job of ``queue``, whatever its payload."""

        def register(function: Callable[[JobContext], Any]) -> Callable[[JobContext], Any]:
            self._raw_handlers[queue] = function
            return function

        return register

    # --- running ----------------------------------------------------------------------------

    def stop(self) -> None:
        """Graceful shutdown: stop reserving, let running handlers finish. Thread-safe."""
        self._stopping.set()

    def run(self, *, install_signal_handlers: bool = True) -> int:
        """Works until stop() or SIGTERM/SIGINT. Returns the number of jobs that were
        still running when ``shutdown_timeout`` ran out (their leases will expire and
        they will be delivered again)."""
        if install_signal_handlers:
            self._install_signal_handlers()
        self.log.info("worker %s: queues=%s concurrency=%d lease_ms=%d", self.name,
                      ",".join(self.queues), self.concurrency, self.lease_ms)

        threads = [
            threading.Thread(target=self._work, name=f"{self.name}-{i}", daemon=True)
            for i in range(self.concurrency)
        ]
        heartbeat = threading.Thread(target=self._heartbeat, name=f"{self.name}-hb", daemon=True)
        for thread in threads:
            thread.start()
        heartbeat.start()

        while not self._stopping.wait(0.2):
            pass
        self.log.info("worker %s: shutting down, waiting up to %.0fs for %d running job(s)",
                      self.name, self.shutdown_timeout, len(self._snapshot()))
        deadline = time.monotonic() + self.shutdown_timeout
        for thread in threads:
            while thread.is_alive() and time.monotonic() < deadline and not self._abandon.is_set():
                thread.join(0.1)

        abandoned = len(self._snapshot())
        self._heartbeats_done.set()
        heartbeat.join(2.0)
        if abandoned:
            self.log.warning("worker %s: abandoning %d running job(s); they will be redelivered "
                             "when their leases expire", self.name, abandoned)
        self.log.info("worker %s: stopped %s", self.name, self.stats)
        return abandoned

    def _install_signal_handlers(self) -> None:
        if threading.current_thread() is not threading.main_thread():
            self.log.warning("not the main thread: no signal handlers; call stop() to shut down")
            return

        def handle(signum: int, _frame: Any) -> None:
            if self._stopping.is_set():
                self._abandon.set()  # a second signal: do not wait for handlers
            self._stopping.set()

        signal.signal(signal.SIGTERM, handle)
        signal.signal(signal.SIGINT, handle)

    def _snapshot(self) -> List[JobContext]:
        with self._lock:
            return list(self._running.values())

    def _count(self, outcome: str) -> None:
        with self._lock:
            self.stats[outcome] += 1

    # --- worker threads ---------------------------------------------------------------------

    def _work(self) -> None:
        client = Client(name=threading.current_thread().name, **self._connect)
        try:
            while not self._stopping.is_set():
                try:
                    job = client.reserve(
                        self.queues, timeout_ms=self.reserve_timeout_ms, lease_ms=self.lease_ms
                    )
                except BatonError as error:
                    self.log.warning("reserve failed: %s", error)
                    self._stopping.wait(1.0)
                    continue
                if job is None:
                    continue
                # Also when shutdown began while RESERVE was in flight: dropping
                # the job now would cost one of its attempts.
                try:
                    self._run_job(client, job)
                except Exception:  # a bug in the SDK must not silently cost a thread
                    self.log.exception("job %d: internal error; its lease will expire", job.id)
        finally:
            client.close()

    def _run_job(self, client: Client, job: Job) -> None:
        context = JobContext(job, self.lease_ms)
        with self._lock:
            self._running[job.id] = context
        _current.context = context
        try:
            self._dispatch(context)
        except Retry as retry:
            self._report_failure(client, context, str(retry) or "retry requested",
                                 retry_in_ms=retry.in_ms)
        except Fatal as fatal:
            self._report_failure(client, context, f"Fatal: {fatal}", no_retry=True)
        except Exception as error:  # a handler may raise anything; the attempt failed
            self.log.exception("job %d (%s) failed on attempt %d/%d", job.id, job.queue,
                               job.attempt, job.max_attempts)
            self._report_failure(client, context, f"{type(error).__name__}: {error}")
        else:
            self._report_success(client, context)
        finally:
            _current.context = None
            with self._lock:
                self._running.pop(job.id, None)

    def _dispatch(self, context: JobContext) -> None:
        raw_handler = self._raw_handlers.get(context.queue)
        if raw_handler is not None:
            raw_handler(context)
            return
        try:
            envelope = json.loads(context.payload)
            task_name = envelope["task"]
            args = envelope.get("args", [])
            kwargs = envelope.get("kwargs", {})
        except (ValueError, KeyError, TypeError) as error:
            raise Fatal(f"payload is not a task envelope: {error}") from None
        function = self._tasks.get(task_name)
        if function is None:
            # Retried, not dead-lettered: during a rolling deploy the next attempt
            # may reach a worker that knows this task.
            raise LookupError(f"no task named {task_name!r} is registered on this worker")
        function(*args, **kwargs)

    def _report_success(self, client: Client, context: JobContext) -> None:
        if context.lease_lost:
            self._discard(context, "finished")
            return
        try:
            client.ack(context.id, context.token)
            self._count("succeeded")
            self._notify(context, "acked")
        except (StaleLease, NotFound):
            self._discard(context, "finished")
        except BatonError as error:
            self.log.error("job %d: could not ack (%s); it will be delivered again", context.id,
                           error)
            self._notify(context, "unreported")

    def _report_failure(self, client: Client, context: JobContext, message: str,
                        **options: Any) -> None:
        if context.lease_lost:
            self._discard(context, "failed")
            return
        try:
            result = client.fail(context.id, context.token, message, **options)
            self._count("failed")
            self.log.info("job %d: attempt %d failed (%s): %s", context.id, context.attempt,
                          message, result.outcome)
            self._notify(context, "failed")
        except (StaleLease, NotFound):
            self._discard(context, "failed")
        except BatonError as error:
            self.log.error("job %d: could not report the failure (%s); the lease will expire",
                           context.id, error)
            self._notify(context, "unreported")

    def _discard(self, context: JobContext, what: str) -> None:
        self._count("lease_lost")
        self.log.warning("job %d %s after its lease was lost; the result is discarded (the job "
                         "was cancelled or belongs to another worker now)", context.id, what)
        self._notify(context, "lease_lost")

    def _notify(self, context: JobContext, outcome: str) -> None:
        if self._on_result is None:
            return
        try:
            self._on_result(context, outcome)
        except Exception:  # an observer must not be able to break the worker
            self.log.exception("on_result raised for job %d", context.id)

    # --- heartbeats --------------------------------------------------------------------------

    def _heartbeat(self) -> None:
        # No retrying inside the client: a heartbeat that cannot get through must
        # not delay the heartbeats of the other jobs.
        client = Client(name=f"{self.name}-hb", retry_for=0.0,
                        timeout=max(self.lease_ms / 3000.0, 0.5), **self._connect)
        interval = self.lease_ms / 3000.0
        try:
            while not self._heartbeats_done.wait(interval):
                for context in self._snapshot():
                    if context.lease_lost:
                        continue
                    sent_at = time.monotonic()
                    try:
                        client.heartbeat(context.id, context.token, self.lease_ms)
                        context.confirmed_until = sent_at + self.lease_ms / 1000.0
                    except (StaleLease, NotFound):
                        context._lost.set()
                    except (ConnectionLost, BatonError) as error:
                        self.log.warning("heartbeat for job %d failed: %s", context.id, error)
        finally:
            client.close()
