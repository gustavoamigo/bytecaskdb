# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | April 24, 2026  10:23:41 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `8c16b77` |
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

Sequential read speed : 463MiB/s
Sequential write speed: 476MiB/s

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
| Put (NoSync) | 135.3 Kops/s | **153.6 Kops/s** | **0.88×** |
| Put (Sync) | **465.5 ops/s** | 457.0 ops/s | **1.02×** |
| Get | 1.28 Mops/s | **1.29 Mops/s** | **0.99×** |
| Del (Sync) | **670.3 ops/s** | 462.2 ops/s | **1.45×** |
| Range-50 | 28.9 K scans/s | **122.7 K scans/s** | **0.24×** |
| MixedBatch/Sync | **43.5 Kops/s** | 37.6 Kops/s | **1.16×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 734 ns | 732 ns |
| p99 | 1.02 µs | 1.13 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.32 Mops/s | **2.54 Mops/s** | **0.91×** |
| 4 | 4.31 Mops/s | **4.68 Mops/s** | **0.92×** |
| 8 | 5.38 Mops/s | **8.96 Mops/s** | **0.60×** |
| 16 | 8.00 Mops/s | **11.46 Mops/s** | **0.70×** |
| 32 | 12.72 Mops/s | **16.56 Mops/s** | **0.77×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 153.5 ops/s | **245.7 ops/s** | **0.62×** |
| 4 | 309.8 ops/s | **425.3 ops/s** | **0.73×** |
| 8 | 613.3 ops/s | **696.4 ops/s** | **0.88×** |
| 16 | **1.4 Kops/s** | 1.1 Kops/s | **1.30×** |
| 32 | **3.0 Kops/s** | 1.7 Kops/s | **1.71×** |
| 64 | **5.1 Kops/s** | 3.5 Kops/s | **1.48×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.40 Mops/s | **2.58 Mops/s** |
| 4 | 4.26 Mops/s | **4.68 Mops/s** |
| 8 | 5.62 Mops/s | **8.96 Mops/s** |
| 16 | 8.39 Mops/s | **11.59 Mops/s** |
| 32 | 13.81 Mops/s | **18.72 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 138.2 Kops/s | **158.9 Kops/s** | **0.87×** |
| Put (Sync) | **163.2 ops/s** | 158.2 ops/s | **1.03×** |
| Get | **1.27 Mops/s** | 363.8 Kops/s | **3.48×** |
| Del (Sync) | **172.0 ops/s** | 132.9 ops/s | **1.29×** |
| Range-50 | 28.2 K scans/s | **65.1 K scans/s** | **0.43×** |
| MixedBatch/Sync | **15.0 Kops/s** | 14.4 Kops/s | **1.05×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 742 ns | 2.50 µs |
| p99 | 1.01 µs | 5.86 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.26 Mops/s** | 642.7 Kops/s | **3.52×** |
| 4 | **4.14 Mops/s** | 1.21 Mops/s | **3.41×** |
| 8 | **6.04 Mops/s** | 2.81 Mops/s | **2.15×** |
| 16 | **8.41 Mops/s** | 3.90 Mops/s | **2.16×** |
| 32 | **13.38 Mops/s** | 6.40 Mops/s | **2.09×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 151.7 ops/s | **259.3 ops/s** | **0.58×** |
| 4 | 307.6 ops/s | **341.6 ops/s** | **0.90×** |
| 8 | 664.8 ops/s | **703.3 ops/s** | **0.95×** |
| 16 | **1.4 Kops/s** | 777.7 ops/s | **1.83×** |
| 32 | **2.5 Kops/s** | 1.6 Kops/s | **1.55×** |
| 64 | **4.8 Kops/s** | 3.4 Kops/s | **1.41×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.31 Mops/s** | 619.5 Kops/s |
| 4 | **4.26 Mops/s** | 1.17 Mops/s |
| 8 | **6.43 Mops/s** | 2.72 Mops/s |
| 16 | **8.95 Mops/s** | 4.16 Mops/s |
| 32 | **14.50 Mops/s** | 6.39 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 133.8 Kops/s | **162.8 Kops/s** | **0.82×** |
| Put (Sync) | 138.5 ops/s | **148.0 ops/s** | **0.94×** |
| Get | **1.29 Mops/s** | 430.7 Kops/s | **3.00×** |
| Del (Sync) | **183.9 ops/s** | 1.9 ops/s | **96.92×** |
| Range-50 | 27.6 K scans/s | **67.6 K scans/s** | **0.41×** |
| MixedBatch/Sync | **14.2 Kops/s** | 12.8 Kops/s | **1.11×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 728 ns | 2.10 µs |
| p99 | 1.01 µs | 5.37 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.34 Mops/s** | 748.7 Kops/s | **3.12×** |
| 4 | **4.18 Mops/s** | 1.49 Mops/s | **2.80×** |
| 8 | **6.25 Mops/s** | 3.24 Mops/s | **1.93×** |
| 16 | **8.76 Mops/s** | 4.79 Mops/s | **1.83×** |
| 32 | **14.01 Mops/s** | 7.33 Mops/s | **1.91×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 145.0 ops/s | **256.7 ops/s** | **0.56×** |
| 4 | 314.4 ops/s | **439.3 ops/s** | **0.72×** |
| 8 | 616.9 ops/s | **741.3 ops/s** | **0.83×** |
| 16 | **1.3 Kops/s** | 815.5 ops/s | **1.65×** |
| 32 | **2.9 Kops/s** | 1.7 Kops/s | **1.71×** |
| 64 | **4.9 Kops/s** | 3.3 Kops/s | **1.50×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.43 Mops/s** | 720.3 Kops/s |
| 4 | **4.39 Mops/s** | 1.32 Mops/s |
| 8 | **6.52 Mops/s** | 2.91 Mops/s |
| 16 | **9.40 Mops/s** | 4.48 Mops/s |
| 32 | **14.93 Mops/s** | 7.20 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.32 Mops/s | **2.54 Mops/s** | **2.26 Mops/s** | 642.7 Kops/s | **2.34 Mops/s** | 748.7 Kops/s |
| 4 | 4.31 Mops/s | **4.68 Mops/s** | **4.14 Mops/s** | 1.21 Mops/s | **4.18 Mops/s** | 1.49 Mops/s |
| 8 | 5.38 Mops/s | **8.96 Mops/s** | **6.04 Mops/s** | 2.81 Mops/s | **6.25 Mops/s** | 3.24 Mops/s |
| 16 | 8.00 Mops/s | **11.46 Mops/s** | **8.41 Mops/s** | 3.90 Mops/s | **8.76 Mops/s** | 4.79 Mops/s |
| 32 | 12.72 Mops/s | **16.56 Mops/s** | **13.38 Mops/s** | 6.40 Mops/s | **14.01 Mops/s** | 7.33 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | **2.26 µs** | 2.36 µs | **2.43 µs** | 13.71 µs | **2.49 µs** | 12.59 µs |
| 4 | 5.48 µs | **5.27 µs** | **5.99 µs** | 28.74 µs | **5.80 µs** | 26.10 µs |
| 8 | 17.89 µs | **11.63 µs** | **15.61 µs** | 56.40 µs | **15.05 µs** | 51.23 µs |
| 16 | 41.71 µs | **31.53 µs** | **39.75 µs** | 100.06 µs | **39.57 µs** | 78.38 µs |
| 32 | 79.73 µs | **63.31 µs** | **76.55 µs** | 213.81 µs | **69.22 µs** | 140.62 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.023 s |
| 2 | 0.023 s |
| 4 | 0.025 s |
| 8 | 0.027 s |
| 16 | 0.029 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.297 s |
| 2 | 0.149 s |
| 4 | 0.093 s |
| 8 | 0.064 s |
| 16 | 0.058 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 3.003 s |
| 2 | 1.583 s |
| 4 | 0.914 s |
| 8 | 0.578 s |
| 16 | 0.506 s |


