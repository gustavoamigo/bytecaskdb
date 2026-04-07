# ByteCask Benchmark Showcase

| | |
|---|---|
| **Date** | April 07, 2026  14:22:26 |
| **Host** | `linuxpc` |
| **CPUs** | 16 × 4427 MHz |
| **Git commit** | `bd497cd` |
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

Sequential read speed : 483MiB/s
Sequential write speed: 448MiB/s

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

# Throughput Comparison — ByteCask vs RocksDB

## 50k Keys (50,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCask | RocksDB | ByteCask / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | **163.3 Kops/s** | 163.1 Kops/s | **1.00×** |
| Put (Sync) | **480.7 ops/s** | 476.5 ops/s | **1.01×** |
| Get | 1.26 Mops/s | **1.50 Mops/s** | **0.84×** |
| Del (Sync) | **614.3 ops/s** | 467.6 ops/s | **1.31×** |
| Range-50 | 28.6 K scans/s | **159.7 K scans/s** | **0.18×** |
| MixedBatch/Sync | 36.3 Kops/s | **39.4 Kops/s** | **0.92×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCask | RocksDB |
|---|---:|---:|
| p50 | 723 ns | 613 ns |
| p99 | 1.21 µs | 1.06 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | 2.40 Mops/s | **3.16 Mops/s** | **0.76×** |
| 4 | 4.42 Mops/s | **5.69 Mops/s** | **0.78×** |
| 8 | 5.28 Mops/s | **11.39 Mops/s** | **0.46×** |
| 16 | 7.77 Mops/s | **14.10 Mops/s** | **0.55×** |
| 32 | 10.91 Mops/s | **18.14 Mops/s** | **0.60×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | 542.4 ops/s | **660.3 ops/s** | **0.82×** |
| 4 | 879.0 ops/s | **1.2 Kops/s** | **0.76×** |
| 8 | 1.6 Kops/s | **2.0 Kops/s** | **0.79×** |
| 16 | **1.7 Kops/s** | 1.2 Kops/s | **1.39×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCask read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCask | ByteCask BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.52 Mops/s | 2.58 Mops/s | **3.06 Mops/s** |
| 4 | 4.06 Mops/s | 4.17 Mops/s | **5.55 Mops/s** |
| 8 | 5.35 Mops/s | 5.56 Mops/s | **9.55 Mops/s** |
| 16 | 7.55 Mops/s | 7.96 Mops/s | **15.09 Mops/s** |

---
## 500k Keys (500,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCask | RocksDB | ByteCask / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 158.5 Kops/s | **170.1 Kops/s** | **0.93×** |
| Put (Sync) | **455.6 ops/s** | 448.4 ops/s | **1.02×** |
| Get | **1.36 Mops/s** | 440.3 Kops/s | **3.09×** |
| Del (Sync) | **621.7 ops/s** | 454.3 ops/s | **1.37×** |
| Range-50 | 29.8 K scans/s | **82.2 K scans/s** | **0.36×** |
| MixedBatch/Sync | 35.5 Kops/s | **39.7 Kops/s** | **0.89×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCask | RocksDB |
|---|---:|---:|
| p50 | 670 ns | 2.15 µs |
| p99 | 1.11 µs | 4.52 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.58 Mops/s** | 895.5 Kops/s | **2.88×** |
| 4 | **4.21 Mops/s** | 1.71 Mops/s | **2.46×** |
| 8 | **6.01 Mops/s** | 4.10 Mops/s | **1.46×** |
| 16 | **8.42 Mops/s** | 5.61 Mops/s | **1.50×** |
| 32 | **12.97 Mops/s** | 8.05 Mops/s | **1.61×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | 471.1 ops/s | **731.7 ops/s** | **0.64×** |
| 4 | 408.8 ops/s | **1.3 Kops/s** | **0.32×** |
| 8 | 1.5 Kops/s | **2.5 Kops/s** | **0.58×** |
| 16 | **1.7 Kops/s** | 1.6 Kops/s | **1.06×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCask read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCask | ByteCask BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.51 Mops/s | **2.56 Mops/s** | 837.4 Kops/s |
| 4 | 4.14 Mops/s | **4.24 Mops/s** | 1.48 Mops/s |
| 8 | 5.97 Mops/s | **6.26 Mops/s** | 3.59 Mops/s |
| 16 | 7.98 Mops/s | **9.05 Mops/s** | 5.10 Mops/s |

---
## 1M Keys (1,000,000)

### Single-Threaded Throughput

> **CRC verification is disabled** for read operations in this section (Get, Range-50, MixedBatch). Recovery benchmarks run with CRC **enabled**.

| Benchmark | ByteCask | RocksDB | ByteCask / RocksDB |
|---|---:|---:|:---:|
| Put (NoSync) | 157.1 Kops/s | **166.4 Kops/s** | **0.94×** |
| Put (Sync) | 434.8 ops/s | **478.0 ops/s** | **0.91×** |
| Get | **1.34 Mops/s** | 575.5 Kops/s | **2.33×** |
| Del (Sync) | **657.2 ops/s** | 319.6 ops/s | **2.06×** |
| Range-50 | 29.8 K scans/s | **87.2 K scans/s** | **0.34×** |
| MixedBatch/Sync | **34.2 Kops/s** | 33.0 Kops/s | **1.04×** |

