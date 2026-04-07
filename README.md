# ByteCask

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
- **Fast recovery** — parallelised index reconstruction from hint files; 100 M keys recover in under 5 seconds on fast hardware.
- **Vacuum** — vacuum process to reclaim unused space from overwritten or deleted keys; query performance does not degrade as the database grows.
- **Lock-free multi-reader, single-writer** — reads are lock-free and scale to millions of operations per second; a single writer ensures consistent writes even under concurrent multi-threaded access.
- **Crash safety** — CRC-verified entries, atomic hint file generation (`write → fdatasync → rename`), and data files that act as a write-ahead log ensure durability.

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
