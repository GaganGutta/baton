"""Exceptions. Server errors are mapped by their code, never by their message."""

from __future__ import annotations


class BatonError(Exception):
    """Base class of everything this package raises on purpose."""


class ProtocolError(BatonError):
    """The byte stream is not valid RESP, or a reply has an unexpected shape."""


class ConnectionLost(BatonError):
    """The connection failed.

    ``maybe_executed`` says whether the server may have executed the request:
    False if the failure happened before any byte of it was sent.
    """

    def __init__(self, message: str, *, maybe_executed: bool):
        super().__init__(message)
        self.maybe_executed = maybe_executed


class EnqueueUncertain(BatonError):
    """The connection died after ENQUEUE was sent and before its reply arrived.

    The job may or may not exist, and baton will not guess: retrying could
    enqueue it twice, not retrying could lose it. Enqueue with ``key=`` to make
    retries safe - the SDK then retries by itself and this is never raised.
    """


class ServerReplyError(BatonError):
    """An error reply from the server."""

    code = "ERR"

    def __init__(self, message: str, code: str = ""):
        super().__init__(message)
        if code:
            self.code = code


class InvalidRequest(ServerReplyError):
    """ERR: the request is malformed (bad option, bad number, bad queue name)."""

    code = "ERR"


class AuthError(ServerReplyError):
    """NOAUTH or WRONGPASS."""

    code = "NOAUTH"


class NotFound(ServerReplyError):
    """NOTFOUND: the job does not exist, or has been collected."""

    code = "NOTFOUND"


class StaleLease(ServerReplyError):
    """STALE: the token is not the job's current lease. Stop working on the job."""

    code = "STALE"


class WrongState(ServerReplyError):
    """STATE: the job is in the wrong state for this command."""

    code = "STATE"


class LimitExceeded(ServerReplyError):
    """LIMIT: a configured limit was hit; nothing was changed."""

    code = "LIMIT"


class Unavailable(ServerReplyError):
    """UNAVAILABLE: the server is shutting down; nothing was changed."""

    code = "UNAVAILABLE"


_BY_CODE = {
    "ERR": InvalidRequest,
    "NOAUTH": AuthError,
    "WRONGPASS": AuthError,
    "NOTFOUND": NotFound,
    "STALE": StaleLease,
    "STATE": WrongState,
    "LIMIT": LimitExceeded,
    "UNAVAILABLE": Unavailable,
}


def error_from_reply(code: str, message: str) -> ServerReplyError:
    """The exception for an error reply. Unknown codes stay ServerReplyError."""
    return _BY_CODE.get(code, ServerReplyError)(f"{code} {message}".strip(), code)
