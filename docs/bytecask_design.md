# ByteCask Design

## Purpose

ByteCask is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf) implementation with a key architectural difference: it uses an immutable **persistent radix tree** for the Key Directory instead of a Hash Table. This design choice enables efficient **range queries** and **prefix searches** while maintaining Bitcask's core strengths of fast writes and simple recovery. The name "ByteCask" reflects this hybrid approach: **Bitcask algorithm** + **tree index** = **ByteCask**.

**Fundamental Trade-off**: ByteCask keeps **all keys in memory** at all times. This enables extremely fast lookups and range queries but limits database size to available RAM. Considering a memory requirement of approximately 100 bytes per unique key (key data + metadata + tree structure overhead), 10 million keys would require around 1 GB RAM.

This document is the living design reference for the repository. It should track the current implementation state, the intended architecture, and important constraints.

Canonical location: `docs/bytecask_design.md`.

## Goals

- Provide a clean, minimal API surface for key-value operations.
- Support atomic multi-operation batches.
- Support ordered range iteration (enabled by the B-Tree key directory).
- Be idiomatic C++23: no raw pointers, no stringly-typed errors, move-only ownership.

## Non-Goals (for now)

- Multi-writer access, MVCC, or snapshot isolation. ByteCask uses a SWMR model.
- TTL or expiry.
- Async I/O.
- Online (background) compaction. Vacuum is a fully offline operation.

## Design Principles

The design follows these core tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Predictable latency over peak throughput**: Write-path operations must have bounded, predictable latency. Work that can be deferred without compromising correctness must be deferred. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes. This directly influences decisions like deferring hint file writes out of the rotation path.
4. **Performance**: Optimizations are pursued only when they don't compromise correctness or simplicity.

## System Architecture

### Key Directory

ByteCask uses `PersistentRadixTree<KeyDirEntry>` as the in-memory key directory. All keys reside in memory at all times.

The key directory is a persistent (immutable) radix tree with path-compressed nodes, intrusive reference counting, and structural sharing across versions. It provides O(k) get/set/erase (where k = key length), ordered iteration via DFS, `lower_bound()` for range queries, and a `transient()` / `persistent()` API for batch mutations. Implemented in `bytecask.radix_tree` (`src/engine/radix_tree.cppm`).

Keys are stored as byte sequences within the radix tree's prefix-compressed nodes. The radix tree API accepts `std::span<const std::byte>` for all key parameters — no intermediate `Key` wrapper is needed for internal operations. The public `Key` class (wrapping `immer::array<std::byte>`) is retained for the external iterator API (`KeyIterator`, `EntryIterator`) and for the recovery tombstone tracking map. `Key` provides `operator<=>` (lexicographic over raw byte values), `begin()`/`end()`/`size()`/`data()` accessors, and a `view()` method returning `BytesView`. Keys have a hard upper bound of 65 535 bytes (the `u16 key_size` field in the data file header).

**Historical note**: the original key directory used `PersistentOrderedMap<Key, KeyDirEntry>`, backed by `immer::flex_vector<Entry>`. The radix tree replacement (BC-030) delivers O(k) lookups vs O(n log n) binary search, lower memory overhead via prefix compression and intrusive refcounting, and faster batch mutations via the transient API's in-place path copying. `PersistentOrderedMap` is retained in the codebase for benchmarking purposes (`benchmarks/map_bench.cpp`).

### Concurrency Model

ByteCask follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time.
- Multiple readers may operate concurrently.
- MVCC and snapshot isolation are not supported.

#### Write Lock

The engine uses two mutexes instead of one `std::shared_mutex`, eliminating writer-starvation under concurrent readers:

- **`write_mu_`** (`std::mutex`): serialises writers (`put`, `del`, `apply_batch`). The writer appends to the data file and updates `key_dir_`/`files_` under this lock, then calls `publish_snapshot()` (see below) before releasing it. `fdatasync` still happens **outside** the lock — the `shared_ptr<DataFile>` captured before release keeps the file alive even if rotation occurs under the next writer's lock.
- **`snapshot_mu_`** (`std::mutex`, `mutable`): protects a light `DirSnapshot{key_dir, files}` that is the sole state readers observe. Writers publish into it after each mutation; readers copy from it. Hold time is ~10 ns for both sides (two O(1) copies). Readers **never** acquire `write_mu_`.

