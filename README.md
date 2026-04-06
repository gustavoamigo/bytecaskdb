# ByteCask

ByteCask is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf)-inspired embedded key-value store written in C++23. It extends the original Bitcask design by replacing the hash-table key directory with a **persistent radix tree**, enabling efficient range queries and prefix scans while preserving Bitcask's core strengths: append-only writes, fast recovery, and simple crash safety.

> **Trade-off**: ByteCask keeps all keys in memory at all times. This enables O(k) point lookups and ordered iteration without any disk I/O on the read path, but limits the number of unique keys to available RAM. At ~100 bytes per key (key data + metadata + tree overhead), 10 million keys require roughly 1 GB of RAM.

## Features

- **Append-only writes** — every `insert` and `remove` is a sequential append; no random writes.
- **Ordered range iteration** — scan from any key prefix using the in-memory radix tree; no disk I/O for key enumeration.
- **Atomic batches** — `apply_batch` wraps a set of inserts and removes in `BulkBegin`/`BulkEnd` markers; partial batches are discarded on recovery.
- **Parallel recovery** — hint-file replay is partitioned across worker threads and merged with a fan-in strategy; 8-thread recovery is ~5× faster than serial on large datasets.
- **Vacuum / compaction** — `vacuum()` rewrites or absorbs the most fragmented sealed file, reclaiming space left by overwritten or deleted keys.
- **SWMR concurrency** — a single writer serialised by a mutex; readers never acquire the write lock; state is published via `std::atomic<shared_ptr<EngineState>>`.
- **Crash safety** — hint files are written atomically (`write → fdatasync → rename`); the active data file is scanned on recovery to reconstruct any entries written after the last hint.

## Quick Start

```cpp
#include "bytecask.cppm"  // import bytecask.engine;

// Open (or create) a database directory.
auto db = Bytecask::open("my_db");

// Single-key operations.
db.insert(as_bytes("user:1"), as_bytes("alice"));

auto val = db.get(as_bytes("user:1"));   // std::optional<Bytes>
if (val) { /* use *val */ }

bool existed = db.remove(as_bytes("user:1"));  // false if key was absent

// Atomic batch.
Batch batch;
batch.insert(as_bytes("user:2"), as_bytes("bob"));
batch.insert(as_bytes("user:3"), as_bytes("carol"));
batch.remove(as_bytes("user:1"));
db.apply_batch(std::move(batch));

// Range scan — lazy, reads values from disk on demand.
for (auto& [key, value] : db.iter_from(as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan — in-memory radix tree walk, no disk I/O.
for (auto& key : db.keys_from(as_bytes("user:"))) { ... }
```

## API Reference

```cpp
class Bytecask {
public:
    [[nodiscard]] static auto open(std::filesystem::path dir) -> Bytecask;
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   std::size_t max_file_bytes,
                                   std::size_t recovery_threads) -> Bytecask;

    [[nodiscard]] auto get(BytesView key) const -> std::optional<Bytes>;
    void insert(BytesView key, BytesView value);
    [[nodiscard]] bool remove(BytesView key);
    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    void apply_batch(Batch batch);

    [[nodiscard]] auto iter_from(BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    void vacuum(VacuumOptions opts = {});
    [[nodiscard]] auto file_stats() const
        -> std::map<std::uint32_t, FileStats>;
};
```

Error handling follows the throw-on-failure convention used by the C++ standard library: I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; key-not-found is represented by `std::optional`.

## Architecture

```
  Bytecask
  ├── Key Directory  PersistentRadixTree<KeyDirEntry>   (all keys, in memory)
  ├── File Registry  map<file_id, DataFile>             (open file descriptors)
  ├── Active File    append-only .data file             (current writes)
  └── Sealed Files   read-only .data + .hint files      (older segments)
```

**Write path**: every `insert`/`remove` appends a length-prefixed, CRC-32-verified entry to the active data file, updates the in-memory radix tree, and publishes a new immutable `EngineState` snapshot via atomic `shared_ptr`.

**Read path**: readers atomically load the current `EngineState` snapshot, look up the key in the radix tree to find `(file_id, offset)`, then `pread` the value bytes directly from the appropriate data file. Readers never acquire the write mutex.

**Recovery**: on `open`, the engine replays hint files (compact per-file indexes written at rotation time) to rebuild the key directory, then scans the active data file's tail for any entries appended after the last hint was flushed. Recovery can be parallelised by passing `recovery_threads > 1` to `open`.

See [`docs/bytecask_design.md`](docs/bytecask_design.md) for the full design reference.

## Performance

Benchmarks run on a 16-core machine (50 000 keys, 1 KiB random values):

| Operation | ByteCask | LevelDB | RocksDB |
|-----------|----------|---------|---------|
| Put (no-sync) | ~116 K ops/s | ~71 K ops/s | ~141 K ops/s |
| Put (sync) | ~481 ops/s | ~309 ops/s | ~461 ops/s |
| Get | ~1.0 M ops/s | ~1.0 M ops/s | ~1.3 M ops/s |
| Range scan (50 entries) | ~25 K scans/s | ~103 K scans/s | ~179 K scans/s |
| Batch mixed (sync) | ~32.9 K ops/s | ~21.4 K ops/s | ~32.3 K ops/s |

Multi-threaded reads (session consistency, 50 000 keys, 1 KiB values):

| Threads | ByteCask reads |
|---------|----------------|
| 2 | 2.16 M ops/s |
| 4 | 3.01 M ops/s |
| 8 | 3.57 M ops/s |
| 16 | 3.67 M ops/s |

Parallel recovery (1 M keys):

| Threads | Recovery time |
|---------|---------------|
| 1 | ~262 ms |
| 8 | ~58 ms (4.5×) |
| 16 | ~52 ms (5.0×) |

Range scan throughput reflects the cost of reading 1 KiB values from disk on each step; ByteCask's advantage is the keys-only scan (`keys_from`) which is a pure in-memory radix tree walk.

Benchmark source: [`benchmarks/engine_bench.cpp`](benchmarks/engine_bench.cpp). Results recorded in [`benchmarks/engine_bench_results.csv`](benchmarks/engine_bench_results.csv).

## Building

ByteCask requires **Clang** (with C++23 modules support) and [xmake](https://xmake.io).

```bash
# Build and run the test suite.
xmake build bytecask_tests
xmake run bytecask_tests

# Build benchmarks (optional; requires Google Benchmark, LevelDB, RocksDB).
xmake build engine_bench
xmake run engine_bench
```

A ready-to-use development environment is provided via the included [Dev Container](.devcontainer) (Fedora 43, Clang, xmake pre-installed).

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
