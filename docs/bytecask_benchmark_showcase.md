# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | April 10, 2026  16:36:20 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `8e8b01a` |
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

Sequential read speed : 504MiB/s
Sequential write speed: 423MiB/s

==========================================
```

## Methodology

- **Repetitions:** 3 runs per benchmark; mean reported.
- **Value size:** 245 bytes of random, incompressible data per entry.
- **Key shape:** UUIDv7-like with 5 prefixes — `user::`, `order::`, `session::`, `invoice::`, `product::`.
- **CRC on reads:** Disabled for Get, Range, and MixedBatch benchmarks. **Enabled** for recovery benchmarks — recovery validates every byte on disk.
- **NoSync:** writes are flushed to the OS page cache; no `fdatasync` call is made.
- **Sync:** every write calls `fdatasync` before returning.
- Throughput is expressed as ops/second (Mops/s = millions of ops/second, Kops/s = thousands). Wall-clock time is used (`UseRealTime`).
- Each engine is opened in a fresh, empty temporary directory per benchmark fixture.

---

# Throughput Comparison — ByteCaskDB vs RocksDB

## 50k Keys (50,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 162.1 Kops/s | **172.3 Kops/s** | **0.94×** |
| Put (Sync) | 485.7 ops/s | **489.5 ops/s** | **0.99×** |
| Get | 1.33 Mops/s | **1.57 Mops/s** | **0.84×** |
| Del (Sync) | **613.4 ops/s** | 477.9 ops/s | **1.28×** |
| Range-50 | 28.0 K scans/s | **171.8 K scans/s** | **0.16×** |
| MixedBatch/Sync | 36.0 Kops/s | **39.4 Kops/s** | **0.91×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 700 ns | 590 ns |
| p99 | 1.07 µs | 990 ns |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.42 Mops/s | **3.13 Mops/s** | **0.77×** |
| 4 | 4.42 Mops/s | **5.63 Mops/s** | **0.79×** |
| 8 | 5.91 Mops/s | **9.98 Mops/s** | **0.59×** |
| 16 | 8.68 Mops/s | **13.75 Mops/s** | **0.63×** |
| 32 | 12.73 Mops/s | **16.08 Mops/s** | **0.79×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 478.6 ops/s | **691.4 ops/s** | **0.69×** |
| 4 | **882.6 ops/s** | 762.2 ops/s | **1.16×** |
| 8 | **1.8 Kops/s** | 818.6 ops/s | **2.18×** |
| 16 | **4.6 Kops/s** | 796.8 ops/s | **5.78×** |
| 32 | **10.1 Kops/s** | 2.1 Kops/s | **4.92×** |
| 64 | **18.6 Kops/s** | 5.5 Kops/s | **3.42×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCaskDB read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCaskDB | ByteCaskDB BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.39 Mops/s | 2.58 Mops/s | **3.20 Mops/s** |
| 4 | 4.47 Mops/s | 4.58 Mops/s | **5.88 Mops/s** |
| 8 | 5.69 Mops/s | 5.60 Mops/s | **10.87 Mops/s** |
| 16 | 8.44 Mops/s | 8.95 Mops/s | **14.92 Mops/s** |
| 32 | 11.95 Mops/s | 12.24 Mops/s | **26.38 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 147.6 Kops/s | **167.1 Kops/s** | **0.88×** |
| Put (Sync) | **473.0 ops/s** | 463.6 ops/s | **1.02×** |
| Get | **1.36 Mops/s** | 491.4 Kops/s | **2.76×** |
| Del (Sync) | **673.0 ops/s** | 448.4 ops/s | **1.50×** |
| Range-50 | 29.5 K scans/s | **88.0 K scans/s** | **0.34×** |
| MixedBatch/Sync | 34.8 Kops/s | **40.2 Kops/s** | **0.87×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 680 ns | 1.90 µs |
| p99 | 1.04 µs | 4.42 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.57 Mops/s** | 856.0 Kops/s | **3.00×** |
| 4 | **4.51 Mops/s** | 1.59 Mops/s | **2.83×** |
| 8 | **7.21 Mops/s** | 3.75 Mops/s | **1.92×** |
| 16 | **9.96 Mops/s** | 5.03 Mops/s | **1.98×** |
| 32 | **15.47 Mops/s** | 8.36 Mops/s | **1.85×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 544.8 ops/s | **731.6 ops/s** | **0.74×** |
| 4 | 979.0 ops/s | **1.0 Kops/s** | **0.96×** |
| 8 | **1.9 Kops/s** | 1.6 Kops/s | **1.19×** |
| 16 | **4.8 Kops/s** | 1.6 Kops/s | **3.04×** |
| 32 | **10.0 Kops/s** | 2.2 Kops/s | **4.63×** |
| 64 | **16.1 Kops/s** | 5.4 Kops/s | **2.98×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCaskDB read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCaskDB | ByteCaskDB BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | **2.57 Mops/s** | 2.50 Mops/s | 794.0 Kops/s |
| 4 | 4.13 Mops/s | **4.45 Mops/s** | 1.46 Mops/s |
| 8 | 6.26 Mops/s | **6.46 Mops/s** | 3.28 Mops/s |
| 16 | 8.74 Mops/s | **10.17 Mops/s** | 4.92 Mops/s |
| 32 | 11.80 Mops/s | **13.73 Mops/s** | 8.05 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 154.8 Kops/s | **180.8 Kops/s** | **0.86×** |
| Put (Sync) | 465.7 ops/s | **475.5 ops/s** | **0.98×** |
| Get | **1.36 Mops/s** | 508.4 Kops/s | **2.68×** |
| Del (Sync) | **645.1 ops/s** | 379.7 ops/s | **1.70×** |
| Range-50 | 29.6 K scans/s | **83.4 K scans/s** | **0.35×** |
| MixedBatch/Sync | 34.4 Kops/s | **36.8 Kops/s** | **0.93×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 680 ns | 1.97 µs |
| p99 | 1.03 µs | 4.25 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.58 Mops/s** | 1.01 Mops/s | **2.54×** |
| 4 | **4.30 Mops/s** | 1.99 Mops/s | **2.16×** |
| 8 | **6.27 Mops/s** | 4.06 Mops/s | **1.54×** |
| 16 | **8.99 Mops/s** | 6.45 Mops/s | **1.39×** |
| 32 | **14.31 Mops/s** | 8.98 Mops/s | **1.59×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 477.1 ops/s | **689.9 ops/s** | **0.69×** |
| 4 | 930.6 ops/s | **1.2 Kops/s** | **0.80×** |
| 8 | **1.8 Kops/s** | 1.2 Kops/s | **1.47×** |
| 16 | **4.2 Kops/s** | 1.2 Kops/s | **3.49×** |
| 32 | **9.6 Kops/s** | 2.3 Kops/s | **4.20×** |
| 64 | **16.5 Kops/s** | 5.4 Kops/s | **3.08×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCaskDB read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCaskDB | ByteCaskDB BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.44 Mops/s | **2.56 Mops/s** | 1.09 Mops/s |
| 4 | 4.53 Mops/s | **4.61 Mops/s** | 2.03 Mops/s |
| 8 | 6.76 Mops/s | **7.12 Mops/s** | 4.22 Mops/s |
| 16 | 9.98 Mops/s | **10.69 Mops/s** | 6.63 Mops/s |
| 32 | 15.71 Mops/s | **15.99 Mops/s** | 9.62 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.42 Mops/s | **3.13 Mops/s** | **2.57 Mops/s** | 856.0 Kops/s | **2.58 Mops/s** | 1.01 Mops/s |
| 4 | 4.42 Mops/s | **5.63 Mops/s** | **4.51 Mops/s** | 1.59 Mops/s | **4.30 Mops/s** | 1.99 Mops/s |
| 8 | 5.91 Mops/s | **9.98 Mops/s** | **7.21 Mops/s** | 3.75 Mops/s | **6.27 Mops/s** | 4.06 Mops/s |
| 16 | 8.68 Mops/s | **13.75 Mops/s** | **9.96 Mops/s** | 5.03 Mops/s | **8.99 Mops/s** | 6.45 Mops/s |
| 32 | 12.73 Mops/s | **16.08 Mops/s** | **15.47 Mops/s** | 8.36 Mops/s | **14.31 Mops/s** | 8.98 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.26 µs | **1.88 µs** | **2.26 µs** | 10.25 µs | **2.28 µs** | 9.23 µs |
| 4 | 5.03 µs | **4.57 µs** | **5.28 µs** | 23.47 µs | **5.54 µs** | 20.40 µs |
| 8 | 15.89 µs | **10.19 µs** | **12.87 µs** | 46.46 µs | **15.25 µs** | 46.94 µs |
| 16 | 38.88 µs | **23.54 µs** | **31.87 µs** | 64.61 µs | **36.46 µs** | 61.14 µs |
| 32 | 75.81 µs | **45.99 µs** | **63.71 µs** | 107.53 µs | **76.68 µs** | 91.68 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.017 s |
| 2 | 0.018 s |
| 4 | 0.020 s |
| 8 | 0.022 s |
| 16 | 0.023 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.257 s |
| 2 | 0.147 s |
| 4 | 0.094 s |
| 8 | 0.075 s |
| 16 | 0.063 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 2.736 s |
| 2 | 1.676 s |
| 4 | 1.022 s |
| 8 | 0.719 s |
| 16 | 0.631 s |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-10_