This two-lock design removes the `shared_mutex` reader-preference starvation that caused ByteCask/MixedMT/Sync to be 700× slower than LevelDB. Both mutexes are heap-allocated (`std::unique_ptr`) because `std::mutex` is not movable and `Bytecask` is a move-only type.

`WriteOptions::try_lock` (default `false`) controls write-lock acquisition behaviour:

- `false` (default) — blocking acquire via `std::unique_lock`. The caller waits until the lock is available.
- `true` — non-blocking attempt via `std::unique_lock::try_lock()`. If the lock is already held, the call throws `std::system_error` with `std::errc::resource_unavailable_try_again` instead of waiting.

### File Registry

The engine maintains a registry that maps a monotonic `uint32_t` file ID to an open `DataFile`. The type is:

```cpp
using FileRegistry =
    std::shared_ptr<std::map<std::uint32_t, std::shared_ptr<DataFile>>>;
```

Two levels of `shared_ptr` serve distinct purposes:

- **Inner `shared_ptr<DataFile>`**: ensures a `DataFile` (and its fd) remains alive as long as any part of the system holds a reference to it, even after it has been rotated out of the current registry.
- **Outer `shared_ptr<map<...>>`**: enables O(1) copy-on-write snapshotting. `EntryIterator` captures a copy of the outer pointer at construction, giving it an independent lifetime from the `Bytecask` instance.

**Rotation** is a functional update: `rotate_active_file()` clones the inner map into a new allocation, inserts the new `DataFile`, and replaces `files_` with the new outer `shared_ptr`. Any iterator holding the previous snapshot continues reading from the old set of open files without any locking.

**Why not `immer::map`**: `immer::map<K, std::shared_ptr<V>>` triggers a GCC 15 / libstdc++15 regression — the `friend` declaration inside `std::shared_ptr`'s internals is rejected when the type is instantiated from a C++20 module context.

### Data File Lifecycle

Each data file transitions through three phases in sequence:

| Phase | Description |
|-------|-------------|
| **Active** | The current append target; accepts all writes. |
| **Rotating** | Closed to new writes; a companion `.hint` file is being written atomically. |
| **Immutable** | The `.hint` file exists; the data file is sealed and read-only. |

A new active data file is always created on engine startup.

### Vacuum

Vacuum is **fully offline**: the engine must not be running while vacuum operates. No background or online compaction. (This project uses the PostgreSQL term *vacuum*; other systems call it *compaction* or *merge*.)

## Data File Format (.data)

### Entry Structure

```
+------------------+
| Leading Header   | 15 bytes
+------------------+
| Key Data         | key_size bytes
+------------------+
| Value Data       | value_size bytes (0 for Delete/BulkBegin/BulkEnd)
+------------------+
| CRC32            | 4 bytes (trailing)
+------------------+
```

### Leading Header (15 bytes)

| Offset | Size | Field      | Type   | Description                                    |
|--------|------|------------|--------|------------------------------------------------|
| 0      | 8    | Sequence   | u64 LE | Monotonic sequence number (LSN)                |
| 8      | 1    | EntryType  | u8     | Entry kind (see EntryType enum)                |
| 9      | 2    | Key Size   | u16 LE | Key length in bytes (0 for BulkBegin/BulkEnd)  |
| 11     | 4    | Value Size | u32 LE | Value length in bytes (0 for Delete/Bulk*)     |

### EntryType Enum

```cpp
enum class EntryType : uint8_t {
    Put       = 0x01, // Standard key-value pair
    Delete    = 0x02, // Tombstone — key present, value empty
    BulkBegin = 0x03, // Start of atomic batch — key and value empty
    BulkEnd   = 0x04, // End of atomic batch   — key and value empty
};
```

A zero byte in the `EntryType` field is unambiguous corruption or an uninitialized write (no valid type maps to 0).

### Trailing CRC (4 bytes)

| Offset from start of entry      | Size | Field | Type   | Description                     |
|---------------------------------|------|-------|--------|---------------------------------|
| 15 + key_size + value_size      | 4    | CRC32 | u32 LE | Checksum of all preceding bytes |

CRC is at the **end** of the entry so both write and read can be done in a single pass: write all fields and accumulate CRC in one loop, then append the checksum.

### Size constants

- `kHeaderSize = 15` — fixed leading fields (sequence + entry_type + key_size + value_size)
- `kCrcSize = 4` — trailing CRC
- Total entry size: `kHeaderSize + key_size + value_size + kCrcSize`

### Serialization