# Optimistic Concurrency — CAS Benchmark

> Concurrent read-modify-write (increment) on shared stock counters. ByteCaskDB uses `WritePlan` + `apply_batch` with snapshot-based conflict detection; RocksDB uses `OptimisticTransactionDB`. Each iteration is one successful CAS — retries on conflict are included in wall-clock time. Both databases are pre-populated with 1M background keys.

## 100 stock items (high contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 101.0 Kops/s | **154.0 Kops/s** | 38.05 µs | 24.68 µs | 60.85 µs | 47.81 µs | 1.00 | 1.01 |
| 4 | 153.5 Kops/s | **229.6 Kops/s** | 94.34 µs | 65.62 µs | 213.29 µs | 148.27 µs | 1.02 | 1.02 |
| 8 | 205.8 Kops/s | **293.3 Kops/s** | 263.39 µs | 204.63 µs | 584.55 µs | 480.63 µs | 1.04 | 1.05 |
| 16 | **231.4 Kops/s** | 183.4 Kops/s | 975.49 µs | 1.26 ms | 2.76 ms | 4.17 ms | 1.09 | 1.11 |
| 32 | **283.6 Kops/s** | 187.3 Kops/s | 2.69 ms | 4.76 ms | 11.62 ms | 13.12 ms | 1.16 | 1.21 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | **153.0 ops/s** | 137.7 ops/s | 25.95 ms | 26.81 ms | 32.69 ms | 66.68 ms | 1.00 | 1.03 |
| 4 | 262.8 ops/s | **272.3 ops/s** | 50.50 ms | 62.09 ms | 64.74 ms | 84.48 ms | 1.00 | 1.01 |
| 8 | **482.4 ops/s** | 449.7 ops/s | 109.83 ms | 147.80 ms | 164.71 ms | 153.98 ms | 1.00 | 1.00 |
| 16 | 476.3 ops/s | **728.8 ops/s** | 537.75 ms | 294.43 ms | 537.75 ms | 294.43 ms | 1.12 | 1.12 |
| 32 | 889.7 ops/s | **1.2 Kops/s** | 1151.10 ms | 837.99 ms | 1151.10 ms | 837.99 ms | 1.34 | 1.33 |

