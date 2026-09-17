# baton

[![CI](https://github.com/GaganGutta/baton/actions/workflows/ci.yml/badge.svg)](https://github.com/GaganGutta/baton/actions/workflows/ci.yml)

**baton is a durable job and workflow engine in a single binary.** Apps enqueue
work, workers reserve it under a lease, and no acknowledged job is ever lost —
even if the server or the workers are killed mid-job. It speaks the Redis wire
protocol (RESP2) with its own commands, so any Redis client library can connect,
and it needs no Postgres, Redis or Temporal to run. It is written from scratch
in C++20, in the spirit of Beanstalkd and Faktory, with durable workflows in
the spirit of Temporal.

> **Status: under construction.** baton is being built milestone by milestone;
> [`PROGRESS.md`](PROGRESS.md) says exactly what works today and
> [`PLAN.md`](PLAN.md) what is coming. This README only describes what exists.
> Sections such as the quickstart, command reference, guarantees, benchmarks
> and comparisons are added when the code and tests behind them exist.

## Who it is for

Small teams and side projects that want reliable background jobs and multi-step
workflows without running more infrastructure.

**Non-goals:** replication, clustering, TLS, datasets larger than RAM. baton is
a single node; see [`docs/design.md`](docs/design.md) for what that means.

## Building

Linux (primary) or macOS, a C++20 compiler (GCC 13+, Clang 16+), CMake 3.24+
and Ninja:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/src/server/baton --version
```

`scripts/check.sh` runs what CI runs: formatting, ASan+UBSan, TSan and
clang-tidy.

## Documentation

- [`docs/design.md`](docs/design.md) — how baton works and why, decision by decision
- [`docs/guarantees.md`](docs/guarantees.md) — exactly what baton promises, each promise tied to a test
- [`docs/protocol.md`](docs/protocol.md) — the wire protocol and command reference
- [`docs/testing.md`](docs/testing.md) — test layers, fuzzing, the chaos harness and its results
- [`docs/roadmap.md`](docs/roadmap.md) — what is deliberately not built (yet)

## License

[MIT](LICENSE)
