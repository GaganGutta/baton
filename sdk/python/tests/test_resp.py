"""The RESP2 codec, without a server."""

import pytest

from baton.errors import ProtocolError
from baton.resp import Parser, ServerError, encode_command


def parse_all(data: bytes, chunk: int):
    parser = Parser()
    replies = []
    for start in range(0, len(data), chunk):
        parser.feed(data[start:start + chunk])
        while True:
            reply = parser.next_reply()
            if reply is Parser.INCOMPLETE:
                break
            replies.append(reply)
    return replies


def test_encode_command():
    assert encode_command("PING") == b"*1\r\n$4\r\nPING\r\n"
    assert encode_command("ENQUEUE", "q", b"\x00\r\n", 42, -7) == (
        b"*5\r\n$7\r\nENQUEUE\r\n$1\r\nq\r\n$3\r\n\x00\r\n\r\n$2\r\n42\r\n$2\r\n-7\r\n")
    assert encode_command("ECHO", "héllo") == b"*2\r\n$4\r\nECHO\r\n$6\r\nh\xc3\xa9llo\r\n"


@pytest.mark.parametrize("bad", [None, 1.5, True, ["list"]])
def test_encode_refuses_ambiguous_types(bad):
    with pytest.raises(TypeError):
        encode_command("ECHO", bad)


STREAM = (b"+OK\r\n" b":-12\r\n" b"$5\r\nhe\r\nl\r\n" b"$0\r\n\r\n" b"$-1\r\n" b"*-1\r\n" b"*0\r\n"
          b"*3\r\n:1\r\n$2\r\nab\r\n*2\r\n+x\r\n$-1\r\n"
          b"-STALE the token is not current\r\n")


@pytest.mark.parametrize("chunk", [1, 2, 3, 7, 1000])
def test_every_reply_type_at_any_fragmentation(chunk):
    replies = parse_all(STREAM, chunk)
    error = replies.pop()
    assert replies == ["OK", -12, b"he\r\nl", b"", None, None, [], [1, b"ab", ["x", None]]]
    assert isinstance(error, ServerError)
    assert (error.code, error.message) == ("STALE", "the token is not current")


def test_incomplete_replies_consume_nothing():
    parser = Parser()
    parser.feed(b"*2\r\n$3\r\nabc\r\n$3\r\nde")
    assert parser.next_reply() is Parser.INCOMPLETE
    assert parser.next_reply() is Parser.INCOMPLETE
    parser.feed(b"f\r\n+NEXT\r\n")
    assert parser.next_reply() == [b"abc", b"def"]
    assert parser.next_reply() == "NEXT"
    assert parser.next_reply() is Parser.INCOMPLETE


@pytest.mark.parametrize("garbage", [
    b"?what\r\n", b":12x\r\n", b"$abc\r\n", b"$-2\r\n", b"*-5\r\n", b"$3\r\nabcde\r\n",
    b"*1\r\n" * 20 + b":1\r\n",
])
def test_garbage_is_a_protocol_error(garbage):
    parser = Parser()
    parser.feed(garbage)
    with pytest.raises(ProtocolError):
        while parser.next_reply() is not Parser.INCOMPLETE:
            pass


def test_endless_header_line_is_refused():
    parser = Parser()
    parser.feed(b"+" + b"x" * 70_000)
    with pytest.raises(ProtocolError):
        parser.next_reply()
