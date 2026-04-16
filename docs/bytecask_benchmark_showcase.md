# ByteCaskDB Benchmark Showcase

| | |
|---|---|
| **Date** | April 16, 2026  10:48:57 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `8fda9c1` |
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

Sequential read speed : 485MiB/s
Sequential write speed: 475MiB/s

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
| Put (NoSync) | 147.0 Kops/s | **175.2 Kops/s** | **0.84×** |
| Put (Sync) | **507.8 ops/s** | 496.3 ops/s | **1.02×** |
| Get | 1.32 Mops/s | **1.62 Mops/s** | **0.81×** |
| Del (Sync) | **674.0 ops/s** | 482.4 ops/s | **1.40×** |
| Range-50 | 30.0 K scans/s | **174.9 K scans/s** | **0.17×** |
| MixedBatch/Sync | **43.9 Kops/s** | 42.6 Kops/s | **1.03×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 720 ns | 578 ns |
| p99 | 932 ns | 894 ns |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.36 Mops/s | **3.10 Mops/s** | **0.76×** |
| 4 | 4.36 Mops/s | **5.66 Mops/s** | **0.77×** |
| 8 | 5.39 Mops/s | **10.77 Mops/s** | **0.50×** |
| 16 | 7.95 Mops/s | **15.47 Mops/s** | **0.51×** |
| 32 | 12.50 Mops/s | **22.10 Mops/s** | **0.57×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 502.0 ops/s | **705.7 ops/s** | **0.71×** |
| 4 | 911.6 ops/s | **1.2 Kops/s** | **0.77×** |
| 8 | **1.8 Kops/s** | 1.8 Kops/s | **1.01×** |
| 16 | **4.7 Kops/s** | 1.3 Kops/s | **3.54×** |
| 32 | **10.9 Kops/s** | 2.4 Kops/s | **4.57×** |
| 64 | **19.3 Kops/s** | 5.6 Kops/s | **3.44×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.25 Mops/s | **3.19 Mops/s** |
| 4 | 4.13 Mops/s | **5.81 Mops/s** |
| 8 | 5.67 Mops/s | **11.05 Mops/s** |
| 16 | 8.33 Mops/s | **15.51 Mops/s** |
| 32 | 13.40 Mops/s | **27.26 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 146.7 Kops/s | **174.6 Kops/s** | **0.84×** |
| Put (Sync) | 490.9 ops/s | **520.5 ops/s** | **0.94×** |
| Get | **1.33 Mops/s** | 495.3 Kops/s | **2.69×** |
| Del (Sync) | **617.0 ops/s** | 496.5 ops/s | **1.24×** |
| Range-50 | 29.6 K scans/s | **89.2 K scans/s** | **0.33×** |
| MixedBatch/Sync | **42.2 Kops/s** | 42.1 Kops/s | **1.00×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 708 ns | 1.87 µs |
| p99 | 926 ns | 4.42 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.41 Mops/s** | 844.5 Kops/s | **2.85×** |
| 4 | **4.40 Mops/s** | 1.65 Mops/s | **2.66×** |
| 8 | **7.22 Mops/s** | 3.86 Mops/s | **1.87×** |
| 16 | **9.78 Mops/s** | 5.69 Mops/s | **1.72×** |
| 32 | **14.83 Mops/s** | 8.29 Mops/s | **1.79×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 511.8 ops/s | **713.7 ops/s** | **0.72×** |
| 4 | 1.1 Kops/s | **1.2 Kops/s** | **0.94×** |
| 8 | **2.0 Kops/s** | 1.9 Kops/s | **1.05×** |
| 16 | **4.6 Kops/s** | 1.4 Kops/s | **3.29×** |
| 32 | **9.7 Kops/s** | 2.2 Kops/s | **4.41×** |
| 64 | **18.7 Kops/s** | 5.3 Kops/s | **3.50×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.38 Mops/s** | 729.4 Kops/s |
| 4 | **4.11 Mops/s** | 1.46 Mops/s |
| 8 | **6.43 Mops/s** | 3.54 Mops/s |
| 16 | **9.19 Mops/s** | 5.30 Mops/s |
| 32 | **14.53 Mops/s** | 7.87 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 148.7 Kops/s | **178.6 Kops/s** | **0.83×** |
| Put (Sync) | **498.1 ops/s** | 463.0 ops/s | **1.08×** |
| Get | **1.34 Mops/s** | 566.3 Kops/s | **2.37×** |
| Del (Sync) | **677.6 ops/s** | 273.3 ops/s | **2.48×** |
| Range-50 | 29.7 K scans/s | **82.4 K scans/s** | **0.36×** |
| MixedBatch/Sync | **42.4 Kops/s** | 35.0 Kops/s | **1.21×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCaskDB | RocksDB |
|---|---:|---:|
| p50 | 702 ns | 1.53 µs |
| p99 | 920 ns | 4.18 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.45 Mops/s** | 816.3 Kops/s | **3.00×** |
| 4 | **4.49 Mops/s** | 1.96 Mops/s | **2.29×** |
| 8 | **7.42 Mops/s** | 4.19 Mops/s | **1.77×** |
| 16 | **10.33 Mops/s** | 6.70 Mops/s | **1.54×** |
| 32 | **15.32 Mops/s** | 9.70 Mops/s | **1.58×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | 495.9 ops/s | **751.0 ops/s** | **0.66×** |
| 4 | 986.2 ops/s | **1.0 Kops/s** | **0.95×** |
| 8 | 1.8 Kops/s | **1.9 Kops/s** | **0.95×** |
| 16 | **4.6 Kops/s** | 1.3 Kops/s | **3.58×** |
| 32 | **8.9 Kops/s** | 2.3 Kops/s | **3.88×** |
| 64 | **16.6 Kops/s** | 5.4 Kops/s | **3.08×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | **2.44 Mops/s** | 761.1 Kops/s |
| 4 | **4.36 Mops/s** | 1.88 Mops/s |
| 8 | **6.55 Mops/s** | 4.07 Mops/s |
| 16 | **9.43 Mops/s** | 6.50 Mops/s |
| 32 | **15.86 Mops/s** | 10.03 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCaskDB's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.36 Mops/s | **3.10 Mops/s** | **2.41 Mops/s** | 844.5 Kops/s | **2.45 Mops/s** | 816.3 Kops/s |
| 4 | 4.36 Mops/s | **5.66 Mops/s** | **4.40 Mops/s** | 1.65 Mops/s | **4.49 Mops/s** | 1.96 Mops/s |
| 8 | 5.39 Mops/s | **10.77 Mops/s** | **7.22 Mops/s** | 3.86 Mops/s | **7.42 Mops/s** | 4.19 Mops/s |
| 16 | 7.95 Mops/s | **15.47 Mops/s** | **9.78 Mops/s** | 5.69 Mops/s | **10.33 Mops/s** | 6.70 Mops/s |
| 32 | 12.50 Mops/s | **22.10 Mops/s** | **14.83 Mops/s** | 8.29 Mops/s | **15.32 Mops/s** | 9.70 Mops/s |

### p99 Read Latency

| Threads | BCDB 50k | RDB 50k | BCDB 500k | RDB 500k | BCDB 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.27 µs | **1.94 µs** | **2.29 µs** | 10.82 µs | **2.31 µs** | 11.86 µs |
| 4 | 5.39 µs | **4.30 µs** | **5.23 µs** | 21.88 µs | **4.94 µs** | 20.89 µs |
| 8 | 18.02 µs | **9.56 µs** | **12.91 µs** | 44.30 µs | **12.29 µs** | 42.69 µs |
| 16 | 41.72 µs | **23.28 µs** | **32.03 µs** | 62.39 µs | **31.14 µs** | 50.46 µs |
| 32 | 79.62 µs | **46.27 µs** | **63.14 µs** | 101.93 µs | **56.90 µs** | 91.03 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.016 s |
| 2 | 0.020 s |
| 4 | 0.022 s |
| 8 | 0.025 s |
| 16 | 0.027 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.239 s |
| 2 | 0.139 s |
| 4 | 0.085 s |
| 8 | 0.065 s |
| 16 | 0.057 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 2.419 s |
| 2 | 1.520 s |
| 4 | 0.906 s |
| 8 | 0.621 s |
| 16 | 0.528 s |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-16_