#!/usr/bin/env python3
from __future__ import annotations
"""Compare recovery performance between the radix tree and the B+ tree key
directories, interleaved so both share machine state.

This exists because the two trees can't live in one binary (BYTECASK_KEYDIR
is a compile-time choice), and a fair recovery comparison needs radix and B+
runs to alternate round by round rather than run back to back — see the
branch history for how much a single non-interleaved run misled us.

Builds two engine_bench binaries (radix and B+ tree), then for each
requested thread count runs every requested config (radix/B+ x plain/sorted
hints) once per round, interleaved, and prints a table of medians plus the
per-round ratios so you can see whether a result is noise before trusting it.

Usage:
    python3 scripts/compare_recovery.py [options] [-- <extra benchmark flags>]

    --dataset-size N     Key count (default 1000000).
    --threads LIST       Comma-separated recovery_threads to test
                          (default: 1,2,4,8,16).
    --rounds N            Interleaved rounds (default 3).
    --configs LIST        Comma-separated from {radix, btree} (default: both).
    --bench-dir DIR        Where the test DB is written (default: ./.tmp).
                           Point this at a real disk, not tmpfs, to see
                           genuine I/O rather than an in-memory measurement.
    --drop-caches          Evict the page cache before each timed recovery
                           (BC_DROP_CACHES=1). Needs root
                           (CAP_SYS_ADMIN) — the process writes to
                           /proc/sys/vm/drop_caches. Meaningless on a VM
                           whose "cold" reads are still served from host
                           RAM; check with a plain dd first if unsure.
    --cpus RANGE           taskset CPU range, e.g. "0-3" (default: unpinned).
    --skip-build            Skip building; use existing eb_radix/eb_btree
                             binaries in build/linux/x86_64/release/.
    --no-rocksdb            Build with BYTECASK_NO_ROCKSDB=1 (skip if you
                             don't have RocksDB installed and don't want the
                             RocksDB comparison rows).
    --json-out DIR           Save each run's raw benchmark JSON into DIR.
    --csv-out FILE           Append a flat CSV of every reading to FILE.
                              Each invocation stamps its rows with a
                              run_id (UTC timestamp): round numbers reset
                              to 1 on every run, so two runs appended to
                              the same file collide on round alone — group
                              by (run_id, round) if aggregating across
                              multiple invocations, not round alone.
    Extra flags after '--' are forwarded to the benchmark binary
    (e.g. -- --benchmark_min_time=3x).

Requires a Recovery benchmark run per (config, threads, round) — each is
itself an internal Google Benchmark repetition set, min-time controlled by
--benchmark_min_time (default 3x, i.e. 3 iterations).
"""

import argparse
import datetime
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build/linux/x86_64/release"
BENCH_TARGET = "engine_bench"

ALL_CONFIGS = ["radix", "btree"]

# Each config maps to the binary built for that key directory. Hint files are
# always written sorted now, so the plain/sorted split these configs used to
# carry is gone — what differs between the two is only the tree.
CONFIG_SPEC = {
    "radix": "eb_radix",
    "btree": "eb_btree",
}


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    print("+ " + " ".join(cmd))
    try:
        return subprocess.run(cmd, cwd=REPO_ROOT, check=True, **kw)
    except subprocess.CalledProcessError as e:
        print(f"error: command failed (exit {e.returncode}): "
              f"{' '.join(cmd)}", file=sys.stderr)
        sys.exit(1)


def clang_config_flags() -> list[str]:
    """--toolchain=clang plus an explicit -resource-dir.

    Without -resource-dir, clang's module dependency scanner can fail to
    find its own builtin headers (stdarg.h and friends) when scanning the
    std module — this repo's own CI hits exactly this and works around it
    the same way (.github/workflows/ci.yml); it is not optional, only
    silent on hosts where some other mechanism happens to paper over it.
    Falls back to bare defaults if `clang` is not on PATH; the resulting
    xmake error will at least be the real one instead of a masked one.
    """
    try:
        clang = shutil.which("clang") or "clang"
        resource_dir = subprocess.run(
            [clang, "--print-resource-dir"], capture_output=True, text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return []
    return ["--toolchain=clang", f"--cxflags=-resource-dir={resource_dir}"]


def build_binaries(no_rocksdb: bool) -> None:
    """Builds engine_bench for both key directories into distinctly named
    binaries. The two trees are a compile-time choice (BYTECASK_KEYDIR), so
    this is two full builds, not one build with a runtime switch."""
    env_base = os.environ.copy()
    if no_rocksdb:
        env_base["BYTECASK_NO_ROCKSDB"] = "1"
    config_flags = clang_config_flags()

    # Radix tree. Both arms name the tree explicitly: the B+ tree is the
    # default build, so leaving BYTECASK_KEYDIR unset would build it twice.
    env = env_base.copy()
    env["BYTECASK_KEYDIR"] = "radix"
    run(["xmake", "f", "-m", "release", "--sanitizer="] + config_flags, env=env)
    run(["xmake", "build", BENCH_TARGET], env=env)
    shutil.copy(BUILD_DIR / "engine_bench", BUILD_DIR / "eb_radix")

    # B+ tree.
    env = env_base.copy()
    env["BYTECASK_KEYDIR"] = "btree"
    run(["xmake", "f", "-m", "release", "--sanitizer="] + config_flags, env=env)
    run(["xmake", "build", BENCH_TARGET], env=env)
    shutil.copy(BUILD_DIR / "engine_bench", BUILD_DIR / "eb_btree")

    for name in ("eb_radix", "eb_btree"):
        if not (BUILD_DIR / name).exists():
            print(f"error: {name} was not built", file=sys.stderr)
            sys.exit(1)


def thread_filter(threads: list[int]) -> str:
    alt = "|".join(str(t) for t in threads)
    return f"^ByteCaskDB/Recovery/threads:({alt})/"


def run_one(
    config: str,
    threads_filter: str,
    dataset_size: int,
    bench_dir: str,
    drop_caches: bool,
    cpus: str | None,
    extra_flags: list[str],
    json_out_dir: Path | None,
    round_num: int,
) -> dict:
    binary = BUILD_DIR / CONFIG_SPEC[config]
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name

    cmd: list[str] = []
    if cpus:
        cmd += ["taskset", "-c", cpus]
    cmd += [
        str(binary),
        f"--benchmark_filter={threads_filter}",
        f"--benchmark_out={out_path}",
        "--benchmark_out_format=json",
    ]
    if not any(f.startswith("--benchmark_min_time") for f in extra_flags):
        cmd.append("--benchmark_min_time=3x")
    cmd += extra_flags

    env = os.environ.copy()
    env["BC_DATASET_SIZE"] = str(dataset_size)
    env["BC_BENCH_DIR"] = bench_dir
    if drop_caches:
        env["BC_DROP_CACHES"] = "1"

    print(f"round {round_num} {config}: " + " ".join(cmd))
    subprocess.run(cmd, cwd=REPO_ROOT, check=True, env=env)
    with open(out_path, encoding="utf-8") as f:
        data = json.load(f)
    os.unlink(out_path)

    if json_out_dir:
        json_out_dir.mkdir(parents=True, exist_ok=True)
        dest = json_out_dir / f"{config}_r{round_num}.json"
        dest.write_text(json.dumps(data))

    return data


def parse_threads_from_name(name: str) -> int:
    # e.g. "ByteCaskDB/Recovery/threads:4/real_time"
    import re
    m = re.search(r"threads:(\d+)", name)
    return int(m.group(1)) if m else 0


def main() -> None:
    run_id = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    p = argparse.ArgumentParser(
        description="Compare radix vs B+ tree recovery, interleaved.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--dataset-size", type=int, default=1_000_000)
    p.add_argument("--threads", default="1,2,4,8,16")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--configs", default=",".join(ALL_CONFIGS))
    p.add_argument("--bench-dir", default=str(REPO_ROOT / ".tmp"))
    p.add_argument("--drop-caches", action="store_true")
    p.add_argument("--cpus", default=None)
    p.add_argument("--skip-build", action="store_true")
    p.add_argument("--no-rocksdb", action="store_true")
    p.add_argument("--json-out", default=None)
    p.add_argument("--csv-out", default=None)
    p.add_argument("extra", nargs=argparse.REMAINDER)
    args = p.parse_args()

    extra_flags = args.extra
    if extra_flags and extra_flags[0] == "--":
        extra_flags = extra_flags[1:]

    threads = [int(t) for t in args.threads.split(",") if t]
    configs = [c for c in args.configs.split(",") if c]
    for c in configs:
        if c not in CONFIG_SPEC:
            print(f"error: unknown config {c!r}; choose from {ALL_CONFIGS}",
                  file=sys.stderr)
            sys.exit(1)

    if args.drop_caches and os.geteuid() != 0:
        print("warning: --drop-caches needs root to write "
              "/proc/sys/vm/drop_caches; it will silently no-op otherwise.",
              file=sys.stderr)

    os.makedirs(args.bench_dir, exist_ok=True)
    json_out_dir = Path(args.json_out) if args.json_out else None

    if not args.skip_build:
        build_binaries(args.no_rocksdb)
    else:
        print("[skip-build] using existing eb_radix / eb_btree binaries")

    tfilter = thread_filter(threads)

    # results[config][threads] -> list of real_time_ns, one per round.
    results: dict[str, dict[int, list[float]]] = {
        c: {t: [] for t in threads} for c in configs
    }
    csv_rows: list[dict] = []

    try:
        for r in range(1, args.rounds + 1):
            for config in configs:
                data = run_one(
                    config, tfilter, args.dataset_size, args.bench_dir,
                    args.drop_caches, args.cpus, extra_flags, json_out_dir, r,
                )
                for b in data.get("benchmarks", []):
                    if b.get("run_type") != "iteration":
                        continue
                    t = parse_threads_from_name(b["name"])
                    ns = b["real_time"] * {
                        "ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9,
                    }.get(b.get("time_unit", "ns"), 1.0)
                    results[config].setdefault(t, []).append(ns)
                    csv_rows.append({
                        "run_id": run_id, "config": config, "threads": t,
                        "round": r, "dataset_size": args.dataset_size,
                        "real_time_ns": ns,
                    })
    finally:
        if os.path.isdir(args.bench_dir) and args.bench_dir.startswith(
            str(REPO_ROOT)
        ):
            shutil.rmtree(args.bench_dir, ignore_errors=True)

    if args.csv_out:
        import csv
        path = Path(args.csv_out)
        write_header = not path.exists()
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(
                f, fieldnames=["run_id", "config", "threads", "round",
                              "dataset_size", "real_time_ns"])
            if write_header:
                w.writeheader()
            w.writerows(csv_rows)
        print(f"Appended {len(csv_rows)} rows to {path}")

    print_table(results, configs, threads, args.dataset_size, args.rounds)


def fmt_ms(ns_values: list[float]) -> str:
    if not ns_values:
        return "-"
    return f"{statistics.median(ns_values) / 1e6:.1f} ms"


def print_table(
    results: dict[str, dict[int, list[float]]],
    configs: list[str],
    threads: list[int],
    dataset_size: int,
    rounds: int,
) -> None:
    print()
    print(f"Recovery, {dataset_size:,} keys, {rounds} interleaved rounds "
          f"(median of {rounds}):")
    print()
    header = ["Threads"] + configs
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join(["---:"] * len(header)) + "|")
    for t in threads:
        row = [str(t)] + [fmt_ms(results[c].get(t, [])) for c in configs]
        print("| " + " | ".join(row) + " |")

    # A radix-vs-B+ ratio row, if both a radix and a btree config were run.
    radix_keys = [c for c in configs if c.startswith("radix")]
    btree_keys = [c for c in configs if c.startswith("btree")]
    if radix_keys and btree_keys:
        print()
        print("Per-round ratio (best radix / best B+ tree; >1 means the "
              "B+ tree is faster):")
        for t in threads:
            ratios = []
            n_rounds = max(
                (len(results[c].get(t, [])) for c in configs), default=0
            )
            for i in range(n_rounds):
                r_vals = [
                    results[c][t][i] for c in radix_keys
                    if t in results[c] and i < len(results[c][t])
                ]
                b_vals = [
                    results[c][t][i] for c in btree_keys
                    if t in results[c] and i < len(results[c][t])
                ]
                if not r_vals or not b_vals:
                    continue
                ratios.append(min(r_vals) / min(b_vals))
            if ratios:
                print(f"  {t:>3d} threads: "
                      + ", ".join(f"{x:.2f}" for x in ratios)
                      + f"  (median {statistics.median(ratios):.2f})")


if __name__ == "__main__":
    main()
