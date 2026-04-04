# ByteCask Recovery Time Estimates

## Methodology

Recovery time was measured directly via `BM_Recovery/WithHints` and
`BM_Recovery/NoHints` in `benchmarks/engine_bench.cpp`, run on the test
machine below. The benchmark rebuilds the full key directory from scratch —
opening a new `Bytecask` instance and draining all hint (or data) files — so
it reflects real startup cost, not just I/O.

**Test machine**

| Property | Value |
|---|---|
| CPU | 16 × 4427 MHz |
| L1d / L2 / L3 | 32 KiB / 512 KiB / 16 MiB |
| RAM | 31 GB |
| OS page cache | warm (files resident) |

**Benchmark parameters** (matching `BM_Recovery`)

| Parameter | Value |
|---|---|
| Value size | 1,024 bytes |
| Key format | prefixed UUIDs, ~42 bytes (e.g. `user::018f6e2c-…`) |
| Data entry on disk | 15 B header + 42 B key + 1,024 B value + 4 B CRC = **1,085 bytes** |
| Hint entry on disk | 19 B header + 42 B key + 4 B CRC = **65 bytes** |
| Hint/data size ratio | ~6% |

---

## Measured results

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

---

## Per-key cost model

From the 1M and 10M data points, the per-key insertion cost is
approximately **277 ns/key** warm-cache. The mild superlinear growth between
1M and 10M (~11.1× time for 10× keys) reflects L3 cache pressure as the tree
outgrows the 16 MiB L3.

The model used for extrapolation:

$$T_{\text{hints}}(N) \approx 277\,\text{ms} \times \left(\frac{N}{10^6}\right)^{1.05}$$

Cold-cache I/O adds hint file read time on top. Hint file size is
$N \times 65\,\text{bytes}$.

| Storage tier | Sequential read bandwidth | Cold-start I/O overhead |
|---|---|---|
| NVMe SSD | ~3 GB/s | $N \times 65\,\text{B} \div 3\,\text{GB/s}$ |
| SATA SSD | ~500 MB/s | $N \times 65\,\text{B} \div 500\,\text{MB/s}$ |

---

## Estimates for target database sizes

Values assume 1 KB average value size. All times round to the nearest
second for large figures. "WithHints" is the normal production path.

### WithHints (normal restart)

| DB size | Keys | Tree construction | + NVMe I/O | + SATA I/O |
|---------|------|-------------------|------------|------------|
| 10 GB   | 9.2 M   | ~2.9 s  | +0.2 s → **3.1 s**  | +1.2 s → **4.1 s**  |
| 100 GB  | 92 M    | ~33 s   | +2 s → **35 s**     | +12 s → **45 s**    |
| 1 TB    | 922 M   | ~5.5 min | +20 s → **~6 min** | +2 min → **~7.5 min** |
| 2 TB    | 1.84 B  | ~11 min  | +40 s → **~12 min** | +4 min → **~15 min**  |

### NoHints (hint files missing or corrupted — scans raw data files)

NoHints recovery is ~6× slower at scale because data files are 16× larger than
hint files, and every entry is fully deserialised including the value header.

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

| Average value size | Keys in 1 TB | Key directory RAM | WithHints recovery |
|---|---|---|---|
| 1 KB   | 922 M | ~80–100 GB | ~6 min (NVMe) |
| 64 KB  | 14.5 M | ~1.3 GB    | ~5 s (NVMe) |
| 1 MB   | 953 k  | ~85 MB     | < 1 s (NVMe) |

ByteCask is best suited to **value-heavy workloads** where the value/key
byte ratio is large and the total unique-key count stays well within available
RAM.

---

## Source

Benchmarks run on 2026-04-04 (commits `09222ef`, `1308e67`).
Raw results are in `benchmarks/engine_bench_results.csv`.
Benchmark source: `benchmarks/engine_bench.cpp` — `BM_Recovery`.
