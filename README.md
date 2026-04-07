# ByteCask

> **Status: early development.** The core engine works and is well-tested, but the API and on-disk format may change before a stable release. Not recommended for production use yet.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecask)

ByteCask is a fast, predictable embedded key-value store written in C++23 that scales to **hundreds of millions of keys** across multiple cores with flat, consistent latency.

Built on the [Bitcask](https://riak.com/assets/bitcask-intro.pdf) append-only foundation, ByteCask replaces the original hash-table key directory with a **persistent radix tree** — enabling ordered range queries, prefix scans, and prefix compaction while keeping the simplicity that makes Bitcask fast. Prefix scans are pure in-memory radix tree walks:

```cpp
// Scan all keys starting with "user:" — no disk I/O.
for (auto& key : db.keys_from({}, to_bytes("user:"))) { ... }

// Scan with values — values fetched lazily from disk.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) { ... }
```

**O(1) reads and writes**: point lookups and writes take constant time regardless of database size — performance stays flat whether you have 1 000 or 100 M records. Very few moving parts (an in-memory radix tree + append-only data files) is what keeps latency predictable. O(1) snapshots are planned.

> **Trade-off**: ByteCask keeps all keys in memory at all times. This enables O(1) point lookups and ordered iteration without any disk I/O on the read path, but limits the number of unique keys to available RAM. At ~100 bytes per key (key data + metadata + tree overhead), 10 million keys require roughly 1 GB of RAM.

## Features

- **Sequential write path** — every `put`, `del`, and `apply_batch` performs a single sequential append; no WAL, no random writes, just one I/O operation per write.
- **Ordered range iteration** — scan from any key prefix using the in-memory radix tree; no disk I/O for key enumeration.
- **Atomic writes** — every `put` and `del` is atomic. `apply_batch` makes multiple puts and deletes atomic as a group.
- **Fast recovery** — parallelised index reconstruction from hint files; 10 M keys recover in under 600 ms and 100 M keys in under 6 s on a SATA SSD.
- **Vacuum** — vacuum process to reclaim unused space from overwritten or deleted keys; query performance does not degrade as the database grows.
- **Lock-free multi-reader, single-writer** — reads are lock-free and scale to millions of operations per second; a single writer ensures consistent writes even under concurrent multi-threaded access.
- **Crash safety** — CRC-verified entries, atomic hint file generation (`write → fdatasync → rename`), and data files that act as a write-ahead log ensure durability.

## Performance

ByteCask is built around a single design bet: keep all keys in memory, always. This lets every point lookup resolve in one in-memory radix tree traversal followed by a single pread at a known file offset — no block cache churn, no compaction stalls, no indirection through SST index blocks. The trade-off is RAM: the key directory grows with the number of unique keys, not the value size.

What that looks like in practice compared to [RocksDB](https://rocksdb.org/) at 1 M keys:

- **Reads are 2–3× faster** when the working set exceeds RocksDB's block cache. At 50 k keys the caches are warm and RocksDB is faster; from 500 k keys onward ByteCask's flat-cost lookup wins consistently. p50 Get latency is **680 ns** vs 1.57 µs; p99 is **1.15 µs** vs 3.96 µs.
- **Concurrent reads scale linearly.** Lock-free reads reach **11 Mops/s at 32 threads** vs 8.3 Mops/s for RocksDB. Mixed read-while-write workloads show the same gap.
- **Sequential writes are comparable.** Both engines perform sequential appends; throughput is within ±10 % across all write benchmarks. ByteCask does not have write amplification from compaction, so Sync Delete is **2×** faster.
- **Range scans over values are slower.** ByteCask must read each value from disk individually; RocksDB prefetches sequential blocks. For key-only iteration (`keys_from`) ByteCask wins — it is a pure in-memory radix tree walk with no disk I/O.
- **Recovery is fast and parallel.** On restart, ByteCask rebuilds the in-memory key directory by replaying compact hint files in parallel across all available cores, with full CRC verification. At 16 threads: 1 M keys recover in **~60 ms**, 10 M in **~580 ms**. Extrapolating linearly, 100 M keys recover in roughly **~6 s** and 1 B keys in roughly **~60 s** — both with CRC validation of every byte on disk.

See [`docs/bytecask_benchmark_showcase.md`](docs/bytecask_benchmark_showcase.md) for the full benchmark report with all thread counts, dataset sizes, and hardware details.

---

### Single-Threaded Throughput (1M keys)

> CRC verification is disabled for read operations; enabled for recovery.

| Operation | ByteCask | RocksDB | Notes |
|-----------|----------|---------|-------|
| Put (NoSync) | 157 Kops/s | **166 Kops/s** | Comparable with an edge for RocksDB. Sequential append on both sides |
| Put (Sync) | 435 ops/s |**478.0 ops/s** | Comparable  with an edge for RocksDB. Disk-bound — limited by `fdatasync` round-trip latency |
| Get | **1.34 Mops/s** | 575 Kops/s | **2.3×** — in-memory radix tree lookup; no block cache miss risk |
| Del (Sync) | **657 ops/s** | 320 ops/s | **2.1×** — single tombstone append vs RocksDB write amplification |
| Range-50 | 30 K scans/s | **87 K scans/s** | RocksDB prefetches sequential blocks; ByteCask fetches each value individually. |
| MixedBatch (Sync) | 34 Kops/s | 33 Kops/s | Comparable |

ByteCask's read advantage grows with dataset size: at 50k keys RocksDB's block cache covers the entire working set and leads; from 500k keys onward the cache misses and ByteCask pulls ahead by **2–3×**.

### Get Latency (1M keys, CRC disabled)

| Percentile | ByteCask | RocksDB |
|-----------|---------|----------|
| p50 | **680 ns** | 1.57 µs |
| p99 | **1.15 µs** | 3.96 µs |

Latency stays flat as the dataset grows: ByteCask always reads from the OS page cache at a known offset; RocksDB's latency climbs when key metadata exceeds the block cache.

### Concurrent Reads — `GetMT` (1M keys, CRC disabled)

> ByteCask reads are lock-free; each thread holds an immutable snapshot of the engine state.

| Threads | ByteCask | RocksDB | ByteCask / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.56 Mops/s** | 1.13 Mops/s | 2.27× |
| 4 | **4.27 Mops/s** | 2.15 Mops/s | 1.99× |
| 8 | **6.10 Mops/s** | 4.44 Mops/s | 1.37× |
| 16 | **8.61 Mops/s** | 6.17 Mops/s | 1.40× |
| 32 | **11.43 Mops/s** | 8.30 Mops/s | 1.38× |

### Read-While-Writing (1M keys, 1 writer + N readers, Sync, CRC disabled)

> Two read consistency modes: the default acquires a per-read epoch lock; **BoundedStaleness** snapshots the keydir once per write batch, eliminating reader-writer contention at the cost of readers seeing writes that are at most one batch behind.

| Readers | ByteCask | ByteCask BoundedStaleness | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.54 Mops/s | 2.61 Mops/s | 1.12 Mops/s |
| 4 | 4.26 Mops/s | 4.46 Mops/s | 2.14 Mops/s |
| 8 | 5.94 Mops/s | 6.22 Mops/s | 4.30 Mops/s |
| 16 | 8.55 Mops/s | 9.34 Mops/s | 6.27 Mops/s |

### Recovery

Recovery is the process that runs when ByteCask opens an existing database: it rebuilds the in-memory key directory by reading compact hint files from disk, then verifies every entry with CRC-32. ByteCask parallelises this across all available CPU cores — each core processes a disjoint set of data files independently, and the results are merged before the database becomes available.

| Keys | Threads | Recovery Time | Speedup vs 1T |
|---:|---:|---:|---:|
| 1M | 1 | 252 ms | — |
| 1M | 4 | 87 ms | 2.9× |
| 1M | 8 | 63 ms | 4.0× |
| 1M | 16 | 61 ms | 4.1× |
| 10M | 1 | 2.74 s | — |
| 10M | 4 | 0.96 s | 2.9× |
| 10M | 8 | 0.64 s | 4.3× |
| 10M | 16 | 0.58 s | 4.7× |

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (483 MiB/s read), 31 GiB RAM. Each result is the mean of 3 runs. Benchmark source: [`benchmarks/engine_bench.cpp`](benchmarks/engine_bench.cpp)._

## Quick Start

```cpp
import bytecask;
using namespace bytecask;

// Open (or create) a database directory.
auto db = DB::open("my_db");

// Single-key operations.
db.put({}, to_bytes("user:1"), to_bytes("alice"));

Bytes out;
bool found = db.get({}, to_bytes("user:1"), out);   // true; value in out
bool existed = db.del({}, to_bytes("user:1"));       // false if key was absent

// Atomic batch — all operations land atomically.
Batch batch;
batch.put(to_bytes("user:2"), to_bytes("bob"));
batch.put(to_bytes("user:3"), to_bytes("carol"));
batch.del(to_bytes("user:1"));
db.apply_batch({}, std::move(batch));

// Prefix scan — in-memory key walk, values fetched lazily from disk.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only prefix scan — pure in-memory, no disk I/O.
for (auto& key : db.keys_from({}, to_bytes("user:"))) { ... }
```

> `to_bytes` is a small helper that converts a `std::string_view` to `BytesView`:
> ```cpp
> auto to_bytes(std::string_view sv) -> BytesView {
>     return std::as_bytes(std::span{sv.data(), sv.size()});
> }
> ```

## API Reference

```cpp
namespace bytecask {

class DB {
public:
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   Options opts = {}) -> DB;

    // Writes value for key into out, reusing its capacity. Returns true if found.
    // Throws std::system_error on I/O failure or std::runtime_error on CRC mismatch.
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;

    // Writes key → value. Overwrites any existing value.
    void put(const WriteOptions& opts, BytesView key, BytesView value);

    // Writes a tombstone for key. Returns true if the key existed.
    [[nodiscard]] auto del(const WriteOptions& opts, BytesView key) -> bool;

    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // Atomically applies all operations in batch. Consumes batch (move-only).
    void apply_batch(const WriteOptions& opts, Batch batch);

    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    // Returns true if a file was vacuumed, false if no file qualified.
    [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;
};

} // namespace bytecask
```

Error handling follows the throw-on-failure convention used by the C++ standard library: I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; key-not-found is signalled by `get` returning `false`.

## Architecture

### Design Principles

ByteCask is designed around four core tenets, in priority order:

1. **Correctness** — data integrity above all else.
2. **Simplicity** — few moving parts; the design is easy to understand and maintain.
3. **Predictable latency over peak throughput** — bounded, flat write latency at every scale. A steady 1 ms per write is preferable to an average 0.1 ms with occasional 500 ms spikes.
4. **Performance** — optimisations are pursued only when they do not compromise correctness or simplicity.

### Components

```
  ByteCask
  ├── Key Directory  PersistentRadixTree<KeyDirEntry>   (all keys, in memory)
  ├── File Registry  map<file_id, DataFile>             (open file descriptors)
  ├── Active File    append-only .data file             (current writes)
  └── Sealed Files   read-only .data + .hint files      (older segments)
```

**Write path**: every `put`/`del`/`apply_batch` appends a CRC-32-verified, length-prefixed record to the active data file and updates the in-memory radix tree. No random I/O; one sequential write per operation.

**Read path**: readers obtain an immutable snapshot of the engine state, look up the key in the radix tree to find its file and offset, then read the value directly. Reads are lock-free and scale linearly across cores.

**Recovery**: on `open`, the engine replays hint files (compact per-file indexes written atomically at rotation time) to rebuild the key directory, then scans the active file's tail for entries written after the last hint flush. Recovery is parallelised across files for fast startup.

See [`docs/bytecask_design.md`](docs/bytecask_design.md) for the full design reference.

## Building

ByteCask requires **Clang** (with C++23 modules support) and [xmake](https://xmake.io).

```bash
# Build and run the test suite.
xmake build 
xmake run bytecask_tests

# Build benchmarks (optional; requires RocksDB).
python ./scripts/run_engine_bench.py
```

A ready-to-use development environment is provided via the included [Dev Container](.devcontainer) (Fedora 43, Clang, xmake pre-installed).

## Want to hack on it?

ByteCask is early-stage and there's plenty of room to explore — new features, performance ideas, test coverage, documentation, or just poking around the internals. All of it is welcome.

See [CONTRIBUTING.md](CONTRIBUTING.md) to get started. The fastest path is to open it directly in GitHub Codespaces — no local setup required.

If you want to take it in a different direction and fork it into your own thing, go for it — that's what the MIT license is for.

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/bytecask_design.md`](docs/bytecask_design.md) | Living design reference: architecture, concurrency model, file format, vacuum, recovery |
| [`docs/engine_api_design.md`](docs/engine_api_design.md) | Public API specification with usage examples |
| [`docs/parallel_recovery_design.md`](docs/parallel_recovery_design.md) | Parallel recovery algorithm and fan-in merge strategy |
| [`docs/persistent_radix_tree_design.md`](docs/persistent_radix_tree_design.md) | Persistent radix tree data structure design |
| [`docs/bytecask_project_plan.md`](docs/bytecask_project_plan.md) | Issue tracker and project history |

## License

MIT — see [`LICENSE`](LICENSE).
