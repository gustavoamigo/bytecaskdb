# ByteCaskDB Engine API Design

This document describes the public C++ API for the main `DB` class — the entry point for all ByteCaskDB operations.

Canonical location: `docs/engine_api_design.md`.

---

## Goals

- Provide a clean, minimal API surface for key-value operations.
- Support atomic multi-operation batches.
- Support ordered range iteration (enabled by the radix-tree key directory).
- Be idiomatic C++20: no raw pointers, no stringly-typed errors, move-only ownership.

## Non-Goals (for now)

- Multi-writer access, MVCC, or snapshot isolation. ByteCaskDB uses a SWMR model; see Architecture below.
- TTL or expiry.
- Async I/O.

---

## Architecture

### Key Directory

ByteCaskDB uses `PersistentRadixTree<KeyDirEntry>` as the in-memory key directory. All keys reside in memory at all times. The immutable trie structure provides structural sharing so readers take a cheap snapshot of the root without acquiring any lock.

`PersistentRadixTree<V>` is a custom copy-on-write adaptive radix tree (ART-style) implemented in `src/radix_tree.cppm`. It supports O(k) get/set/erase (k = key length), structural sharing between versions, and in-order iteration via `RadixTreeIterator<V>`.

### Concurrency Model

ByteCaskDB follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time (`write_mu_` serialises `put`, `del`, `apply_batch`). Concurrent sync writers are batched via group commit.
- Multiple readers may operate concurrently, isolated from writes by a persistent snapshot of the key directory loaded via `state_.load()`.
- `ReadOptions::staleness_tolerance` lets readers trade freshness for throughput: a non-zero tolerance allows reading from a snapshot that is at most that many milliseconds old (bounded staleness). The default (0) provides read-your-writes session consistency.
- When `sync` is requested, `fdatasync` completes before the new state becomes visible to readers (durability before visibility).
- MVCC snapshot isolation is provided via `snapshot()` + `apply_batch(WritePlan)`.

### Data File Lifecycle

Each data file transitions through three phases in sequence:

| Phase | Description |
|-------|-------------|
| **Active** | The current append target; accepts all writes. |
| **Rotating** | Closed to new writes; a companion `.hint` file is being written atomically. |
| **Immutable** | The `.hint` file exists; the data file is sealed and read-only. |

A new active data file is always created on engine startup.

### File Naming Convention

Files use a timestamp-based stem: `data_{YYYYMMDDHHmmssUUUUUU}` where `UUUUUU` is the microsecond sub-second component (zero-padded, 6 digits).

Examples:
- `data_20260329123456123456.data`
- `data_20260329123456123456.hint`

Rationale:
- `std::chrono::system_clock` reliably delivers microsecond precision on Linux; nanoseconds would add false precision since kernel clock granularity is often coarser.
- Lexicographic sort equals chronological order, so file scanning needs no secondary sort key.
- The timestamp string doubles as the unique **file ID** stored in key directory entries.

### Vacuum

Vacuum is **online**: the engine continues to serve reads and writes while vacuum scans and rewrites data. `vacuum_mu_` serialises concurrent `vacuum()` calls independently from `write_mu_`, so only the brief commit step (remapping key-directory entries and swapping files) blocks writers. `vacuum()` is designed to be called safely from a dedicated background thread.

The caller drives the vacuum loop:

```cpp
while (!stop_requested) {
    sleep(1h);
    while (db.vacuum()) {
        sleep(2s);   // more files may still qualify; keep draining
    }
}
```

### Log Sequence Number (LSN)

The LSN is a **globally monotonic** counter — not a per-file counter. This is a correctness invariant: recovery compares LSNs from entries in different files to decide which is fresher; any per-file counter reset would silently allow stale data to overwrite live data.

- The engine owns and increments the global LSN. `DataFile` is a passive consumer — the caller passes `sequence` to every `append` call.
- On startup the engine scans all hint files and the active data file to find `max_lsn`, then seeds the new active `DataFile` at `max_lsn + 1`.

### Recovery

On engine startup:

1. Discard any `.hint.tmp` files — they are incomplete hint files from a crash mid-rotation.
2. Read hint files oldest-to-newest. For each entry: verify the trailing CRC (**panic on any mismatch**); then apply based on `entry_type`:
   - `Put`: insert `(key → {sequence, file_id, file_offset, value_size})` only if `entry.sequence > dir[key].sequence`.
   - `Delete`: remove the key from the directory if `entry.sequence > dir[key].sequence`; otherwise skip.
