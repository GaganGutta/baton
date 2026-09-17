"""RESP2 for baton: an encoder, an incremental reply parser, one blocking socket.

This is deliberately not a general Redis client. It does what the SDK needs and
nothing behind the caller's back: no connection pool, no transparent reconnect,
no resending. Whether a request may be repeated after a failure depends on the
command, so that decision lives one layer up (``baton.client``), and this layer
only reports faithfully what happened: see :class:`ConnectionLost`.
"""

from __future__ import annotations

import socket
import time
from typing import Any, List, Optional, Union

from .errors import ConnectionLost, ProtocolError, error_from_reply

Reply = Union[None, int, str, bytes, "ServerError", List[Any]]

_CRLF = b"\r\n"
_INCOMPLETE = object()


class ServerError:
    """An error reply, ``-CODE message``. Returned by the parser, not raised."""

    __slots__ = ("code", "message")

    def __init__(self, line: str):
        code, _, message = line.partition(" ")
        self.code = code
        self.message = message

    def __repr__(self) -> str:
        return f"ServerError({self.code!r}, {self.message!r})"


def encode_command(*args: Union[str, bytes, int]) -> bytes:
    """Encodes one request as an array of bulk strings."""
    if not args:
        raise ValueError("a command needs at least a name")
    out = [b"*%d\r\n" % len(args)]
    for arg in args:
        if isinstance(arg, bool):
            raise TypeError("bool is ambiguous on the wire; pass 0/1 or a string")
        if isinstance(arg, int):
            data = b"%d" % arg
        elif isinstance(arg, str):
            data = arg.encode("utf-8")
        elif isinstance(arg, (bytes, bytearray, memoryview)):
            data = bytes(arg)
        else:
            raise TypeError(f"cannot send {type(arg).__name__} as a command argument")
        out.append(b"$%d\r\n" % len(data))
        out.append(data)
        out.append(_CRLF)
    return b"".join(out)


class Parser:
    """Incremental RESP2 reply parser.

    ``feed()`` bytes as they arrive, call ``next_reply()`` until it returns
    ``Parser.INCOMPLETE``. Simple strings become ``str``, bulk strings ``bytes``,
    integers ``int``, arrays ``list``, nulls ``None``, errors ``ServerError``.
    """

    INCOMPLETE = _INCOMPLETE
    MAX_DEPTH = 8

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> None:
        self._buffer += data

    def next_reply(self) -> Any:
        parsed = self._parse(0, 0)
        if parsed is _INCOMPLETE:
            return _INCOMPLETE
        value, end = parsed
        del self._buffer[:end]
        return value

    def _line(self, pos: int):
        end = self._buffer.find(_CRLF, pos)
        if end < 0:
            if len(self._buffer) - pos > 64 * 1024:
                raise ProtocolError("reply header line is implausibly long")
            return _INCOMPLETE
        return bytes(self._buffer[pos:end]), end + 2

    @staticmethod
    def _int(text: bytes) -> int:
        try:
            return int(text)
        except ValueError:
            raise ProtocolError(f"expected an integer, got {text[:32]!r}") from None

    def _parse(self, pos: int, depth: int):
        if depth > self.MAX_DEPTH:
            raise ProtocolError("reply is nested too deeply")
        if pos >= len(self._buffer):
            return _INCOMPLETE
        kind = self._buffer[pos : pos + 1]
        line = self._line(pos + 1)
        if line is _INCOMPLETE:
            return _INCOMPLETE
        text, pos = line

        if kind == b"+":
            return text.decode("utf-8", "replace"), pos
        if kind == b"-":
            return ServerError(text.decode("utf-8", "replace")), pos
        if kind == b":":
            return self._int(text), pos
        if kind == b"$":
            length = self._int(text)
            if length == -1:
                return None, pos
            if length < 0:
                raise ProtocolError(f"negative bulk length {length}")
            if len(self._buffer) < pos + length + 2:
                return _INCOMPLETE
            if self._buffer[pos + length : pos + length + 2] != _CRLF:
                raise ProtocolError("bulk string is not terminated by CRLF")
            return bytes(self._buffer[pos : pos + length]), pos + length + 2
        if kind == b"*":
            count = self._int(text)
            if count == -1:
                return None, pos
            if count < 0:
                raise ProtocolError(f"negative array length {count}")
            items = []
            for _ in range(count):
                parsed = self._parse(pos, depth + 1)
                if parsed is _INCOMPLETE:
                    return _INCOMPLETE
                item, pos = parsed
                items.append(item)
            return items, pos
        raise ProtocolError(f"unknown reply type {bytes(kind)!r}")


class Connection:
    """One socket to a baton server. Not thread-safe: one thread, one connection."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 7379,
        *,
        password: Optional[str] = None,
        name: Optional[str] = None,
        connect_timeout: float = 5.0,
        timeout: float = 10.0,
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._password = password
        self._name = name
        self._connect_timeout = connect_timeout
        self._socket: Optional[socket.socket] = None
        self._parser = Parser()

    @property
    def connected(self) -> bool:
        return self._socket is not None

    def connect(self) -> None:
        """Opens the socket and authenticates. Nothing has been executed if this fails."""
        self.close()
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self._connect_timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError as error:
            raise ConnectionLost(
                f"cannot connect to {self.host}:{self.port}: {error}", maybe_executed=False
            ) from error
        self._socket = sock
        self._parser = Parser()
        try:
            if self._password is not None:
                reply = self.call("AUTH", self._password)
                if isinstance(reply, ServerError):
                    raise error_from_reply(reply.code, reply.message)
            if self._name:
                self.call("CLIENT", "SETNAME", self._name)
        except ConnectionLost as error:
            # The handshake changes nothing on the server: safe to start over.
            raise ConnectionLost(str(error), maybe_executed=False) from error
        except BaseException:
            self.close()
            raise

    def call(self, *args: Union[str, bytes, int], timeout: Optional[float] = None) -> Reply:
        """Sends one request and reads its reply.

        Raises ConnectionLost. Its ``maybe_executed`` is False only if not a
        single byte of the request reached the socket.
        """
        if self._socket is None:
            raise ConnectionLost("not connected", maybe_executed=False)
        request = encode_command(*args)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        sent = False
        try:
            self._socket.settimeout(max(deadline - time.monotonic(), 0.001))
            sent = True  # sendall() may have written part of the request when it fails
            self._socket.sendall(request)
            while True:
                reply = self._parser.next_reply()
                if reply is not Parser.INCOMPLETE:
                    return reply
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise socket.timeout("timed out waiting for the reply")
                self._socket.settimeout(remaining)
                data = self._socket.recv(65536)
                if not data:
                    raise ConnectionError("the server closed the connection")
                self._parser.feed(data)
        except (OSError, ProtocolError) as error:
            # After a timeout or a garbled reply the stream is out of step with
            # our requests; the only safe thing to do with it is to drop it.
            self.close()
            raise ConnectionLost(f"{args[0]}: {error}", maybe_executed=sent) from error

    def close(self) -> None:
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass
            self._socket = None
