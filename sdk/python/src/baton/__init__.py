"""Python SDK for baton, a durable job and workflow engine.

    import baton

    client = baton.Client()
    client.enqueue_task("emails", "send_email", ["ada@example.com"], key="welcome-ada")

    worker = baton.Worker(["emails"], concurrency=8)

    @worker.task()
    def send_email(to):
        ...

    worker.run()   # until SIGTERM

Delivery is at-least-once: a handler may run more than once for the same job.
See ``baton.idempotent`` for what to do about it.
"""

from .client import Client, FailResult, Job, JobStatus, QueueStats, task_payload
from .errors import (
    AuthError,
    BatonError,
    ConnectionLost,
    EnqueueUncertain,
    InvalidRequest,
    LimitExceeded,
    NotFound,
    ProtocolError,
    ServerReplyError,
    StaleLease,
    Unavailable,
    WrongState,
)
from .worker import Fatal, JobContext, Retry, Worker, current_job

__version__ = "0.1.0"

__all__ = [
    "AuthError",
    "BatonError",
    "Client",
    "ConnectionLost",
    "EnqueueUncertain",
    "FailResult",
    "Fatal",
    "InvalidRequest",
    "Job",
    "JobContext",
    "JobStatus",
    "LimitExceeded",
    "NotFound",
    "ProtocolError",
    "QueueStats",
    "Retry",
    "ServerReplyError",
    "StaleLease",
    "Unavailable",
    "Worker",
    "WrongState",
    "current_job",
    "task_payload",
]