3. For any data file without a companion `.hint`, scan its raw bytes using the same Put/Delete rules. Skip `BulkBegin`/`BulkEnd` records. If a `BulkBegin` has no matching `BulkEnd`, discard all entries from that point onward and log a warning.
4. Record `max_lsn` — the largest sequence number seen across all files.
5. Create a new active data file seeded at `max_lsn + 1`.

### Hint File Atomicity

Hint files are written during file rotation (active → immutable). To guarantee a hint file is either complete or absent:

1. Scan the data file and write one hint entry per `Put`, `Delete`, or `RangeDel` record to `data_{timestamp}.hint.tmp`. `BulkBegin`/`BulkEnd` structural markers are written with their sequence numbers (for accurate `next_seq` computation on recovery) but carry no key/value payload.
2. Call `fdatasync` to flush all bytes to physical storage.
3. Atomically `rename(2)` to `data_{timestamp}.hint` — POSIX guarantees this is atomic on the same filesystem.

Any `.hint.tmp` file found at startup indicates a crash mid-rotation and is discarded; recovery re-scans the corresponding `.data` file instead.

### Incomplete Batch Recovery

If the engine crashes after writing a `BulkBegin` but before the matching `BulkEnd`, the active data file contains an incomplete batch. Recovery discards all entries from the unmatched `BulkBegin` onward and logs a warning. No partial-batch entries are inserted into the key directory. Because hint files are only written for immutable (fully rotated) files, an incomplete batch can only appear in the active data file scan — never in a hint file.

---

## Type Aliases

```cpp
// Owned byte buffer — used for return values and batch storage.
using Bytes = std::vector<std::byte>;

// Non-owning view — used for all input parameters.
using BytesView = std::span<const std::byte>;
```

**Rationale:** `std::byte` makes the intent clear (raw bytes, not text) and prevents accidental arithmetic. `BytesView` as the universal input type avoids copies at call sites and accepts any contiguous range (`std::vector<std::byte>`, `std::string`, `std::array`, string literals via a small helper).

---

## Options

```cpp
// Passed to DB::open().
struct Options {
    // Active-file rotation threshold in bytes (default 64 MiB).
    std::uint64_t max_file_bytes{64ULL * 1024 * 1024};
    // Number of threads used to rebuild the key directory at open time.
    // 1 selects the serial path; >1 uses file-level fan-in parallelism.
    unsigned recovery_threads{4};
};
```

## WriteOptions

```cpp
// Controls durability behaviour for put, del, apply_batch.
struct WriteOptions {
    // When true (default), fdatasync is called after the write.
    // Set to false for higher throughput when durability can be relaxed.
    bool sync{true};

    // When true, bypasses the write group and executes the write alone under
    // write_mu_. Default false — writes go through the group commit path.
    bool solo{false};
};
```

## ReadOptions

```cpp
// Controls consistency behaviour for get, contains_key, iter_from, keys_from.
struct ReadOptions {
    // Maximum age of the cached snapshot before the reader refreshes it.
    // 0 (default): read-your-writes session consistency — refresh on every write.
    // > 0: bounded staleness — snapshot may be up to this many milliseconds old.
    //      Useful for write-heavy workloads where read throughput matters more
    //      than freshness. The staleness check is a single relaxed load (no lock,
    //      no clock read on the reader side).
    std::chrono::milliseconds staleness_tolerance{0};

    // When true, CRC32 is verified for every value read from disk.
    // Default true for data integrity; disable for higher throughput when
    // silent corruption detection is not required.
    bool verify_checksums{true};
};
```

## VacuumOptions

```cpp
// Controls which sealed files are eligible for vacuum.
struct VacuumOptions {
    // Minimum fragmentation ratio (1 − live_bytes / total_bytes) a sealed
    // file must exceed to be eligible. Range [0.0, 1.0]. Default 0.5.
    double fragmentation_threshold{0.5};
};
```

---

## WritePlan