- Little-endian byte order throughout.
- Serialization backend: [bitsery](https://github.com/fraillt/bitsery) v5.2.5 (header-only, `add_requires("bitsery")` in xmake).
- CRC32 uses polynomial `0xEDB88320` (standard reflected CRC-32/ISO-HDLC).
- CRC32 is computed over **all bytes of the entry except the trailing CRC field itself** (i.e., the leading header + key data + value data).
- `CrcOutputAdapter<TAdapter>` (in `data_entry.cppm`, anonymous namespace) wraps any bitsery output adapter and accumulates CRC as bytes are written. It is reusable: any future component that needs a running CRC while writing can use the same adapter without re-implementing the checksum logic.

### Log Sequence Number (LSN)

The LSN is a **globally monotonic** counter across all data files and all engine sessions — not a per-file counter. This is a correctness invariant: recovery determines which of two entries for the same key is fresher by comparing LSNs from potentially different files. Any per-file counter reset would silently allow stale data to overwrite live data.

- The engine owns and increments the global LSN. `DataFile` is a passive consumer: the caller passes `sequence` to every `append` call.
- `DataFile` does not start its own counter; it does not know or care what value the sequence starts at.
- On startup, the engine scans all hint files and the active data file to find `max_lsn`, then seeds the new active `DataFile` at `max_lsn + 1`.

### Log-Structured Naming Convention

Files use a timestamp-based stem: `data_{YYYYMMDDHHmmssUUUUUU}` where `UUUUUU` is the microsecond sub-second component (zero-padded, 6 digits).

Examples:
- `data_20260329123456123456.data`
- `data_20260329123456123456.hint`

Rationale:
- Lexicographic sort equals chronological order.
- `std::chrono::system_clock` reliably delivers microsecond precision on Linux; nanosecond precision would add false precision since kernel clock granularity is often coarser.
- The timestamp string serves as the unique **file ID** referenced by key directory entries.

Not yet implemented — callers currently provide the full file path.

### DataFile API

`DataFile` (in `bytecask.data_file` module) is the primary storage-engine component. It owns a single data file and provides:

- **Constructor**: Takes a `std::filesystem::path`. Opens (or creates) the file via POSIX `open(O_WRONLY | O_CREAT | O_APPEND)`. Throws `std::system_error` on failure.
- **`append(sequence, entry_type, key, value) -> Offset`**: Serializes a new entry with the given sequence number and `EntryType`, writes it to the OS page cache via `::write()`, and returns the byte offset where the entry starts. `BulkBegin`/`BulkEnd` entries pass empty key and value spans. Does **not** guarantee durability on its own.
- **`sync()`**: Calls `::fdatasync()` to flush all pending writes to physical storage. Must be called explicitly to guarantee crash-safety. Decoupled from `append()` to enable Group Commit: callers can batch multiple `append()` calls before a single `sync()`.
- **`read_entry(offset, key_size, value_size, io_buf)`**: Single-pread read primitive. Resizes `io_buf` (reusing existing capacity) and preads the full entry into it. Callers then pass `io_buf` to `deserialize_entry()` (recovery, scan) or `extract_value_into()` (get, iterator) depending on what they need. `scan()` uses this internally after its header pread.
- **`read_value_into`** was removed — `read_entry` + `extract_value_into` replaces it with better composability.
- Key and value are accepted as `std::span<const std::byte>` for binary safety.

### I/O Back-end Rationale

- **POSIX over `std::ofstream`**: `::write()` with `O_APPEND` issues a single system call per entry, skipping the buffering layers and locale state overhead of C++ streams.
- **`fdatasync` over `fflush`/`flush()`**: `fdatasync` syncs data to physical media while skipping inode metadata updates (access time etc.), making it faster than `fsync` for a pure append-only log.
- **Group Commit pattern**: Separating `append()` (writes to page cache) from `sync()` (forces to disk) lets future code batch hundreds of writes before a single expensive `fdatasync`, which is the primary lever for high write throughput on NVMe hardware (see `io_uring` paper reference).

### Source Code Module Architecture

We use fine-grained C++20 modules:
- `bytecask.crc32`: General purpose mathematical utilities (`Crc32`, checked `narrow<To>(From)` conversion).
- `bytecask.serialization`: Core bitsery abstractions (`CrcOutputAdapter`, legacy memory wrappers).
- `bytecask.data_entry`: Logical entry definition, single-entry memory formatting, and `verify_entry()` / `deserialize_entry()` / `extract_value_into()` — CRC verification is factored into `verify_entry()` and shared by both extraction functions.
- `bytecask.data_file`: Disk I/O, writing streams sequentially to `.data` files.
- `bytecask.hint_file`: Hint file writer and reader (`HintFile`, `HintEntry`).
- `bytecask.persistent_ordered_map`: Immutable sorted map (`PersistentOrderedMap<K,V>`, `OrderedMapTransient<K,V>`) backed by `immer::flex_vector`; retained for benchmarking.
- `bytecask.radix_tree`: Persistent radix tree (`PersistentRadixTree<V>`, `TransientRadixTree<V>`, `RadixTreeIterator<V>`) with path compression and intrusive refcounting; used as the key directory.
- `bytecask.engine`: Public engine API (`Bytecask`, `Batch`, `KeyIterator`, `EntryIterator`, `FileRegistry`, type aliases).

### Current scope boundaries

- `EntryType` is written and read back on `DataFile::read()`; atomic bulk semantics are enforced at a layer above `DataFile`.
- No file rotation or size limits.
- No read path for the key directory — append-only for now.

### DataFile fd mode after rotation

`DataFile` opens with `O_RDWR | O_CREAT | O_APPEND`. After a data file is rotated it is logically immutable — no new entries should be appended. The engine enforces this at a higher level; the fd mode is not downgraded to `O_RDONLY` after rotation. Rationale:

- `DataFile` is an internal class; the engine exclusively controls when `append()` is called.
- Re-opening the fd purely for semantic enforcement adds syscall overhead and complexity without improving correctness for the production path.
- A `sealed_` flag with `assert(!sealed_)` at the top of `append()` is sufficient: it catches programming errors in debug builds at zero production cost.

Contrast with `HintFile`, which uses `OpenForWrite` / `OpenForRead` factory functions. That split models two *externally visible*, non-overlapping lifecycles at different call sites: one site writes during `flush_hints()`, a completely separate site reads during recovery. Encoding that distinction in the type prevents mixing them up. `DataFile` has no equivalent external semantic split.

## Hint File Format (.hint)

### Purpose

Hint files are compact companion files to sealed (rotated) data files. Each hint entry summarises one data file entry — just enough metadata and the full key — so that the in-memory Key Directory (B-Tree) can be rebuilt at startup by scanning the smaller hint files instead of the raw data files. Only sealed data files have a corresponding hint file; the active data file is recovered by scanning its raw bytes if needed.

### Entry Structure

```
+------------------+
| Hint Header      | 23 bytes
+------------------+
| Key Data         | key_size bytes
+------------------+
| CRC32            | 4 bytes (trailing)
+------------------+
```

Total fixed overhead per entry: **27 bytes** (23-byte header + 4-byte trailing CRC).

> **Note on the 27-byte fixed overhead**: the five header fields sum to 23 bytes (u64 + u8 + u64 + u16 + u32). The "27 bytes" refers to the total fixed overhead including the trailing CRC, not the header alone. `BulkBegin`/`BulkEnd` data file entries are **never** written to hint files — only `Put` and `Delete` entries are included.

### Hint Header (23 bytes)

| Offset | Size | Field       | Type   | Description                                    |
|--------|------|-------------|--------|------------------------------------------------|
| 0      | 8    | Sequence    | u64 LE | Entry sequence number (LSN)                    |
| 8      | 1    | EntryType   | u8     | Entry kind: `Put` (0x01) or `Delete` (0x02) only — `BulkBegin`/`BulkEnd` are never written to hint files |
| 9      | 8    | File Offset | u64 LE | Byte offset of the entry in the data file      |
| 17     | 2    | Key Size    | u16 LE | Key length in bytes                            |
| 19     | 4    | Value Size  | u32 LE | Value length in bytes (to rebuild the key directory without reading the data file) |

`Value Size` is stored so the reader can compute the full on-disk entry size in the data file without reading it, enabling future space-accounting features.

### Trailing CRC (4 bytes)

| Offset from entry start | Size | Field | Type   | Description                          |
|-------------------------|------|-------|--------|--------------------------------------|
| 23 + key_size           | 4    | CRC32 | u32 LE | Checksum of all preceding entry bytes |

CRC placement mirrors data file entries: accumulate header + key data in one sequential pass, then append the checksum at the end.

### Size Constants

- `kHintHeaderSize = 23` — fixed header fields
- `kHintCrcSize = 4` — trailing CRC
- Total fixed overhead: `kHintHeaderSize + kHintCrcSize = 27`
- Total entry size: `kHintHeaderSize + key_size + kHintCrcSize`

### Recovery (planned)

On engine startup:

1. Discard any `.hint.tmp` files — incomplete hint files from a crash mid-rotation.
2. Read hint files oldest-to-newest. For each entry: verify the trailing CRC (**panic on any mismatch**); then apply based on `entry_type`:
   - `Put`: insert `(key → {sequence, file_id, file_offset, value_size})` only if `entry.sequence > dir[key].sequence` (skip if a fresher entry is already present).
   - `Delete`: remove the key from the B-Tree if `entry.sequence > dir[key].sequence`; otherwise skip.
3. For any data file without a companion `.hint`, scan its raw bytes using the same Put/Delete rules. Skip `BulkBegin`/`BulkEnd` records. If a `BulkBegin` has no matching `BulkEnd`, discard all entries from that point onward and log a warning.
4. Record `max_lsn` — the largest sequence number seen across all hint files and the active data file scan.
5. Create a new active data file seeded at `max_lsn + 1`.

### Incomplete Batch Recovery

If the engine crashes after writing a `BulkBegin` but before the matching `BulkEnd`, the active data file contains an incomplete batch. Recovery discards all entries from the unmatched `BulkBegin` onward and logs a warning. No partial-batch entries are inserted into the key directory. Because hint files are only written for immutable (fully rotated) files, an incomplete batch can only appear in the active data file scan — never in a hint file.

### Relationship to Data Files

| Property | Data file | Hint file |
|---|---|---|
| Extension | `.data` | `.hint` |
| Contents | Full key + value | Key + location metadata only |
| Created | At engine open / rotation | When a data file is sealed (rotated) |
| Read at startup | Only for the active file | For all sealed files |

One hint file corresponds to exactly one data file (same timestamp stem, different extension).

**Hint files are a startup-optimization artifact, not a correctness requirement.** Recovery always falls back to scanning the raw `.data` file if no `.hint` companion exists. This makes hint file writes safe to defer out of the rotation path entirely.

Hint files are written **deferred**: at engine close, or via an explicit `flush_hints()` call — never inline on the write path. This keeps write-path latency flat and bounded regardless of file size at the time of rotation. The cost is that a crash after rotation but before `flush_hints()` causes recovery to scan the `.data` file instead of the `.hint` file, which is always correct and only slower.

### Hint File Atomicity

To guarantee hint files are either complete or absent, writing uses a temp-then-rename protocol:

1. Write the complete hint file to `data_{timestamp}.hint.tmp`.
2. Call `fdatasync` to flush all bytes to physical storage.
3. Atomically `rename(2)` to `data_{timestamp}.hint` — POSIX guarantees this rename is atomic on the same filesystem.

Any `.hint.tmp` file found at startup is discarded (it represents an incomplete write interrupted by a crash). Recovery will re-scan the corresponding `.data` file instead.

Because hint file writes are deferred (see above), this protocol is exercised at engine close or during an explicit `flush_hints()` call — not inside the rotation critical path.

### Module Plan

`HintFile` will live in a new `bytecask.hint_file` C++20 module (`src/engine/hint_file.cppm`), symmetric with `DataFile`:

Construction uses named static factory functions to make intent explicit at the call site:

- **`HintFile::OpenForWrite(path) -> HintFile`** — opens (or creates) the file with `O_WRONLY | O_CREAT | O_APPEND`.
- **`HintFile::OpenForRead(path) -> HintFile`** — opens an existing file with `O_RDONLY`. Read and write descriptors are kept independent so scanning a sealed hint file never interferes with an active writer.

Write API:
- **`append(sequence, entry_type, file_offset, key, value_size) -> void`**: Serializes one hint entry using `CrcOutputAdapter` and writes it to the file. Only `Put` and `Delete` are valid entry types; passing `BulkBegin` or `BulkEnd` is a programming error.
- **`sync() -> void`**: `::fdatasync()` flush, decoupled from `append()` for Group Commit consistency.

Read API:
- **`read(offset) -> std::optional<std::pair<HintEntry, uint64_t>>`**: Reads the hint entry at `offset` bytes from the start of the file. Returns the parsed entry and the offset of the next entry (`offset + kHintHeaderSize + key_size + kHintCrcSize`). Pass `0` to start scanning from the beginning. Returns `std::nullopt` when `offset` equals file size (end-of-file). Panics on CRC mismatch. Typical usage: `while (auto result = file.read(offset)) { auto [entry, next] = *result; offset = next; }`.

`HintEntry` is a plain struct holding `{uint64_t sequence, EntryType entry_type, uint64_t file_offset, std::vector<std::byte> key, uint32_t value_size}`.

## Range Scan: `iter_from`

`iter_from` returns a lazy input range of `(Key, Bytes)` pairs in ascending key order. Each dereference issues a single `pread` via `DataFile::read_value_into()` — the caller-supplied `key_size` and `value_size` (from `KeyDirEntry`) let the engine compute the total entry size upfront, halving the syscall count compared to the recovery path (`scan()`, which needs two preads because sizes are unknown). The iterator reuses an internal I/O buffer and the cached value vector across advances, so sequential scans incur zero allocations after the first dereference.

Results are always in ascending key order (radix tree iteration order).

`ReadOptions` is currently an empty struct reserved for future knobs (e.g. `verify_checksums`).


### [Draft] PMR - Memory Allocation (BC-024)

This is a solid design to add to your backlog. In the world of systems programming, this pattern is often called **"Caller-Controlled Allocation"** or the **"Arena Injection"** pattern.

By decoupling the *logic* of fetching data from the *policy* of how that data's memory is managed, you’ve given Bytecask a massive performance advantage over engines that hardcode `std::allocator` (the standard heap).

---

## Backlog Item: Polymorphic Memory Injection (PMR)
**Title:** Implement Configurable Memory Allocation via PMR and Function Overloading

### 1. The Design Intent
The goal is to allow `Bytecask` to remain "low-friction" for standard users (who just want the heap) while being "zero-friction" for high-performance callers (like the Vacuum process or a MySQL Bridge) who need to reuse memory to avoid GC-like pauses or heap fragmentation.

### 2. Implementation Specs

* **Instance Default:** The `Bytecask` instance holds a `memory_resource*` (defaulting to the system heap). This acts as the "Standard Life-cycle" manager.
* **Signature Overloading:** * **Convenience API:** `get(key)` → Internalizes the default pool. Perfect for UI or one-off app requests.
    * **Expert API:** `get(key, pool)` → Allows the caller to "inject" a temporary Arena. This is the **High-Performance** path.
* **Container Binding:** All returned values must be `std::pmr::vector<std::byte>` (or your `PmrBytes` alias) to ensure the container honors the injected resource during its `resize()` and `destructor` phases.



---

### 3. Usage Scenarios for the Backlog

| Scenario | Logic | Memory Outcome |
| :--- | :--- | :--- |
| **Standard App Get** | `db.get("user:1")` | Allocated on Heap. Deleted when variable goes out of scope. |
| **Heavy Scan Loop** | `db.get(key, &local_arena)` | Allocated in a pre-allocated "scratchpad." No system calls. |
| **Long-Running Task** | `db.get(key, &shared_pool)` | Memory is recycled into "buckets" for the next operation. |

### 4. Technical Trade-offs to Note
* **Virtual Dispatch:** Every allocation now goes through a virtual function call (`do_allocate`). In a database engine, the cost of I/O (reading the disk) so heavily outweighs a virtual call that this is essentially "free" performance.
* **Pointer Stability:** The `memory_resource` pointer must remain valid for the entire lifetime of the returned `PmrBytes`. Since your design uses a class member or a caller-provided arena, this is safe as long as the caller doesn't destroy their arena before processing the result.


```cpp
export class Bytecask {
private:
    // This is configured in the constructor
    std::pmr::memory_resource* default_pool_; 

public:
    // Constructor: User can pass a specific pool (like a long-lived unsynchronized_pool_resource)
    explicit Bytecask(std::pmr::memory_resource* pool = std::pmr::get_default_resource())
        : default_pool_(pool) {}

    /**
     * @brief Signature 1: Uses the constructor-configured pool.
     * This is what your loop "for (auto val : db.get(key))" will use.
     */
    [[nodiscard]] auto get(BytesView key) const -> std::optional<PmrBytes> {
        return get(key, default_pool_);
    }

    /**
     * @brief Signature 2: Allows overriding the pool for a specific call.
     * Use this for your Vacuum Pump or high-performance scans.
     */
    [[nodiscard]] auto get(BytesView key, std::pmr::memory_resource* pool) const 
        -> std::optional<PmrBytes> 
    {
        auto meta = index_.get(key);
        if (!meta) return std::nullopt;

        PmrBytes buffer(pool); 
        buffer.resize(meta->size);
        file_io.read_at(meta->offset, buffer.data(), meta->size);
        return buffer;
    }
};
```

### Type Aliases

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

`std::byte` makes the intent clear (raw bytes, not text) and prevents accidental arithmetic. `Key` is kept distinct from `Bytes` so the key directory type (`PersistentOrderedMap<Key, KeyDirEntry>`) reads as its intent and provides a single point of change if the key type needs to evolve. `BytesView` as the universal input type avoids copies at call sites and accepts any contiguous range.

### Batch

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

    void put(BytesView key, BytesView value);
    void del(BytesView key);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<BatchOperation> operations_;
    friend class Bytecask;
};
```

`std::variant` over an inheritance hierarchy keeps `BatchOperation` a value type (no heap allocation per item, trivially movable). `Batch` is move-only and single-use; `Bytecask::apply_batch` consumes it by move.

### Iterators

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

- **Lazy**: `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries.
- **`KeyIterator` is in-memory only**: walks the B-Tree key directory without touching any data file.
- **Error handling**: throws `std::system_error` on I/O failure.

