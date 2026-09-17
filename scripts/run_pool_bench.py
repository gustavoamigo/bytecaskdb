#!/usr/bin/env python3
from __future__ import annotations
"""Build pool_bench in release mode, run the pool_bytes/dataset_bytes sweep,
and append results to benchmarks/pool_bench_results.csv.

This is the buffer pool's benchmark. It is separate from run_engine_bench.py on
purpose: every benchmark there fits in RAM, which is the regime where the pool
should be switched off, so running the pool against them measures overhead and
nothing else. The axis that means anything is how much of the dataset the pool
can hold.

Read p50 and p99, not ops_per_sec. Tenet 3 makes the tail the deciding number,
and a change that lifts mean throughput while pushing out p99 is a regression.

Usage:
    python3 scripts/run_pool_bench.py [--skip-build] [--full]
                                      [--keys N] [--value-bytes N] [--ops N]
                                      [--ratios a,b,c] [--tmpdir DIR]

    --skip-build    Skip the xmake build (binary must already exist).
    --full          240 000 keys / 240 000 ops. Default is a lighter 60 000.
    --ratios        pool_bytes / dataset_bytes points (default 0.1,0.25,0.5,1.0,2.0).

CAVEAT — what this measures. The pool runs each ratio twice: direct_io=1
(O_DIRECT fills, the file's page-cache residency dropped at open) and
direct_io=0 (fills through the page cache). Without --cold every backend
reads from a warm page cache, so the direct arm is the only one paying
device reads and the comparison is one-sided. --cold fsyncs and drops every
data file's pages before each run (no root needed: POSIX_FADV_DONTNEED on
clean pages), and each run is done on its own thread so the engine's
thread-local read snapshot — which pins an mmap run's mapping — dies before
the next drop. Read cached_bytes (mincore over the data files after the run)
against rss_bytes: bounding the first is what the pool is for. Showing the
pool's *benefit* still needs a dataset larger than RAM or a cgroup limit,
and neither is set up here.
"""

import argparse
import csv
import os
import platform
import shutil
import socket
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_TARGET = "pool_bench"
BENCH_BINARY = REPO_ROOT / "build/linux/x86_64/release/pool_bench"
CSV_PATH = REPO_ROOT / "benchmarks/pool_bench_results.csv"

BENCH_COLUMNS = [
    "backend", "direct_io", "threads", "ryow", "ratio", "pool_bytes",
    "dataset_bytes",
    "keys",
    "value_bytes",
    "ops", "zipf_s", "ops_per_sec", "p50_ns", "p99_ns", "p999_ns",
    "hit_ratio", "evictions", "cached_bytes",
    "rss_bytes",
]
CSV_COLUMNS = ["git_commit", "timestamp", "host_name", "num_cpus",
               "memory_gb"] + BENCH_COLUMNS


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT,
            text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def memory_gb() -> str:
    try:
        with open("/proc/meminfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return f"{int(line.split()[1]) / (1024 * 1024):.1f}"
    except OSError:
        pass
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--keys", type=int)
    ap.add_argument("--value-bytes", type=int, default=512)
    ap.add_argument("--ops", type=int)
    ap.add_argument("--zipf", default="0.99")
    ap.add_argument("--max-file-bytes", type=int, default=1024 * 1024)
    ap.add_argument("--ratios", default="0.1,0.25,0.5,1.0,2.0")
    ap.add_argument("--tmpdir", default=str(REPO_ROOT / ".tmp"))
    ap.add_argument("--cold", action="store_true",
                    help="drop the data files' page cache before each run")
    ap.add_argument("--ryow", type=int, default=0,
                    help="put/get pairs for the read-your-own-writes arm")
    ap.add_argument("--mt-threads", default="",
                    help="comma list for the multi-reader arm, e.g. 2,4,8")
    args = ap.parse_args()

    keys = args.keys if args.keys else (240_000 if args.full else 60_000)
    ops = args.ops if args.ops else keys

    if not args.skip_build:
        subprocess.check_call(["xmake", "f", "-m", "release"], cwd=REPO_ROOT)
        subprocess.check_call(["xmake", "build", BENCH_TARGET], cwd=REPO_ROOT)

    if not BENCH_BINARY.exists():
        print(f"benchmark binary not found: {BENCH_BINARY}", file=sys.stderr)
        return 1

    data_dir = Path(args.tmpdir) / "pool_bench"
    data_dir.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(BENCH_BINARY),
        "--keys", str(keys),
        "--value-bytes", str(args.value_bytes),
        "--ops", str(ops),
        "--zipf", args.zipf,
        "--max-file-bytes", str(args.max_file_bytes),
        "--ratios", args.ratios,
        "--dir", str(data_dir),
    ]
    if args.mt_threads:
        cmd += ["--mt-threads", args.mt_threads]
    if args.ryow:
        cmd += ["--ryow", str(args.ryow)]
    if args.cold:
        cmd += ["--cold"]
    print(" ".join(cmd), file=sys.stderr)
    try:
        out = subprocess.check_output(cmd, cwd=REPO_ROOT, text=True)
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)

    print(out)

    rows = list(csv.DictReader(out.splitlines()))
    if not rows:
        print("benchmark produced no rows", file=sys.stderr)
        return 1

    prefix = {
        "git_commit": git_commit(),
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "host_name": socket.gethostname(),
        "num_cpus": str(os.cpu_count() or ""),
        "memory_gb": memory_gb(),
    }

    write_header = not CSV_PATH.exists()
    CSV_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CSV_PATH, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        if write_header:
            w.writeheader()
        for r in rows:
            w.writerow({**prefix, **{k: r.get(k, "") for k in BENCH_COLUMNS}})

    print(f"appended {len(rows)} rows -> {CSV_PATH}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
