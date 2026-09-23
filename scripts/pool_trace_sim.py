#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Replay a buffer pool trace through libCacheSim's policies (issue #148).

Reads a BYTECASK_POOL_TRACE file (24-byte records, see PoolTraceRecord in
bytecaskdb/buffer_pool.cppm), orders it by timestamp, expands every demand
read to one request per 4 KiB frame, and runs each policy at each cache size,
Belady (OPT) included. Sizes are fractions of the trace's footprint — the
distinct frames it reads — so they mean the same thing on every trace.

libCacheSim models neither block prefetch nor an unevictable active file;
benchmarks/pool_trace_sim.cpp does, and --pool-sim runs it on the same trace
and sizes so both tables line up.

Needs `pip install libcachesim numpy`. Counting starts after --warmup of the
reads, or at --count-from-ns (a trace timestamp; pool_trace_split.py prints
the one for each sysbench cell): every policy is run over the warm-up prefix alone as well, and its
misses subtracted, which is exact because a cache's state after N requests
does not depend on what follows them.
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

REC = np.dtype([("ts", "<u8"), ("file", "<u4"), ("frame", "<u4"), ("n", "<u4"),
                ("off", "<u2"), ("kind", "u1"), ("pad", "u1")])
ORACLE = np.dtype([("t", "<u4"), ("id", "<u8"), ("sz", "<u4"), ("next", "<i8")])
assert REC.itemsize == 24 and ORACLE.itemsize == 24
K_ACTIVE, K_APPEND, K_LEND = 1, 2, 4
FRAME = 4096

POLICIES = ["LRU", "Clock", "GClock2", "S3FIFO", "Sieve", "WTinyLFU", "ARC",
            "TwoQ", "LIRS", "Belady"]


def load(path: str, reads_only: bool) -> np.ndarray:
    recs = np.fromfile(path, dtype=REC)
    recs = recs[np.argsort(recs["ts"], kind="stable")]
    if reads_only:
        recs = recs[(recs["kind"] & (K_ACTIVE | K_APPEND)) == 0]
    else:
        recs = recs[(recs["kind"] & K_APPEND) == 0]
    return recs


def expand(recs: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """One object id and timestamp per frame read, in order."""
    n = recs["n"].astype(np.int64)
    starts = np.repeat(recs["frame"].astype(np.uint64), n)
    within = np.arange(n.sum()) - np.repeat(np.cumsum(n) - n, n)
    frames = starts + within.astype(np.uint64)
    ids = (np.repeat(recs["file"].astype(np.uint64), n) << np.uint64(32)) | frames
    ts = np.repeat(recs["ts"], n)
    return ids, ts


def write_oracle(ids: np.ndarray, ts: np.ndarray, path: str) -> None:
    out = np.empty(len(ids), dtype=ORACLE)
    out["t"] = ((ts - ts[0]) // 1_000_000_000).astype(np.uint32) + 1
    out["id"] = ids
    out["sz"] = FRAME
    # Belady needs each request's next access; -1 marks "never again".
    order = np.lexsort((np.arange(len(ids)), ids))
    nxt = np.full(len(ids), -1, dtype=np.int64)
    same = ids[order[1:]] == ids[order[:-1]]
    nxt[order[:-1][same]] = order[1:][same]
    out["next"] = nxt
    out.tofile(path)


def make_cache(lcs, name: str, size_bytes: int):
    if name == "GClock2":
        return lcs.Clock(size_bytes, n_bit_counter=2)
    return getattr(lcs, name)(size_bytes)


def miss_count(lcs, name, size_bytes, path, max_req) -> float:
    reader = lcs.TraceReader(path, lcs.TraceType.ORACLE_GENERAL_TRACE)
    obj_miss, _ = make_cache(lcs, name, size_bytes).process_trace(reader, 0, max_req)
    return obj_miss * max_req


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--label", default=None, help="trace name in the CSV")
    ap.add_argument("--sizes", default="0.02,0.05,0.1,0.25,0.5",
                    help="cache sizes as fractions of the footprint")
    ap.add_argument("--policies", default=",".join(POLICIES))
    ap.add_argument("--warmup", type=float, default=0.1,
                    help="fraction of reads before counting starts")
    ap.add_argument("--count-from-ns", type=int, default=0,
                    help="count reads from this trace timestamp on (overrides --warmup)")
    ap.add_argument("--reads-only", action="store_true",
                    help="drop active-file reads too (appends are always dropped here)")
    ap.add_argument("--pool-sim", default=None,
                    help="path to pool_trace_sim; also run its CLOCK/fill variants")
    ap.add_argument("--csv", default=None, help="append rows to this CSV")
    args = ap.parse_args()

    import libcachesim as lcs

    label = args.label or os.path.basename(args.trace)
    recs = load(args.trace, args.reads_only)
    ids, ts = expand(recs)
    n = len(ids)
    footprint = len(np.unique(ids))
    warm_reads = (int(np.searchsorted(recs["ts"], args.count_from_ns))
                  if args.count_from_ns else int(len(recs) * args.warmup))
    warm = int(recs["n"][:warm_reads].astype(np.int64).sum())
    kinds = recs["kind"]
    print(f"{label}: {len(recs):,} reads ({(kinds & K_LEND != 0).mean():.0%} iterator, "
          f"{(kinds & K_ACTIVE != 0).mean():.0%} active file), {n:,} frame requests, "
          f"footprint {footprint:,} frames ({footprint * FRAME / 2**20:.0f} MiB), "
          f"counting after {warm:,}", file=sys.stderr)

    rows = []
    sizes = [float(s) for s in args.sizes.split(",")]
    with tempfile.TemporaryDirectory() as tmp:
        oracle = os.path.join(tmp, "trace.oracleGeneral.bin")
        write_oracle(ids, ts, oracle)
        for frac in sizes:
            frames = max(1, int(footprint * frac))
            for name in args.policies.split(","):
                t0 = time.time()
                misses = miss_count(lcs, name, frames * FRAME, oracle, n)
                if warm > 0:
                    misses -= miss_count(lcs, name, frames * FRAME, oracle, warm)
                hit = 1.0 - misses / (n - warm)
                rows.append({"trace": label, "sim": "libcachesim", "policy": name,
                             "size_frac": frac, "frames": frames,
                             "frame_hit_ratio": round(hit, 4)})
                print(f"  {frac:>5} {name:<9} hit {hit:.4f}  ({time.time() - t0:.1f}s)",
                      file=sys.stderr)

    if args.pool_sim:
        cmd = [args.pool_sim, "--trace", args.trace,
               *(["--count-from-ns", str(args.count_from_ns)] if args.count_from_ns
                 else ["--warmup", str(args.warmup)]),
               "--frames", ",".join(str(max(1, int(footprint * f))) for f in sizes),
               "--admit-ref", "1,0"]
        if args.reads_only:
            cmd.append("--reads-only")
        out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
        by_frames = {max(1, int(footprint * f)): f for f in sizes}
        for r in csv.DictReader(out.splitlines()):
            rows.append({"trace": label, "sim": "pool_trace_sim",
                         "policy": f"{r['policy']}_clock/ref{r['admit_ref']}/{r['fill']}",
                         "size_frac": by_frames[int(r["frames"])],
                         "frames": int(r["frames"]),
                         "frame_hit_ratio": float(r["frame_hit_ratio"]),
                         "read_hit_ratio": float(r["read_hit_ratio"]),
                         "device_reads": int(r["device_reads"]),
                         "device_frames": int(r["device_frames"])})

    fields = ["trace", "sim", "policy", "size_frac", "frames", "frame_hit_ratio",
              "read_hit_ratio", "device_reads", "device_frames"]
    if args.csv:
        new = not os.path.exists(args.csv)
        with open(args.csv, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            if new:
                w.writeheader()
            w.writerows(rows)

    # Table: policies down, sizes across.
    names = list(dict.fromkeys(r["policy"] for r in rows))
    print(f"\n{label} — frame hit ratio, cache size as a fraction of the footprint "
          f"({footprint * FRAME / 2**20:.0f} MiB)")
    print(f"{'policy':<44}" + "".join(f"{s:>8}" for s in sizes))
    for name in names:
        cells = {r["size_frac"]: r["frame_hit_ratio"] for r in rows if r["policy"] == name}
        print(f"{name:<44}" + "".join(f"{cells.get(s, float('nan')):>8.3f}" for s in sizes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
