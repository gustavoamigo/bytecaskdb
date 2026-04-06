# ByteCask Recovery Time Estimates

## Methodology

Recovery time was measured directly via `BM_Recovery/WithHints`,
`BM_Recovery/NoHints`, and `BM_RecoveryParallel` in
`benchmarks/engine_bench.cpp`, run on the test machines below. The benchmark
rebuilds the full key directory from scratch — opening a new `Bytecask`
instance and draining all hint (or data) files — so it reflects real startup
cost, not just I/O. All benchmarks use `TMPDIR=./.tmp` (real disk, not
tmpfs).

**Test machine A (8-core / 16 vCPU, local)**

| Property | Value |
|---|---|
| CPU | 16 × 4427 MHz |
| L1d / L2 / L3 | 32 KiB / 512 KiB / 16 MiB |
| RAM | 31 GB |
| Storage | Samsung SSD 860 EVO 500 GB (SATA 6 Gbps) |
| OS page cache | warm (files resident after setup) |

**Test machine B (32-core / 64 vCPU, AWS EC2)**

| Property | Value |
|---|---|
| CPU | 64 × 3351 MHz |
| L1d / L2 / L3 | 48 KiB / 1280 KiB / 54 MiB |
| Storage | NVMe instance store (~3.16 GB/s sequential read, measured via fio) |
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

**10M keys (machine A):**

| Threads | Mean (ms) | Speedup | CV |
|---------|-----------|---------|------|
| 1 (serial) | 2,858 | 1.00× | 4.59% |
| 2 | 1,647 | 1.73× | 10.22% |
| 4 | 962 | 2.97× | 3.40% |
| 8 | 567 | 5.04× | 0.71% |
| 16 | 503 | 5.68× | 4.56% |

**5M keys (machine B — 32-core / 64 vCPU AWS EC2):**

No 1T serial baseline was run on this machine. Speedups below are estimated
using the 2T/serial ratio (~1.72×) measured on machine A, giving an estimated
serial of ~1,756 ms.

| Threads | Mean (ms) | Speedup (est.) | CV |
|---------|-----------|----------------|------|
| 2 | 1,021 | 1.72× | 4.03% |
| 4 | 722 | 2.43× | 1.97% |
| 8 | 584 | 3.01× | 0.47% |
| 16 | 514 | 3.42× | 1.78% |
| **32** | **493** | **3.56×** | **1.37%** |
| 64 | 543 | 3.23× | 1.59% |

The 32T sweet spot matches the physical core count. 64T regresses by ~10%:
at 5M keys the radix tree (~500 MB working set) spills the 54 MiB L3 but not
enough to generate the sustained cache-miss stalls that HT threads could hide.
The extra thread coordination overhead dominates instead.

Scaling is sub-linear: the pairwise fan-in merge grows with total keys and
the radix tree's pointer-chasing insertion is cache-miss-bound at large
dataset sizes. The practical sweet spot is the **physical core count** of the
recovery machine:

- **8-core machine (16 vCPU, local):** 16T achieves 5.68× at 10M keys,
  marginally better than 8T's 5.04×. HT threads help slightly because tree
  insertions generate L3 cache misses that HT can hide.
- **16-core machine (32 vCPU, AWS):** 32T achieves **8.66×** at 10M keys.
  For smaller datasets (1M) that fit in L3, 16T is the sweet spot (6.49×)
  and 32T regresses because there are no stalls for HT to hide.
- **32-core machine (64 vCPU, AWS):** 32T achieves **3.56×** at 5M keys.
  64T regresses to 3.23×. The lower speedup (vs 8.66× on the 16-core machine
  at 10M) reflects 5M keys generating fewer cache-miss stalls in the large
  54 MiB L3, plus ⌈log₂(32)⌉ = 5 merge rounds (vs 4–5 for 16–32 workers).
  A 10M-key run on this machine would likely show better scaling.

General rule: set `recovery_threads` to physical core count. For very large
datasets (≥10M keys, tree spills L3) the full vCPU count can provide
additional gain on smaller machines; on large machines the merge overhead
grows with worker count and the physical core count remains the sweet spot.

The 1T serial baseline routes through `recover_existing_files()` (original
serial path), not the parallel path with 1 worker.

---

## Per-key cost model

From the 1M and 10M data points, the per-key insertion cost is
approximately **277 ns/key** warm-cache (serial). The mild superlinear growth
between 1M and 10M (~11.1× time for 10× keys) reflects L3 cache pressure as
the tree outgrows the 16 MiB L3.

The model used for serial extrapolation:

$$T_{\text{serial}}(N) \approx 277\,\text{ms} \times \left(\frac{N}{10^6}\right)^{1.05}$$

For parallel recovery, speedup scales with physical core count. Measured
speedups at 10M keys:

