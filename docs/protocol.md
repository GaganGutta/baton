# Protocol

baton speaks RESP2, the Redis serialization protocol, with its own command set.
Any Redis client library that can send arbitrary commands can talk to baton.

This document is written at the start of milestone M3, **before** the
networking code, and is the contract the server is tested against. Until then
there is no server to talk to; see `PROGRESS.md` for the current state.