## 10,000 stock items (low contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 97.2 Kops/s | **153.6 Kops/s** | 40.05 µs | 25.02 µs | 65.61 µs | 39.71 µs | 1.00 | 1.00 |
| 4 | 114.8 Kops/s | **217.1 Kops/s** | 108.76 µs | 72.20 µs | 485.01 µs | 108.16 µs | 1.00 | 1.00 |
| 8 | 167.1 Kops/s | **302.7 Kops/s** | 326.75 µs | 203.92 µs | 536.64 µs | 347.80 µs | 1.00 | 1.00 |
| 16 | 194.9 Kops/s | **264.2 Kops/s** | 1.22 ms | 957.64 µs | 2.17 ms | 1.91 ms | 1.00 | 1.00 |
| 32 | **226.2 Kops/s** | 217.8 Kops/s | 3.94 ms | 4.51 ms | 9.03 ms | 7.56 ms | 1.00 | 1.00 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 150.2 ops/s | **161.4 ops/s** | 25.84 ms | 22.76 ms | 38.33 ms | 69.21 ms | 1.00 | 1.00 |
| 4 | 252.4 ops/s | **291.6 ops/s** | 54.43 ms | 48.30 ms | 70.88 ms | 85.57 ms | 1.00 | 1.00 |
| 8 | 340.2 ops/s | **506.9 ops/s** | 199.11 ms | 114.72 ms | 199.11 ms | 150.00 ms | 1.00 | 1.00 |
| 16 | 499.4 ops/s | **827.9 ops/s** | 512.67 ms | 276.43 ms | 512.67 ms | 276.43 ms | 1.00 | 1.00 |
| 32 | 987.2 ops/s | **1.7 Kops/s** | 1037.50 ms | 586.36 ms | 1037.50 ms | 586.36 ms | 1.03 | 1.03 |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-24_