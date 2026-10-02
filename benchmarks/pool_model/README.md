# Buffer pool under a write-heavy workload

Measurement tooling, not for merging. It answers two questions from a box
run where the buffer pool missed 106K times a second on sysbench
`oltp_read_write` (acid profile, 3 GiB pool, vacuum threshold 0.9) while
InnoDB, with the same 3 GiB, read nothing from disk:

1. Should vacuum run harder, so the data files stay within reach of the pool?
2. Should the pool use a different policy, given the shape of the workload?

## Method

`BYTECASK_POOL_TRACE=<file>` makes the engine record what it does to its data
files (`PoolTrace` in `bytecaskdb/buffer_pool.cppm`): every record read, with
the caller (point read, write path, or other — iterators); every append; every
record written live and every record superseded; and every file vacuum creates
or deletes. The stream does not depend on the pool, so one trace replays at
any pool size and under any policy.

`sim.cpp` replays a trace through the pool's rules — SIEVE, the writer's
frames admitted visited, the active file pinned, 128 KiB fills while the pool
has free frames — and through variants. It knows how many live bytes each
4 KiB frame holds, so policies can use liveness.

```bash
clang++ -std=c++20 -O2 -march=native -o sim sim.cpp
record.sh --out=DIR --vacuum=0.5 --rand-type=special   # 1M rows, 8 threads, 30 s warm-up, 120 s run
./sim sort DIR/trace.bin DIR/sorted.bin
./sim stats DIR/sorted.bin DIR/phases.txt
ABS=1 sweep.sh DIR ./sim 250 375 500 750               # pool sizes in MiB
```

Policies (`sim run ... <policy>`, tokens joined with `+`):

| token | what changes |
|---|---|
| `pool` | the pool as it is |
| `wnv` | the writer's frames admitted not visited (plain SIEVE) |
| `deadfirst` | frames whose records are all superseded are evicted first |
| `sparse=F` | a frame less than F live loses its second chance |
| `ra` | every miss reads its 128 KiB block |
| `ra_dense=F` | read ahead only in files at least F live |
| `ra_young=N` | read ahead only in the newest N files |
| `ra_live` | read the block, admit only frames at least half live |
| `vacwarm` | records vacuum moves enter the pool as it writes them |
| `orphans` | a deleted file's frames stay until the hand reaches them, as in the pool; without it they are freed at once |
| `rewarm` | a record vacuum moves enters the pool at its new place when its old frame was in it, with its visited bit |
| `opt` | Belady with bypass: the best any policy could do |

Recorded locally (Ryzen 7 3700X, SATA SSD): 1M rows (242 MiB live: rows and
secondary-index entries), 16 MiB data files so the dataset spans as many files
as the box's does at 64 MiB, fast profile. sysbench's default key distribution
is `special` (1 % of keys take 75 % of accesses), which is what
`run-sysbench.sh` runs on the box; `uniform` and `pareto` were recorded too.
`results/` holds every sweep.

## What a trace shows

- **Every row moves about every 50 s.** A transaction rewrites four rows, so at
  ~4,700 tps the 1M rows are rewritten 2.3 times in a 120 s run. 450 MiB is
  appended per run against 242 MiB live.
- **Without vacuum the files grow at the write rate**: 368 MiB at the start of
  the run, 820 MiB at the end (threshold 0.9 — nothing qualified in the
  window). The frames that still hold a live record grow with them, 349 to 727
  MiB: a 4 KiB frame holds ~18 records and keeps a live one until nearly all
  are superseded.
- **Vacuum caps both**: files end at 432 MiB at 0.5 and 378 MiB at 0.25.
- **Range scans are scattered, whatever vacuum does.** The four 100-row scans
  of a transaction touch ~340 distinct frames; packed in key order they would
  touch ~12. Each update moves its row to the newest file, and vacuum copies
  records in file order, not key order.
- **Reads are 95 % iterator reads**, 3 % point reads, 2 % the write path.

## Results

Read misses (share of record reads that needed a fill) and bytes read from
disk, `pool` policy. With vacuum running, these rows free a deleted file's
frames at once, which the pool does not do; the next section has the pool's
own behaviour, which misses more:

| distribution, vacuum | 250 MiB | 375 MiB | 500 MiB | 750 MiB |
|---|---|---|---|---|
| special, 0.9 | 3.67 %, 301 MiB/s | 1.00 %, 81 MiB/s | 0.11 %, 9 MiB/s | 0 |
| special, 0.5 | 0.37 %, 40 MiB/s | 0.01 %, 6 MiB/s | 0 | 0 |
| special, 0.25 | 0.05 %, 11 MiB/s | 0 | 0 | 0 |
| uniform, 0.9 | 26.95 %, 2286 MiB/s | 12.46 % | 4.58 % | 0.07 % |
| uniform, 0.5 | 20.09 % | 4.49 % | 0 | 0 |
| uniform, 0.25 | 13.23 % | 0.68 % | 0 | 0 |
| pareto, 0.9 | 4.75 % | 1.60 % | 0.36 % | 0 |

