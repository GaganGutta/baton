#!/usr/bin/env python3
"""baton next to Beanstalkd and Faktory, on the same machine, under the same load.

    bench/compare/run_compare.sh            # builds the images, then runs this
    bench/compare/run_compare.py --quick

All three servers run the same way: in a container, on the host's network (no
port-mapping proxy in the path), with their data on a fresh named volume on the
same disk. The load generator runs on the host. Every server setting is in
SYSTEMS below and nowhere else; docs/benchmarks.md explains what each means for
durability, because "jobs per second" without "and what survives a power cut" is
not a comparison.

Writes bench/out/compare.json and bench/out/compare.md.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LOADGEN = REPO / "build" / "release" / "bench" / "baton-loadgen"

FAKTORY_IMAGE = "contribsys/faktory:1.10.0"

# label, protocol, port, image, container arguments, volume mount point, durability
SYSTEMS = [
    {"label": "baton --fsync always", "group": "fsync before every acknowledgement",
     "protocol": "baton", "port": 7379, "image": "baton:bench", "mount": "/var/lib/baton",
     "args": ["--fsync", "always"],
     "durability": "fdatasync before any reply that depends on the record; group commit"},
    {"label": "beanstalkd -f 0", "group": "fsync before every acknowledgement",
     "protocol": "beanstalkd", "port": 11300, "image": "beanstalkd:bench", "mount": "/data",
     "args": ["-f", "0"],
     "durability": "binlog, fsync after every write"},
    {"label": "baton --fsync interval (50 ms)", "group": "fsync every 50 ms",
     "protocol": "baton", "port": 7379, "image": "baton:bench", "mount": "/var/lib/baton",
     "args": ["--fsync", "interval", "--fsync-interval", "50ms"],
     "durability": "reply after write(); fdatasync every 50 ms: a power cut can lose 50 ms"},
    {"label": "beanstalkd -f 50", "group": "fsync every 50 ms",
     "protocol": "beanstalkd", "port": 11300, "image": "beanstalkd:bench", "mount": "/data",
     "args": ["-f", "50"],
     "durability": "binlog, fsync at most every 50 ms (its default): a power cut can lose 50 ms"},
    {"label": "faktory (defaults)", "group": "snapshots only",
     "protocol": "faktory", "port": 7419, "image": FAKTORY_IMAGE, "mount": "/root/.faktory",
     "args": ["/faktory", "-b", "127.0.0.1:7419", "-w", "127.0.0.1:7420"],
     "durability": "embedded Redis with RDB snapshots (save 30 5 / save 120 1), no AOF: a "
                   "crash can lose up to 30 s of acknowledged jobs"},
]


def sh(*command: str, check: bool = True) -> str:
    done = subprocess.run(command, capture_output=True, text=True)
    if check and done.returncode != 0:
        raise RuntimeError(f"{' '.join(command)}: {done.stderr.strip()}")
    return done.stdout.strip()


def start(system: dict) -> None:
    sh("docker", "rm", "-f", "baton-compare", check=False)
    sh("docker", "volume", "rm", "-f", "baton-compare-data", check=False)
    sh("docker", "volume", "create", "baton-compare-data")
    sh("docker", "run", "-d", "--name", "baton-compare", "--network", "host",
       "-v", f"baton-compare-data:{system['mount']}", system["image"], *system["args"])
    deadline = time.monotonic() + 60
    while True:
        try:
            with socket.create_connection(("127.0.0.1", system["port"]), timeout=0.5):
                return
        except OSError:
            if time.monotonic() > deadline:
                raise RuntimeError(f"{system['label']} did not start:\n"
                                   + sh("docker", "logs", "baton-compare", check=False)) from None
            time.sleep(0.1)


def stop() -> None:
    sh("docker", "rm", "-f", "baton-compare", check=False)
    sh("docker", "volume", "rm", "-f", "baton-compare-data", check=False)


def loadgen(system: dict, duration: int, warmup: int, **options) -> dict:
    command = [str(LOADGEN), "--protocol", system["protocol"], "--port", str(system["port"]),
               "--duration", str(duration), "--warmup", str(warmup)]
    for key, value in options.items():
        command += [f"--{key}", str(value)]
    done = subprocess.run(command, capture_output=True, text=True, timeout=3600)
    if not done.stdout.strip():
        raise RuntimeError(f"loadgen printed nothing: {done.stderr}")
    return json.loads(done.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--out", default=str(REPO / "bench" / "out"))
    args = parser.parse_args()
    duration, warmup = (3, 1) if args.quick else (10, 2)

    versions = {
        "docker": sh("docker", "version", "--format", "{{.Server.Version}}"),
        "beanstalkd": sh("docker", "run", "--rm", "--entrypoint", "beanstalkd",
                         "beanstalkd:bench", "-v"),
        "faktory": FAKTORY_IMAGE,
        "baton": sh("docker", "run", "--rm", "baton:bench", "--version"),
    }
    rows = []
    try:
        for system in SYSTEMS:
            print(f"== {system['label']}", file=sys.stderr, flush=True)
            for connections in (1, 8, 64):
                start(system)
                run = loadgen(system, duration, warmup, mode="enqueue", connections=connections)
                rows.append({"system": system["label"], "group": system["group"],
                             "scenario": f"enqueue, {connections} connections", **run})
            start(system)
            run = loadgen(system, duration, warmup, mode="e2e", producers=8, workers=32)
            rows.append({"system": system["label"], "group": system["group"],
                         "scenario": "end to end, 8 producers, 32 workers", **run})
    finally:
        stop()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    suffix = "-quick" if args.quick else ""
    result = {"versions": versions, "systems": SYSTEMS, "rows": rows}
    (out / f"compare{suffix}.json").write_text(json.dumps(result, indent=2) + "\n")
    report = markdown(result)
    (out / f"compare{suffix}.md").write_text(report + "\n")
    print(report)
    return 0


def markdown(result: dict) -> str:
    def ms(h: dict) -> str:
        return f"{h['p50'] / 1000:.2f} / {h['p99'] / 1000:.2f}"

    out = ["```"] + [f"{k + ':':<12}{v}" for k, v in result["versions"].items()] + ["```", ""]
    out += ["| system | what an acknowledgement means |", "|---|---|"]
    out += [f"| {s['label']} | {s['durability']} |" for s in result["systems"]]
    out += ["", "| durability | system | scenario | enqueued/s | enqueue p50 / p99 ms | "
            "completed/s | pickup p50 / p99 ms | errors |", "|---|---|---|---:|---:|---:|---:|---:|"]
    for r in result["rows"]:
        e2e = r["mode"] == "e2e"
        out.append(
            f"| {r['group']} | {r['system']} | {r['scenario']} | {r['enqueued_per_s']:,.0f} | "
            f"{ms(r['enqueue_latency_us'])} | "
            f"{format(r['completed_per_s'], ',.0f') if e2e else ''} | "
            f"{ms(r['pickup_latency_us']) if e2e else ''} | {r['errors']} |")
    return "\n".join(out)


if __name__ == "__main__":
    sys.exit(main())
