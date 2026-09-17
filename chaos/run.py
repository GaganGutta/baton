#!/usr/bin/env python3
"""The chaos harness (docs/design.md 10, docs/testing.md).

    chaos/run.py --seeds 1-3 --duration 20            # kill rounds
    chaos/run.py --faults                             # the four fault scenarios
    chaos/run.py --seeds 1-40 --duration 45 --faults  # the long run

Binaries: $BATON_BIN and $BATON_LOGCHECK, or the first build tree that has them.
Everything a round produces stays in --out (default chaos/out/<seed>); a passing
round's directory is deleted unless --keep is given. Exit code 0: every
invariant held.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from harness.round import Round, Scale  # noqa: E402


def parse_seeds(text: str):
    seeds = []
    for part in text.split(","):
        first, _, last = part.partition("-")
        seeds += range(int(first), int(last or first) + 1)
    return seeds


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seeds", default="", help="for example 1,2,5-9")
    parser.add_argument("--duration", type=float, default=20.0, help="seconds of chaos per seed")
    parser.add_argument("--faults", action="store_true", help="run the fault scenarios")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "out")
    parser.add_argument("--keep", action="store_true", help="keep the output of passing rounds")
    parser.add_argument("--producers", type=int, default=2)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--pace-ms", type=float, default=2.0,
                        help="mean pause of a producer between enqueues")
    parser.add_argument("--rogues", type=int, default=2,
                        help="clients that use dead lease tokens on purpose")
    parser.add_argument("--work-ms", type=float, default=100.0,
                        help="a handler works for up to this long")
    args = parser.parse_args()
    if not args.seeds and not args.faults:
        parser.error("nothing to do: give --seeds and/or --faults")

    scale = Scale(args.producers, args.workers, args.concurrency, args.pace_ms, args.work_ms,
                  args.rogues)
    summary = {"rounds": [], "faults": [], "ok": True}
    began = time.monotonic()

    def save() -> None:
        # After every round and scenario: a run that dies on the way keeps what it has.
        summary["elapsed_s"] = round(time.monotonic() - began, 1)
        summary["totals"] = totals(summary["rounds"])
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    for seed in parse_seeds(args.seeds) if args.seeds else []:
        out = args.out / f"seed-{seed}"
        shutil.rmtree(out, ignore_errors=True)
        chaos_round = Round(seed, args.duration, out, scale)
        try:
            report = chaos_round.run()
        except Exception as error:  # the harness itself broke: that is a failed round too
            report = {"seed": seed, "ok": False, "violations": {
                "harness": [f"{type(error).__name__}: {error}"]}}
        finally:
            chaos_round.cleanup()
        summary["rounds"].append(report)
        summary["ok"] &= report["ok"]
        save()
        print(round_line(report), flush=True)
        for name, found in report["violations"].items():
            for violation in found:
                print(f"    VIOLATED {name}: {violation}", flush=True)
        if report["ok"] and not args.keep:
            shutil.rmtree(out, ignore_errors=True)
            out.parent.mkdir(parents=True, exist_ok=True)

    if args.faults:
        from harness import faults

        for scenario in faults.SCENARIOS:
            out = args.out / f"fault-{scenario.__name__}"
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            result = faults.run(scenario, out)
            summary["faults"].append(result)
            summary["ok"] &= result["ok"]
            save()
            print(f"fault {result['name']:<22} {'ok  ' if result['ok'] else 'FAIL'} "
                  f"{result['detail']}", flush=True)
            if result["ok"] and not args.keep:
                shutil.rmtree(out, ignore_errors=True)

    save()
    print(json.dumps({"ok": summary["ok"], "elapsed_s": summary["elapsed_s"],
                      **summary["totals"]}, indent=2))
    return 0 if summary["ok"] else 1


def round_line(report: dict) -> str:
    status = "ok  " if report["ok"] else "FAIL"
    restart = report.get("restart_wall_ms", {})
    return (f"seed {report['seed']:<4} {status} kills={report.get('server_kills', 0)} "
            f"jobs={report.get('jobs_acknowledged', 0)} runs={report.get('handler_runs', 0)} "
            f"extra_runs={report.get('duplicate_runs', 0)} "
            f"stale_attempts={report.get('stale_attempts_sent', 0)} "
            f"({report.get('stale_attempts_against_a_successor', 0)} vs a successor, "
            f"{report.get('stale_attempts_accepted', 0)} accepted) "
            f"restart_ms(med/max)={restart.get('median', '-')}/{restart.get('max', '-')} "
            f"drain_s={report.get('drain_s')}")


def totals(rounds) -> dict:
    keys = ["server_kills", "jobs_acknowledged", "jobs_succeeded", "jobs_dead", "handler_runs",
            "duplicate_runs", "effects_in_ledger", "effects_refused_as_duplicates",
            "rogue_reservations", "stale_attempts_sent", "stale_attempts_against_a_successor",
            "stale_attempts_accepted", "snapshots_written", "log_segments_compacted",
            "restarts_from_snapshot"]
    result = {"rounds": len(rounds), "rounds_failed": sum(1 for r in rounds if not r["ok"])}
    for key in keys:
        result[key] = sum(r.get(key, 0) for r in rounds)
    walls = [r["restart_wall_ms"]["max"] for r in rounds if "restart_wall_ms" in r]
    medians = sorted(r["restart_wall_ms"]["median"] for r in rounds if "restart_wall_ms" in r)
    if walls:
        result["restart_wall_ms_max"] = max(walls)
        result["restart_wall_ms_median_of_medians"] = medians[len(medians) // 2]
        result["recovery_ms_max"] = max(r.get("recovery_ms_max", 0) for r in rounds)
    records = sum(r.get("logcheck", {}).get("records", 0) for r in rounds)
    result["log_records_checked"] = records
    result["leases_checked"] = sum(r.get("logcheck", {}).get("leases_granted", 0) for r in rounds)
    result["lease_expiries"] = sum(r.get("logcheck", {}).get("lease_expiries", 0) for r in rounds)
    return result


if __name__ == "__main__":
    sys.exit(main())
