# Durable Append Methods: Empirical Comparison

Comparison of Linux durability primitives for the ByteCaskDB write path — one
record appended, then made durable, on btrfs.

## TL;DR

For sequential durable appends at record sizes ≥ 4 KiB, **`writev` + `fdatasync`** is
the simplest method and matches every more elaborate alternative within noise.
Other methods only "win" through artifacts that do not apply to ByteCaskDB's
workload (sub-page consolidation in mmap, or batched I/O at the cost of an extra
copy).

ByteCaskDB already uses this pattern: one `writev` per write, one `fdatasync` at
group-commit boundaries (see `benchmarks/append_bench.cpp:51` for the canonical
loop body).

## Methods Tested

| Method | Description |
|---|---|
| `writev + fdatasync` | One `writev` per record, one `fdatasync` after |
| `writev + O_DSYNC` | Open with `O_DSYNC`; kernel syncs on each `writev` |
| `writev + O_DIRECT \| O_DSYNC` | Bypass page cache, sync per write, aligned buffer |
| `pwritev2 + RWF_DSYNC` | Per-call sync flag, regular fd |
| `mmap + msync(MS_SYNC)` | Memcpy into mapping, `msync` over the just-written range |
| `mmap + fdatasync` | Memcpy into mapping, full-file `fdatasync` |
| `io_uring write+fsync linked` | `IOSQE_IO_LINK` chains a write to a sync SQE |

All variants pre-allocate the file (`fallocate` + `ftruncate`) before the timed
loop, so no file-extension metadata work is included in the measurement. Each
variant runs on a fresh path to avoid cross-contamination of btrfs log-tree
state.

## Results — sync every write

`kNumRecords = 2000` after a 1000-record warmup. btrfs on SATA SSD.

| Method | 1 KiB | 2 KiB | 4 KiB |
|---|---:|---:|---:|
| writev + fdatasync | 134 ops/s | 414 | 443 |
| writev + O_DSYNC | 135 | 431 | 434 |
| writev block-rewrite O_DIRECT\|O_DSYNC | 131 | 429 | 447 |
| pwritev2 + RWF_DSYNC | 132 | 455 | 410 |
| mmap + msync(MS_SYNC) | **540** | **801** | 313 |
| mmap + fdatasync | 129 | 175 | 151 |
| io_uring write+fsync linked | 412 | 156 | 150 |

## Results — sync every 10 writes

| Method | 1 KiB | 2 KiB | 4 KiB |
|---|---:|---:|---:|
| writev + fdatasync (per-record I/O, batched sync) | 4364 | 1404 | 1537 |
| writev batched I/O + fdatasync | 4576 | 1526 | 1580 |
| writev batched + O_DIRECT \| O_DSYNC | 4260 | 1489 | 1476 |
| mmap + msync(MS_SYNC) | **8957** | 1564 | 1523 |
| mmap + fdatasync | 4404 | 1480 | 1369 |
| io_uring write+fsync batched | 4217 | 1538 | 1174 |

## Analysis

### `writev + fdatasync` is the per-flush floor

For records ≥ 1 page (4 KiB), every method lands within ~10% of `writev +
fdatasync`. That number is the per-flush latency floor on the device — one
device-level FLUSH (`REQ_PREFLUSH`) costs about 2.2 ms regardless of how the
write reached the block layer. Below this floor, nothing can go.

### `mmap + msync` only wins via sub-page consolidation

The 4× speedup of `mmap + msync` at 1 KiB records collapses to a 0.7× *slowdown*
at 4 KiB. The pattern matches `kPageSize / kRecordSize`:

| Record size | mmap+msync / writev+fdatasync | Reason |
|---:|---:|---|
| 1024 | 4.0× | 4 records share one 4 KiB page |
| 2048 | 1.9× | 2 records share one page |
| 4096 | 0.7× | 1 record = 1 page; msync pays page-fault overhead per write |

`msync(addr, N, MS_SYNC)` flushes only the pages overlapping the range. At
sub-page record sizes, consecutive `msync` calls hit pages the first call
already flushed and return cheaply. This is **not real durability gain** — it is
the same page being made durable once per group of records that happen to live
on it. A workload that packs many entries per page can benefit; ByteCaskDB
entries (key + value + framing) typically span ≥ 1 page.

At 4 KiB, every record is its own page and each `msync` flushes a new page,
plus pays per-page minor faults on the memcpy — making mmap *slower* than
writev.

### `O_DSYNC`, `RWF_DSYNC`, `O_DIRECT | O_DSYNC` — equivalent

All three are within noise of `writev + fdatasync` at every record size. They
remove the separate `fdatasync` syscall but don't change the underlying work:
one device FLUSH per writeable unit.

`O_DIRECT` brings buffer-alignment and copy constraints with no observed
throughput benefit on this workload, and loses the page cache (so subsequent
reads are slower). Not worth the complexity.

### `mmap + fdatasync` — worst of both

Pays page-fault cost on memcpy *and* full-file `fdatasync`. Strictly dominated
by every other method at every-write sync (129–175 ops/s across all record
sizes).

### `io_uring write+fsync linked` — no advantage for one-write-then-sync

`IOSQE_IO_LINK` chains a write SQE to a sync SQE. At sync-every-write it ties
or loses to `writev + fdatasync` (412 ops/s at 1 KiB is an outlier; 150 ops/s
at 4 KiB is worse than the 443 baseline). The serialization in the linked chain
removes io_uring's main advantage — overlap. At batch sync, io_uring also
underperforms slightly (1174 vs 1537 at 4 KiB).

io_uring is useful when there are many independent I/Os in flight whose
completions can interleave. The ByteCaskDB write path is a single ordered
append followed by a single sync, which is exactly the pattern io_uring is
*not* designed to accelerate.

### Batching the I/O is orthogonal to the sync method

At sync-every-10 with one large `writev` covering 10 records, throughput is the
same as 10 separate `writev` calls followed by one `fdatasync` — both ~1500
ops/s at 4 KiB. The lever is the **sync cadence**, not the I/O packing. Group
commit (one `fdatasync` for many writers) is the win; packing the writes
themselves is not.

## Implication for ByteCaskDB

The current write path — one `writev` per `put`/`del`/`apply_batch`, one
`fdatasync` per group-commit batch — is at the per-flush floor for our typical
entry sizes. The simpler method is not slower. The complex methods
(`io_uring`, `O_DIRECT`, mmap-based) trade code complexity, alignment
constraints, or page-cache loss for no measured throughput gain.

The remaining lever is **group commit width**: more concurrent writers per
`fdatasync` directly reduces per-write sync cost. This is what the existing
group writer already exploits.

## Reproducing

```bash
g++ -O2 -std=c++20 -o append_bench benchmarks/append_bench.cpp -luring
./append_bench
```

Change `kRecordSize` in `benchmarks/append_bench.cpp` to test different sizes.
Each method writes to its own path under `tmp/append_bench_tmp/` and removes it
after the run, so results are independent across methods within a single
process invocation.

Measured on AMD Ryzen 7 3700X, Samsung 860 EVO SATA SSD, btrfs.
