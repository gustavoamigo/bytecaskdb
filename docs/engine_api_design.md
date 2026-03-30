# ByteCask Engine API Design

This document proposes the public C++ API for the main `Bytecask` class — the entry point for all ByteCask operations. It is intended as a discussion artifact before implementation begins.

Canonical location: `docs/engine_api_design.md`.

---

## Goals

- Provide a clean, minimal API surface for key-value operations.
- Support atomic multi-operation batches.
- Support ordered range iteration (enabled by the B-Tree key directory).
- Be idiomatic C++23: no raw pointers, no stringly-typed errors, move-only ownership.

## Non-Goals (for now)

- Multi-writer access, MVCC, or snapshot isolation. ByteCask uses a SWMR model; see Architecture below.
- TTL or expiry.
- Async I/O.
- Online (background) compaction. Vacuum is a fully offline operation.

---

## Architecture

### Key Directory

ByteCask uses `PersistentOrderedMap<Key, KeyDirEntry>` as the in-memory key directory. All keys reside in memory at all times. The immutable sorted-map structure enables cheap snapshots for concurrent reads without locks.

`immer::btree_map` does not exist in the immer library. `PersistentOrderedMap<K, V>` is a thin wrapper backed by `immer::flex_vector<Entry>` (Radix Balanced Tree) that provides sorted-map semantics: O(log n) get/set/erase, structural sharing, and a `transient()` / `persistent()` round-trip for batch mutations. See `src/engine/persistent_ordered_map.cppm`.

### Concurrency Model

ByteCask follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time.
- Multiple readers may operate concurrently, isolated from writes by a persistent snapshot of the key directory.
- MVCC and snapshot isolation are not supported.

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

Vacuum (also called *compaction* or *merge* in other systems; this project uses the PostgreSQL term) is **fully offline**: the engine must not be running while vacuum operates. There is no background or online compaction. This simplifies the storage engine because the key directory never needs to handle files being rewritten underneath it.

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

1. Scan the data file and write one hint entry per `Put` or `Delete` record to `data_{timestamp}.hint.tmp`. `BulkBegin`/`BulkEnd` structural markers are skipped — they carry no key/value and are not needed for key directory reconstruction.
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

// Owned key — semantically distinct from a generic byte buffer.
// Keys have an upper bound of 65 535 bytes (u16 key_size in the data file header).
// Starting as an alias for Bytes; may be refined to enforce the size invariant.
using Key = Bytes;

// Non-owning view — used for all input parameters.
using BytesView = std::span<const std::byte>;
```

**Rationale:** `std::byte` makes the intent clear (raw bytes, not text) and prevents accidental arithmetic. `Key` is kept distinct from `Bytes` so the key directory type (`PersistentOrderedMap<Key, KeyDirEntry>`) reads as its intent and provides a single point of change if the key type needs to evolve (e.g., to enforce the u16 size limit or adopt a small-buffer-optimized representation). `BytesView` as the universal input type avoids copies at call sites and accepts any contiguous range (`std::vector<std::byte>`, `std::string`, `std::array`, string literals via a small helper).

---

## Batch

```cpp
struct BatchInsert {
    Bytes key;
    Bytes value;
};

struct BatchRemove {
    Bytes key;
};

using BatchOperation = std::variant<BatchInsert, BatchRemove>;

class Batch {
public:
    Batch() = default;
    Batch(const Batch&)            = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) noexcept        = default;
    Batch& operator=(Batch&&) noexcept = default;

    void insert(BytesView key, BytesView value);
    void remove(BytesView key);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<BatchOperation> operations_;
    friend class Bytecask;
};
```

**Rationale:** `std::variant` over an inheritance hierarchy keeps `BatchOperation` a value type (no heap allocation per item, trivially movable). `Batch` is move-only and single-use; `Bytecask::apply_batch` consumes it by move. No size limit is imposed by the engine.

---

## Iterators

Both iterators satisfy `std::input_iterator`. They are forward-only and yield entries in ascending key order.

```cpp
// Yields (key, value) pairs.
class EntryIterator {
public:
    using value_type      = std::pair<Bytes, Bytes>;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> EntryIterator&;
    void operator++(int);                        // advance only; no copy
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};

// Yields keys only (no value I/O).
class KeyIterator {
public:
    using value_type      = Bytes;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> KeyIterator&;
    void operator++(int);                        // advance only; no copy
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};
```

Both integrate with `std::ranges::subrange` so callers can use range-for directly:

```cpp
for (auto& [key, value] : db.iter_from(start_key)) { ... }
for (auto& key : db.keys_from(prefix))              { ... }
```

**Decisions:**
- **Lazy**: `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries.
- **`KeyIterator` is in-memory only**: walks the B-Tree key directory without touching any data file.
- **Error handling**: throws `std::system_error` on I/O failure (consistent with all other operations).

