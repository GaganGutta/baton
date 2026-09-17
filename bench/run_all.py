#!/usr/bin/env python3
"""Every number in the README and docs/benchmarks.md comes out of this script.

    bench/run_all.sh                 # builds the release preset, then runs this
    bench/run_all.py --quick         # 3-second runs, to check that everything works
    bench/run_all.py --only enqueue,e2e

It starts a fresh baton server per experiment (release build, data directory on
a real file system), drives it with bench/loadgen, and writes

    bench/out/results.json     everything, machine-readable
    bench/out/results.md       the tables that the docs quote verbatim

Experiments
    enqueue     closed-loop enqueue throughput and latency against the number of
                connections, under --fsync always and --fsync interval, with the
                group-commit batch sizes the server reports for each run
    pipelined   the same with pipelining (depth 32)
    latency     open-loop enqueue latency at fixed rates (no coordinated omission)
    e2e         producers + workers: completed jobs per second, pickup latency
    pickup      pickup latency at a low, fixed rate with idle workers waiting
    memory      resident memory per queued job
    recovery    time to restart after kill -9 with a million jobs: from the log
                alone, and from a snapshot
    micro       Google Benchmark microbenchmarks (parser, records, CRC, wheel)
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "sdk" / "python" / "src"))

import baton  # noqa: E402

BUILD = REPO / "build" / "release"
SERVER = BUILD / "src" / "server" / "baton"
LOADGEN = BUILD / "bench" / "baton-loadgen"
MICROBENCH = BUILD / "bench" / "baton_microbench"
PORT = 7391


class Bench:
    def __init__(self, args):
        self.quick = args.quick
        self.duration = 3 if args.quick else 10
        self.warmup = 1 if args.quick else 2
        self.data_root = Path(args.data_dir).resolve()
        self.out = Path(args.out).resolve()
        self.results = {"environment": environment(self.data_root), "experiments": {}}
        self.server = None

    # --- plumbing -------------------------------------------------------------------------

    def start_server(self, *server_args: str, wipe: bool = True) -> subprocess.Popen:
        data = self.data_root / "data"
        if wipe:
            shutil.rmtree(data, ignore_errors=True)
        self.data_root.mkdir(parents=True, exist_ok=True)
        log = open(self.data_root / "server.log", "ab")
        self.server = subprocess.Popen(
            [str(SERVER), "--dir", str(data), "--port", str(PORT), "--log-level", "warn",
             *server_args], stdout=log, stderr=log)
        deadline = time.monotonic() + 300
        while True:
            try:
                with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                    return self.server
            except OSError:
                if self.server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("the server did not start; see server.log") from None
                time.sleep(0.005)

    def stop_server(self, kill: bool = False) -> None:
        if self.server is not None and self.server.poll() is None:
            self.server.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
            self.server.wait(timeout=300)
        self.server = None

    def info(self, section: str) -> dict:
        with baton.Client(port=PORT) as client:
            return client.info(section)

    def loadgen(self, **options) -> dict:
        options.setdefault("duration", self.duration)
        options.setdefault("warmup", self.warmup)
        command = [str(LOADGEN), "--port", str(PORT)]
        for key, value in options.items():
            command += [f"--{key}", str(value)]
        done = subprocess.run(command, capture_output=True, text=True, timeout=3600)
        if not done.stdout.strip():
            raise RuntimeError(f"loadgen printed nothing: {done.stderr}")
        result = json.loads(done.stdout)
        if result["errors"]:
            raise RuntimeError(f"loadgen reported errors: {result['first_error']}")
        return result

    def group_commit(self) -> dict:
        p = self.info("persistence")
        return {"batches": p["log_batches"], "records": p["log_records"],
                "records_per_batch_avg": float(p["log_batch_records_avg"]),
                "records_per_batch_max": p["log_batch_records_max"],
                "fsyncs": p["log_fsync_count"], "fsync_p50_us": p["log_fsync_p50_us"],
                "fsync_p99_us": p["log_fsync_p99_us"]}

    # --- experiments ----------------------------------------------------------------------

    def enqueue(self) -> list:
        rows = []
        for policy in ("always", "interval"):
            for connections in (1, 8, 64, 256):
                self.start_server("--fsync", policy)
                run = self.loadgen(mode="enqueue", connections=connections, depth=1)
                rows.append({"fsync": policy, "connections": connections, "depth": 1, **run,
                             "group_commit": self.group_commit()})
                self.stop_server()
        return rows

    def pipelined(self) -> list:
        rows = []
        for policy in ("always", "interval"):
            self.start_server("--fsync", policy)
            run = self.loadgen(mode="enqueue", connections=8, depth=32)
            rows.append({"fsync": policy, "connections": 8, "depth": 32, **run,
                         "group_commit": self.group_commit()})
            self.stop_server()
        return rows

    def latency(self) -> list:
        rows = []
        for policy, rates in (("always", (1_000, 10_000)), ("interval", (1_000, 10_000, 50_000))):
            for rate in rates:
                self.start_server("--fsync", policy)
                run = self.loadgen(mode="enqueue", connections=16, rate=rate)
                rows.append({"fsync": policy, "connections": 16, "target_rate": rate, **run})
                self.stop_server()
        return rows

    def e2e(self) -> list:
        rows = []
        for policy in ("always", "interval"):
            for workers in (1, 8, 64, 256):
                self.start_server("--fsync", policy)
                run = self.loadgen(mode="e2e", producers=8, workers=workers, depth=8)
                rows.append({"fsync": policy, **run})
                self.stop_server()
        return rows

    def pickup(self) -> list:
        rows = []
        for policy in ("always", "interval"):
            for rate in (1_000, 5_000):
                self.start_server("--fsync", policy)
                run = self.loadgen(mode="e2e", producers=4, workers=64, rate=rate)
                rows.append({"fsync": policy, "target_rate": rate, **run})
                self.stop_server()
        return rows

    def memory(self) -> list:
        rows = []
        jobs_wanted = 100_000 if self.quick else 1_000_000
        for payload in (100, 1_000):
            server = self.start_server("--fsync", "interval", "--snapshot-every", "0")
            before = rss_kb(server.pid)
            seconds = 0
            while self.info("jobs")["jobs_in_memory"] < jobs_wanted:
                self.loadgen(mode="enqueue", connections=8, depth=64, payload=payload,
                             duration=2, warmup=0)
                seconds += 2
                if seconds > 600:
                    raise RuntimeError("could not enqueue enough jobs for the memory test")
            jobs = self.info("jobs")["jobs_in_memory"]
            estimate = self.info("memory")["used_memory_estimate"]
            after = rss_kb(server.pid)
            rows.append({"payload_bytes": payload, "jobs": jobs, "rss_before_kb": before,
                         "rss_after_kb": after,
                         "rss_bytes_per_job": round((after - before) * 1024 / jobs, 1),
                         "server_estimate_bytes_per_job": round(estimate / jobs, 1)})
            self.stop_server(kill=True)
        return rows

    def recovery(self) -> list:
        rows = []
        jobs_wanted = 100_000 if self.quick else 1_000_000
        self.start_server("--fsync", "interval", "--snapshot-every", "0")
        while self.info("jobs")["jobs_in_memory"] < jobs_wanted:
            self.loadgen(mode="enqueue", connections=8, depth=64, payload=256, duration=2,
                         warmup=0)
        jobs = self.info("jobs")["jobs_in_memory"]
        log_bytes = self.info("persistence")["log_bytes"]
        self.stop_server(kill=True)

        rows.append({"from": "log only", "jobs": jobs, "log_mb": round(log_bytes / 1e6, 1),
                     **self.timed_restart()})
        with baton.Client(port=PORT) as client:
            client.snapshot()
            while client.info("persistence")["snapshots_taken"] < 1:
                time.sleep(0.05)
        self.stop_server(kill=True)
        rows.append({"from": "snapshot", "jobs": jobs, "log_mb": round(log_bytes / 1e6, 1),
                     **self.timed_restart()})
        self.stop_server(kill=True)
        return rows

    def timed_restart(self) -> dict:
        began = time.monotonic()
        self.start_server("--fsync", "interval", "--snapshot-every", "0", wipe=False)
        wall_ms = (time.monotonic() - began) * 1000
        p = self.info("persistence")
        return {"restart_wall_ms": round(wall_ms, 1), "recovery_ms": p["recovery_ms"],
                "recovered_records": p["recovered_records"],
                "from_snapshot_lsn": p["recovered_from_snapshot_lsn"]}

    def micro(self) -> list:
        done = subprocess.run(
            [str(MICROBENCH), "--benchmark_format=json",
             f"--benchmark_min_time={'0.05s' if self.quick else '0.5s'}"],
            capture_output=True, text=True, timeout=3600, check=True)
        rows = []
        for b in json.loads(done.stdout)["benchmarks"]:
            row = {"name": b["name"], "ns_per_op": round(b["cpu_time"], 1)}
            if "bytes_per_second" in b:
                row["mb_per_s"] = round(b["bytes_per_second"] / 1e6, 1)
            if "items_per_second" in b:
                row["items_per_s"] = round(b["items_per_second"])
            rows.append(row)
        return rows


def rss_kb(pid: int) -> int:
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError("no VmRSS")


def environment(data_root: Path) -> dict:
    def sh(command: str) -> str:
        return subprocess.run(command, shell=True, capture_output=True, text=True).stdout.strip()

    data_root.mkdir(parents=True, exist_ok=True)
    return {
        "date": time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()),
        "commit": os.environ.get("BATON_COMMIT") or sh("git rev-parse --short HEAD") or "unknown",
        "kernel": f"{platform.system()} {platform.release()}",
        "cpu": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip(),
        "threads": os.cpu_count(),
        "memory_gb": round(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2**30, 1),
        "filesystem": sh(f"df -PT {data_root} | awk 'NR==2 {{print $2 \" on \" $1}}'"),
        "compiler": sh(f"$(grep -m1 CMAKE_CXX_COMPILER: {BUILD}/CMakeCache.txt | cut -d= -f2) "
                       "--version | head -n1"),
        "build": "CMAKE_BUILD_TYPE=Release",
    }


# --- reporting ----------------------------------------------------------------------------


def lat(h: dict) -> str:
    return f"{h['p50'] / 1000:.2f} / {h['p99'] / 1000:.2f} / {h['p999'] / 1000:.2f}"


def markdown(results: dict) -> str:
    env = results["environment"]
    out = ["```"] + [f"{key + ':':<12}{value}" for key, value in env.items()] + ["```", ""]
    e = results["experiments"]

    if "enqueue" in e or "pipelined" in e:
        out += ["### Enqueue throughput (closed loop)", "",
                "| fsync | connections | depth | jobs/s | latency p50 / p99 / p99.9 ms | "
                "records per fsync batch avg / max | fsync p50 / p99 ms |",
                "|---|---:|---:|---:|---:|---:|---:|"]
        for r in e.get("enqueue", []) + e.get("pipelined", []):
            g = r["group_commit"]
            out.append(
                f"| {r['fsync']} | {r['connections']} | {r['depth']} | "
                f"{r['enqueued_per_s']:,.0f} | {lat(r['enqueue_latency_us'])} | "
                f"{g['records_per_batch_avg']:.1f} / {g['records_per_batch_max']} | "
                f"{g['fsync_p50_us'] / 1000:.2f} / {g['fsync_p99_us'] / 1000:.2f} |")
        out.append("")
    if "latency" in e:
        out += ["### Enqueue latency at a fixed rate (open loop, 16 connections)", "",
                "| fsync | target jobs/s | achieved jobs/s | latency p50 / p99 / p99.9 ms | max ms |",
                "|---|---:|---:|---:|---:|"]
        for r in e["latency"]:
            h = r["enqueue_latency_us"]
            out.append(f"| {r['fsync']} | {r['target_rate']:,} | {r['enqueued_per_s']:,.0f} | "
                       f"{lat(h)} | {h['max'] / 1000:.1f} |")
        out.append("")
    if "e2e" in e:
        out += ["### End to end: 8 pipelining producers, N workers (closed loop)", "",
                "Each job is enqueued, reserved and acknowledged: three durable operations.",
                "Where completed/s is below enqueued/s the workers are the bottleneck and the",
                "queue is growing, so no latency is quoted here; see the next table.", "",
                "| fsync | workers | enqueued/s | completed/s |", "|---|---:|---:|---:|"]
        for r in e["e2e"]:
            out.append(f"| {r['fsync']} | {r['workers']} | {r['enqueued_per_s']:,.0f} | "
                       f"{r['completed_per_s']:,.0f} |")
        out.append("")
    if "pickup" in e:
        out += ["### Pickup latency at a fixed rate, 64 workers waiting (open loop)", "",
                "From the moment the ENQUEUE was due to be sent to the moment a worker holds the",
                "job: the enqueue's fsync, the hand-off to a parked RESERVE, and the lease's fsync.",
                "", "| fsync | jobs/s | completed/s | enqueue p50 / p99 / p99.9 ms | "
                "pickup p50 / p99 / p99.9 ms |", "|---|---:|---:|---:|---:|"]
        for r in e["pickup"]:
            out.append(f"| {r['fsync']} | {r['target_rate']:,} | {r['completed_per_s']:,.0f} | "
                       f"{lat(r['enqueue_latency_us'])} | {lat(r['pickup_latency_us'])} |")
        out.append("")
    if "memory" in e:
        out += ["### Memory per queued job", "",
                "| payload bytes | jobs | RSS bytes per job | server's own estimate per job |",
                "|---:|---:|---:|---:|"]
        for r in e["memory"]:
            out.append(f"| {r['payload_bytes']:,} | {r['jobs']:,} | {r['rss_bytes_per_job']:,} | "
                       f"{r['server_estimate_bytes_per_job']:,} |")
        out.append("")
    if "recovery" in e:
        out += ["### Restart after kill -9", "",
                "| recovering from | jobs | log MB | records replayed | server recovery ms | "
                "exec to first reply ms |", "|---|---:|---:|---:|---:|---:|"]
        for r in e["recovery"]:
            out.append(f"| {r['from']} | {r['jobs']:,} | {r['log_mb']:,} | "
                       f"{r['recovered_records']:,} | {r['recovery_ms']:,} | "
                       f"{r['restart_wall_ms']:,} |")
        out.append("")
    if "micro" in e:
        out += ["### Microbenchmarks", "", "| benchmark | ns/op | throughput |", "|---|---:|---:|"]
        for r in e["micro"]:
            rate = (f"{r['items_per_s']:,}/s" if "items_per_s" in r else
                    f"{r['mb_per_s']:,} MB/s" if "mb_per_s" in r else "")
            out.append(f"| `{r['name']}` | {r['ns_per_op']:,} | {rate} |")
        out.append("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--only", default="", help="comma-separated experiment names")
    parser.add_argument("--data-dir", default=str(BUILD / "bench-data"))
    parser.add_argument("--out", default=str(REPO / "bench" / "out"))
    args = parser.parse_args()

    for binary in (SERVER, LOADGEN, MICROBENCH):
        if not binary.is_file():
            sys.exit(f"{binary} is missing: run bench/run_all.sh, which builds it")

    bench = Bench(args)
    names = ["enqueue", "pipelined", "latency", "e2e", "pickup", "memory", "recovery", "micro"]
    wanted = [n for n in args.only.split(",") if n] or names
    try:
        for name in wanted:
            print(f"== {name}", file=sys.stderr, flush=True)
            bench.results["experiments"][name] = getattr(bench, name)()
    finally:
        bench.stop_server(kill=True)
        shutil.rmtree(bench.data_root / "data", ignore_errors=True)

    bench.out.mkdir(parents=True, exist_ok=True)
    suffix = "-quick" if args.quick else ""
    (bench.out / f"results{suffix}.json").write_text(json.dumps(bench.results, indent=2) + "\n")
    report = markdown(bench.results)
    (bench.out / f"results{suffix}.md").write_text(report + "\n")
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
