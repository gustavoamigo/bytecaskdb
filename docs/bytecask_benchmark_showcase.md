# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | April 25, 2026  19:43:33 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `a1c28f4` |
| **Mode** | Full |

## Hardware

```
==========================================
       SYSTEM HARDWARE CHARACTERISTICS     
==========================================

[CPU INFORMATION]
Architecture:                            x86_64
Model name:                              AMD Ryzen 7 3700X 8-Core Processor
Core(s) per socket:                      8

[INSTALLED MEMORY]
Installed RAM: 31.3 GiB

[DISK HARDWARE DESCRIPTION]
NAME MODEL                       SIZE FSTYPE TRAN
sda  Samsung SSD 860 EVO 500GB 465.8G        sata

Sequential read speed : 469MiB/s
Sequential write speed: 450MiB/s

==========================================
```

## Methodology

- **Repetitions:** 5 runs per benchmark; mean reported.
- **Value size:** 245 bytes of random, incompressible data per entry.
- **Key shape:** UUIDv7-like with 5 prefixes — `user::`, `order::`, `session::`, `invoice::`, `product::`.
- **CRC on reads:** Disabled for Get, Range, and MixedBatch benchmarks. **Enabled** for recovery benchmarks — recovery validates every byte on disk.
- **NoSync:** writes are flushed to the OS page cache; no `fdatasync` call is made.
- **Sync:** every write calls `fdatasync` before returning.
- Throughput is expressed as ops/second (Mops/s = millions of ops/second, Kops/s = thousands). Wall-clock time is used (`UseRealTime`).
- Each engine is opened in a fresh, empty temporary directory per benchmark fixture.
- **CAS benchmarks:** concurrent read-modify-write on shared stock counters, pre-populated with 1M background keys. ByteCaskDB uses `WritePlan` + `apply_batch`; RocksDB uses `OptimisticTransactionDB`.

---

# Throughput Comparison — ByteCaskDB vs RocksDB