### WriteOptions and ReadOptions

Modelled after LevelDB/RocksDB. All write operations accept a `WriteOptions`; all read operations accept a `ReadOptions`. Both default-construct to the same behaviour as the old bare signatures.

```cpp
struct WriteOptions {
    // When true (default), fdatasync is called after every write.
    // Set to false to skip the sync for higher throughput at the cost of
    // durability on crash: data is in the OS page cache but not on disk.
    bool sync{true};
};

struct ReadOptions {
    // Placeholder for future read-path knobs (e.g. verify_checksums).
};
```

**`sync` default of `true`**: preserves the pre-existing crash-safe behaviour. Callers that deliberately trade durability for throughput (e.g., bulk import, benchmarks) opt out explicitly by setting `sync = false`. The destructor always calls `sync()` unconditionally, so `sync = false` on individual writes does not risk losing data on clean shutdown — only on an OS/power failure between the last write and the destructor.

### Bytecask

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
    [[nodiscard]] auto get(const ReadOptions& opts, BytesView key) const
        -> std::optional<Bytes>;

    // Output-parameter variant: writes the value into `out`, reusing its
    // existing capacity to amortize allocation across calls. Returns true
    // if the key was found, false otherwise.
    [[nodiscard]] auto get(const ReadOptions& opts, BytesView key,
                           Bytes& out) const -> bool;

    // Writes `key` → `value`. Overwrites any existing value.
    // Throws std::system_error on I/O failure.
    void put(const WriteOptions& opts, BytesView key, BytesView value);

    // Writes a tombstone for `key`.
    // Returns true if the key existed and was removed, false if it was absent.
    // Throws std::system_error on I/O failure.
    [[nodiscard]] bool del(const WriteOptions& opts, BytesView key);

    // Returns true if `key` exists in the index (no disk I/O).
    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // ── Batch ─────────────────────────────────────────────────────────────

    // Atomically applies all operations in `batch` wrapped in BulkBegin/BulkEnd entries.
    // `batch` is consumed (move-only). No-op if batch.empty().
    // Throws std::system_error on I/O failure; the database is left consistent on failure.
    void apply_batch(const WriteOptions& opts, Batch batch);

    // ── Range iteration ───────────────────────────────────────────────────

    // Returns an input range of (key, value) pairs with keys >= `from`.
    // Pass an empty span to start from the first key. Each increment reads one
    // value from disk (lazy). Throws std::system_error on I/O failure.
    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    // Returns an input range of keys >= `from` without reading values.
    // Walks the in-memory B-Tree only; no disk I/O.
    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