---

## Bytecask

```cpp
class Bytecask {
public:
    // Opens or creates a database rooted at `dir`.
    // Throws std::system_error if the directory cannot be opened or recovery fails.
    [[nodiscard]] static auto open(std::filesystem::path dir) -> Bytecask;

    Bytecask(const Bytecask&)            = delete;
    Bytecask& operator=(const Bytecask&) = delete;
    Bytecask(Bytecask&&) noexcept        = default;
    Bytecask& operator=(Bytecask&&) noexcept = default;
    ~Bytecask();

    // ── Primary operations ────────────────────────────────────────────────

    // Returns the value for `key`, or std::nullopt if the key does not exist.
    // Throws std::system_error on I/O failure or std::runtime_error on corruption.
    [[nodiscard]] auto get(BytesView key) const -> std::optional<Bytes>;

    // Writes `key` → `value`. Overwrites any existing value.
    // Throws std::system_error on I/O failure.
    void insert(BytesView key, BytesView value);

    // Writes a tombstone for `key`.
    // Returns true if the key existed and was removed, false if it was absent.
    // Throws std::system_error on I/O failure.
    [[nodiscard]] bool remove(BytesView key);

    // Returns true if `key` exists in the index (no disk I/O).
    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // ── Batch ─────────────────────────────────────────────────────────────

    // Atomically applies all operations in `batch` wrapped in BulkBegin/BulkEnd entries.
    // `batch` is consumed (move-only). No-op if batch.empty().
    // Throws std::system_error on I/O failure; the database is left consistent on failure.
    void apply_batch(Batch batch);

    // ── Range iteration ───────────────────────────────────────────────────

    // Returns an input range of (key, value) pairs with keys >= `from`.
    // Pass an empty span to start from the first key. Each increment reads one
    // value from disk (lazy). Throws std::system_error on I/O failure.
    [[nodiscard]] auto iter_from(BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    // Returns an input range of keys >= `from` without reading values.
    // Walks the in-memory B-Tree only; no disk I/O.
    [[nodiscard]] auto keys_from(BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

private:
    explicit Bytecask(std::filesystem::path dir);
};
```

---

## Usage Examples

```cpp
// Open (or create) a database.
auto db = Bytecask::open("my_db");

// Single-key operations.
db.insert(as_bytes("user:1"), as_bytes("alice"));

auto val = db.get(as_bytes("user:1"));
if (val) { /* use *val */ }

bool existed = db.remove(as_bytes("user:1")); // false if key was absent

// Atomic batch.
Batch batch;
batch.insert(as_bytes("user:2"), as_bytes("bob"));
batch.insert(as_bytes("user:3"), as_bytes("carol"));
batch.remove(as_bytes("user:1"));
db.apply_batch(std::move(batch));

// Range scan.
for (auto& [key, value] : db.iter_from(as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan (no disk I/O — B-Tree walk only).
for (auto& key : db.keys_from(as_bytes("user:"))) { ... }
```

> `as_bytes` is a small helper that converts a string literal or `std::string_view` to `BytesView`. Its exact form is TBD.

---

## Design Decisions

| # | Decision |
|---|----------|
| D1 | **Error handling**: Throw (`std::system_error` for I/O, `std::runtime_error` for corruption). These are panic-level events the caller cannot meaningfully recover from inline. `std::optional` covers the key-not-found case for `get`. No `std::expected` at this boundary — there are no anticipated recoverable error conditions in normal operation. |
| D2 | **Config**: Deferred — removed from the initial API scope. |
| D3 | **Batch ownership**: `Batch` is move-only (copy constructor and copy assignment deleted). Single-use by design. |
| D4 | **Batch size limit**: None — the caller is responsible. |
| D5 | **Iterator strategy**: Lazy — each `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries. |
| D6 | **`KeyIterator` source**: In-memory only — walks the B-Tree key directory without opening any data file. |
| D7 | **`remove` on missing key**: Returns `bool` — `true` if the key existed and was removed, `false` if it was absent. Consistent with `std::set::erase` returning a count. |
| D8 | **Error handling during iteration**: Throw `std::system_error` on I/O failure (consistent with D1 and standard C++ practice — `std::istream_iterator` propagates stream errors the same way). |
| D9 | **Concurrency model**: SWMR — exactly one writer at a time; reads are concurrent. MVCC and snapshot isolation are not provided. |
| D10 | **Vacuum**: Fully offline. The engine must not be running while vacuum operates. No background or online compaction. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmssUUUUUU}` using microsecond precision. Gives lexicographic == chronological ordering and avoids the false precision of nanosecond timestamps whose sub-microsecond bits are often zero on Linux. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be silently discarded with a logged warning. No partial-batch entries enter the key directory. |
