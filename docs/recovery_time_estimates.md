# ByteCask Recovery Time Estimates

## Methodology

Recovery time was measured directly via `BM_Recovery/WithHints`,
`BM_Recovery/NoHints`, and `BM_RecoveryParallel` in
`benchmarks/engine_bench.cpp`, run on the test machine below. The benchmark
rebuilds the full key directory from scratch — opening a new `Bytecask`
instance and draining all hint (or data) files — so it reflects real startup
cost, not just I/O. All benchmarks use `TMPDIR=./.tmp` (real disk, not
tmpfs).

**Test machine**

| Property | Value |
|---|---|
| CPU | 16 × 4427 MHz |
| L1d / L2 / L3 | 32 KiB / 512 KiB / 16 MiB |
| RAM | 31 GB |
| Storage | Samsung SSD 860 EVO 500 GB (SATA 6 Gbps) |
| OS page cache | warm (files resident after setup) |

**Benchmark parameters** (matching `BM_Recovery`)

| Parameter | Value |
|---|---|
| Value size | 1,024 bytes |
| Key format | prefixed UUIDs, ~42 bytes (e.g. `user::018f6e2c-…`) |
| Data entry on disk | 15 B header + 42 B key + 1,024 B value + 4 B CRC = **1,085 bytes** |
| Hint entry on disk | 19 B header + 42 B key + 4 B CRC = **65 bytes** |
| Hint/data size ratio | ~6% |

**Parallel recovery benchmark parameters** (matching `BM_RecoveryParallel`)

| Parameter | Value |
|---|---|
| Value size | 1 byte (hint-only workload) |
| Key format | prefixed UUIDs, ~42 bytes |
| Rotation threshold | 256 KiB (creates many small files for parallelism) |

---

## Measured results — serial recovery

| Keys | WithHints | NoHints |
|------|-----------|---------|
| 10k  | 17 ms     | 17 ms   |
| 50k  | 87 ms     | 85 ms   |
| 100k | 160 ms    | 165 ms  |
| 1M   | 277 ms    | 1,654 ms |
| 10M  | 3,080 ms  | 16,200 ms |

At small key counts (≤100k) both paths are indistinguishable because the data
files fit in a handful of OS read buffers and the tree has minimal depth. At
1M+ keys the hint path stays nearly flat while the no-hints path scales with
total data file size (~16× larger than hint files).

**Recovery bottleneck (WithHints):** radix tree `set()` dominates. Parsing
50k entries from hint file bytes takes ~1 ms; inserting them into the tree
takes ~86 ms. The hint file I/O is <5% of total time once the page cache is
warm.

## Measured results — parallel recovery (disk-backed TMPDIR)

`BM_RecoveryParallel` uses `Bytecask::open(dir, max_bytes, recovery_threads)`.
Each worker builds an independent radix tree from its assigned hint files,
then a pairwise fan-in merge combines results in ⌈log₂(W)⌉ rounds.

**1M keys:**

| Threads | Mean (ms) | Speedup | CV |
|---------|-----------|---------|------|
| 1 (serial) | 262 | 1.00× | 0.45% |
| 2 | 153 | 1.71× | 5.79% |
| 4 | 91 | 2.88× | 4.61% |
| 8 | 58 | 4.51× | 2.44% |
| 16 | 52 | 5.04× | 4.26% |

**10M keys:**

| Threads | Mean (ms) | Speedup | CV |
|---------|-----------|---------|------|
| 1 (serial) | 2,858 | 1.00× | 4.59% |
| 2 | 1,647 | 1.73× | 10.22% |
| 4 | 962 | 2.97× | 3.40% |
| 8 | 567 | 5.04× | 0.71% |
| 16 | 503 | 5.68× | 4.56% |

Scaling is sub-linear: the sequential fan-in merge phase grows with total
keys, and memory bandwidth saturates beyond 8 threads. The practical sweet
spot is **8 threads** (~5× speedup with low variance). The 1T serial
baseline routes through `recover_existing_files()` (original serial path),
not the parallel path with 1 worker.

---

## Per-key cost model

From the 1M and 10M data points, the per-key insertion cost is
approximately **277 ns/key** warm-cache (serial). The mild superlinear growth
between 1M and 10M (~11.1× time for 10× keys) reflects L3 cache pressure as
the tree outgrows the 16 MiB L3.