private:
    explicit Bytecask(std::filesystem::path dir);
};
```

### Usage Examples

```cpp
// Open (or create) a database.
auto db = Bytecask::open("my_db");

// Single-key operations.
db.put(as_bytes("user:1"), as_bytes("alice"));

auto val = db.get(as_bytes("user:1"));
if (val) { /* use *val */ }

bool existed = db.del(as_bytes("user:1")); // false if key was absent

// Atomic batch.
Batch batch;
batch.put(as_bytes("user:2"), as_bytes("bob"));
batch.put(as_bytes("user:3"), as_bytes("carol"));
batch.del(as_bytes("user:1"));
db.apply_batch(std::move(batch));

// Range scan.
for (auto& [key, value] : db.iter_from(as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan (no disk I/O — B-Tree walk only).
for (auto& key : db.keys_from(as_bytes("user:"))) { ... }
```

> `as_bytes` is a small helper that converts a string literal or `std::string_view` to `BytesView`. Its exact form is TBD.

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: bitsery v5.2.5 (header-only binary serialization), immer (header-only persistent data structures)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: Full `Bytecask` SWMR engine with `open`, `get`, `put`, `del`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`. Key directory backed by `PersistentOrderedMap<Key, KeyDirEntry>`. `open()` always creates a fresh active data file; recovery is BC-019. 143 assertions, 34 test cases.

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/crc32.cppm`: C++23 module (`bytecask.crc32`) — `Crc32` accumulator, `narrow<To>(From)` checked conversion
- `src/engine/serialization.cppm`: C++23 module (`bytecask.serialization`) — `CrcOutputAdapter`, `write_bytes`
- `src/engine/data_entry.cppm`: C++23 module (`bytecask.data_entry`) — `EntryType`, `EntryHeader`, `DataEntry`, serialization helpers
- `src/engine/data_file.cppm`: C++23 module (`bytecask.data_file`) — `DataFile` POSIX I/O, `Offset`
- `src/engine/hint_entry.cppm`: C++23 module (`bytecask.hint_entry`) — `HintEntry`, `serialize_hint_entry`
- `src/engine/hint_file.cppm`: C++23 module (`bytecask.hint_file`) — `HintFile`, `OpenForWrite`/`OpenForRead`
- `src/engine/persistent_ordered_map.cppm`: C++23 module (`bytecask.persistent_ordered_map`) — `PersistentOrderedMap<K,V>`, `OrderedMapTransient<K,V>`
- `src/engine/bytecask.cppm`: C++23 module (`bytecask.engine`) — `Bytecask`, `Batch`, `KeyIterator`, `EntryIterator`, type aliases
- `tests/data_entry_test.cpp`: behavior tests for data entry serialization and file append
- `tests/hint_file_test.cpp`: behavior tests for hint file append, round-trip, and CRC panic
- `tests/bytecask_test.cpp`: behavior tests for the full `Bytecask` engine API
- `xmake.lua`: build and test target definitions
- `docs/bytecask_design.md`: living design reference
- `docs/bytecask_project_plan.md`: simple task tracker

## Near-term design direction

- Keep the implementation simple enough to validate correctness before optimizing.
- Evolve the current executable into a real storage engine with separable components that can be tested independently.
- Treat design changes as documentation changes: code and this file should move together.

## Immediate engineering constraints

- Tests must remain runnable from the repository with a single clear command.
- Architectural decisions should prefer small, composable units over logic embedded in `main.cpp`.
- Design notes in `docs/old_bytecask_design.md` are historical reference material, not the current source of truth.
- The living design and project tracker live under `docs/`.

## Design Decisions

| # | Decision |
|---|----------|
| D1 | **Error handling**: Throw (`std::system_error` for I/O, `std::runtime_error` for corruption). These are panic-level events the caller cannot meaningfully recover from inline. `std::optional` covers the key-not-found case for `get`. No `std::expected` at this boundary — there are no anticipated recoverable error conditions in normal operation. |
| D2 | **Config**: Deferred — removed from the initial API scope. |
| D3 | **Batch ownership**: `Batch` is move-only (copy constructor and copy assignment deleted). Single-use by design. |
| D4 | **Batch size limit**: None — the caller is responsible. |
| D5 | **Iterator strategy**: Lazy — each `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries. |
| D6 | **`KeyIterator` source**: In-memory only — walks the B-Tree key directory without opening any data file. |
| D7 | **`del` on missing key**: Returns `bool` — `true` if the key existed and was removed, `false` if it was absent. Consistent with `std::set::erase` returning a count. |
| D8 | **Error handling during iteration**: Throw `std::system_error` on I/O failure (consistent with D1 and standard C++ practice). |
| D9 | **Concurrency model**: SWMR — exactly one writer at a time; reads are concurrent. MVCC and snapshot isolation are not provided. |
| D10 | **Vacuum**: Fully offline. The engine must not be running while vacuum operates. No background or online compaction. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmssUUUUUU}` using microsecond precision. Gives lexicographic == chronological ordering and avoids the false precision of nanosecond timestamps whose sub-microsecond bits are often zero on Linux. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be discarded with a logged warning. No partial-batch entries enter the key directory. |

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints