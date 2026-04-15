# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | April 15, 2026  23:28:21 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `173b9e9` |
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

Sequential read speed : 484MiB/s
Sequential write speed: 456MiB/s

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

---

# Throughput Comparison — ByteCaskDB vs RocksDB

## 50k Keys (50,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 142.3 Kops/s | **174.0 Kops/s** | **0.82×** |
| Put (Sync) | **483.2 ops/s** | 470.2 ops/s | **1.03×** |
| Get | 1.22 Mops/s | **1.55 Mops/s** | **0.79×** |
| Del (Sync) | **595.9 ops/s** | 491.5 ops/s | **1.21×** |
| Range-50 | 26.9 K scans/s | **176.4 K scans/s** | **0.15×** |
| MixedBatch/Sync | **42.3 Kops/s** | 39.0 Kops/s | **1.09×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 776 ns | 604 ns |
| p99 | 1.10 µs | 940 ns |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.22 Mops/s | **3.18 Mops/s** | **0.70×** |
| 4 | 3.87 Mops/s | **5.78 Mops/s** | **0.67×** |
| 8 | 5.07 Mops/s | **10.17 Mops/s** | **0.50×** |
| 16 | 7.09 Mops/s | **14.29 Mops/s** | **0.50×** |
| 32 | 10.88 Mops/s | **19.62 Mops/s** | **0.55×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 484.8 ops/s | **704.0 ops/s** | **0.69×** |
| 4 | 950.6 ops/s | **1.1 Kops/s** | **0.87×** |
| 8 | **1.9 Kops/s** | 1.6 Kops/s | **1.17×** |
| 16 | **4.2 Kops/s** | 2.0 Kops/s | **2.12×** |
| 32 | **10.1 Kops/s** | 2.1 Kops/s | **4.77×** |
| 64 | **18.7 Kops/s** | 5.5 Kops/s | **3.41×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.15 Mops/s | **3.20 Mops/s** |
| 4 | 3.72 Mops/s | **5.78 Mops/s** |
| 8 | 5.23 Mops/s | **11.40 Mops/s** |
| 16 | 7.33 Mops/s | **14.66 Mops/s** |
| 32 | 11.46 Mops/s | **22.09 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 143.3 Kops/s | **176.9 Kops/s** | **0.81×** |
| Put (Sync) | 482.2 ops/s | **507.6 ops/s** | **0.95×** |
| Get | **1.13 Mops/s** | 514.7 Kops/s | **2.20×** |
| Del (Sync) | **662.2 ops/s** | 472.9 ops/s | **1.40×** |
| Range-50 | 25.4 K scans/s | **91.0 K scans/s** | **0.28×** |
| MixedBatch/Sync | **41.0 Kops/s** | 40.4 Kops/s | **1.01×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 834 ns | 1.76 µs |
| p99 | 1.18 µs | 4.38 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.08 Mops/s** | 840.0 Kops/s | **2.48×** |
| 4 | **3.30 Mops/s** | 1.62 Mops/s | **2.04×** |
| 8 | **5.06 Mops/s** | 3.87 Mops/s | **1.31×** |
| 16 | **7.10 Mops/s** | 5.85 Mops/s | **1.21×** |
| 32 | **10.64 Mops/s** | 7.55 Mops/s | **1.41×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 530.7 ops/s | **728.4 ops/s** | **0.73×** |
| 4 | 879.3 ops/s | **1.2 Kops/s** | **0.72×** |
| 8 | **1.8 Kops/s** | 1.4 Kops/s | **1.30×** |
| 16 | **4.4 Kops/s** | 1.4 Kops/s | **3.06×** |
| 32 | **10.0 Kops/s** | 2.1 Kops/s | **4.72×** |
| 64 | **18.2 Kops/s** | 5.4 Kops/s | **3.39×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.18 Mops/s** | 804.8 Kops/s |
| 4 | **3.55 Mops/s** | 1.51 Mops/s |
| 8 | **4.81 Mops/s** | 3.61 Mops/s |
| 16 | **6.49 Mops/s** | 5.53 Mops/s |
| 32 | **9.57 Mops/s** | 8.81 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 145.8 Kops/s | **170.6 Kops/s** | **0.85×** |
| Put (Sync) | **477.2 ops/s** | 465.9 ops/s | **1.02×** |
| Get | **1.22 Mops/s** | 548.3 Kops/s | **2.22×** |
| Del (Sync) | **667.1 ops/s** | 290.6 ops/s | **2.30×** |
| Range-50 | 27.8 K scans/s | **82.6 K scans/s** | **0.34×** |
| MixedBatch/Sync | **41.4 Kops/s** | 32.3 Kops/s | **1.28×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 782 ns | 1.63 µs |
| p99 | 1.13 µs | 4.26 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.11 Mops/s** | 893.5 Kops/s | **2.37×** |
| 4 | **3.69 Mops/s** | 2.06 Mops/s | **1.79×** |
| 8 | **5.64 Mops/s** | 4.27 Mops/s | **1.32×** |
| 16 | **7.57 Mops/s** | 6.72 Mops/s | **1.13×** |
| 32 | **10.85 Mops/s** | 8.76 Mops/s | **1.24×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 481.9 ops/s | **710.0 ops/s** | **0.68×** |
| 4 | 954.8 ops/s | **1.1 Kops/s** | **0.89×** |
| 8 | **1.8 Kops/s** | 1.8 Kops/s | **1.02×** |
| 16 | **4.0 Kops/s** | 1.6 Kops/s | **2.52×** |
| 32 | **9.5 Kops/s** | 2.1 Kops/s | **4.46×** |
| 64 | **18.7 Kops/s** | 5.4 Kops/s | **3.49×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.20 Mops/s** | 1.05 Mops/s |
| 4 | **3.59 Mops/s** | 2.03 Mops/s |
| 8 | **5.37 Mops/s** | 4.24 Mops/s |
| 16 | **7.35 Mops/s** | 6.31 Mops/s |
| 32 | **10.75 Mops/s** | 10.27 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.22 Mops/s | **3.18 Mops/s** | **2.08 Mops/s** | 840.0 Kops/s | **2.11 Mops/s** | 893.5 Kops/s |
| 4 | 3.87 Mops/s | **5.78 Mops/s** | **3.30 Mops/s** | 1.62 Mops/s | **3.69 Mops/s** | 2.06 Mops/s |
| 8 | 5.07 Mops/s | **10.17 Mops/s** | **5.06 Mops/s** | 3.87 Mops/s | **5.64 Mops/s** | 4.27 Mops/s |
| 16 | 7.09 Mops/s | **14.29 Mops/s** | **7.10 Mops/s** | 5.85 Mops/s | **7.57 Mops/s** | 6.72 Mops/s |
| 32 | 10.88 Mops/s | **19.62 Mops/s** | **10.64 Mops/s** | 7.55 Mops/s | **10.85 Mops/s** | 8.76 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.46 µs | **1.83 µs** | **3.09 µs** | 10.85 µs | **3.49 µs** | 11.15 µs |
| 4 | 6.16 µs | **4.22 µs** | **8.42 µs** | 22.14 µs | **7.93 µs** | 19.88 µs |
| 8 | 19.16 µs | **9.72 µs** | **19.71 µs** | 44.82 µs | **18.14 µs** | 42.15 µs |
| 16 | 49.12 µs | **22.66 µs** | **48.25 µs** | 54.90 µs | **49.62 µs** | 49.92 µs |
| 32 | 96.29 µs | **43.22 µs** | **100.86 µs** | 105.10 µs | 103.75 µs | **85.18 µs** |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.017 s |
| 2 | 0.020 s |
| 4 | 0.022 s |
| 8 | 0.025 s |
| 16 | 0.027 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.249 s |
| 2 | 0.139 s |
| 4 | 0.086 s |
| 8 | 0.061 s |
| 16 | 0.056 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 2.482 s |
| 2 | 1.508 s |
| 4 | 0.891 s |
| 8 | 0.576 s |
| 16 | 0.513 s |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-15_