The model used for serial extrapolation:

$$T_{\text{serial}}(N) \approx 277\,\text{ms} \times \left(\frac{N}{10^6}\right)^{1.05}$$

For parallel recovery at 8 threads (measured 5.04× at 10M keys):

$$T_{\text{8T}}(N) \approx \frac{T_{\text{serial}}(N)}{5}$$

Cold-cache I/O adds hint file read time on top. Hint file size is
$N \times 65\,\text{bytes}$.

| Storage tier | Sequential read bandwidth | Cold-start I/O overhead |
|---|---|---|
| NVMe SSD | ~3 GB/s | $N \times 65\,\text{B} \div 3\,\text{GB/s}$ |
| SATA SSD | ~500 MB/s | $N \times 65\,\text{B} \div 500\,\text{MB/s}$ |

---

## Estimates for target database sizes

Values assume 1 KB average value size. All times round to the nearest
second for large figures. "Serial" is recovery_threads=1, "8T" is
recovery_threads=8 (~5× speedup).

### WithHints (normal restart)

| DB size | Keys | Serial | 8T parallel | + NVMe I/O (serial) | + NVMe I/O (8T) |
|---------|------|--------|-------------|---------------------|-----------------|
| 10 GB   | 9.2 M   | ~2.9 s   | ~0.6 s  | **3.1 s**  | **0.8 s**  |
| 100 GB  | 92 M    | ~32 s    | ~6.4 s  | **34 s**   | **8.4 s**  |
| 1 TB    | 922 M   | ~6 min   | ~72 s   | **~6.3 min** | **~92 s**  |
| 2 TB    | 1.84 B  | ~12.4 min | ~2.5 min | **~13 min** | **~3.2 min** |

### NoHints (hint files missing or corrupted — scans raw data files)

NoHints recovery is ~6× slower at scale because data files are 16× larger than
hint files, and every entry is fully deserialised including the value header.
Parallel recovery does not apply to NoHints (it operates on hint files only).

| DB size | Keys | Estimated time |
|---------|------|---------------|
| 10 GB   | 9.2 M   | ~17–20 s |
| 100 GB  | 92 M    | ~3–4 min |
| 1 TB    | 922 M   | ~30–40 min |
| 2 TB    | 1.84 B  | ~60–80 min |

---

## Memory requirement: the real ceiling

ByteCask holds the entire key directory in RAM. The radix tree uses
approximately **80–120 bytes per key** accounting for key bytes and internal
node overhead (with good prefix sharing for the prefixed-UUID key distribution
used in the benchmark).

| DB size | Keys | Estimated key directory RAM |
|---------|------|-----------------------------|
| 10 GB   | 9.2 M   | ~0.9 GB — fits on any machine |
| 100 GB  | 92 M    | ~8–9 GB — fits on a 16 GB workstation or server |
| 1 TB    | 922 M   | ~80–100 GB — requires a high-memory server (256 GB+) |
| 2 TB    | 1.84 B  | ~160–200 GB — requires a high-memory server (256 GB+) |

**Memory is the true scalability limit, not recovery time.** On a 32 GB
machine (as used for benchmarking) the practical key-directory ceiling is
~250–320 M keys, corresponding to roughly 250–320 GB of 1 KB-value data.

### Scaling with larger values

The key/memory relationship does not change with value size — larger values
reduce key count for the same DB size, making ByteCask *more* practical at
scale. For example:

| Average value size | Keys in 1 TB | Key directory RAM | Serial recovery | 8T parallel |
|---|---|---|---|---|
| 1 KB   | 922 M | ~80–100 GB | ~6.3 min (NVMe) | ~92 s (NVMe) |
| 64 KB  | 14.5 M | ~1.3 GB    | ~5 s (NVMe) | ~1 s (NVMe) |
| 1 MB   | 953 k  | ~85 MB     | < 1 s (NVMe) | < 1 s (NVMe) |

ByteCask is best suited to **value-heavy workloads** where the value/key
byte ratio is large and the total unique-key count stays well within available
RAM.

---

## Source

Benchmarks run on 2026-04-05 with `TMPDIR=./.tmp` (disk-backed).
Raw results are in `benchmarks/engine_bench_results.csv`.
Benchmark source: `benchmarks/engine_bench.cpp` — `BM_Recovery`,
`BM_RecoveryParallel`.
