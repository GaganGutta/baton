"""The five invariants of a chaos round (docs/design.md 10.2).

Each check returns a list of violations - human-readable, with the job ids and
keys involved - and adds its numbers to the report. An empty list is a pass.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from typing import Dict, List

from .evidence import Journals, RunLogs

MAX_LISTED = 10


def _listed(items) -> str:
    items = sorted(items)
    more = f" and {len(items) - MAX_LISTED} more" if len(items) > MAX_LISTED else ""
    return ", ".join(map(str, items[:MAX_LISTED])) + more


def no_lost_jobs(journals: Journals, states: Dict[int, str], report: dict) -> List[str]:
    """1. Every enqueue that received OK ends up succeeded or dead."""
    acknowledged = journals.acknowledged
    wrong = defaultdict(list)
    for job_id, key in acknowledged.items():
        expected = "dead" if key.startswith("poison-") else "succeeded"
        state = states.get(job_id, "missing")
        if state != expected:
            wrong[f"{state} instead of {expected}"].append(job_id)
    report["jobs_acknowledged"] = len(acknowledged)
    report["jobs_succeeded"] = sum(1 for s in states.values() if s == "succeeded")
    report["jobs_dead"] = sum(1 for s in states.values() if s == "dead")
    return [f"acknowledged jobs that are {what}: {_listed(ids)}" for what, ids in wrong.items()]


def one_valid_lease(runs: RunLogs, logcheck: dict, report: dict) -> List[str]:
    """2. Never two valid leases on a job; stale-token ACKs are always rejected."""
    violations = []
    report["logcheck"] = {k: v for k, v in logcheck.items() if k != "violations"}
    if not logcheck.get("ok"):
        found = logcheck.get("violations") or [logcheck.get("error", "no report")]
        violations += [f"baton-logcheck: {v}" for v in found[:MAX_LISTED]]

    report["rogue_reservations"] = runs.rogue_reserved
    report["stale_attempts"] = dict(runs.rogue_attempts)
    report["stale_attempts_sent"] = sum(runs.rogue_attempts.values())
    report["stale_attempts_against_a_successor"] = sum(
        n for what, n in runs.rogue_attempts.items() if what.endswith("held by a successor"))
    report["stale_attempts_accepted"] = len(runs.rogue_accepted)
    for line in runs.rogue_accepted[:MAX_LISTED]:
        violations.append(f"a stale {line['command']} was accepted: job {line['job']} token "
                          f"{line['token']}, while {line['when']}")

    acked = defaultdict(list)
    for line in runs.results:
        if line["outcome"] == "acked":
            acked[line["job"]].append(line["token"])
    twice = [job for job, tokens in acked.items() if len(tokens) > 1]
    if twice:
        violations.append(f"jobs acknowledged more than once: {_listed(twice)}")
    outlived = [job for job, tokens in acked.items()
                if max(runs.tokens_seen[job], default=0) > min(tokens)]
    if outlived:
        violations.append("jobs that were leased again after they had been acknowledged: "
                          + _listed(outlived))
    return violations


def effects_land_once(journals: Journals, runs: RunLogs, states: Dict[int, str],
                      ledger: Counter, report: dict) -> List[str]:
    """3. Handlers may run more than once; each effect is in the ledger exactly once."""
    violations = []
    duplicated = [key for key, count in ledger.items() if count > 1]
    if duplicated:
        violations.append(f"effects recorded more than once: {_listed(duplicated)}")
    missing = [key for job_id, key in journals.acknowledged.items()
               if states.get(job_id) == "succeeded" and ledger[key] == 0]
    if missing:
        violations.append(f"succeeded jobs whose effect is not in the ledger: {_listed(missing)}")
    poisoned = [key for key in ledger if key.startswith("poison-")]
    if poisoned:
        violations.append(f"poison jobs left an effect: {_listed(poisoned)}")

    runs_per_job = Counter(line["job"] for line in runs.starts)
    report["handler_runs"] = len(runs.starts)
    report["jobs_run"] = len(runs_per_job)
    report["duplicate_runs"] = len(runs.starts) - len(runs_per_job)
    report["max_runs_of_one_job"] = max(runs_per_job.values(), default=0)
    report["effects_in_ledger"] = sum(ledger.values())
    report["effects_refused_as_duplicates"] = sum(1 for e in runs.effects if not e["landed"])
    report["results"] = dict(Counter(line["outcome"] for line in runs.results))
    return violations


def one_key_one_job(journals: Journals, total_enqueued: int, report: dict) -> List[str]:
    """4. One idempotency key never creates two jobs."""
    violations = []
    split = {key: ids for key, ids in journals.ids_by_key.items() if len(ids) > 1}
    for key, ids in list(split.items())[:MAX_LISTED]:
        violations.append(f"key {key} was acknowledged with different job ids: {sorted(ids)}")
    by_id = defaultdict(set)
    for key, ids in journals.ids_by_key.items():
        for job_id in ids:
            by_id[job_id].add(key)
    shared = {job_id: keys for job_id, keys in by_id.items() if len(keys) > 1}
    for job_id, keys in list(shared.items())[:MAX_LISTED]:
        violations.append(f"job {job_id} was returned for different keys: {sorted(keys)}")

    acknowledged_keys, attempted_keys = len(journals.ids_by_key), len(journals.intents)
    report["keys_acknowledged"] = acknowledged_keys
    report["keys_attempted"] = attempted_keys
    report["server_total_enqueued"] = total_enqueued
    report["enqueue_errors_seen_by_producers"] = len(journals.errors)
    if not acknowledged_keys <= total_enqueued <= attempted_keys:
        violations.append(
            f"the server counts {total_enqueued} jobs for {acknowledged_keys} acknowledged and "
            f"{attempted_keys} attempted keys")
    return violations


def recovers_every_time(restarts: List[dict], failed_restart: str, report: dict) -> List[str]:
    """5. The server recovers after every kill; how long it took is recorded."""
    report["server_kills"] = len(restarts)
    if restarts:
        walls = sorted(r["wall_ms"] for r in restarts)
        report["restart_wall_ms"] = {"median": walls[len(walls) // 2], "max": walls[-1]}
        report["recovery_ms_max"] = max(r["recovery_ms"] for r in restarts)
        report["recovered_records_max"] = max(r["recovered_records"] for r in restarts)
        report["restarts_from_snapshot"] = sum(1 for r in restarts if r["from_snapshot_lsn"] > 0)
        report["restarts_with_torn_tail"] = sum(1 for r in restarts if r["torn_bytes"] > 0)
    return [failed_restart] if failed_restart else []
