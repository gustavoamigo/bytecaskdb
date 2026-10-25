# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | October 07, 2026  22:41:53 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `e62d83f` |
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
Measured path : /mnt/bench
NAME MODEL                       SIZE FSTYPE TRAN
sda  Samsung SSD 860 EVO 500GB 465.8G        sata

Sequential read speed : 488MiB/s
Sequential write speed: 491MiB/s

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
| Put (NoSync) | 149.7 Kops/s | **177.8 Kops/s** | **0.84×** |
| Put (Sync) | **564.4 ops/s** | 165.5 ops/s | **3.41×** |
| Get | **5.11 Mops/s** | 1.28 Mops/s | **3.98×** |
| Del (Sync) | **560.2 ops/s** | 156.8 ops/s | **3.57×** |
| Range-50 | **382.9 K scans/s** | 117.2 K scans/s | **3.27×** |
| MixedBatch/Sync | **15.9 Kops/s** | 13.9 Kops/s | **1.14×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 154 ns | 738 ns |
| p99 | 340 ns | 1.12 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **8.93 Mops/s** | 2.63 Mops/s | **3.40×** |
| 4 | **18.16 Mops/s** | 4.77 Mops/s | **3.80×** |
| 8 | **36.43 Mops/s** | 9.40 Mops/s | **3.88×** |
| 16 | **44.72 Mops/s** | 11.61 Mops/s | **3.85×** |
| 32 | **73.85 Mops/s** | 20.43 Mops/s | **3.62×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **320.4 ops/s** | 254.0 ops/s | **1.26×** |
| 4 | **711.8 ops/s** | 433.2 ops/s | **1.64×** |
| 8 | **1.3 Kops/s** | 543.9 ops/s | **2.39×** |
| 16 | **1.6 Kops/s** | 870.6 ops/s | **1.85×** |
| 32 | **2.4 Kops/s** | 2.1 Kops/s | **1.13×** |
| 64 | **6.6 Kops/s** | 770.8 ops/s | **8.50×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **9.06 Mops/s** | 2.58 Mops/s |
| 4 | **18.38 Mops/s** | 4.87 Mops/s |
| 8 | **34.98 Mops/s** | 9.71 Mops/s |
| 16 | **47.64 Mops/s** | 11.87 Mops/s |
| 32 | **79.78 Mops/s** | 19.16 Mops/s |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 124.6 Kops/s | **204.4 Kops/s** | **0.61×** |
| Put (Sync) | 152.6 ops/s | **160.3 ops/s** | **0.95×** |
| Get | **4.80 Mops/s** | 316.9 Kops/s | **15.14×** |
| Del (Sync) | 121.9 ops/s | **155.5 ops/s** | **0.78×** |
| Range-50 | **436.9 K scans/s** | 63.3 K scans/s | **6.90×** |
| MixedBatch/Sync | **15.1 Kops/s** | 10.1 Kops/s | **1.50×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 166 ns | 3.00 µs |
| p99 | 378 ns | 6.00 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **9.08 Mops/s** | 670.5 Kops/s | **13.54×** |
| 4 | **17.10 Mops/s** | 1.30 Mops/s | **13.11×** |
| 8 | **33.21 Mops/s** | 3.05 Mops/s | **10.89×** |
| 16 | **44.65 Mops/s** | 4.45 Mops/s | **10.04×** |
| 32 | **67.88 Mops/s** | 6.91 Mops/s | **9.82×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **290.5 ops/s** | 256.9 ops/s | **1.13×** |
| 4 | **596.6 ops/s** | 336.2 ops/s | **1.77×** |
| 8 | **1.2 Kops/s** | 831.7 ops/s | **1.41×** |
| 16 | **1.8 Kops/s** | 816.4 ops/s | **2.22×** |
| 32 | **3.3 Kops/s** | 1.8 Kops/s | **1.86×** |
| 64 | **5.7 Kops/s** | 3.5 Kops/s | **1.64×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **9.36 Mops/s** | 626.4 Kops/s |
| 4 | **17.67 Mops/s** | 1.18 Mops/s |
| 8 | **33.13 Mops/s** | 2.87 Mops/s |
| 16 | **44.80 Mops/s** | 4.23 Mops/s |
| 32 | **71.91 Mops/s** | 6.34 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 99.0 Kops/s | **207.1 Kops/s** | **0.48×** |
| Put (Sync) | **161.8 ops/s** | 150.2 ops/s | **1.08×** |
| Get | **4.72 Mops/s** | 394.9 Kops/s | **11.95×** |
| Del (Sync) | **157.8 ops/s** | 1.3 ops/s | **123.06×** |
| Range-50 | **441.4 K scans/s** | 65.2 K scans/s | **6.77×** |
| MixedBatch/Sync | **14.6 Kops/s** | 11.7 Kops/s | **1.25×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 172 ns | 2.35 µs |
| p99 | 378 ns | 5.58 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **8.82 Mops/s** | 730.2 Kops/s | **12.08×** |
| 4 | **16.37 Mops/s** | 1.55 Mops/s | **10.57×** |
| 8 | **31.47 Mops/s** | 3.33 Mops/s | **9.46×** |
| 16 | **43.07 Mops/s** | 4.93 Mops/s | **8.74×** |
| 32 | **62.86 Mops/s** | 7.34 Mops/s | **8.56×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **296.8 ops/s** | 200.1 ops/s | **1.48×** |
| 4 | **602.0 ops/s** | 398.9 ops/s | **1.51×** |
| 8 | **1.3 Kops/s** | 609.5 ops/s | **2.09×** |
| 16 | **1.7 Kops/s** | 793.0 ops/s | **2.15×** |
| 32 | **3.3 Kops/s** | 1.8 Kops/s | **1.86×** |
| 64 | **8.4 Kops/s** | 3.5 Kops/s | **2.42×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **9.12 Mops/s** | 665.9 Kops/s |
| 4 | **17.46 Mops/s** | 1.27 Mops/s |
| 8 | **32.89 Mops/s** | 2.91 Mops/s |
| 16 | **42.96 Mops/s** | 4.47 Mops/s |
| 32 | **68.99 Mops/s** | 6.78 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | **8.93 Mops/s** | 2.63 Mops/s | **9.08 Mops/s** | 670.5 Kops/s | **8.82 Mops/s** | 730.2 Kops/s |
| 4 | **18.16 Mops/s** | 4.77 Mops/s | **17.10 Mops/s** | 1.30 Mops/s | **16.37 Mops/s** | 1.55 Mops/s |
| 8 | **36.43 Mops/s** | 9.40 Mops/s | **33.21 Mops/s** | 3.05 Mops/s | **31.47 Mops/s** | 3.33 Mops/s |
| 16 | **44.72 Mops/s** | 11.61 Mops/s | **44.65 Mops/s** | 4.45 Mops/s | **43.07 Mops/s** | 4.93 Mops/s |
| 32 | **73.85 Mops/s** | 20.43 Mops/s | **67.88 Mops/s** | 6.91 Mops/s | **62.86 Mops/s** | 7.34 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | **784 ns** | 2.24 µs | **786 ns** | 13.35 µs | **814 ns** | 13.58 µs |
| 4 | **1.47 µs** | 4.93 µs | **1.67 µs** | 27.56 µs | **1.76 µs** | 24.68 µs |
| 8 | **3.11 µs** | 11.07 µs | **3.64 µs** | 52.18 µs | **3.93 µs** | 50.39 µs |
| 16 | **9.33 µs** | 33.30 µs | **10.04 µs** | 109.67 µs | **10.92 µs** | 88.21 µs |
| 32 | **19.23 µs** | 64.13 µs | **21.04 µs** | 238.45 µs | **22.25 µs** | 152.54 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.037 s |
| 2 | 0.038 s |
| 4 | 0.036 s |
| 8 | 0.035 s |
| 16 | 0.036 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.262 s |
| 2 | 0.148 s |
| 4 | 0.097 s |
| 8 | 0.067 s |
| 16 | 0.058 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 2.572 s |
| 2 | 1.405 s |
| 4 | 0.816 s |
| 8 | 0.470 s |
| 16 | 0.616 s |