| Machine | Keys | Threads | Speedup |
|---------|------|---------|--------|
| 8-core / 16 vCPU (local, SATA SSD) | 10M | 8T | 5.0× |
| 8-core / 16 vCPU (local, SATA SSD) | 10M | 16T | 5.7× |
| 16-core / 32 vCPU (AWS, SATA-speed NVMe) | 10M | 32T | 8.7× |
| 32-core / 64 vCPU (AWS, NVMe ~3.16 GB/s) | 5M | 32T | 3.6× (est.) |
| 32-core / 64 vCPU (AWS, NVMe ~3.16 GB/s) | 5M | 64T | 3.2× (est.) |

$$T_{\text{parallel}}(N, W) \approx \frac{T_{\text{serial}}(N)}{S(W)}$$

where $S(W)$ is the measured speedup for $W$ threads on the target machine.
Use $S \approx 5$ for 8 physical cores at 10M keys. Speedup scales with core
count but depends on dataset size: measured ~8.7× on 16 physical cores at 10M
keys, but only ~3.6× on 32 physical cores at 5M keys (merge overhead grows
with worker count and dominates when the per-worker partition is small).

Cold-cache I/O adds hint file read time on top. Hint file size is
$N \times 65\,\text{bytes}$.

| Storage tier | Sequential read bandwidth | Cold-start I/O overhead |
|---|---|---|
| NVMe SSD (datacenter / consumer) | ~3 GB/s | $N \times 65\,\text{B} \div 3\,\text{GB/s}$ |
| SATA SSD | ~500 MB/s | $N \times 65\,\text{B} \div 500\,\text{MB/s}$ |
| AWS instance store NVMe (small instances) | ~400–500 MB/s | treat as SATA tier |

> **Note on AWS instance store:** measured at 437 MB/s sequential write with
> `oflag=direct` on a 32-vCPU instance. Larger instances achieve full NVMe
> speeds: machine B (64-vCPU) measured 3.16 GB/s sequential read via fio.
> AWS throttles instance store I/O per-instance; small-to-medium instances
> should use the SATA column for cold-start estimates.

---

## Estimates for target database sizes

Values assume 1 KB average value size. All times round to the nearest second
for large figures. "Serial" is recovery_threads=1. "16T" uses the measured
5.68× speedup on the **8-core test machine** (recovery_threads=16). "32T†"
uses the measured 8.66× speedup from a 16-core/32-vCPU AWS machine at 10M
keys — larger-dataset rows are extrapolated from that point.

† Requires recovery_threads=32 on a ≥16-core machine.
‡ Requires recovery_threads=64 on a ≥32-core machine. Speedup estimated from
  5M-key benchmarks on a 32-core/64-vCPU AWS EC2 instance; large-dataset
  scaling on this class of machine has not yet been measured.

### WithHints (normal restart) - Estimation based on Benchmarks

| DB size | Keys | Serial | 16T | 32T† | 64T‡ | + NVMe I/O (serial) | + NVMe I/O (16T) | + NVMe I/O (32T†) | + NVMe I/O (64T‡) |
|---------|------|--------|-----|------|------|---------------------|------------------|-------------------|--------------------|
| 10 GB   | 9.2 M   | ~2.9 s   | ~0.5 s | ~0.3 s | ~0.9 s | **3.1 s**  | **0.7 s**  | **0.5 s**  | **1.1 s**  |
| 100 GB  | 92 M    | ~32 s    | ~5.6 s | ~3.7 s | ~10 s  | **34 s**   | **7.6 s**  | **5.7 s**  | **12 s**   |
| 1 TB    | 922 M   | ~6 min   | ~63 s  | ~42 s  | ~112 s | **~6.3 min** | **~83 s** | **~62 s**  | **~2.1 min** |
| 2 TB    | 1.84 B  | ~12.4 min | ~2.2 min | ~86 s | ~3.7 min | **~13 min** | **~2.8 min** | **~2.1 min** | **~4.1 min** |

### NoHints (hint files missing or corrupted — scans raw data files) - Estimation based on Benchmarks

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

## Memory requirement: the real ceiling - Estimation based on Benchmarks

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

| Average value size | Keys in 1 TB | Key directory RAM | Serial recovery | 16T | 32T† | 64T‡ |
|---|---|---|---|---|---|---|
| 1 KB   | 922 M | ~80–100 GB | ~6.3 min (NVMe) | ~83 s (NVMe) | ~62 s (NVMe) | ~2.1 min (NVMe) |
| 64 KB  | 14.5 M | ~1.3 GB    | ~5 s (NVMe) | ~1.2 s (NVMe) | ~0.9 s (NVMe) | ~1.6 s (NVMe) |
| 1 MB   | 953 k  | ~85 MB     | < 1 s (NVMe) | < 1 s (NVMe) | < 1 s (NVMe) | < 1 s (NVMe) |


---

## Source

Machine A benchmarks run on 2026-04-05 with `TMPDIR=./.tmp` (disk-backed).
Machine B benchmarks run on 2026-04-06 on a 32-core/64-vCPU AWS EC2 instance.
Raw results are in `benchmarks/engine_bench_results.csv`.
Benchmark source: `benchmarks/engine_bench.cpp` — `BM_Recovery`,
`BM_RecoveryParallel`.
