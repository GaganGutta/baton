"""Shared helpers for the integration tests."""

from __future__ import annotations

import redis


def client(server, **kwargs) -> redis.Redis:
    return redis.Redis(host="127.0.0.1", port=server.port, socket_timeout=10, **kwargs)


def _text(value):
    return value.decode() if isinstance(value, bytes) else value


def fields(reply) -> dict:
    """STATUS/STATS replies as a dict with str keys.

    RESP2 connections get a flat field-value array (like HGETALL), RESP3
    connections get a map; the contents are the same.
    """
    if isinstance(reply, dict):
        return {_text(key): value for key, value in reply.items()}
    return {_text(reply[i]): reply[i + 1] for i in range(0, len(reply), 2)}
