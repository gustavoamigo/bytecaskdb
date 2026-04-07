#!/usr/bin/env python3
"""ByteCask vs RocksDB Benchmark Showcase

Runs engine_bench across multiple dataset sizes and writes a self-contained
Markdown performance report to the repo root.

Usage:
    python3 scripts/benchmark_showcase.py [--quick-run] [--skip-build]
    python3 scripts/benchmark_showcase.py --from-json <bench_data/dir>

    --quick-run        Small datasets for fast validation:
                       regular: 10k, 50k  |  recovery: 50k, 1M
    --skip-build       Skip the xmake release build step.
    --from-json <dir>  Regenerate the report from previously saved JSON files
                       (skips build and benchmark execution entirely).

Full mode (default):
    regular: 50k, 500k, 1M  |  recovery: 50k, 1M, 10M

Output:
    bytecask_benchmark_showcase_<YYYYMMDD_HHMMSS>.md  (repo root)
    bench_data/<timestamp>_<mode>/regular_<N>.json    (saved on each run)
    bench_data/<timestamp>_<mode>/recovery_<N>.json
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths & mode configuration
# ---------------------------------------------------------------------------

REPO_ROOT    = Path(__file__).resolve().parent.parent
BENCH_BINARY = REPO_ROOT / "build/linux/x86_64/release/engine_bench"
TMPDIR       = REPO_ROOT / ".tmp"

FULL_REGULAR_SIZES   = [50_000, 500_000, 1_000_000]
FULL_RECOVERY_SIZES  = [50_000, 1_000_000, 10_000_000]
QUICK_REGULAR_SIZES  = [10_000, 50_000]
QUICK_RECOVERY_SIZES = [50_000, 1_000_000]

REPETITIONS = 3

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build(skip: bool) -> None:
    if skip:
        print(f"[skip-build] Using existing binary: {BENCH_BINARY}")
        return
    print("Configuring release build...")
    subprocess.run(["xmake", "f", "-m", "release"], check=True, cwd=REPO_ROOT)
    print("Building engine_bench...")
    subprocess.run(["xmake", "build", "engine_bench"], check=True, cwd=REPO_ROOT)
    print("Build complete.\n")

# ---------------------------------------------------------------------------
# Hardware info
# ---------------------------------------------------------------------------

def gather_hw_info() -> str:
    script = REPO_ROOT / "scripts" / "sys-info.sh"
    result = subprocess.run(
        ["bash", str(script)],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
    )
    return result.stdout.strip()

# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def _exclude_filter(patterns: list[str]) -> str | None:
    """Build a positive-allowlist filter that skips benchmarks matching *patterns*."""
    result = subprocess.run(
        [str(BENCH_BINARY), "--benchmark_list_tests=true"],
        capture_output=True, text=True, check=True,
    )
    tests = [t.strip() for t in result.stdout.splitlines() if t.strip()]
    kept  = [t for t in tests if not any(p in t for p in patterns)]
    if not kept:
        return None
    return "|".join(re.escape(t) for t in kept)


def _run(dataset_size: int, extra_flags: list[str]) -> dict:
    run_tmp = TMPDIR / f"run_{dataset_size}_{abs(hash(tuple(extra_flags)))}"
    run_tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        out_path = f.name
    try:
        env = os.environ.copy()
        env["BC_DATASET_SIZE"] = str(dataset_size)
        env["TMPDIR"] = str(run_tmp)
        cmd = [
            str(BENCH_BINARY),
            f"--benchmark_out={out_path}",
            "--benchmark_out_format=json",
            f"--benchmark_repetitions={REPETITIONS}",
            "--benchmark_display_aggregates_only=true",
        ] + extra_flags
        print(f"  Running: BC_DATASET_SIZE={dataset_size} "
              + " ".join(cmd[1:3]) + " ...")
        subprocess.run(cmd, check=True, cwd=REPO_ROOT, env=env)
        with open(out_path, encoding="utf-8") as f:
            return json.load(f)
    finally:
        os.unlink(out_path)
        shutil.rmtree(run_tmp, ignore_errors=True)


def run_regular(dataset_size: int) -> dict:
    filt = _exclude_filter(["Recovery"])
    if filt is None:
        raise RuntimeError("All benchmarks were excluded — nothing to run.")
    return _run(dataset_size, [f"--benchmark_filter={filt}"])


def run_recovery(dataset_size: int) -> dict:
    return _run(dataset_size, ["--benchmark_filter=Recovery"])

# ---------------------------------------------------------------------------
# Result extraction
# ---------------------------------------------------------------------------

def extract_means(data: dict) -> dict[str, dict]:
    """Return {canonical_name: bench_record} for all mean aggregates."""
    out: dict[str, dict] = {}
    for bench in data.get("benchmarks", []):
        if bench.get("aggregate_name") != "mean":
            continue
        name: str = bench.get("name", "")
        # Strip Google Benchmark aggregate suffix, e.g. /real_time_mean
        base = re.sub(r"/real_time(?:_mean|_median|_stddev|_cv)$", "", name)
        base = re.sub(r"(?:_mean|_median|_stddev|_cv)$", "", base)
        out[base] = bench
    return out


def extract_min_throughput_rep(data: dict, bench_name: str) -> dict | None:
    """Return the single repetition for *bench_name* with the lowest ops_per_us.

    Non-idempotent benchmarks (e.g. Del) do real work only in the first
    repetition; later reps operate on an already-empty dataset and return
    near-instantly, making the mean counter meaningless. Using the
    min-throughput rep recovers the true cost of the operation.
    """
    run_name = bench_name + "/real_time"
    best: dict | None = None
    best_ops: float = float("inf")
    for bench in data.get("benchmarks", []):
        if bench.get("run_type") != "iteration":
            continue
        if bench.get("run_name", bench.get("name", "")) != run_name:
            continue
        ops = bench.get("ops_per_us")
        if ops is not None:
            try:
                f = float(ops)
                if f < best_ops:
                    best_ops = f
                    best = bench
            except (TypeError, ValueError):
                pass
    return best


def _val(bench: dict | None, *keys: str) -> float | None:
    if bench is None:
        return None
    for k in keys:
        v = bench.get(k)
        if v not in (None, ""):
            try:
                f = float(v)
                if f != 0.0:
                    return f
            except (TypeError, ValueError):
                continue
    return None


def time_s(bench: dict | None) -> float | None:
    """Wall-clock time in seconds for recovery benchmarks (Unit(kSecond))."""
    if bench is None:
        return None
    rt = bench.get("real_time")
    if rt is None:
        return None
    factors = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0}
    return float(rt) * factors.get(bench.get("time_unit", "s"), 1.0)


def find_any(means: dict[str, dict], *keys: str) -> dict | None:
    for k in keys:
        if k in means:
            return means[k]
    return None


def find_mt(means: dict[str, dict], prefix: str) -> dict[int, dict]:
    """Return {thread_count: bench} for MT benchmarks whose name starts with *prefix*.

    Handles both bare 'threads:N' and 'real_time/threads:N' suffixes that
    Google Benchmark emits for multi-threaded fixtures.

    Only matches names where threads:N follows the prefix directly (with at
    most a 'real_time/' segment in between). This prevents a prefix like
    'ByteCask/ReadAndWriteLoad/Sync' from accidentally matching the longer
    'ByteCask/ReadAndWriteLoad/Sync/BoundedStaleness/...' names.
    """
    result: dict[int, dict] = {}
    for name, bench in means.items():
        if not name.startswith(prefix):
            continue
        suffix = name[len(prefix):].lstrip("/")
        # Require the suffix to start directly with 'real_time/threads:N' or
        # 'threads:N' — no extra sub-path level (e.g. 'BoundedStaleness/...')
        # between the prefix and the thread count.
        if not re.match(r"(?:real_time/)?threads:\d+$", suffix):
            continue
        m = re.search(r"(?:threads:)(\d+)$", suffix)
        if m:
            result[int(m.group(1))] = bench
    return result

# ---------------------------------------------------------------------------
# Markdown formatting helpers
# ---------------------------------------------------------------------------

def fmt_throughput(v: float | None) -> str:
    """Format an ops/second value as Mops/s or Kops/s."""
    if v is None:
        return "—"
    if v >= 1_000_000:
        return f"{v / 1_000_000:.2f} Mops/s"
    if v >= 1_000:
        return f"{v / 1_000:.1f} Kops/s"
    return f"{v:.1f} ops/s"


def fmt_scans(v: float | None) -> str:
    """Format a scans/second value."""
    if v is None:
        return "—"
    if v >= 1_000_000:
        return f"{v / 1_000_000:.2f} M scans/s"
    if v >= 1_000:
        return f"{v / 1_000:.1f} K scans/s"
    return f"{v:.1f} scans/s"


def fmt_lat(ns: float | None) -> str:
    if ns is None:
        return "—"
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.2f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.2f} µs"
    return f"{ns:.0f} ns"


def fmt_time(s: float | None) -> str:
    if s is None:
        return "—"
    return f"{s:.3f} s"


def ratio(a: float | None, b: float | None) -> str:
    if a is None or b is None or b == 0.0:
        return "—"
    r = a / b
    return f"**{r:.2f}×**"


def _bold(s: str, is_best: bool) -> str:
    return f"**{s}**" if is_best else s


def size_label(n: int) -> str:
    if n >= 1_000_000:
        return f"{n // 1_000_000}M"
    if n >= 1_000:
        return f"{n // 1_000}k"
    return str(n)

# ---------------------------------------------------------------------------
# Section renderers
# ---------------------------------------------------------------------------

CRC_NOTE = (
    "> **CRC verification is disabled** for read operations in this section "
    "(Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**."
)

ST_ROWS: list[tuple[str, str, str, str]] = [
    ("Put (NoSync)",    "ByteCask/Put/NoSync",      "RocksDB/Put/NoSync",      "ops"),
    ("Put (Sync)",      "ByteCask/Put/Sync",        "RocksDB/Put/Sync",        "ops"),
    ("Get",             "ByteCask/Get",             "RocksDB/Get",             "ops"),
    ("Del (Sync)",      "ByteCask/Del/Sync",        "RocksDB/Del/Sync",        "ops"),
    ("Range-50",        "ByteCask/Range50",         "RocksDB/Range50",         "scans"),
    ("MixedBatch/Sync", "ByteCask/MixedBatch/Sync", "RocksDB/MixedBatch/Sync", "ops"),
]


def section_regular(size: int, data: dict) -> str:
    means = extract_means(data)
    L: list[str] = []
    label = size_label(size)

    L.append(f"## {label} Keys ({size:,})\n")

    # ── Single-threaded throughput ─────────────────────────────────────────
    L.append("### Single-Threaded Throughput\n")
    L.append(CRC_NOTE)
    L.append("")
    L.append("| Benchmark | ByteCask | RocksDB | ByteCask / RocksDB |")
    L.append("|---|---:|---:|:---:|")

    for row_label, bc_k, rdb_k, kind in ST_ROWS:
        # Del is non-idempotent: later reps operate on an empty DB and return
        # near-instantly, so the mean counter is misleading. Use the slowest
        # (min-throughput) repetition, which is the rep where real work ran.
        if "Del" in bc_k:
            bc_b  = extract_min_throughput_rep(data, bc_k)
            rdb_b = extract_min_throughput_rep(data, rdb_k)
        else:
            # Fallback for binaries that used "Mixed" instead of "MixedBatch"
            bc_b  = find_any(means, bc_k,  bc_k.replace("MixedBatch", "Mixed"))
            rdb_b = find_any(means, rdb_k, rdb_k.replace("MixedBatch", "Mixed"))
        if kind == "scans":
            bc_v  = _val(bc_b,  "scans_per_us")
            rdb_v = _val(rdb_b, "scans_per_us")
            bc_s  = fmt_scans(bc_v)
            rdb_s = fmt_scans(rdb_v)
        else:
            bc_v  = _val(bc_b,  "ops_per_us")
            rdb_v = _val(rdb_b, "ops_per_us")
            bc_s  = fmt_throughput(bc_v)
            rdb_s = fmt_throughput(rdb_v)
        bc_wins  = bc_v is not None and rdb_v is not None and bc_v >= rdb_v
        rdb_wins = rdb_v is not None and bc_v is not None and rdb_v > bc_v
        L.append(
            f"| {row_label} | {_bold(bc_s, bc_wins)} "
            f"| {_bold(rdb_s, rdb_wins)} | {ratio(bc_v, rdb_v)} |"
        )

    L.append("")

    # ── Get latency ────────────────────────────────────────────────────────
    bc_get  = find_any(means, "ByteCask/Get")
    rdb_get = find_any(means, "RocksDB/Get")
    if bc_get is not None or rdb_get is not None:
        L.append("### Get Latency _(CRC disabled)_\n")
        L.append("| Percentile | ByteCask | RocksDB |")
        L.append("|---|---:|---:|")
        for p, key in [("p50", "lat_p50_ns"), ("p99", "lat_p99_ns")]:
            L.append(
                f"| {p} | {fmt_lat(_val(bc_get, key))} "
                f"| {fmt_lat(_val(rdb_get, key))} |"
            )
        L.append("")

    # ── Concurrent reads (GetMT) ───────────────────────────────────────────
    bc_mt  = find_mt(means, "ByteCask/GetMT")
    rdb_mt = find_mt(means, "RocksDB/GetMT")
    if bc_mt or rdb_mt:
        L.append("### Concurrent Reads — GetMT _(CRC disabled)_\n")
        L.append("| Threads | ByteCask | RocksDB | ByteCask / RocksDB |")
        L.append("|---:|---:|---:|:---:|")
        for t in sorted(set(list(bc_mt) + list(rdb_mt))):
            bc_v  = _val(bc_mt.get(t),  "ops_per_us")
            rdb_v = _val(rdb_mt.get(t), "ops_per_us")
            bc_wins  = bc_v is not None and rdb_v is not None and bc_v >= rdb_v
            rdb_wins = rdb_v is not None and bc_v is not None and rdb_v > bc_v
            L.append(
                f"| {t} | {_bold(fmt_throughput(bc_v), bc_wins)} "
                f"| {_bold(fmt_throughput(rdb_v), rdb_wins)} | {ratio(bc_v, rdb_v)} |"
            )
        L.append("")

    # ── Concurrent writes (PutMT) ──────────────────────────────────────────
    bc_put_mt  = find_mt(means, "ByteCask/PutMT/Sync")
    rdb_put_mt = find_mt(means, "RocksDB/PutMT/Sync")
    if bc_put_mt or rdb_put_mt:
        L.append("### Concurrent Writes — PutMT/Sync\n")
        L.append("| Threads | ByteCask | RocksDB | ByteCask / RocksDB |")
        L.append("|---:|---:|---:|:---:|")
        for t in sorted(set(list(bc_put_mt) + list(rdb_put_mt))):
            bc_v  = _val(bc_put_mt.get(t),  "ops_per_us")
            rdb_v = _val(rdb_put_mt.get(t), "ops_per_us")
            bc_wins  = bc_v is not None and rdb_v is not None and bc_v >= rdb_v
            rdb_wins = rdb_v is not None and bc_v is not None and rdb_v > bc_v
            L.append(
                f"| {t} | {_bold(fmt_throughput(bc_v), bc_wins)} "
                f"| {_bold(fmt_throughput(rdb_v), rdb_wins)} | {ratio(bc_v, rdb_v)} |"
            )
        L.append("")

    # ── Read-while-writing ─────────────────────────────────────────────────
    bc_rww    = find_mt(means, "ByteCask/ReadAndWriteLoad/Sync")
    rdb_rww   = find_mt(means, "RocksDB/ReadAndWriteLoad/Sync")
    bc_rww_bs = find_mt(means, "ByteCask/ReadAndWriteLoad/Sync/BoundedStaleness")
    if bc_rww or rdb_rww:
        L.append(
            "### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_\n"
        )
        L.append(
            "> **BoundedStaleness** is a ByteCask read mode where readers observe "
            "the keydir snapshot from the previous completed write batch instead of "
            "acquiring a per-read epoch lock. This eliminates reader-writer "
            "contention at high thread counts at the cost of seeing writes that are "
            "at most one batch behind.\n"
        )
        L.append(
            "| Readers | ByteCask | ByteCask BoundedStaleness | RocksDB |"
        )
        L.append("|---:|---:|---:|---:|")
        for t in sorted(set(list(bc_rww) + list(rdb_rww))):
            bc_v  = _val(bc_rww.get(t),    "ops_per_us")
            bc_bs = _val(bc_rww_bs.get(t), "ops_per_us")
            rdb_v = _val(rdb_rww.get(t),   "ops_per_us")
            best  = max(v for v in [bc_v, bc_bs, rdb_v] if v is not None)
            L.append(
                f"| {t} | {_bold(fmt_throughput(bc_v),  bc_v  == best)} "
                f"| {_bold(fmt_throughput(bc_bs), bc_bs == best)} "
                f"| {_bold(fmt_throughput(rdb_v), rdb_v == best)} |"
            )
        L.append("")

    return "\n".join(L)


def section_scalability_table(regular: dict[int, dict]) -> str:
    """Tables comparing GetMT throughput and p99 latency across dataset sizes."""
    sizes   = sorted(regular)
    labels  = [size_label(s) for s in sizes]
    means_s = [extract_means(regular[s]) for s in sizes]

    bc_mt_s  = [find_mt(m, "ByteCask/GetMT") for m in means_s]
    rdb_mt_s = [find_mt(m, "RocksDB/GetMT")  for m in means_s]
    threads  = sorted({t for bc_m in bc_mt_s for t in bc_m} |
                      {t for rdb_m in rdb_mt_s for t in rdb_m})

    L: list[str] = []
    L.append("## GetMT Scalability — Throughput and Latency vs Dataset Size\n")
    L.append(
        "> Throughput (Mops/s) and p99 read latency at each thread count as the "
        "dataset grows. ByteCask's in-memory keydir keeps read latency flat; "
        "RocksDB's block cache hit rate falls as the working set exceeds the cache.\n"
    )

    header = "| Threads | " + " | ".join(
        f"BC {lbl} | RDB {lbl}" for lbl in labels
    ) + " |"
    sep = "|---:| " + " | ".join("---: | ---:" for _ in labels) + " |"

    # ── throughput ────────────────────────────────────────────────────────
    L.append("### Throughput (Mops/s)\n")
    L.append(header)
    L.append(sep)
    for t in threads:
        cells: list[str] = [str(t)]
        for bc_m, rdb_m in zip(bc_mt_s, rdb_mt_s):
            bc_v  = _val(bc_m.get(t),  "ops_per_us")
            rdb_v = _val(rdb_m.get(t), "ops_per_us")
            bc_wins  = bc_v is not None and rdb_v is not None and bc_v >= rdb_v
            rdb_wins = rdb_v is not None and bc_v is not None and rdb_v > bc_v
            cells.append(_bold(fmt_throughput(bc_v),  bc_wins))
            cells.append(_bold(fmt_throughput(rdb_v), rdb_wins))
        L.append("| " + " | ".join(cells) + " |")
    L.append("")

    # ── p99 latency ───────────────────────────────────────────────────────
    L.append("### p99 Read Latency\n")
    L.append(header)
    L.append(sep)
    for t in threads:
        cells = [str(t)]
        for bc_m, rdb_m in zip(bc_mt_s, rdb_mt_s):
            bc_v  = _val(bc_m.get(t),  "lat_p99_ns")
            rdb_v = _val(rdb_m.get(t), "lat_p99_ns")
            # lower latency is better
            bc_wins  = bc_v is not None and rdb_v is not None and bc_v <= rdb_v
            rdb_wins = rdb_v is not None and bc_v is not None and rdb_v < bc_v
            cells.append(_bold(fmt_lat(bc_v),  bc_wins))
            cells.append(_bold(fmt_lat(rdb_v), rdb_wins))
        L.append("| " + " | ".join(cells) + " |")
    L.append("")

    return "\n".join(L)


def section_recovery(recovery: dict[int, dict]) -> str:
    L: list[str] = []
    L.append("## Parallel Recovery\n")
    L.append(
        "> ✅ **CRC verification is enabled** during recovery. "
        "Times reflect full disk I/O and CRC validation across all data files.\n"
    )

    for size in sorted(recovery):
        data  = recovery[size]
        means = extract_means(data)
        L.append(f"### {size_label(size)} Keys ({size:,})\n")
        L.append("| Threads | Recovery Time (mean) |")
        L.append("|---:|---:|")
        for threads in [1, 2, 4, 8, 16]:
            bench = find_any(
                means,
                f"ByteCask/Recovery/threads:{threads}",
                f"ByteCask/Recovery/{threads}",
                f"ByteCask/Recovery/Parallel/{threads}",
            )
            L.append(f"| {threads} | {fmt_time(time_s(bench))} |")
        L.append("")

    return "\n".join(L)

# ---------------------------------------------------------------------------
# Full report
# ---------------------------------------------------------------------------

def render_report(
    hw_info: str,
    bm_context: dict,
    regular: dict[int, dict],
    recovery: dict[int, dict],
    mode: str,
    commit: str,
) -> str:
    now      = datetime.now()
    date_str = now.strftime("%B %d, %Y  %H:%M:%S")
    date_iso = now.strftime("%Y-%m-%d")

    host = bm_context.get("host_name", "unknown")
    cpus = bm_context.get("num_cpus", "?")
    mhz  = bm_context.get("mhz_per_cpu", "?")

    L: list[str] = []

    # ── Title & meta ──────────────────────────────────────────────────────
    L.append("# ByteCask Benchmark Showcase\n")
    L.append("| | |")
    L.append("|---|---|")
    L.append(f"| **Date** | {date_str} |")
    L.append(f"| **Host** | `{host}` |")
    L.append(f"| **CPUs** | {cpus} × {mhz} MHz |")
    L.append(f"| **Git commit** | `{commit}` |")
    L.append(f"| **Mode** | {mode} |")
    L.append("")

    # ── Hardware ──────────────────────────────────────────────────────────
    L.append("## Hardware\n")
    L.append("```")
    L.append(hw_info)
    L.append("```")
    L.append("")

    # ── Methodology ───────────────────────────────────────────────────────
    L.append("## Methodology\n")
    L.append(f"- **Repetitions:** {REPETITIONS} runs per benchmark; mean reported.")
    L.append(
        "- **Value size:** 245 bytes of random, incompressible data per entry."
    )
    L.append(
        "- **Key shape:** UUIDv7-like with 5 prefixes — "
        "`user::`, `order::`, `session::`, `invoice::`, `product::`."
    )
    L.append(
        "- **CRC on reads:** Disabled for Get, Range, and MixedBatch benchmarks. "
        "**Enabled** for recovery benchmarks — recovery validates every byte on disk."
    )
    L.append(
        "- **NoSync:** writes are flushed to the OS page cache; "
        "no `fdatasync` call is made."
    )
    L.append(
        "- **Sync:** every write calls `fdatasync` before returning."
    )
    L.append(
        "- Throughput is expressed as ops/second "
        "(Mops/s = millions of ops/second, Kops/s = thousands). "
        "Wall-clock time is used (`UseRealTime`)."
    )
    L.append(
        "- Each engine is opened in a fresh, empty temporary directory "
        "per benchmark fixture."
    )
    L.append("")

    # ── Throughput sections ────────────────────────────────────────────────
    L.append("---\n")
    L.append("# Throughput Comparison — ByteCask vs RocksDB\n")
    for size in sorted(regular):
        L.append(section_regular(size, regular[size]))
        L.append("---")

    # ── Scalability table ─────────────────────────────────────────────────
    L.append("")
    L.append("# Scalability\n")
    L.append(section_scalability_table(regular))

    # ── Recovery ──────────────────────────────────────────────────────────
    L.append("")
    L.append("# Recovery\n")
    L.append(section_recovery(recovery))

    L.append("---")
    L.append(f"_Generated by `scripts/benchmark_showcase.py` · {date_iso}_")
    return "\n".join(L)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def save_json(data: dict, path: Path) -> None:
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def load_from_json(json_dir: Path) -> tuple[dict[int, dict], dict[int, dict]]:
    """Reconstruct regular and recovery dicts from a previously saved JSON directory."""
    regular: dict[int, dict] = {}
    recovery: dict[int, dict] = {}
    for path in sorted(json_dir.glob("regular_*.json")):
        size = int(path.stem.split("_", 1)[1])
        with path.open(encoding="utf-8") as f:
            regular[size] = json.load(f)
    for path in sorted(json_dir.glob("recovery_*.json")):
        size = int(path.stem.split("_", 1)[1])
        with path.open(encoding="utf-8") as f:
            recovery[size] = json.load(f)
    if not regular and not recovery:
        raise SystemExit(
            f"No regular_*.json or recovery_*.json found in {json_dir}"
        )
    return regular, recovery


def git_commit() -> str:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True, cwd=REPO_ROOT,
        )
        return r.stdout.strip()
    except subprocess.CalledProcessError:
        return "unknown"

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    argv = sys.argv[1:]
    flags = set(argv)

    # Parse --from-json <dir>
    from_json_dir: Path | None = None
    for i, a in enumerate(argv):
        if a == "--from-json" and i + 1 < len(argv):
            from_json_dir = Path(argv[i + 1]).resolve()
            break

    quick    = "--quick-run"  in flags
    skip_bld = "--skip-build" in flags or from_json_dir is not None

    regular_sizes  = QUICK_REGULAR_SIZES  if quick else FULL_REGULAR_SIZES
    recovery_sizes = QUICK_RECOVERY_SIZES if quick else FULL_RECOVERY_SIZES
    mode = "Quick" if quick else "Full"

    print(f"\n=== ByteCask Benchmark Showcase [{mode}] ===\n")
    TMPDIR.mkdir(exist_ok=True)

    try:
        build(skip_bld)
        commit = git_commit()

        print("=== Gathering hardware information ===")
        hw = gather_hw_info()
        print(hw)
        print()

        if from_json_dir is not None:
            # ── Reload from saved JSONs ────────────────────────────────────
            print(f"=== Loading results from: {from_json_dir} ===")
            regular, recovery = load_from_json(from_json_dir)
            # Infer mode label and sizes from loaded data.
            regular_sizes  = sorted(regular)
            recovery_sizes = sorted(recovery)
            max_regular = max(regular_sizes) if regular_sizes else 0
            mode = "Quick" if max_regular <= 50_000 else "Full"
        else:
            # ── Run benchmarks and save JSONs ──────────────────────────────
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            json_save_dir = REPO_ROOT / "bench_data" / f"{ts}_{mode.lower()}"
            json_save_dir.mkdir(parents=True, exist_ok=True)

            regular: dict[int, dict] = {}
            sizes_str = ", ".join(size_label(s) for s in regular_sizes)
            print(f"=== Regular benchmarks ({sizes_str} keys) ===")
            for i, size in enumerate(regular_sizes, 1):
                print(f"\n[{i}/{len(regular_sizes)}] {size_label(size)} keys...")
                regular[size] = run_regular(size)
                save_json(regular[size], json_save_dir / f"regular_{size}.json")

            recovery: dict[int, dict] = {}
            sizes_str = ", ".join(size_label(s) for s in recovery_sizes)
            print(f"\n=== Recovery benchmarks ({sizes_str} keys) ===")
            for i, size in enumerate(recovery_sizes, 1):
                print(f"\n[{i}/{len(recovery_sizes)}] {size_label(size)} keys (recovery)...")
                recovery[size] = run_recovery(size)
                save_json(recovery[size], json_save_dir / f"recovery_{size}.json")

            print(f"\n=== JSON data saved to: {json_save_dir.relative_to(REPO_ROOT)} ===")

        # Pull benchmark context (CPU info etc.) from the first result.
        bm_context = next(iter(regular.values()), {}).get("context", {})

        report = render_report(hw, bm_context, regular, recovery, mode, commit)

        ts  = datetime.now().strftime("%Y%m%d_%H%M%S")
        out = REPO_ROOT / f"bytecask_benchmark_showcase_{ts}.md"
        out.write_text(report, encoding="utf-8")
        print(f"\n=== Report written to: {out.name} ===")

    finally:
        shutil.rmtree(TMPDIR, ignore_errors=True)


if __name__ == "__main__":
    main()
