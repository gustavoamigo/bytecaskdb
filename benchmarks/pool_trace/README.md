# Buffer pool trace replay (issue #148)

Measurement tooling, kept on this branch and not merged: it records what the
buffer pool is asked for and replays that stream through other eviction and
fill policies offline, so a policy can be judged before the pool's lock-free
path is touched.

## Why a trace is enough

The stream of reads a workload issues does not depend on the cache: sysbench
and `pool_bench` pick keys from a seeded generator, so the same frames are
requested whether the pool hits or misses. One trace therefore replays at every
cache size and under every policy, and the run that records it needs no memory
limit.

## Recording

`BYTECASK_POOL_TRACE=<file>` in the environment turns tracing on
(`PoolTrace` in `bytecaskdb/buffer_pool.cppm`). One 24-byte record per logical
read (`read_value`, `lend_entry`) — file, first frame, frame count, kind — and
one per append to the active file. Off, the cost is one null check per read.

```bash
# pool_bench: build the dataset untraced, then trace one run.
B=build/linux/x86_64/release/pool_bench
A="--keys 2000000 --ops 5000000 --dir /mnt/data/pb/db --reuse --max-file-bytes 16777216"
$B $A --build-only
BYTECASK_POOL_TRACE=/mnt/data/pb/zipf.bin $B $A --backends buffer_pool \
  --direct-only --ratios 1.0 --zipf 0.99 --scramble

# MariaDB: the memory-pressure harness writes each cell's warm-up, measure and
# end times next to the trace; the split gives one trace per cell.
BYTECASK_POOL_TRACE=/mnt/data/sb/trace.bin \
  bytecaskdb-mariadb-plugin/benchmarks/memory-pressure/run-memory-pressure.sh \
  --data-root=/mnt/data/sb --engines=bytecaskdb-pool --mem-limit=0 \
  --workloads=oltp_point_select,oltp_read_write --threads=16 --warmup=60 --time=120 \
  --sysbench-extra=--rand-type=pareto
python3 scripts/pool_trace_split.py /mnt/data/sb/trace.bin
```

The plugin links `libbytecask.a`: run `xmake build bytecask` first, or the
plugin is built without the hooks. `--scramble` spreads `pool_bench`'s Zipf
ranks over the key space; without it the hottest keys are adjacent on disk,
which flatters block fills.

## Replaying

- `benchmarks/pool_trace_sim.cpp` (`xmake build pool_trace_sim`): the pool's
  own CLOCK (hand in hash order), textbook CLOCK, and SIEVE, each with frames
  admitted referenced or not, and block fills never, while the pool has free
  frames (the pool's rule), always, or into a probation FIFO. It mirrors
  `read_at` and `append_resident`, so the pool's configuration replays exactly.
- `scripts/pool_trace_sim.py` (`pip install libcachesim numpy`): libCacheSim's
  LRU, CLOCK, GCLOCK, S3-FIFO, SIEVE, W-TinyLFU, ARC, 2Q, LIRS and Belady on the
  same trace, plus `--pool-sim` to add the simulator's rows. Sizes are fractions
  of the trace's footprint (distinct frames read).

Both take `--count-from-ns` (from `pool_trace_split.py`) or `--warmup` so the
warm-up is not counted.

## Validation

The simulator's replay of the pool's configuration matched the measured
`pool_hits / (hits + misses)` to four decimals: `pool_bench` at pool/data 0.05,
0.10 and 0.25 (0.5498, 0.6168, 0.7265; the 0.05 and 0.25 values predicted
before they were measured), and MariaDB `special` point select at 512 MiB
(0.9432). Its textbook CLOCK and SIEVE match libCacheSim's to three decimals.

`results.csv` holds the sweeps of the traces recorded for #148.