`WritePlan` is the unified type for all write operations submitted to `apply_batch`. It carries both **writes** (`put`, `del`, `del_range`) and optional **guards** (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`).

```cpp
class WritePlan {
public:
    WritePlan();                         // snapshot-less: only ensure_present/ensure_absent available
    explicit WritePlan(Snapshot snap);   // snapshot embedded; all guards available

    WritePlan(const WritePlan&)            = delete;
    WritePlan& operator=(const WritePlan&) = delete;
    WritePlan(WritePlan&&) noexcept        = default;
    WritePlan& operator=(WritePlan&&) noexcept = default;

    void put(BytesView key, BytesView value);
    void del(BytesView key);
    void del_range(BytesView from, BytesView to);  // range delete: [from, to)

    void ensure_present(BytesView key);                         // guard: key must exist
    void ensure_absent(BytesView key);                          // guard: key must be absent
    void ensure_unchanged(BytesView key);                       // guard: key unchanged since snapshot
    void ensure_range_unchanged(BytesView from, BytesView to);  // guard: no key change in [from, to)

    [[nodiscard]] auto has_snapshot() const noexcept -> bool;
    [[nodiscard]] auto empty() const noexcept -> bool;

private:
    // Per-key merged representation.
    struct KeyAction { ... };
    std::map<Bytes, KeyAction> actions_;

    struct RangeGuard { Bytes from; Bytes to; };
    std::vector<RangeGuard> range_guards_;

    friend class DB;
};
```

**Rationale:** `WritePlan` unifies unconditional writes and conditional (guarded) writes into a single type consumed by `apply_batch`. When constructed without a snapshot, it behaves as a simple unconditional batch. When constructed with a snapshot, guards and implicit W-W checks are available. `WritePlan` is move-only and single-use; `DB::apply_batch` consumes it by move. No size limit is imposed by the engine.

---

## Iterators

Both iterators satisfy `std::input_iterator`. They are forward-only and yield entries in ascending key order.

```cpp
// Yields keys only (no data file I/O). Walks the in-memory radix tree.
class KeyIterator {
public:
    using value_type      = Bytes;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> KeyIterator&;
    void operator++(int);
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};

// Yields (key, value) pairs. Reads values lazily from disk via pread.
class EntryIterator {
public:
    using value_type      = std::pair<Bytes, Bytes>;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> EntryIterator&;
    void operator++(int);
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};
```

Both integrate with `std::ranges::subrange` so callers can use range-for directly:

```cpp
for (auto& [key, value] : db.iter_from(opts, start_key)) { ... }
for (auto& key : db.keys_from(opts, prefix))              { ... }
```

**Decisions:**
- **Lazy**: `operator*` reads one value from disk on demand via a single `pread`. Early-termination scans pay no I/O cost for unvisited entries.
- **`KeyIterator` is in-memory only**: walks the radix-tree key directory without touching any data file.
- **Error handling**: throws `std::system_error` on I/O failure (consistent with all other operations).

---

## DB

```cpp
class DB {
public:
    // Opens or creates a database rooted at dir.
    // Always creates a new active data file on open.
    // Throws std::system_error if the directory cannot be prepared.
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   Options opts = {}) -> DB;

    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&)                 = delete;
    DB& operator=(DB&&)      = delete;
    ~DB();

    // ── Primary operations ────────────────────────────────────────────────

    // Writes the value for key into out, reusing its existing capacity.
    // Returns true if the key was found, false otherwise.
    // opts.verify_checksums controls CRC verification on the read path.
    // Throws std::system_error on I/O failure or std::runtime_error on CRC mismatch.
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;

    // Writes key → value. Overwrites any existing value.
    // Throws std::system_error on I/O failure or DbDegraded if degraded.
    void put(const WriteOptions& opts, BytesView key, BytesView value);

    // Writes a tombstone for key.
    // Returns true if the key existed and was removed, false if it was absent.
    // Throws std::system_error on I/O failure or DbDegraded if degraded.
    [[nodiscard]] auto del(const WriteOptions& opts, BytesView key) -> bool;

    // Returns true if key exists in the index (no disk I/O).
    [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                    BytesView key) const -> bool;

    // ── Batch ─────────────────────────────────────────────────────────────

    // Atomically applies all operations in plan, wrapped in BulkBegin/BulkEnd.
    // plan is consumed (move-only). No-op if plan is empty and has no guards.
    // When the plan carries a snapshot, guards and implicit W-W checks are
    // evaluated; returns false on conflict. Returns true if committed.
    // opts.sync controls whether a single fdatasync is issued at the end.
    // Throws std::system_error on I/O failure or DbDegraded if degraded.
    [[nodiscard]] auto apply_batch(const WriteOptions& opts, WritePlan plan) -> bool;

    // ── Range iteration ───────────────────────────────────────────────────

    // Returns an input range of (key, value) pairs with keys >= from.
    // Pass an empty span to start from the first key. Each dereference reads
    // one value from disk via a single pread (lazy). Results are in ascending
    // key order.
    // Throws std::system_error on I/O failure.
    [[nodiscard]] auto iter_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    // Returns an input range of keys >= from. Walks the in-memory radix tree
    // only; no disk I/O.
    [[nodiscard]] auto keys_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    // ── Vacuum ────────────────────────────────────────────────────────────

    // Selects the highest-fragmentation sealed file above the threshold and
    // either removes it (if it has no live entries) or compacts it into a 
    // new sealed file.
    // Returns true if a file was vacuumed, false if no file qualified.
    // Thread-safe: safe to call from a dedicated background thread without
    // any external synchronisation; only the brief commit step blocks writers.
    [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

private:
    explicit DB(std::filesystem::path dir, Options opts);
};
```

---

## Usage Examples

```cpp
// Open (or create) a database.
auto db = DB::open("my_db");

