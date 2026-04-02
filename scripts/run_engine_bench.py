#!/usr/bin/env python3
"""Build engine_bench in release mode, run it, and append results to
benchmarks/engine_bench_results.csv for longitudinal performance tracking.

Usage:
    python3 scripts/run_engine_bench.py [--skip-build] [--full] [--dataset-size N] [-- <extra benchmark flags>]

    --skip-build      Skip the xmake build step (binary must already exist).
    --full            Run with 1 000 000 keys (full benchmark). Default is 50 000 (light).
    --dataset-size N  Run with exactly N keys.
    Extra flags after '--' are forwarded to the benchmark binary.

CSV columns:
    git_commit, timestamp, host_name, num_cpus, mhz_per_cpu,
    cpu_scaling_enabled, memory_gb, load_avg_1m, dataset_size,
    bench_name, run_type, aggregate_name, iterations,
    real_time_ns, cpu_time_ns,
    ops_per_us, scans_per_us, lat_p50_ns, lat_p99_ns
"""

import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_TARGET = "engine_bench"
BENCH_BINARY = REPO_ROOT / "build/linux/x86_64/release/engine_bench"
CSV_PATH = REPO_ROOT / "benchmarks/engine_bench_results.csv"

CSV_COLUMNS = [
    "git_commit",
    "timestamp",
    "host_name",
    "num_cpus",
    "mhz_per_cpu",
    "cpu_scaling_enabled",
    "memory_gb",
    "load_avg_1m",
    "dataset_size",
    "bench_name",
    "run_type",
    "aggregate_name",
    "iterations",
    "real_time_ns",
    "cpu_time_ns",
    "ops_per_us",
    "scans_per_us",
    "lat_p50_ns",
    "lat_p99_ns",
]

# Multiplier to convert from the benchmark's time_unit to nanoseconds.
_TIME_UNIT_TO_NS: dict[str, float] = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def get_git_commit() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
        cwd=REPO_ROOT,
    )
    return result.stdout.strip()


def get_memory_gb() -> float:
    """Read total installed RAM from /proc/meminfo (Linux only)."""
    try:
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    return round(kb / 1024 / 1024, 2)
    except (OSError, ValueError):
        pass
    return 0.0


def build(skip: bool) -> None:
    if skip:
        print(f"[skip-build] Using existing binary: {BENCH_BINARY}")
        return
    print("Configuring release mode...")
    subprocess.run(["xmake", "f", "-m", "release"], check=True, cwd=REPO_ROOT)
    print(f"Building {BENCH_TARGET}...")
    subprocess.run(["xmake", "build", BENCH_TARGET], check=True, cwd=REPO_ROOT)


def run_benchmark(extra_flags: list[str], dataset_size: int) -> dict:
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name
    try:
        cmd = [
            str(BENCH_BINARY),
            f"--benchmark_out={out_path}",
            "--benchmark_out_format=json",
        ] + extra_flags
        env = os.environ.copy()
        env["BC_DATASET_SIZE"] = str(dataset_size)
        print(f"Running: BC_DATASET_SIZE={dataset_size} {' '.join(cmd)}")
        subprocess.run(cmd, check=True, cwd=REPO_ROOT, env=env)
        with open(out_path, encoding="utf-8") as f:
            return json.load(f)
    finally:
        os.unlink(out_path)


def _migrate_csv_header() -> None:
    """Insert 'dataset_size' into the CSV header if it is absent.

    Rewrites the file in-place: replaces the first line (header) with the
    current CSV_COLUMNS list, leaving all data rows unchanged. Old rows will
    have an empty dataset_size cell when read back.
    """
    if not CSV_PATH.exists():
        return
    with open(CSV_PATH, encoding="utf-8") as f:
        lines = f.readlines()
    if not lines:
        return
    existing_header = lines[0].rstrip("\r\n").split(",")
    if "dataset_size" in existing_header:
        return  # already migrated
    new_header = ",".join(CSV_COLUMNS) + "\n"
    with open(CSV_PATH, "w", encoding="utf-8") as f:
        f.write(new_header)
        f.writelines(lines[1:])
    print("Migrated CSV header to include 'dataset_size'.")


def append_results(data: dict, git_commit: str, memory_gb: float) -> None:
    ctx = data["context"]
    load_avg = ctx.get("load_avg") or []
    common = {
        "git_commit": git_commit,
        "timestamp": ctx.get("date", ""),
        "host_name": ctx.get("host_name", ""),
        "num_cpus": ctx.get("num_cpus", ""),
        "mhz_per_cpu": ctx.get("mhz_per_cpu", ""),
        "cpu_scaling_enabled": ctx.get("cpu_scaling_enabled", ""),
        "memory_gb": memory_gb,
        "load_avg_1m": load_avg[0] if load_avg else "",
        "dataset_size": ctx.get("dataset_size", ""),
    }

    _migrate_csv_header()
    write_header = not CSV_PATH.exists()
    with open(CSV_PATH, "a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        if write_header:
            writer.writeheader()

        for bench in data["benchmarks"]:
            ns_factor = _TIME_UNIT_TO_NS.get(bench.get("time_unit", "ns"), 1.0)
            row = {
                **common,
                "bench_name": bench.get("name", ""),
                "run_type": bench.get("run_type", ""),
                "aggregate_name": bench.get("aggregate_name", ""),
                "iterations": bench.get("iterations", ""),
                "real_time_ns": bench.get("real_time", 0.0) * ns_factor,
                "cpu_time_ns": bench.get("cpu_time", 0.0) * ns_factor,
                "ops_per_us": bench.get("ops_per_us", ""),
                "scans_per_us": bench.get("scans_per_us", ""),
                "lat_p50_ns": bench.get("lat_p50_ns", ""),
                "lat_p99_ns": bench.get("lat_p99_ns", ""),
            }
            writer.writerow(row)

    row_count = len(data["benchmarks"])
    print(f"Appended {row_count} rows to {CSV_PATH.relative_to(REPO_ROOT)}")


def main() -> None:
    args = sys.argv[1:]
    skip_build = "--skip-build" in args
    if skip_build:
        args.remove("--skip-build")

    # --full is shorthand for --dataset-size=1000000.
    if "--full" in args:
        args.remove("--full")
        dataset_size = 1_000_000
    else:
        dataset_size = 50_000

    for arg in list(args):
        if arg.startswith("--dataset-size="):
            dataset_size = int(arg.split("=", 1)[1])
            args.remove(arg)
        elif arg == "--dataset-size":
            idx = args.index(arg)
            dataset_size = int(args[idx + 1])
            args.remove(args[idx + 1])
            args.remove(arg)

    # Strip leading '--' separator for extra benchmark flags.
    if "--" in args:
        sep = args.index("--")
        extra_flags = args[sep + 1 :]
    else:
        extra_flags = []

    build(skip_build)
    git_commit = get_git_commit()
    memory_gb = get_memory_gb()
    data = run_benchmark(extra_flags, dataset_size)
    append_results(data, git_commit, memory_gb)


if __name__ == "__main__":
    main()
