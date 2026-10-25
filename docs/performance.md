# ByteCaskDB Performance

Benchmark results against RocksDB at 1M keys. The generated multi-size report is [`bytecask_benchmark_showcase.md`](bytecask_benchmark_showcase.md); the tables below are the current, hand-maintained figures.

Benchmarked at 1 M keys with [RocksDB](https://rocksdb.org/) as a reference point. The tables below include both engines for context.

- **Point reads** reach 1.29 Mops/s at 1 M keys with flat, sub-microsecond latency (p50 728 ns, p99 1.01 µs). Latency stays flat as the dataset grows because every lookup is an in-memory key directory traversal followed by a single read at a known offset, which confirms the key and returns the value.
- **Concurrent reads scale linearly** — lock-free snapshots with no shared mutex. 14.0 Mops/s at 32 threads.
- **Sequential writes** sustain 134 Kops/s (NoSync) and 139 ops/s (Sync), limited by `fdatasync` round-trip latency. No write amplification from compaction.
- **Concurrent sync writes scale via group commit** — writers share a single `fdatasync` call. 4.9 Kops/s at 64 threads.
- **Range scans over values** fetch each value individually from disk. LSM-based engines pack values contiguously in sorted runs and perform better here. Key-only iteration (`keys_from`) reads each key's record header and skips the value.
- **Recovery is fast and parallel** — compressed hint files replayed across all cores with per-file CRC verification. From a cold start: 1 M keys in 37 ms, 10 M in 0.33 s at 16 threads.

See [`docs/bytecask_benchmark_showcase.md`](bytecask_benchmark_showcase.md) for the full benchmark report with all thread counts, dataset sizes, and hardware details.

---

## Single-Threaded Throughput (1M keys)

> Per-value CRC verification is disabled for read benchmarks; hint file CRC is always verified during recovery.

| Operation | ByteCaskDB | RocksDB | Notes |
|-----------|----------|---------|-------|
| Put (NoSync) | 134 Kops/s | 163 Kops/s | Sequential append on both sides |
| Put (Sync) | 139 ops/s | 148 ops/s | Disk-bound — limited by `fdatasync` round-trip latency |
| Get | 1.29 Mops/s | 431 Kops/s | In-memory key directory lookup; flat latency regardless of dataset size |
| Del (Sync) | 184 ops/s | 2 ops/s | Single tombstone append; no compaction write amplification |
| Range-50 | 28 K scans/s | 68 K scans/s | LSM sorted runs favour sequential value scans |
| MixedBatch (Sync) | 14 Kops/s | 13 Kops/s | Atomic batch with single `pwritev` + `fdatasync` |

At small dataset sizes (50 k keys), all keys fit in RocksDB's block cache and reads are fast on both engines. From 500 k keys onward, block cache misses begin to dominate and the in-memory key directory approach shows its advantage.

## Get Latency (1M keys, CRC disabled)

| Percentile | ByteCaskDB | RocksDB |
|-----------|---------|----------|
| p50 | 728 ns | 2.10 µs |
| p99 | 1.01 µs | 5.37 µs |

Latency stays flat as the dataset grows: every read resolves to a known file offset via the in-memory key directory, so there is no metadata amplification from multiple levels or bloom filter checks.

## Concurrent Reads — `GetMT` (1M keys, CRC disabled)

> Reads are lock-free; each thread holds an immutable snapshot of the engine state.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.34 Mops/s | 749 Kops/s |
| 4 | 4.18 Mops/s | 1.49 Mops/s |
| 8 | 6.25 Mops/s | 3.24 Mops/s |
| 16 | 8.76 Mops/s | 4.79 Mops/s |
| 32 | 14.01 Mops/s | 7.33 Mops/s |

## Concurrent Sync Writes — `PutMT/Sync` (1M keys)

> Group commit: concurrent sync writers share a single `fdatasync` call, amortising the dominant cost. The benefit grows with concurrency — more contending writers means larger batches and fewer `fdatasync` calls per write.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 145 ops/s | 257 ops/s |
| 4 | 314 ops/s | 439 ops/s |
| 8 | 617 ops/s | 741 ops/s |
| 16 | 1.3 Kops/s | 816 ops/s |
| 32 | 2.9 Kops/s | 1.7 Kops/s |
| 64 | 4.9 Kops/s | 3.3 Kops/s |

## Read-While-Writing (1M keys, 1 writer + N readers, Sync, CRC disabled)

> Lock-free readers are unaffected by concurrent writes. Read throughput scales the same whether the database is idle or under write load.

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.43 Mops/s | 720 Kops/s |
| 4 | 4.39 Mops/s | 1.32 Mops/s |
| 8 | 6.52 Mops/s | 2.91 Mops/s |
| 16 | 9.40 Mops/s | 4.48 Mops/s |
| 32 | 14.93 Mops/s | 7.20 Mops/s |

## Recovery

Recovery runs when ByteCaskDB opens an existing database: it rebuilds the in-memory key directory by reading compact hint files from disk. Each hint file holds its entries in zstd frames and is verified by a file-level CRC-32C trailer before parsing. This is parallelised across all available CPU cores — each core processes a disjoint set of hint files independently, and the results are merged before the database becomes available.

Measured from a cold start: every file of the database is evicted from the page cache before each open, so the hint files come off the SSD, as after a reboot or on a new node.

| Keys | Threads | Recovery Time | Speedup vs 1T |
|---:|---:|---:|---:|
| 1M | 1 | 206 ms | — |
| 1M | 4 | 63 ms | 3.3× |
| 1M | 8 | 42 ms | 4.9× |
| 1M | 16 | 37 ms | 5.6× |
| 10M | 1 | 2.24 s | — |
| 10M | 4 | 0.63 s | 3.6× |
| 10M | 8 | 0.37 s | 6.1× |
| 10M | 16 | 0.33 s | 6.9× |

The benchmark's keys — prefixed UUID text written in order, with 1-byte values — compress 14×: at 10M keys the hint files hold 675 MB of entries in 47 MB. Keys with less in common compress less (2.4× for random 16-hex-digit ids with realistic values, 1.6× for random binary keys), and a cold start then reads more; [`docs/hint_compression_design.md`](hint_compression_design.md) has the measurements.

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (463 MiB/s read), 31 GiB RAM. Each result is the mean of 5 runs. Benchmark source: [`benchmarks/engine_bench.cpp`](../benchmarks/engine_bench.cpp)._

_The recovery figures were measured on the default blind-leaf key directory, on the machine above, in September 2026. The other figures were measured on the radix key directory, before the B+ tree became the default, and on the `mmap` read path, before `engine_bench` switched its default to the buffer pool. A head-to-head run of both trees on one engine put the B+ tree ahead on reads and batched writes and behind on unsynced single puts, but that run was on different hardware, so the absolute numbers above have not been re-measured on this machine and are not restated here. The blind-leaf tree, now the default, was then measured against the keyed B+ tree on one machine at 1M keys: level on `Get` and `GetMT`, 1.25–1.3× on random 16-byte keys, and 0.89× on unsynced single puts, with the key directory 2–7× smaller ([`docs/blind_leaf_btree_design.md`](blind_leaf_btree_design.md)). Reproduce the keyed trees with [`scripts/compare_engine_bench.py`](../scripts/compare_engine_bench.py)._
