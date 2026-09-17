# Testing

baton's bar is that every guarantee in `docs/guarantees.md` is proven by a test.
This document describes the layers of testing, how to run them, and (from M7)
the chaos harness and its recorded results.

## Layers

| Layer | What it proves | Where | Status |
|---|---|---|---|
| Unit tests (GoogleTest) | each module against its contract, incl. death tests for aborts | `tests/unit/` | from M0 |
| Model-based tests | random operation sequences keep state invariants; live state == replayed state | `tests/unit/state/` | M2 |
| Fault-injection tests | torn writes, bit flips, fsync failure, ENOSPC through a fake filesystem | `tests/unit/log/`, `tests/unit/snapshot/` | M1, M5 |
| Fuzzing (libFuzzer) | parsers and decoders never crash or over-read on arbitrary bytes | `fuzz/` | from M1 |
| Integration tests (pytest) | the real binary with redis-cli, redis-py and the SDK | `tests/integration/`, `sdk/python/tests/` | M3, M6 |
| Chaos harness | invariants hold under repeated SIGKILL and injected disk faults | `chaos/` | M7 |

## Running

```bash
scripts/check.sh              # format check, ASan+UBSan, TSan, clang-tidy: what CI runs
scripts/check.sh asan         # one step only
ctest --preset asan -R Result # a subset, after `cmake --preset asan && cmake --build --preset asan`
```

All unit tests run under AddressSanitizer + UndefinedBehaviorSanitizer and again
under ThreadSanitizer, locally and in CI. UBSan is configured with
`-fno-sanitize-recover=all`, so undefined behaviour fails the run instead of
printing a warning.

## Rules

- A failing test is fixed in the code, never skipped, disabled or weakened.
- Every bug found by fuzzing or chaos gets a regression test before the fix.
- Tests that involve time use `FakeClock`; no test sleeps to wait for a timer.
