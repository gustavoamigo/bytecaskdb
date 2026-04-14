# ByteCaskDB

> **Status: early development.** The core engine works and is well-tested, but the API and on-disk format may change before a stable release. Not recommended for production use yet.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecaskdb)

**ByteCaskDB** is a fast, predictable embedded key-value store written in C++. Reads and writes have flat, predictable latency from thousands of keys to hundreds of millions.

All keys in memory at all times — a deliberate design choice that removes an entire class of complexity that exists solely to minimise disk access and makes every point lookup O(1) with flat, predictable latency. At ~100 bytes per key, 128 GB of RAM holds roughly a billion keys. Very few moving parts — an in-memory key directory and an append-only data file — is what keeps that latency flat whether you have 1,000 records or 100 million. 

Built on the [Bitcask](https://riak.com/assets/bitcask-intro.pdf) append-only foundation, ByteCaskDB replaces the original hash-table key directory with a **[persistent radix tree](docs/persistent_radix_tree_design.md)** — enabling ordered range queries, prefix scans, and prefix compaction, while keeping the simplicity that makes Bitcask fast. Snapshots are O(1) — just a root pointer copy. Full MVCC and serializable conflict detection are supported with no separate transaction type required.

## Features

- **Sequential write path** — all I/O is sequential appends; no random writes. Every `put` and `del` is one append. `apply_batch` with N operations appends a begin marker, N entries, and an end marker in a single `writev` — still no WAL, no random writes.
- **Ordered range iteration** — scan from any key prefix using the in-memory radix tree; no disk I/O for key enumeration. Bidirectional: scan forward with `iter_from`/`keys_from` or backward with `riter_from`/`rkeys_from`.
- **Atomic writes** — every `put` and `del` is atomic. `apply_batch` makes multiple puts and deletes atomic as a group.
- **MVCC transactions** — `snapshot` captures a consistent point-in-time read-only view; `apply_batch_if(opts, plan)` applies a `WritePlan` atomically only when every precondition holds (**key present / absent / unchanged**, **range unchanged**), returning `false` on conflict. The snapshot is embedded in the `WritePlan` at construction time. Together they cover the full isolation spectrum: read from a `Snapshot` for **snapshot isolation**, add `ensure_unchanged` / range guards for **serializable** conflict detection, or use bare `put`/`del` for **read-uncommitted** fast paths. All precondition checks are in-memory radix tree traversals — no disk I/O, no separate transaction type required.
- **Fast recovery** — parallelised index reconstruction from hint files; 10 M keys recover in under 600 ms on a SATA SSD.
- **Vacuum** — vacuum process to reclaim unused space from overwritten or deleted keys; query performance does not degrade as the database grows.
- **Lock-free multi-reader, single-writer** — reads are lock-free and scale to millions of operations per second. Writes are serialised under a single mutex; on the success path, `state_.store()` happens after `fdatasync`, guaranteeing durability before visibility.
- **Crash safety** — CRC-verified entries, atomic hint file generation (`write → fdatasync → rename`), and append-only data files as the primary durable store. On unrecoverable write-path failures (e.g. isolation rotation fails), the engine enters a degraded state: reads remain available, all writes throw `DbDegraded`, and the service calls `resume()` to recover without a restart.

## Performance

Benchmarked against [RocksDB](https://rocksdb.org/) at 1 M keys. The tables below have the numbers; the summary:

- **Reads are 2–3× faster** once the working set exceeds RocksDB's block cache (from ~500 k keys onward).
- **Concurrent reads scale linearly** — lock-free snapshots, no shared mutex.
- **Sequential writes are comparable** — both engines append; ±10 % across all write benchmarks. No write amplification from compaction, so Sync Delete is 2× faster.
- **Range scans over values are slower** — each value is a separate disk read. Key-only iteration (`keys_from`) is a pure in-memory tree walk.
- **Recovery is fast and parallel** — hint files replayed across all cores with full CRC verification. 1 M keys in ~60 ms, 10 M in ~580 ms at 16 threads.

See [`docs/bytecask_benchmark_showcase.md`](docs/bytecask_benchmark_showcase.md) for the full benchmark report with all thread counts, dataset sizes, and hardware details.

---

### Single-Threaded Throughput (1M keys)

> CRC verification is disabled for read operations; enabled for recovery.

| Operation | ByteCaskDB | RocksDB | Notes |
|-----------|----------|---------|-------|
| Put (NoSync) | 157 Kops/s | **166 Kops/s** | Comparable with an edge for RocksDB. Sequential append on both sides |
| Put (Sync) | 435 ops/s |**478.0 ops/s** | Comparable  with an edge for RocksDB. Disk-bound — limited by `fdatasync` round-trip latency |
| Get | **1.34 Mops/s** | 575 Kops/s | **2.3×** — in-memory radix tree lookup; no block cache miss risk |
| Del (Sync) | **657 ops/s** | 320 ops/s | **2.1×** — single tombstone append vs RocksDB write amplification |
| Range-50 | 30 K scans/s | **87 K scans/s** | RocksDB prefetches sequential blocks; ByteCaskDB fetches each value individually. |
| MixedBatch (Sync) | 34 Kops/s | 33 Kops/s | Comparable |

ByteCaskDB's read advantage grows with dataset size: at 50k keys RocksDB's block cache covers the entire working set and leads; from 500k keys onward the cache misses and ByteCaskDB pulls ahead by **2–3×**.

### Get Latency (1M keys, CRC disabled)

| Percentile | ByteCaskDB | RocksDB |
|-----------|---------|----------|
| p50 | **680 ns** | 1.57 µs |
| p99 | **1.15 µs** | 3.96 µs |

Latency stays flat as the dataset grows: ByteCaskDB always reads from the OS page cache at a known offset; RocksDB's latency climbs when key metadata exceeds the block cache.

### Concurrent Reads — `GetMT` (1M keys, CRC disabled)

> ByteCaskDB reads are lock-free; each thread holds an immutable snapshot of the engine state.

| Threads | ByteCaskDB | RocksDB | ByteCaskDB / RocksDB |
|---:|---:|---:|:---:|
| 2 | **2.56 Mops/s** | 1.13 Mops/s | 2.27× |
| 4 | **4.27 Mops/s** | 2.15 Mops/s | 1.99× |
| 8 | **6.10 Mops/s** | 4.44 Mops/s | 1.37× |
| 16 | **8.61 Mops/s** | 6.17 Mops/s | 1.40× |
| 32 | **11.43 Mops/s** | 8.30 Mops/s | 1.38× |

### Read-While-Writing (1M keys, 1 writer + N readers, Sync, CRC disabled)

> Two read consistency modes, both controlled via `ReadOptions::staleness_tolerance`: the default (`staleness_tolerance = 0`) refreshes the thread-local snapshot on every write, guaranteeing same-thread read-your-writes; setting `staleness_tolerance > 0` refreshes only when the tolerance window has elapsed — the hot path is a single relaxed load with no locked instructions or refcount traffic.

| Readers | ByteCaskDB | ByteCaskDB (staleness_tolerance > 0) | RocksDB |
|---:|---:|---:|---:|
| 2 | 2.54 Mops/s | 2.61 Mops/s | 1.12 Mops/s |
| 4 | 4.26 Mops/s | 4.46 Mops/s | 2.14 Mops/s |
| 8 | 5.94 Mops/s | 6.22 Mops/s | 4.30 Mops/s |
| 16 | 8.55 Mops/s | 9.34 Mops/s | 6.27 Mops/s |

### Recovery

Recovery is the process that runs when ByteCaskDB opens an existing database: it rebuilds the in-memory key directory by reading compact hint files from disk, then verifies every entry with CRC-32. ByteCaskDB parallelises this across all available CPU cores — each core processes a disjoint set of data files independently, and the results are merged before the database becomes available.

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

// Conflict-safe CAS write — reads from a consistent snapshot,
// applies the plan only if every precondition holds.
auto snap = db.snapshot();
Bytes balance_out;
bool found = snap.get(to_bytes("account:42"), balance_out);
// ... compute new_balance ...
WritePlan plan{std::move(snap)};          // snapshot embedded in the plan
plan.ensure_unchanged(to_bytes("account:42"));   // guard: no concurrent write
plan.put(to_bytes("account:42"), new_balance);
if (!db.apply_batch_if({}, std::move(plan))) {
    // a concurrent writer modified "account:42" — retry
}

// Prefix scan — in-memory key walk, values fetched lazily from disk.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only prefix scan — pure in-memory, no disk I/O.
for (auto& key : db.keys_from({}, to_bytes("user:"))) { ... }

// Reverse scan — descending key order. Starts at last key <= "user:~".
for (auto& [key, value] : db.riter_from({}, to_bytes("user:~"))) { ... }

// Reverse keys-only — pure in-memory, descending order.
for (auto& key : db.rkeys_from({}, to_bytes("user:~"))) { ... }
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

struct Options {
    uint64_t max_file_bytes{64 * 1024 * 1024};  // active file rotation threshold (default 64 MiB)
    unsigned recovery_threads{4};                // parallelism for hint-file replay at open
    // When true (default): any CRC error during recovery causes DB::open to throw.
    // When false: corrupt entries and hint files are skipped; DB opens with the
    // keys that were successfully recovered. A warning is printed to stderr for
    // each skipped item.
    bool fail_recovery_on_crc_errors{true};
};

struct WriteOptions {
    bool sync{true};      // call fdatasync after write (default true)
    bool try_lock{false}; // non-blocking lock; throws std::system_error(EBUSY) if contended
};

struct ReadOptions {
    // staleness_tolerance == 0 (default): refresh thread-local snapshot on every write.
    // staleness_tolerance  > 0: refresh only when the last write is older than this window.
    std::chrono::milliseconds staleness_tolerance{0};
    bool verify_checksums{false}; // CRC-verify each value read from disk (default false)
};

class DB {
public:
    // DB is non-copyable and non-moveable; open() relies on mandatory copy elision.
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   Options opts = {}) -> DB;

    // Writes value for key into out, reusing its capacity. Returns true if found.
    // Throws std::system_error on I/O failure, std::runtime_error on CRC mismatch,
    // or DbDegraded if the engine is degraded.
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;

    // Writes key → value. Overwrites any existing value.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    void put(const WriteOptions& opts, BytesView key, BytesView value);

    // Writes a tombstone for key. Returns true if the key existed.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto del(const WriteOptions& opts, BytesView key) -> bool;

    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // Atomically applies all operations in batch. Consumes batch (move-only).
    void apply_batch(const WriteOptions& opts, Batch batch);

    // Returns a frozen, move-only, read-only view of the DB at this instant.
    // Holds open referenced data files until destroyed — vacuum deferred automatically.
    [[nodiscard]] auto snapshot() const -> Snapshot;

    // Applies plan atomically iff every guard holds and no write key was modified
    // since the plan's snapshot. Returns true if committed (including when the plan
    // is empty — no-op). Returns false on conflict.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto apply_batch_if(WriteOptions opts,
                                      WritePlan plan) -> bool;

    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

    [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

    // Returns true if a file was vacuumed, false if no file qualified.
    [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

    // True if the engine has entered a degraded state from a write-path failure.
    // Reads remain available; all write operations throw DbDegraded.
    [[nodiscard]] auto is_degraded() const noexcept -> bool;
    [[nodiscard]] auto degraded_reason() const noexcept -> const std::string&;

    // Attempts to recover from a degraded state. Scans the active file to find
    // the last valid committed offset, truncates garbage bytes, syncs, seals
    // the file, and opens a new active file. On success, clears the degraded
    // flag. On failure, the engine stays degraded and the caller may retry.
    // No-op if the engine is not degraded.
    void resume();
};

// Frozen, move-only, read-only view of DB state at a point in time.
// Holds open any referenced data files until destroyed.
class Snapshot {
public:
    [[nodiscard]] auto get(BytesView key, Bytes& out) const -> bool;
    [[nodiscard]] auto contains_key(BytesView key) const -> bool;
    [[nodiscard]] auto iter_from(BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
    [[nodiscard]] auto riter_from(BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;
    [[nodiscard]] auto rkeys_from(BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;
};

// Conditional write plan for apply_batch_if.
// Construct with WritePlan(snap) to enable ensure_unchanged / ensure_range_unchanged guards;
// those methods throw std::logic_error if called on a snapshot-less WritePlan().
class WritePlan {
public:
    WritePlan();                         // snapshot-less: only ensure_present/ensure_absent available
    explicit WritePlan(Snapshot snap);   // snapshot embedded; all guards available

    void put(BytesView key, BytesView value);
    void del(BytesView key);

    void ensure_present(BytesView key);                         // guard: key must exist
    void ensure_absent(BytesView key);                          // guard: key must be absent
    void ensure_unchanged(BytesView key);                       // guard: key unchanged since snapshot
    void ensure_range_unchanged(BytesView from, BytesView to);  // guard: no key change in [from, to)

    [[nodiscard]] auto has_snapshot() const noexcept -> bool;
};

// Thrown by write operations when the engine is in a degraded state.
class DbDegraded : public std::runtime_error { /* ... */ };

} // namespace bytecask
```

Error handling follows the throw-on-failure convention used by the C++ standard library: I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; write operations on a degraded engine throw `DbDegraded` (a `std::runtime_error` subclass, catchable separately). Key-not-found is signalled by `get` returning `false`; `apply_batch_if` returns `false` on precondition or W-W conflict — conflicts are expected outcomes, not exceptional errors.


## Architecture

### Design Principles

ByteCaskDB is designed around four core tenets, in priority order:

1. **Correctness** — data integrity above all else.
2. **Simplicity** — few moving parts; the design is easy to understand and maintain.
3. **Predictable latency over peak throughput** — bounded, flat write latency at every scale. A steady 1 ms per write is preferable to an average 0.1 ms with occasional 500 ms spikes.
4. **Performance** — optimisations require a real use case. Without one, correctness and simplicity take priority.

### Components

```
  ByteCaskDB
  ├── Key Directory  PersistentRadixTree<KeyDirEntry>   (all keys, in memory)
  ├── File Registry  map<file_id, DataFile>             (open file descriptors)
  ├── Active File    append-only .data file             (current writes)
  └── Sealed Files   read-only .data + .hint files      (older segments)
```

**Write path**: all writes route through a single coordinator (`apply_batch_if`). Each write appends one or more CRC-32-verified, length-prefixed records to the active data file (single-entry writes: one record; multi-entry batches: begin marker + N entry records + end marker), then applies pure in-memory state transitions via `TransientEngineState`. When `sync` is requested, `fdatasync` completes before the new state becomes visible to readers.

**Read path**: readers obtain an immutable snapshot of the engine state, look up the key in the radix tree to find its file and offset, then read the value directly. Reads are lock-free and scale linearly across cores.

**Recovery**: on `open`, the engine generates a hint file for any data file that lacks one (including the most recent active file), then replays all hint files in parallel to rebuild the key directory. Hint files are compact per-file indexes written atomically (`write → fdatasync → rename`) by a background worker after each file rotation and synchronously at engine close. No raw data-file scan is performed — recovery reads only hint files.

See [`docs/bytecask_design.md`](docs/bytecask_design.md) for the full design reference.

## Building

ByteCaskDB requires **Clang** (with C++23 modules support) and [xmake](https://xmake.io).

```bash
# Build and run the test suite.
xmake build 
xmake run bytecask_tests

# Build benchmarks (optional; requires RocksDB).
python ./scripts/run_engine_bench.py
```

A ready-to-use development environment is provided via the included [Dev Container](.devcontainer) (Fedora 43, Clang, xmake pre-installed).

## Want to hack on it?

ByteCaskDB is early-stage and there's plenty of room to explore — new features, performance ideas, test coverage, documentation, or just poking around the internals. All of it is welcome.

See [CONTRIBUTING.md](CONTRIBUTING.md) to get started. The fastest path is to open it directly in GitHub Codespaces — no local setup required.

If you want to take it in a different direction and fork it into your own thing, go for it — that's what the MIT license is for.

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/bytecask_design.md`](docs/bytecask_design.md) | Living design reference: architecture, concurrency model, file format, vacuum, recovery |
| [`docs/file_format.md`](docs/file_format.md) | On-disk file format reference: data file entries, hint file entries, CRC, byte order, naming |
| [`docs/engine_api_design.md`](docs/engine_api_design.md) | Public API specification with usage examples |
| [`docs/parallel_recovery_design.md`](docs/parallel_recovery_design.md) | Parallel recovery algorithm and fan-in merge strategy |
| [`docs/persistent_radix_tree_design.md`](docs/persistent_radix_tree_design.md) | Persistent radix tree data structure design |
| [`docs/bytecask_project_plan.md`](docs/bytecask_project_plan.md) | Issue tracker and project history |
| [`docs/correctness_validation.md`](docs/correctness_validation.md) | Write-path correctness validation: failure classes, proof test matrix, fault injection framework |
| [`docs/failure_mode_comparison.md`](docs/failure_mode_comparison.md) | Write-path failure mode comparison: ByteCaskDB vs RocksDB, LevelDB, SQLite WAL, LMDB, WiredTiger |

## License

ByteCaskDB is dual-licensed:

- **MIT** — core engine (`src/`, `include/`, `tests/`, `benchmarks/`). See [`LICENSE`](LICENSE).
- **GPL-2.0-only** — MariaDB plugin (`mariadb/`). Required by the MariaDB plugin API.

See [`docs/project_organization.md`](docs/project_organization.md) for the full license boundary and rationale.
