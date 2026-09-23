#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Cut a whole-server pool trace into one trace per sysbench cell (issue #148).

run-memory-pressure.sh, run with BYTECASK_POOL_TRACE=FILE, writes each cell's
warm-up, measure and end times to FILE.markers.csv. This writes
FILE.<workload>-<threads>.bin holding the records from that cell's warm-up to
its end, and prints the --count-from-ns that makes the simulators count only
the measured part — the part the harness's pool_hit_ratio covers.
"""

import csv
import sys

import numpy as np

REC = np.dtype([("ts", "<u8"), ("file", "<u4"), ("frame", "<u4"), ("n", "<u4"),
                ("off", "<u2"), ("kind", "u1"), ("pad", "u1")])


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = sys.argv[1]
    cells: dict[tuple[str, str], dict[str, int]] = {}
    with open(path + ".markers.csv") as f:
        for label, wl, threads, phase, ns in csv.reader(f):
            cells.setdefault((wl, threads), {})[phase] = int(ns)
    recs = np.fromfile(path, dtype=REC)
    recs = recs[np.argsort(recs["ts"], kind="stable")]
    for (wl, threads), m in cells.items():
        lo, hi = np.searchsorted(recs["ts"], [m["warmup"], m["end"]])
        out = f"{path}.{wl}-{threads}.bin"
        recs[lo:hi].tofile(out)
        print(f"{out}: {hi - lo:,} records, --count-from-ns {m['measure']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