## 50k Keys (50,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 137.7 Kops/s | **154.5 Kops/s** | **0.89×** |
| Put (Sync) | **454.3 ops/s** | 445.2 ops/s | **1.02×** |
| Get | 1.15 Mops/s | **1.30 Mops/s** | **0.88×** |
| Del (Sync) | **656.9 ops/s** | 431.4 ops/s | **1.52×** |
| Range-50 | 27.8 K scans/s | **116.4 K scans/s** | **0.24×** |
| MixedBatch/Sync | **40.8 Kops/s** | 39.4 Kops/s | **1.03×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 816 ns | 720 ns |
| p99 | 1.15 µs | 1.14 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.25 Mops/s | **2.73 Mops/s** | **0.82×** |
| 4 | 3.95 Mops/s | **4.91 Mops/s** | **0.80×** |
| 8 | 5.16 Mops/s | **8.91 Mops/s** | **0.58×** |
| 16 | 7.65 Mops/s | **11.27 Mops/s** | **0.68×** |
| 32 | 11.27 Mops/s | **15.27 Mops/s** | **0.74×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 161.0 ops/s | **250.8 ops/s** | **0.64×** |
| 4 | 306.8 ops/s | **459.7 ops/s** | **0.67×** |
| 8 | **653.8 ops/s** | 538.6 ops/s | **1.21×** |
| 16 | **1.4 Kops/s** | 863.6 ops/s | **1.64×** |
| 32 | **2.7 Kops/s** | 1.8 Kops/s | **1.51×** |
| 64 | **4.7 Kops/s** | 3.5 Kops/s | **1.34×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.28 Mops/s | **2.45 Mops/s** |
| 4 | 4.04 Mops/s | **4.65 Mops/s** |
| 8 | 5.44 Mops/s | **8.78 Mops/s** |
| 16 | 8.26 Mops/s | **11.41 Mops/s** |
| 32 | 12.23 Mops/s | **17.39 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 131.4 Kops/s | **159.8 Kops/s** | **0.82×** |
| Put (Sync) | **162.9 ops/s** | 159.5 ops/s | **1.02×** |
| Get | **1.16 Mops/s** | 351.6 Kops/s | **3.29×** |
| Del (Sync) | **177.2 ops/s** | 136.3 ops/s | **1.30×** |
| Range-50 | 27.4 K scans/s | **61.5 K scans/s** | **0.44×** |
| MixedBatch/Sync | **14.9 Kops/s** | 14.7 Kops/s | **1.01×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 820 ns | 2.67 µs |
| p99 | 1.09 µs | 5.77 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.21 Mops/s** | 677.9 Kops/s | **3.26×** |
| 4 | **3.88 Mops/s** | 1.28 Mops/s | **3.03×** |
| 8 | **5.71 Mops/s** | 2.84 Mops/s | **2.01×** |
| 16 | **8.24 Mops/s** | 4.28 Mops/s | **1.93×** |
| 32 | **13.08 Mops/s** | 6.30 Mops/s | **2.08×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 160.7 ops/s | **253.5 ops/s** | **0.63×** |
| 4 | 333.4 ops/s | **464.5 ops/s** | **0.72×** |
| 8 | **625.3 ops/s** | 596.0 ops/s | **1.05×** |
| 16 | **1.4 Kops/s** | 886.2 ops/s | **1.52×** |
| 32 | **2.8 Kops/s** | 1.9 Kops/s | **1.43×** |
| 64 | **4.8 Kops/s** | 3.6 Kops/s | **1.34×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.21 Mops/s** | 632.1 Kops/s |
| 4 | **3.99 Mops/s** | 1.21 Mops/s |
| 8 | **6.18 Mops/s** | 2.74 Mops/s |
| 16 | **8.65 Mops/s** | 4.09 Mops/s |
| 32 | **13.25 Mops/s** | 5.77 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 134.2 Kops/s | **160.7 Kops/s** | **0.84×** |
| Put (Sync) | **157.2 ops/s** | 153.2 ops/s | **1.03×** |
| Get | **1.14 Mops/s** | 437.3 Kops/s | **2.60×** |
| Del (Sync) | **165.3 ops/s** | 134.0 ops/s | **1.23×** |
| Range-50 | 28.0 K scans/s | **69.2 K scans/s** | **0.40×** |
| MixedBatch/Sync | **14.9 Kops/s** | 14.4 Kops/s | **1.04×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 834 ns | 2.09 µs |
| p99 | 1.10 µs | 5.09 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.25 Mops/s** | 797.7 Kops/s | **2.82×** |
| 4 | **4.11 Mops/s** | 1.54 Mops/s | **2.67×** |
| 8 | **6.73 Mops/s** | 3.16 Mops/s | **2.13×** |
| 16 | **9.49 Mops/s** | 4.88 Mops/s | **1.94×** |
| 32 | **14.17 Mops/s** | 6.99 Mops/s | **2.03×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 161.0 ops/s | **268.7 ops/s** | **0.60×** |
| 4 | 307.4 ops/s | **456.7 ops/s** | **0.67×** |
| 8 | 652.8 ops/s | **771.5 ops/s** | **0.85×** |
| 16 | **1.6 Kops/s** | 862.7 ops/s | **1.81×** |
| 32 | **3.0 Kops/s** | 1.8 Kops/s | **1.66×** |
| 64 | **4.7 Kops/s** | 3.5 Kops/s | **1.35×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.28 Mops/s** | 759.3 Kops/s |
| 4 | **4.17 Mops/s** | 1.50 Mops/s |
| 8 | **6.33 Mops/s** | 3.06 Mops/s |
| 16 | **9.19 Mops/s** | 4.82 Mops/s |
| 32 | **14.05 Mops/s** | 6.85 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.25 Mops/s | **2.73 Mops/s** | **2.21 Mops/s** | 677.9 Kops/s | **2.25 Mops/s** | 797.7 Kops/s |
| 4 | 3.95 Mops/s | **4.91 Mops/s** | **3.88 Mops/s** | 1.28 Mops/s | **4.11 Mops/s** | 1.54 Mops/s |
| 8 | 5.16 Mops/s | **8.91 Mops/s** | **5.71 Mops/s** | 2.84 Mops/s | **6.73 Mops/s** | 3.16 Mops/s |
| 16 | 7.65 Mops/s | **11.27 Mops/s** | **8.24 Mops/s** | 4.28 Mops/s | **9.49 Mops/s** | 4.88 Mops/s |
| 32 | 11.27 Mops/s | **15.27 Mops/s** | **13.08 Mops/s** | 6.30 Mops/s | **14.17 Mops/s** | 6.99 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.27 µs | **2.13 µs** | **2.43 µs** | 12.79 µs | **2.40 µs** | 11.58 µs |
| 4 | 6.21 µs | **4.72 µs** | **6.26 µs** | 28.66 µs | **5.41 µs** | 26.17 µs |
| 8 | 18.33 µs | **11.35 µs** | **16.45 µs** | 59.46 µs | **13.54 µs** | 54.36 µs |
| 16 | 43.41 µs | **29.00 µs** | **40.95 µs** | 99.46 µs | **33.09 µs** | 78.61 µs |
| 32 | 90.51 µs | **58.77 µs** | **77.54 µs** | 172.16 µs | **63.64 µs** | 142.34 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.024 s |
| 2 | 0.026 s |
| 4 | 0.028 s |
| 8 | 0.030 s |
| 16 | 0.032 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.335 s |
| 2 | 0.192 s |
| 4 | 0.118 s |
| 8 | 0.074 s |
| 16 | 0.066 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 3.411 s |
| 2 | 1.934 s |
| 4 | 1.159 s |
| 8 | 0.708 s |
| 16 | 0.568 s |