# Optimistic Concurrency — CAS Benchmark

> Concurrent read-modify-write (increment) on shared stock counters. ByteCaskDB uses `WritePlan` + `apply_batch` with snapshot-based conflict detection; RocksDB uses `OptimisticTransactionDB`. Each iteration is one successful CAS — retries on conflict are included in wall-clock time. Both databases are pre-populated with 1M background keys.

## 100 stock items (high contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 122.3 Kops/s | **182.4 Kops/s** | 30.59 µs | 20.72 µs | 59.09 µs | 43.14 µs | 1.01 | 1.01 |
| 4 | 151.1 Kops/s | **290.0 Kops/s** | 101.41 µs | 51.24 µs | 210.52 µs | 121.99 µs | 1.03 | 1.02 |
| 8 | 177.4 Kops/s | **346.7 Kops/s** | 336.15 µs | 167.08 µs | 730.32 µs | 428.80 µs | 1.05 | 1.05 |
| 16 | 185.0 Kops/s | **378.1 Kops/s** | 1.23 ms | 603.61 µs | 3.66 ms | 1.94 ms | 1.11 | 1.11 |
| 32 | 188.1 Kops/s | **201.0 Kops/s** | 4.20 ms | 4.56 ms | 18.56 ms | 13.34 ms | 1.20 | 1.21 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | **292.2 ops/s** | 169.3 ops/s | 13.44 ms | 22.91 ms | 19.48 ms | 33.28 ms | 1.00 | 1.02 |
| 4 | **572.7 ops/s** | 321.7 ops/s | 26.95 ms | 50.08 ms | 38.08 ms | 61.60 ms | 1.04 | 1.00 |
| 8 | **1.1 Kops/s** | 586.3 ops/s | 55.17 ms | 98.58 ms | 59.79 ms | 134.90 ms | 1.01 | 1.01 |
| 16 | **1.4 Kops/s** | 760.3 ops/s | 185.92 ms | 260.44 ms | 197.73 ms | 260.44 ms | 1.07 | 1.12 |
| 32 | **2.3 Kops/s** | 1.4 Kops/s | 273.51 ms | 745.56 ms | 273.51 ms | 745.56 ms | 1.36 | 1.33 |

