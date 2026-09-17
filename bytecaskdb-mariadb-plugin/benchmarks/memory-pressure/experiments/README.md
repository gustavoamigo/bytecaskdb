# Experiment drivers

Each script runs a set of arms through `../run-memory-pressure.sh`, every arm
from a byte-identical snapshot of one prepared dataset (the write workloads
mutate it), and writes its CSVs and logs to
`<DATA_ROOT>/experiments/<name>-<timestamp>/`. `lib.sh` holds the shared
parts. Requirements are those of the main script, plus a swap device for the
limited arms.

```bash
# Put the data on the disk you mean to test; snapshots are kept there too.
export DATA_ROOT=/mnt/data/memory-pressure

# §2 of FINDINGS.md: two branches, unpressured and under 1 GiB + swap.
# Both branches must carry this benchmark (cherry-pick if not).
./experiments/tree-comparison.sh single-wait-epoch-buffer-pool refcount-bench
SECONDARY=on ROWS=10000000 ./experiments/tree-comparison.sh <A> <B>   # random keys

# §3: recovery thread counts under the limit (startup is what is measured).
SECONDARY=on ROWS=10000000 ./experiments/recovery-threads.sh

# §4: buffer pool vs mmap vs pread.
./experiments/backend-comparison.sh

# §5: InnoDB buffer pool sweep inside the limit, plus its ceiling.
./experiments/innodb-sizing.sh
```

Defaults: 20 M rows, no secondary index, 8 threads, 15 s warm-up, 30 s
measured, limit 1 GiB with 2 GiB swap, 256 MiB ByteCaskDB pool. Override with
`ROWS`, `SECONDARY`, `THREADS`, `WARMUP`, `TIME`, `WORKLOADS` in the
environment and the positional limit/pool arguments. Budget roughly:
prepare 3–6 min per dataset shape; an unpressured arm ~3 min; a limited arm
~3 min plus recovery, which is seconds with sequential keys and up to ten
minutes with random ones.

`data/2026-09-17-results.csv` is every run behind FINDINGS.md, one row per
cell, tagged by experiment and tree.
