# Chaos harness

Real processes, real sockets, a real file system, and `kill -9` — checked
against five invariants. The why and how are in
[docs/design.md, section 10](../docs/design.md#10-m7--the-chaos-harness); results
and the bugs it found are in [docs/testing.md](../docs/testing.md#chaos-harness).

```bash
cmake --preset release && cmake --build --preset release --target baton baton-logcheck

chaos/run.py --seeds 1-3 --duration 20      # three kill rounds
chaos/run.py --faults                       # the four fault scenarios
chaos/run.py --seeds 17 --duration 45 --keep   # reproduce seed 17 and keep every file
chaos/selftest.sh                           # prove that the harness can fail
```

Needs Python 3.9+ and nothing else (it uses the SDK in `sdk/python`). Binaries
come from `$BATON_BIN` / `$BATON_LOGCHECK` or the first build tree that has them.
The second half of the full-disk scenario needs a small file system:

```bash
sudo mkdir -p /mnt/baton-small && sudo mount -t tmpfs -o size=24m tmpfs /mnt/baton-small
sudo chmod 1777 /mnt/baton-small
BATON_CHAOS_SMALL_FS=/mnt/baton-small chaos/run.py --faults
```

| File | |
|---|---|
| `run.py` | command line, summary, exit code |
| `harness/round.py` | one kill round: the seeded schedule, the drain, the evidence gathering, the segment archive |
| `harness/actors.py` | the processes of a round: producers, workers, rogues |
| `harness/evidence.py` | reads journals, run logs and the raw ledger file |
| `harness/invariants.py` | the five checks |
| `harness/faults.py` | torn log tail, flipped bit, torn snapshot, full disk |
| `harness/procs.py` | starting, killing and restarting the server |
| `selftest.sh` | plants bugs in a scratch copy and requires the rounds to fail |
| `../tools/logcheck.cpp` | `baton-logcheck`, the offline lease check of the server's own log |

A failing round leaves everything in `--out/seed-N/`: `report.json` (the violated
invariants with job ids), `actions.json` (what was killed when), the data
directory, `log-archive/` (every log segment that ever existed), the server's
and every actor's stderr, the producers' journals, the workers' run logs, the
ledger and `logcheck.json`.