### Get Latency _(CRC disabled)_

| Percentile | ByteCask | RocksDB |
|---|---:|---:|
| p50 | 680 ns | 1.57 µs |
| p99 | 1.15 µs | 3.96 µs |

### Concurrent Reads — GetMT _(CRC disabled)_

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.56 Mops/s** | 1.13 Mops/s | **2.27×** |
| 4 | **4.27 Mops/s** | 2.15 Mops/s | **1.99×** |
| 8 | **6.10 Mops/s** | 4.44 Mops/s | **1.37×** |
| 16 | **8.61 Mops/s** | 6.17 Mops/s | **1.40×** |
| 32 | **11.43 Mops/s** | 8.30 Mops/s | **1.38×** |

### Concurrent Writes — PutMT/Sync

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | 440.9 ops/s | **721.4 ops/s** | **0.61×** |
| 4 | 785.3 ops/s | **1.3 Kops/s** | **0.61×** |
| 8 | 1.1 Kops/s | **1.7 Kops/s** | **0.65×** |
| 16 | **2.5 Kops/s** | 964.1 ops/s | **2.60×** |

### Read-While-Writing — 1 writer + N readers, Sync _(CRC disabled)_

> **BoundedStaleness** is a ByteCask read mode where readers observe the keydir snapshot from the previous completed write batch instead of acquiring a per-read epoch lock. This eliminates reader-writer contention at high thread counts at the cost of seeing writes that are at most one batch behind.

| Readers | ByteCask | ByteCask BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.54 Mops/s | **2.61 Mops/s** | 1.12 Mops/s |
| 4 | 4.26 Mops/s | **4.46 Mops/s** | 2.14 Mops/s |
| 8 | 5.94 Mops/s | **6.22 Mops/s** | 4.30 Mops/s |
| 16 | 8.55 Mops/s | **9.34 Mops/s** | 6.27 Mops/s |

---

# Scalability

## GetMT Scalability — Throughput and Latency vs Dataset Size

> Throughput (Mops/s) and p99 read latency at each thread count as the dataset grows. ByteCask's in-memory keydir keeps read latency flat; RocksDB's block cache hit rate falls as the working set exceeds the cache.

### Throughput (Mops/s)

| Threads | BC 50k | RDB 50k | BC 500k | RDB 500k | BC 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.40 Mops/s | **3.16 Mops/s** | **2.58 Mops/s** | 895.5 Kops/s | **2.56 Mops/s** | 1.13 Mops/s |
| 4 | 4.42 Mops/s | **5.69 Mops/s** | **4.21 Mops/s** | 1.71 Mops/s | **4.27 Mops/s** | 2.15 Mops/s |
| 8 | 5.28 Mops/s | **11.39 Mops/s** | **6.01 Mops/s** | 4.10 Mops/s | **6.10 Mops/s** | 4.44 Mops/s |
| 16 | 7.77 Mops/s | **14.10 Mops/s** | **8.42 Mops/s** | 5.61 Mops/s | **8.61 Mops/s** | 6.17 Mops/s |
| 32 | 10.91 Mops/s | **18.14 Mops/s** | **12.97 Mops/s** | 8.05 Mops/s | **11.43 Mops/s** | 8.30 Mops/s |

### p99 Read Latency

| Threads | BC 50k | RDB 50k | BC 500k | RDB 500k | BC 1M | RDB 1M |
|---:| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2.54 µs | **1.95 µs** | **2.45 µs** | 9.20 µs | **2.49 µs** | 8.35 µs |
| 4 | 5.31 µs | **4.50 µs** | **6.25 µs** | 20.82 µs | **6.13 µs** | 18.67 µs |
| 8 | 19.07 µs | **8.62 µs** | **16.20 µs** | 40.98 µs | **16.25 µs** | 39.28 µs |
| 16 | 43.90 µs | **21.79 µs** | **42.04 µs** | 59.12 µs | **40.69 µs** | 55.23 µs |
| 32 | 93.73 µs | **48.36 µs** | **75.84 µs** | 106.89 µs | **80.48 µs** | 95.19 µs |


# Recovery

## Parallel Recovery

> ✅ **CRC verification is enabled** during recovery. Times reflect full disk I/O and CRC validation across all data files.

### 50k Keys (50,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.016 s |
| 2 | 0.018 s |
| 4 | 0.020 s |
| 8 | 0.021 s |
| 16 | 0.023 s |

### 1M Keys (1,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 0.252 s |
| 2 | 0.133 s |
| 4 | 0.087 s |
| 8 | 0.063 s |
| 16 | 0.061 s |

### 10M Keys (10,000,000)

| Threads | Recovery Time (mean) |
|---:|---:|
| 1 | 2.744 s |
| 2 | 1.545 s |
| 4 | 0.964 s |
| 8 | 0.642 s |
| 16 | 0.576 s |

---
_Generated by `scripts/benchmark_showcase.py` · 2026-04-07_