// Default options.
constexpr WriteOptions kSync{};
constexpr ReadOptions  kRead{};

// Single-key operations.
db.put(kSync, as_bytes("user:1"), as_bytes("alice"));

Bytes out;
bool found = db.get(kRead, as_bytes("user:1"), out);

bool existed = db.del(kSync, as_bytes("user:1"));  // false if key was absent

// Atomic batch.
WritePlan plan;
plan.put(as_bytes("user:2"), as_bytes("bob"));
plan.put(as_bytes("user:3"), as_bytes("carol"));
plan.del(as_bytes("user:1"));
db.apply_batch(kSync, std::move(plan));

// Range scan.
for (auto& [key, value] : db.iter_from(kRead, as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan (no disk I/O — radix tree walk only).
for (auto& key : db.keys_from(kRead, as_bytes("user:"))) { ... }

// Background vacuum loop (run in a dedicated thread).
while (!stop_requested) {
    sleep(1h);
    while (db.vacuum()) {
        sleep(2s);
    }
}
```

> `as_bytes` is a small helper that converts a string literal or `std::string_view` to `BytesView`.

---

## Design Decisions

| # | Decision |
|---|----------|
| D1 | **Error handling**: Throw (`std::system_error` for I/O, `std::runtime_error` for corruption). These are panic-level events the caller cannot meaningfully recover from inline. `get` uses an output parameter + `bool` return instead of `std::optional` so the caller can reuse an existing buffer across repeated calls. No `std::expected` at this boundary. |
| D2 | **Config**: `Options` (open-time), `WriteOptions` (per-write durability), `ReadOptions` (per-read freshness and CRC), `VacuumOptions` (fragmentation thresholds). Modelled after LevelDB / RocksDB patterns. |
| D3 | **WritePlan ownership**: `WritePlan` is move-only (copy constructor and copy assignment deleted). Single-use by design; `apply_batch` consumes it. |
| D4 | **WritePlan size limit**: None — the caller is responsible. |
| D5 | **Iterator strategy**: Lazy — `operator*` reads one value from disk on demand via a single `pread`. Early-termination scans pay no I/O cost for unvisited entries. |
| D6 | **`KeyIterator` source**: In-memory only — walks the radix-tree key directory without opening any data file. |
| D7 | **`del` on missing key**: Returns `bool` — `true` if the key existed and was removed, `false` if it was absent. Consistent with `std::set::erase` returning a count. |
| D8 | **Error handling during iteration**: Throw `std::system_error` on I/O failure (consistent with D1). |
| D9 | **Concurrency model**: SWMR — exactly one writer at a time; reads are concurrent. Concurrent sync writers are batched via group commit. `ReadOptions::staleness_tolerance` enables bounded-staleness reads. |
| D10 | **Vacuum**: Online. `vacuum()` is safe to call from a background thread. Only the brief commit step (key-dir remap + file swap) blocks writers via `write_mu_`. A separate `vacuum_mu_` serialises concurrent `vacuum()` calls. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmss}_{RRRRRRRR}_V{XX}`. Timestamp is UTC second precision — a human-readable creation-time hint, not content age (compaction produces new files with old entries). `RRRRRRRR` is a 4-byte random hex salt for collision avoidance. `V{XX}` is the file format version (`V01` initially). Filename ordering carries no semantic meaning; entry sequence numbers are authoritative. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be silently discarded with a logged warning. No partial-batch entries enter the key directory. |
| D14 | **`DB` is not movable**: Both move constructor and move assignment are deleted. `DB` owns a mutex by `unique_ptr` and a `BackgroundWorker` with an internal thread; move semantics would leave the source in an indeterminate state. Use `DB::open` exclusively and store the result in place (e.g. in a `std::optional<DB>`). |
