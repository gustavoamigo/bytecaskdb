#!/usr/bin/env python3
from __future__ import annotations
"""Compare radix tree vs B+ tree across the full engine_bench suite —
Get, Range50, Put (Sync/NoSync), Del, MixedBatch, and the multi-threaded
Get/Put variants — interleaved round by round so both trees share machine
state. Sibling to compare_recovery.py, which covers Recovery only.

Usage:
    python3 scripts/compare_engine_bench.py [options] [-- <extra flags>]

    --dataset-size N     Key count (default 1000000).
    --rounds N            Interleaved rounds (default 3).
    --suite LIST           Comma-separated benchmark keys to run (default:
                            all — see BENCHMARKS below for the full list
                            and --list to print it).
    --list                  Print the available benchmark keys and exit.
    --bench-dir DIR         Where the test DB is written (default: ./.tmp).
    --cpus RANGE            taskset CPU range, e.g. "0-3" (default: unpinned).
    --skip-build             Skip building; use existing eb_radix/eb_btree
                              binaries in build/linux/x86_64/release/.
    --no-rocksdb             Build with BYTECASK_NO_ROCKSDB=1.
    --json-out DIR            Save each run's raw benchmark JSON into DIR.
    --csv-out FILE             Append a flat CSV of every reading to FILE.
                                Each invocation stamps its rows with a
                                run_id (UTC timestamp): round numbers
                                reset to 1 on every run, so two runs
                                appended to the same file collide on round
                                alone — group by (run_id, round) if
                                aggregating across multiple invocations.
    Extra flags after '--' are forwarded to the benchmark binary
    (e.g. -- --benchmark_min_time=5x).

Known gaps in what engine_bench registers, so the requested "Del NoSync"
and "PutMT NoSync" are not run here: there is no BM_Del<..., false> or
BM_PutMT<..., false> registration in benchmarks/engine_bench.cpp, only
Del/Sync, PutMT/Sync and PutMT/PeriodicSync (group-commit amortized sync,
the closest thing to a NoSync multi-writer benchmark). Add the missing
registrations in engine_bench.cpp first if those numbers matter — this
script only runs what is already registered, matching benchmark names
verified against --benchmark_list_tests rather than guessed.

The ratio column is B+ tree / radix tree (matching the convention already
used when this comparison was reported in chat) — below 1.0 means the B+
tree is faster, above 1.0 means the radix tree is faster. This is the
INVERSE of the ratio convention in compare_recovery.py (radix / B+, where
above 1.0 means the B+ tree is faster) — that script matched an existing
report already phrased "radix / B+ tree"; this one matches this one.
Read the column header, not your memory of the other script.
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

# key -> (friendly label, --benchmark_filter regex fragment, multithreaded)
# Regexes are anchored on the exact names engine_bench.cpp registers, as
# printed by --benchmark_list_tests — not guessed from the source macros.
BENCHMARKS: dict[str, tuple[str, str, bool]] = {
    "get":        ("Get",              r"ByteCaskDB/Get/real_time$",               False),
    "range50":    ("Range50",          r"ByteCaskDB/Range50/real_time$",           False),
    "put-nosync": ("Put/NoSync",       r"ByteCaskDB/Put/NoSync/iterations:[0-9]+/real_time$", False),
    "put-sync":   ("Put/Sync",         r"ByteCaskDB/Put/Sync/real_time$",          False),
    "del-sync":   ("Del/Sync",         r"ByteCaskDB/Del/Sync/real_time$",          False),
    "mixedbatch": ("MixedBatch/Sync",  r"ByteCaskDB/MixedBatch/Sync/real_time$",   False),
    "getmt":      ("GetMT",            r"ByteCaskDB/GetMT/real_time/threads:[0-9]+$", True),
    "putmt-sync": ("PutMT/Sync",       r"ByteCaskDB/PutMT/Sync/real_time/threads:[0-9]+$", True),
    "putmt-periodicsync": ("PutMT/PeriodicSync",
                            r"ByteCaskDB/PutMT/PeriodicSync/real_time/threads:[0-9]+$", True),
}

CONFIGS = ["radix", "btree"]
_TIME_UNIT_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


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
    env_base = os.environ.copy()
    if no_rocksdb:
        env_base["BYTECASK_NO_ROCKSDB"] = "1"
    config_flags = clang_config_flags()

    env = env_base.copy()
    env.pop("BYTECASK_KEYDIR", None)
    run(["xmake", "f", "-m", "release", "--sanitizer="] + config_flags, env=env)
    run(["xmake", "build", BENCH_TARGET], env=env)
    shutil.copy(BUILD_DIR / "engine_bench", BUILD_DIR / "eb_radix")

    env = env_base.copy()
    env["BYTECASK_KEYDIR"] = "btree"
    run(["xmake", "f", "-m", "release", "--sanitizer="] + config_flags, env=env)
    run(["xmake", "build", BENCH_TARGET], env=env)
    shutil.copy(BUILD_DIR / "engine_bench", BUILD_DIR / "eb_btree")

    for name in ("eb_radix", "eb_btree"):
        if not (BUILD_DIR / name).exists():
            print(f"error: {name} was not built", file=sys.stderr)
            sys.exit(1)


def combined_filter(keys: list[str]) -> str:
    parts = [BENCHMARKS[k][1] for k in keys]
    return "^ByteCaskDB/(" + "|".join(p.removeprefix("ByteCaskDB/") for p in parts) + ")"


def run_one(
    config: str,
    keys: list[str],
    dataset_size: int,
    bench_dir: str,
    cpus: str | None,
    extra_flags: list[str],
    json_out_dir: Path | None,
    round_num: int,
) -> dict:
    binary = BUILD_DIR / ("eb_radix" if config == "radix" else "eb_btree")
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name

    cmd: list[str] = []
    if cpus:
        cmd += ["taskset", "-c", cpus]
    cmd += [
        str(binary),
        f"--benchmark_filter={combined_filter(keys)}",
        f"--benchmark_out={out_path}",
        "--benchmark_out_format=json",
    ]
    if not any(f.startswith("--benchmark_min_time") for f in extra_flags):
        cmd.append("--benchmark_min_time=3x")
    cmd += extra_flags

    env = os.environ.copy()
    env["BC_DATASET_SIZE"] = str(dataset_size)
    env["BC_BENCH_DIR"] = bench_dir

    print(f"round {round_num} {config}: " + " ".join(cmd))
    proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env,
                          capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0 or "Could not compile benchmark re" in proc.stderr:
        os.unlink(out_path) if os.path.exists(out_path) else None
        print(f"error: {binary.name} failed on filter "
              f"{combined_filter(keys)!r} (exit {proc.returncode})",
              file=sys.stderr)
        sys.exit(1)
    with open(out_path, encoding="utf-8") as f:
        data = json.load(f)
    os.unlink(out_path)

    if json_out_dir:
        json_out_dir.mkdir(parents=True, exist_ok=True)
        (json_out_dir / f"{config}_r{round_num}.json").write_text(json.dumps(data))

    return data


def classify(name: str, keys: list[str]) -> tuple[str, int] | None:
    """Matches a benchmark result name back to (key, thread_count). thread_count
    is 0 for a single-threaded benchmark."""
    import re
    for k in keys:
        label, pattern, mt = BENCHMARKS[k]
        m = re.match(pattern, name)
        if not m:
            continue
        if mt:
            tm = re.search(r"threads:(\d+)", name)
            return k, int(tm.group(1)) if tm else 0
        return k, 0
    return None


def fmt_time(ns: float) -> str:
    if ns < 1e3:
        return f"{ns:.0f} ns"
    if ns < 1e6:
        return f"{ns / 1e3:.0f} us"
    if ns < 1e9:
        return f"{ns / 1e6:.0f} ms" if ns >= 1e7 else f"{ns / 1e6:.1f} ms"
    return f"{ns / 1e9:.2f} s"


def main() -> None:
    run_id = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    p = argparse.ArgumentParser(
        description="Compare radix vs B+ tree across the engine_bench suite.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--dataset-size", type=int, default=1_000_000)
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--suite", default=",".join(BENCHMARKS.keys()))
    p.add_argument("--list", action="store_true")
    p.add_argument("--bench-dir", default=str(REPO_ROOT / ".tmp"))
    p.add_argument("--cpus", default=None)
    p.add_argument("--skip-build", action="store_true")
    p.add_argument("--no-rocksdb", action="store_true")
    p.add_argument("--json-out", default=None)
    p.add_argument("--csv-out", default=None)
    p.add_argument("extra", nargs=argparse.REMAINDER)
    args = p.parse_args()

    if args.list:
        print(f"{'key':<22}{'label':<20}multithreaded")
        for k, (label, _, mt) in BENCHMARKS.items():
            print(f"{k:<22}{label:<20}{mt}")
        return

    extra_flags = args.extra
    if extra_flags and extra_flags[0] == "--":
        extra_flags = extra_flags[1:]

    keys = [k for k in args.suite.split(",") if k]
    for k in keys:
        if k not in BENCHMARKS:
            print(f"error: unknown benchmark key {k!r}; --list to see valid keys",
                  file=sys.stderr)
            sys.exit(1)

    os.makedirs(args.bench_dir, exist_ok=True)
    json_out_dir = Path(args.json_out) if args.json_out else None

    if not args.skip_build:
        build_binaries(args.no_rocksdb)
    else:
        print("[skip-build] using existing eb_radix / eb_btree binaries")

    # results[key][config][threads] -> list of real_time_ns.
    results: dict[str, dict[str, dict[int, list[float]]]] = {
        k: {c: {} for c in CONFIGS} for k in keys
    }
    csv_rows: list[dict] = []

    try:
        for r in range(1, args.rounds + 1):
            for config in CONFIGS:
                data = run_one(
                    config, keys, args.dataset_size, args.bench_dir,
                    args.cpus, extra_flags, json_out_dir, r,
                )
                for b in data.get("benchmarks", []):
                    if b.get("run_type") != "iteration":
                        continue
                    hit = classify(b["name"], keys)
                    if hit is None:
                        continue
                    key, threads = hit
                    ns = b["real_time"] * _TIME_UNIT_TO_NS.get(
                        b.get("time_unit", "ns"), 1.0)
                    results[key][config].setdefault(threads, []).append(ns)
                    csv_rows.append({
                        "run_id": run_id, "benchmark": key, "config": config,
                        "threads": threads, "round": r,
                        "dataset_size": args.dataset_size, "real_time_ns": ns,
                    })
    finally:
        if os.path.isdir(args.bench_dir) and args.bench_dir.startswith(str(REPO_ROOT)):
            shutil.rmtree(args.bench_dir, ignore_errors=True)

    if args.csv_out:
        import csv
        path = Path(args.csv_out)
        write_header = not path.exists()
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=[
                "run_id", "benchmark", "config", "threads", "round",
                "dataset_size", "real_time_ns"])
            if write_header:
                w.writeheader()
            w.writerows(csv_rows)
        print(f"Appended {len(csv_rows)} rows to {path}")

    print_tables(results, keys, args.dataset_size, args.rounds)


def print_tables(
    results: dict[str, dict[str, dict[int, list[float]]]],
    keys: list[str],
    dataset_size: int,
    rounds: int,
) -> None:
    print()
    print(f"engine_bench, {dataset_size:,} keys, {rounds} interleaved rounds")
    print("(ratio = B+ tree / radix tree; below 1.0 = B+ tree faster)")

    single = [k for k in keys if not BENCHMARKS[k][2]]
    multi = [k for k in keys if BENCHMARKS[k][2]]

    if single:
        print()
        print("| Benchmark | radix | B+ tree | ratio |")
        print("|---|---:|---:|---:|")
        for k in single:
            label = BENCHMARKS[k][0]
            r_vals = results[k]["radix"].get(0, [])
            b_vals = results[k]["btree"].get(0, [])
            if not r_vals or not b_vals:
                print(f"| {label} | - | - | - |")
                continue
            r_med = statistics.median(r_vals)
            b_med = statistics.median(b_vals)
            print(f"| {label} | {fmt_time(r_med)} | {fmt_time(b_med)} | "
                  f"{b_med / r_med:.2f} |")

    for k in multi:
        label = BENCHMARKS[k][0]
        r_threads = sorted(results[k]["radix"].keys())
        b_threads = sorted(results[k]["btree"].keys())
        all_threads = sorted(set(r_threads) | set(b_threads))
        if not all_threads:
            continue
        print()
        print(f"### {label}")
        print()
        print("| Threads | radix | B+ tree | ratio |")
        print("|---:|---:|---:|---:|")
        for t in all_threads:
            r_vals = results[k]["radix"].get(t, [])
            b_vals = results[k]["btree"].get(t, [])
            if not r_vals or not b_vals:
                print(f"| {t} | - | - | - |")
                continue
            r_med = statistics.median(r_vals)
            b_med = statistics.median(b_vals)
            print(f"| {t} | {fmt_time(r_med)} | {fmt_time(b_med)} | "
                  f"{b_med / r_med:.2f} |")


if __name__ == "__main__":
    main()