Vacuum's own I/O in the 120 s window (reads bypass the pool): at 0.5 it read
587 MiB and wrote 198 MiB (`special`), ~6.5 MiB/s; at 0.25, 712 MiB and 342
MiB, ~9 MiB/s. Throughput: 4,583 tps at 0.9, 4,800 at 0.5, 4,480 at 0.25
(local run-to-run variance is ~4 %).

Policies, `special` at 0.9 (the box's configuration), 250 MiB:

| policy | misses | read MiB/s |
|---|---|---|
| pool | 3.67 % | 301 |
| wnv | 3.67 % | 301 |
| deadfirst | 3.66 % | 300 |
| sparse=0.25 | 7.20 % | 598 |
| ra | 3.42 % | 8,750 |
| ra_dense=0.5 | 3.94 % | 695 |
| ra_young=4 | 3.63 % | 593 |
| ra_live | 3.96 % | 10,162 |
| opt (Belady) | 1.27 % | — |

The same holds on every trace: no practical policy moves misses by more than
~10 % either way, and the read-ahead variants multiply the bytes read. With
vacuum at 0.5, `vacwarm` changes nothing (0.37 % against 0.37 %): almost every
miss is a capacity miss, not a first read of a moved record. Belady leaves
room — 0.14 % against 0.37 % (`special`, 0.5, 250 MiB), 4.3 % against 12.5 %
(uniform, 0.9, 375 MiB) — but none of the policies tried captures it.

## Answers

1. **Yes, vacuum harder.** It is the lever: at a pool the size of the live
   data, threshold 0.5 instead of 0.9 cuts misses tenfold on the box's
   distribution (3.67 % to 0.37 %) for ~6.5 MiB/s of vacuum I/O against
   ~260 MiB/s of misses saved, and 0.25 to 0.20 %. Freeing a deleted file's
   frames at once takes a further 17–97 % off, depending on the pool size. What the pool
   has to hold is the frames that still contain a live record, and that is
   the file bytes vacuum has not reclaimed; the threshold bounds them.
2. **No policy change is worth making.** Liveness-aware eviction, plain SIEVE
   for the writer's frames, and every read-ahead rule tried are within ~10 %
   of the pool as it is, or worse.

## Vacuum's deleted files, and rewarming what it moved

When vacuum deletes a file, the pool keeps its frames: nothing can look them
up again (file ids are not reused), and the hand reclaims each one when it
reaches it — a whole pass later if the frame was read just before. With few
misses the hand moves slowly, so they linger. Recorded again with each moved
record's old location (`special`):

| vacuum, pool | as today (`orphans`) | `orphans+rewarm` | freed at once (`pool`) | freed + `rewarm` |
|---|---|---|---|---|
| 0.5, 250 MiB | 0.372 % | 0.362 % | 0.308 % | 0.313 % |
| 0.5, 375 MiB | 0.051 % | 0.034 % | 0.010 % | 0.010 % |
| 0.5, 500 MiB | 0.031 % | 0.013 % | 0.001 % | 0.000 % |
| 0.25, 250 MiB | 0.201 % | 0.188 % | 0.151 % | 0.154 % |
| 0.25, 375 MiB | 0.045 % | 0.026 % | 0.004 % | 0.004 % |
| 0.25, 500 MiB | 0.032 % | 0.012 % | 0.001 % | 0.000 % |

Freeing a deleted file's frames when vacuum deletes it takes 17–25 % of the
misses off at a pool the size of the live data and 80–97 % at 1.5–2×.
Rewarming moved records helps only while orphans are left in place; once they
are freed it adds nothing. With the pool's own behaviour, vacuum at 0.5 and
0.25 misses 0.37 % and 0.20 % at 250 MiB, against 3.67 % at 0.9.

What neither touches: range scans read ~30× more frames than the rows need,
because writes destroy key order. Only where records are written could change
that (for example a vacuum that rewrites live records in key order).

## Caveats

- One machine, one workload, 1M rows, 120 s runs. The 0.9 runs never reach a
  steady state — the files are still growing at the end — so a longer run
  would widen the gap.
- Vacuum's sweep reads are not modelled as pool traffic (they
  bypass it) but are counted above.
- A trace records what the engine asked for; how long a miss takes, and so
  throughput, is not modelled.