## 10,000 stock items (low contention)

#### NoSync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 119.6 Kops/s | **190.9 Kops/s** | 31.59 µs | 20.18 µs | 53.22 µs | 32.92 µs | 1.00 | 1.00 |
| 4 | 144.8 Kops/s | **299.4 Kops/s** | 106.59 µs | 51.42 µs | 185.02 µs | 85.37 µs | 1.00 | 1.00 |
| 8 | 168.2 Kops/s | **373.5 Kops/s** | 373.17 µs | 161.06 µs | 554.63 µs | 307.36 µs | 1.00 | 1.00 |
| 16 | 181.8 Kops/s | **391.0 Kops/s** | 1.38 ms | 617.49 µs | 1.98 ms | 1.44 ms | 1.00 | 1.00 |
| 32 | 183.2 Kops/s | **206.6 Kops/s** | 4.87 ms | 4.91 ms | 8.77 ms | 7.64 ms | 1.00 | 1.00 |

#### Sync

| Threads | ByteCaskDB | RocksDB | BCDB p50 | RDB p50 | BCDB p99 | RDB p99 | BCDB avg attempts | RDB avg attempts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | **318.5 ops/s** | 151.0 ops/s | 12.11 ms | 26.18 ms | 19.62 ms | 36.93 ms | 1.00 | 1.00 |
| 4 | **625.7 ops/s** | 318.3 ops/s | 23.95 ms | 49.13 ms | 33.66 ms | 64.19 ms | 1.00 | 1.00 |
| 8 | **1.2 Kops/s** | 490.5 ops/s | 47.84 ms | 132.23 ms | 69.49 ms | 149.53 ms | 1.00 | 1.00 |
| 16 | **1.6 Kops/s** | 926.1 ops/s | 183.98 ms | 286.99 ms | 186.82 ms | 286.99 ms | 1.00 | 1.00 |
| 32 | **3.2 Kops/s** | 1.8 Kops/s | 359.72 ms | 570.65 ms | 359.72 ms | 570.65 ms | 1.01 | 1.03 |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-10-07_