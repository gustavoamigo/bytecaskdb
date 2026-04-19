# ByteCaskDB

> **Status: early development.** The core engine works and is well-tested, but the API and on-disk format may change before a stable release. Not recommended for production use yet.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecaskdb)

**ByteCaskDB** is a fast, predictable embedded key-value store written in C++. Reads and writes have flat, predictable latency from thousands of keys to hundreds of millions.

All keys in memory at all times — a deliberate design choice that removes an entire class of complexity that exists solely to minimise disk access and makes every point lookup O(1) with flat, predictable latency. At ~100 bytes per key, 128 GB of RAM holds roughly a billion keys. Very few moving parts — an in-memory key directory and an append-only data file — is what keeps that latency flat whether you have 1,000 records or 100 million. 

Built on the [Bitcask](https://riak.com/assets/bitcask-intro.pdf) append-only foundation, ByteCaskDB replaces the original hash-table key directory with a **[persistent radix tree](docs/persistent_radix_tree_design.md)** — enabling ordered range queries, prefix scans, and prefix compaction, while keeping the simplicity that makes Bitcask fast. Snapshots are O(1) — just a root pointer copy. Full MVCC and serializable conflict detection are supported with no separate transaction type required.

## Features

- **Sequential write path** — all I/O is sequential appends; no random writes. Every `put` and `del` is one append. `apply_batch` with N operations appends a begin marker, N entries, and an end marker in a single `writev` — still no WAL, no random writes.
- **Ordered range iteration** — scan from any key prefix using the in-memory radix tree; no disk I/O for key enumeration. Bidirectional: scan forward with `iter_from`/`keys_from` or backward with `riter_from`/`rkeys_from`.
- **Range deletion** — `del_range(opts, from, to)` deletes all keys in `[from, to)` with a single data file append. In-memory cleanup walks the radix tree; disk cost is O(1) regardless of how many keys fall in the range. Available on `DB` and `WritePlan`.
- **Atomic writes** — every `put`, `del`, and `del_range` is atomic. `apply_batch` makes multiple puts, deletes, and range deletes atomic as a group.
- **MVCC transactions** — `snapshot` captures a consistent point-in-time read-only view; `apply_batch(opts, plan)` applies a `WritePlan` atomically only when every precondition holds (**key present / absent / unchanged**, **range unchanged**), returning `false` on conflict. The snapshot is embedded in the `WritePlan` at construction time. When a snapshot is present, every key in the write set is automatically checked for concurrent modification — no explicit guard needed on keys you write. Use `ensure_unchanged` for keys you read but don't write, and range guards for serializable conflict detection. Together they cover the full isolation spectrum: read from a `Snapshot` for **snapshot isolation**, add guards for **serializable** conflict detection, or use bare `put`/`del` for **read-uncommitted** fast paths. All precondition checks are in-memory radix tree traversals — no disk I/O, no separate transaction type required.
- **Fast recovery** — parallelised index reconstruction from hint files; 10 M keys recover in under 600 ms on a SATA SSD.
- **Vacuum** — vacuum process to reclaim unused space from overwritten or deleted keys; query performance does not degrade as the database grows.
- **Lock-free multi-reader, single-writer** — reads are lock-free and scale to millions of operations per second. Writes are serialised under a single mutex with group commit: concurrent sync writers share a single `fdatasync` call, amortising the dominant cost. On the success path, `state_.store()` happens after `fdatasync`, guaranteeing durability before visibility.
- **Crash safety** — CRC-verified entries, atomic hint file generation (`write → fdatasync → rename`), and append-only data files as the primary durable store. On unrecoverable write-path failures (e.g. isolation rotation fails), the engine enters a degraded state: reads remain available, all writes throw `DbDegraded`, and the service calls `resume()` to recover without a restart.

## Performance

Benchmarked at 1 M keys with [RocksDB](https://rocksdb.org/) as a reference point. The tables below include both engines for context.

- **Point reads** reach 1.34 Mops/s at 1 M keys with flat, sub-microsecond latency (p50 702 ns, p99 920 ns). Latency stays flat as the dataset grows because every lookup is an in-memory radix tree traversal followed by a single `pread` at a known offset.
- **Concurrent reads scale linearly** — lock-free snapshots with no shared mutex. 15.3 Mops/s at 32 threads.
- **Sequential writes** sustain 149 Kops/s (NoSync) and 498 ops/s (Sync), limited by `fdatasync` round-trip latency. No write amplification from compaction.
- **Concurrent sync writes scale via group commit** — writers share a single `fdatasync` call. 16.6 Kops/s at 64 threads.
- **Range scans over values** fetch each value individually from disk. LSM-based engines pack values contiguously in sorted runs and perform better here. Key-only iteration (`keys_from`) is a pure in-memory tree walk with no disk I/O.
- **Recovery is fast and parallel** — hint files replayed across all cores with full CRC verification. 1 M keys in ~57 ms, 10 M in ~528 ms at 16 threads.

See [`docs/bytecask_benchmark_showcase.md`](docs/bytecask_benchmark_showcase.md) for the full benchmark report with all thread counts, dataset sizes, and hardware details.

---

### Single-Threaded Throughput (1M keys)

> CRC verification is disabled for read operations; enabled for recovery.

| Operation | ByteCaskDB | RocksDB | Notes |
|-----------|----------|---------|-------|
| Put (NoSync) | 149 Kops/s | 179 Kops/s | Sequential append on both sides |
| Put (Sync) | 498 ops/s | 463 ops/s | Disk-bound — limited by `fdatasync` round-trip latency |
| Get | 1.34 Mops/s | 566 Kops/s | In-memory radix tree lookup; flat latency regardless of dataset size |
| Del (Sync) | 678 ops/s | 273 ops/s | Single tombstone append; no compaction write amplification |
| Range-50 | 30 K scans/s | 82 K scans/s | LSM sorted runs favour sequential value scans |
| MixedBatch (Sync) | 42 Kops/s | 35 Kops/s | Atomic batch with single `writev` + `fdatasync` |

At small dataset sizes (50 k keys), all keys fit in RocksDB's block cache and reads are fast on both engines. From 500 k keys onward, block cache misses begin to dominate and the in-memory key directory approach shows its advantage.

### Get Latency (1M keys, CRC disabled)

| Percentile | ByteCaskDB | RocksDB |
|-----------|---------|----------|
| p50 | 702 ns | 1.53 µs |
| p99 | 920 ns | 4.18 µs |

Latency stays flat as the dataset grows: every read resolves to a known file offset via the in-memory key directory, so there is no metadata amplification from multiple levels or bloom filter checks.

### Concurrent Reads — `GetMT` (1M keys, CRC disabled)

> Reads are lock-free; each thread holds an immutable snapshot of the engine state.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.45 Mops/s | 816 Kops/s |
| 4 | 4.49 Mops/s | 1.96 Mops/s |
| 8 | 7.42 Mops/s | 4.19 Mops/s |
| 16 | 10.33 Mops/s | 6.70 Mops/s |
| 32 | 15.32 Mops/s | 9.70 Mops/s |

### Concurrent Sync Writes — `PutMT/Sync` (1M keys)

> Group commit: concurrent sync writers share a single `fdatasync` call, amortising the dominant cost. The benefit grows with concurrency — more contending writers means larger batches and fewer `fdatasync` calls per write.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 496 ops/s | 751 ops/s |
| 4 | 986 ops/s | 1.0 Kops/s |
| 8 | 1.8 Kops/s | 1.9 Kops/s |
| 16 | 4.6 Kops/s | 1.3 Kops/s |
| 32 | 8.9 Kops/s | 2.3 Kops/s |
| 64 | 16.6 Kops/s | 5.4 Kops/s |

### Read-While-Writing (1M keys, 1 writer + N readers, Sync, CRC disabled)

> Lock-free readers are unaffected by concurrent writes. Read throughput scales the same whether the database is idle or under write load.

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.44 Mops/s | 761 Kops/s |
| 4 | 4.36 Mops/s | 1.88 Mops/s |
| 8 | 6.55 Mops/s | 4.07 Mops/s |
| 16 | 9.43 Mops/s | 6.50 Mops/s |
| 32 | 15.86 Mops/s | 10.03 Mops/s |

### Recovery

Recovery runs when ByteCaskDB opens an existing database: it rebuilds the in-memory key directory by reading compact hint files from disk, then verifies every entry with CRC-32. This is parallelised across all available CPU cores — each core processes a disjoint set of data files independently, and the results are merged before the database becomes available.

| Keys | Threads | Recovery Time | Speedup vs 1T |
|---:|---:|---:|---:|
| 1M | 1 | 239 ms | — |
| 1M | 4 | 85 ms | 2.8× |
| 1M | 8 | 65 ms | 3.7× |
| 1M | 16 | 57 ms | 4.2× |
| 10M | 1 | 2.42 s | — |
| 10M | 4 | 0.91 s | 2.7× |
| 10M | 8 | 0.62 s | 3.9× |
| 10M | 16 | 0.53 s | 4.6× |

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (485 MiB/s read), 31 GiB RAM. Each result is the mean of 5 runs. Benchmark source: [`benchmarks/engine_bench.cpp`](benchmarks/engine_bench.cpp)._

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

// Range deletion — delete all keys in [from, to) with a single disk append.
db.del_range({}, to_bytes("session:"), to_bytes("session:~"));

// Atomic batch — all operations land atomically.
WritePlan plan;
plan.put(to_bytes("user:2"), to_bytes("bob"));
plan.put(to_bytes("user:3"), to_bytes("carol"));
plan.del(to_bytes("user:1"));
(void)db.apply_batch({}, std::move(plan));

// Decrement stock — write keys are checked for conflicts automatically.
auto snap = db.snapshot();
Bytes stock_out;
snap.get(to_bytes("stock:widget"), stock_out);
// ... decrement stock count ...
WritePlan plan2{std::move(snap)};
plan2.put(to_bytes("stock:widget"), new_stock);
if (!db.apply_batch({}, std::move(plan2))) {
    // another writer changed stock:widget since our snapshot — retry
}

// Place order at current price — ensure_unchanged guards keys you read
// but don't write. Write keys are checked automatically.
auto snap2 = db.snapshot();
Bytes price_out;
snap2.get(to_bytes("price:widget"), price_out);
// ... compute order_total from price ...
WritePlan order{std::move(snap2)};
order.ensure_unchanged(to_bytes("price:widget"));  // reject if price changed
order.put(to_bytes("order:99"), order_total);
if (!db.apply_batch({}, std::move(order))) {
    // price changed since snapshot — re-read price and recompute
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
    bool solo{false};     // bypass group commit — route to solo writer (for benchmarking)
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

    // Deletes all keys in [from, to) with a single data file append.
    // No-op if from >= to. Throws std::system_error on I/O failure or DbDegraded.
    void del_range(const WriteOptions& opts, BytesView from, BytesView to);

    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // Atomically applies all operations in plan. When the plan has a snapshot
    // or explicit guards, returns false on conflict. A guardless, snapshot-less
    // plan always returns true (no conflict possible).
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto apply_batch(WriteOptions opts,
                                   WritePlan plan) -> bool;

    // Returns a frozen, move-only, read-only view of the DB at this instant.
    // Holds open referenced data files until destroyed — vacuum deferred automatically.
    [[nodiscard]] auto snapshot() const -> Snapshot;

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

// Write plan for apply_batch. Groups multiple operations into a single atomic write.
// Construct with WritePlan(snap) to enable ensure_unchanged / ensure_range_unchanged guards;
// those methods throw std::logic_error if called on a snapshot-less WritePlan().
// When a snapshot is present, apply_batch automatically rejects the plan if any
// write key (put or del) changed since the snapshot — no explicit guard needed on
// keys in the write set. Use ensure_unchanged for read-only dependencies: keys whose
// value influenced the plan but that the plan does not modify.
// A guardless, snapshot-less WritePlan always commits successfully (no conflict possible).
class WritePlan {
public:
    WritePlan();                         // snapshot-less: only ensure_present/ensure_absent available
    explicit WritePlan(Snapshot snap);   // snapshot embedded; all guards available

    void put(BytesView key, BytesView value);
    void del(BytesView key);
    void del_range(BytesView from, BytesView to);  // range delete: [from, to)

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

Error handling follows the throw-on-failure convention used by the C++ standard library: I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; write operations on a degraded engine throw `DbDegraded` (a `std::runtime_error` subclass, catchable separately). Key-not-found is signalled by `get` returning `false`; `apply_batch` returns `false` on precondition or W-W conflict — conflicts are expected outcomes, not exceptional errors.


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

**Write path**: all writes route through a single coordinator (`apply_batch`). Concurrent sync writers are batched via group commit — the first writer becomes leader, drains the queue, and executes all pending writes under one lock hold with a single `fdatasync`. Each write appends CRC-32-verified, length-prefixed records to the active data file, then applies pure in-memory state transitions via `TransientEngineState`. Durability before visibility: `state_.store()` happens after `fdatasync`.

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
| [`docs/replication_primitives_design.md`](docs/replication_primitives_design.md) | Replication primitives: minimal API surface for building leader-follower replication on top of ByteCaskDB |
| [`docs/xa_support_design.md`](docs/xa_support_design.md) | XA / two-phase commit: generic 2PC primitives (`BulkPrepare`, `Bulk2PCCommit`, `Bulk2PCRollback`) for external coordinators |
| [`CONTRACT.md`](CONTRACT.md) | Per-function behavioral contracts: atomicity, durability, I/O failure safety, LSN invariants |

## License

ByteCaskDB is dual-licensed:

- **MIT** — core engine (`src/`, `include/`, `tests/`, `benchmarks/`). See [`LICENSE`](LICENSE).
- **GPL-2.0-only** — MariaDB plugin (`mariadb/`). Required by the MariaDB plugin API.

See [`docs/project_organization.md`](docs/project_organization.md) for the full license boundary and rationale.