# Optimistic Concurrency — CAS Benchmark

> Concurrent read-modify-write (increment) on shared stock counters. ByteCaskDB uses `WritePlan` + `apply_batch` with snapshot-based conflict detection; RocksDB uses `OptimisticTransactionDB`. Each iteration is one successful CAS — retries on conflict are included in wall-clock time. Both databases are pre-populated with 1M background keys.

## 100 stock items (high contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 101.3 Kops/s | **153.8 Kops/s** | 38.02 µs | 24.54 µs | 60.57 µs | 47.07 µs | 1.00 | 1.01 |
| 4 | 153.2 Kops/s | **221.2 Kops/s** | 95.37 µs | 68.87 µs | 197.66 µs | 148.77 µs | 1.02 | 1.02 |
| 8 | 207.6 Kops/s | **299.6 Kops/s** | 259.28 µs | 197.60 µs | 583.91 µs | 475.66 µs | 1.04 | 1.05 |
| 16 | 235.5 Kops/s | **255.3 Kops/s** | 958.11 µs | 946.98 µs | 2.73 ms | 2.50 ms | 1.09 | 1.11 |
| 32 | **292.3 Kops/s** | 205.8 Kops/s | 2.61 ms | 4.41 ms | 11.31 ms | 11.87 ms | 1.16 | 1.21 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 157.8 ops/s | **177.3 ops/s** | 25.27 ms | 23.24 ms | 26.58 ms | 40.87 ms | 1.00 | 1.01 |
| 4 | **327.2 ops/s** | 264.2 ops/s | 49.29 ms | 49.92 ms | 50.85 ms | 89.98 ms | 1.00 | 1.00 |
| 8 | **576.5 ops/s** | 514.8 ops/s | 101.09 ms | 131.14 ms | 108.17 ms | 141.86 ms | 1.00 | 1.01 |
| 16 | **1.4 Kops/s** | 803.9 ops/s | 161.54 ms | 307.41 ms | 161.54 ms | 307.41 ms | 1.08 | 1.08 |
| 32 | **2.1 Kops/s** | 1.3 Kops/s | 481.77 ms | 797.36 ms | 481.77 ms | 797.36 ms | 1.34 | 1.33 |

## 10,000 stock items (low contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 90.7 Kops/s | **166.5 Kops/s** | 42.42 µs | 23.18 µs | 65.33 µs | 36.93 µs | 1.00 | 1.00 |
| 4 | 113.9 Kops/s | **219.0 Kops/s** | 108.61 µs | 71.70 µs | 235.06 µs | 106.19 µs | 1.00 | 1.00 |
| 8 | 159.0 Kops/s | **310.4 Kops/s** | 354.61 µs | 195.88 µs | 458.71 µs | 349.05 µs | 1.00 | 1.00 |
| 16 | 204.9 Kops/s | **256.8 Kops/s** | 1.13 ms | 974.70 µs | 2.21 ms | 2.21 ms | 1.00 | 1.00 |
| 32 | **238.3 Kops/s** | 224.3 Kops/s | 3.72 ms | 4.48 ms | 8.35 ms | 6.72 ms | 1.00 | 1.00 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 144.0 ops/s | **144.3 ops/s** | 26.47 ms | 23.32 ms | 37.85 ms | 76.21 ms | 1.00 | 1.00 |
| 4 | **316.6 ops/s** | 250.0 ops/s | 46.66 ms | 64.83 ms | 48.76 ms | 98.62 ms | 1.00 | 1.00 |
| 8 | **649.9 ops/s** | 544.9 ops/s | 87.01 ms | 122.75 ms | 97.52 ms | 135.95 ms | 1.00 | 1.00 |
| 16 | **1.3 Kops/s** | 903.1 ops/s | 181.28 ms | 286.11 ms | 188.75 ms | 286.11 ms | 1.00 | 1.00 |
| 32 | **3.1 Kops/s** | 1.8 Kops/s | 255.86 ms | 575.86 ms | 255.86 ms | 575.86 ms | 1.02 | 1.03 |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-25_