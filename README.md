# ByteCask

ByteCask is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf)-inspired embedded key-value store written in C++23. It extends the original Bitcask design by replacing the hash-table key directory with a **persistent radix tree**, enabling efficient range queries and prefix scans while preserving Bitcask's core strengths: append-only writes, fast recovery, and simple crash safety.

> **Trade-off**: ByteCask keeps all keys in memory at all times. This enables O(k) point lookups and ordered iteration without any disk I/O on the read path, but limits the number of unique keys to available RAM. At ~100 bytes per key (key data + metadata + tree overhead), 10 million keys require roughly 1 GB of RAM.

## Features

- **Sequential write path** — every `put`, `del`, and `apply_batch` performs a single sequential append; no WAL, no random writes, just one I/O operation per write.
- **Ordered range iteration** — scan from any key prefix using the in-memory radix tree; no disk I/O for key enumeration.
- **Atomic operations** — `apply_batch` atomically applies a set of puts and deletes.
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

// Atomic batch.
Batch batch;
batch.put(to_bytes("user:2"), to_bytes("bob"));
batch.put(to_bytes("user:3"), to_bytes("carol"));
batch.del(to_bytes("user:1"));
db.apply_batch({}, std::move(batch));

// Range scan — lazy, reads values from disk on demand.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan — in-memory radix tree walk, no disk I/O.
for (auto& key : db.keys_from({})) { ... }
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
