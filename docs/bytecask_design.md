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
- Background (auto) vacuum. Vacuum is called explicitly by the user.

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

#### Lock-free strategy

ByteCask's read path is designed so that **readers never acquire a mutex** and never spin. The strategy combines three ideas:

1. **A single writer mutex** (`write_mu_`) that serialises mutations — readers are completely unaffected by it.
2. **An immutable, copy-on-write snapshot** (`DirSnapshot`) published via an atomic `shared_ptr` — readers capture the current snapshot without blocking the writer.
3. **A thread-local generation cache** — on the common path a reader performs only a single atomic integer comparison and returns immediately, touching no shared state at all.

##### Shared state layout

```
  Bytecask object
  ┌─────────────────────────────────────────────────────────────┐
  │  write_mu_      std::mutex (heap-allocated)                 │  ← writers only
  │                                                             │
  │  snapshot_      atomic<shared_ptr<DirSnapshot>>             │  ← written by writer
  │                                                             │    read (relaxed) on slow path
  │  snapshot_gen_  atomic<uint64_t>                            │  ← bumped by writer (release)
  └─────────────────────────────────────────────────────────────┘    loaded by readers (acquire)

  DirSnapshot  (heap, reference-counted, never mutated in place)
  ┌─────────────────────────────────────────────────┐
  │  key_dir        shared_ptr<RadixTree>            │  key → (file_id, offset, seq)
  │  files          shared_ptr<FileMap>              │  file_id → open DataFile fd
  │  active_file_id uint32_t                         │
  └─────────────────────────────────────────────────┘
```

`DirSnapshot` is immutable once published. Old snapshots stay alive as long as any reader holds a `shared_ptr` reference — the writer always allocates a fresh snapshot and never mutates one in place.

##### Write path

```
  Writer thread
  ─────────────
  1. acquire write_mu_
     └─ serialises concurrent writers; readers never touch this mutex

  2. append entry to active DataFile   (I/O, under write_mu_)

  3. update key_dir_ and files_        (in-memory, under write_mu_)

  4. publish_snapshot()
     │
     ├─ a. snapshot_.store( make_shared<DirSnapshot>(key_dir_, files_, ...) , release )
     │        └─ allocates a fresh, immutable snapshot; release ensures all
     │           preceding writes are visible before the store completes
     │
     └─ b. snapshot_gen_.fetch_add( 1 , release )
              └─ bumps the generation counter; the release fence here
                 synchronizes-with any reader's subsequent acquire load

  5. release write_mu_

  6. fdatasync()   ← outside write_mu_; DataFile kept alive by a captured shared_ptr
```

Step 4a is sequenced before step 4b. This ordering is the foundation of the entire lock-free read protocol.

##### Read path — fast path (no lock, no atomic write)

Every reader thread has a `thread_local` cache containing its last-seen generation and the corresponding snapshot.

```
  Reader thread N  (thread-local storage, private to this thread)
  ┌─────────────────────────────────────────────────────────────┐
  │  TlCache                                                    │
  │   instance_id   uint64_t                ← which Bytecask   │
  │   gen           uint64_t                ← last seen gen     │
  │   snap          shared_ptr<DirSnapshot> ← cached snapshot   │
  └─────────────────────────────────────────────────────────────┘

  read_snapshot() logic:

  ┌─ 1. current_gen = snapshot_gen_.load(acquire)   ← ONE atomic read
  │
  ├── instance_id matches AND gen == current_gen?
  │         │
  │        YES ──────────────────────────────────────> return tls.snap
  │                                                    (no shared-state writes,
  │                                                     no mutex, no CAS, no spin)
  │
  └── NO  (first call on this thread, or a write happened since last call)
           │
           ├─ 2. snap = snapshot_.load(relaxed)   ← one atomic read, no mutex
           │         └─ safe: the acquire on snapshot_gen_ already establishes
           │               happens-before with the writer's release store
           │
           ├─ 3. update tls: { instance_id, current_gen, snap }
           │
           └─────────────────────────────────────────> return snap
```

On the **fast path** the only shared-memory operation is a single 64-bit `acquire` load. All other work is pure thread-local. Any number of readers can race down this path simultaneously — they never modify shared state, so there is literally nothing to contend on.

On the **slow path** (gen mismatch) the reader does one relaxed atomic load of `snapshot_`. No mutex, no CAS, no spinning. It is still fully lock-free.

##### Why the relaxed `snapshot_` load is safe

The acquire on `snapshot_gen_` **synchronizes-with** the release `fetch_add` the writer performed. Because that `fetch_add` is sequenced after the release store on `snapshot_`, the standard's happens-before chain guarantees:

```
  WRITER                                    READER (slow path)
  ────────────────────────────────────      ──────────────────────────────────────
  [1] mutate key_dir_, files_
       │ sequenced-before
       ▼
  [2] snapshot_.store(S, release)
       │ sequenced-before
       ▼
  [3] snapshot_gen_.fetch_add(release)
                │                                    synchronizes-with
                └──────────────────────────> [4] snapshot_gen_.load(acquire)
                                                  (observes new gen → takes slow path)
                                                      │ safe: [1],[2] happen-before [4]
                                                      ▼
                                             [5] snapshot_.load(relaxed)
                                                  (guaranteed to observe S from [2])
```

Because [1]→[2]→[3] happens-before [4], and [4] is sequenced before [5], the reader is guaranteed to observe the fully-updated snapshot that the writer published.

##### The brief stale-snapshot window

Between [2] and [3] in the writer there is a tiny window where `snapshot_` already holds new data but `snapshot_gen_` has not been bumped yet. A reader that loads the generation in that window sees the old value and continues using its cached snapshot — coherent, but one write behind. This is exactly the consistency level of a `shared_mutex` design: the snapshot is never torn, just momentarily stale. Once [3] completes every subsequent reader will take the slow path and refresh.

**Same-thread guarantee**: a `put` followed by a `get` on the **same thread** always observes the put — the release `fetch_add` in step [3] is sequenced before the acquire load in the next `read_snapshot` call on the same thread, so the fast-path gen check always misses and the thread refreshes immediately.

#### Read-scaling behaviour

Thread-local caching eliminates all shared-state contention on the read fast path. Benchmarks on a 22-vCPU instance (50k keys, 1 KiB random values):

| Threads | Before (mutex only) | After (TL cache) | Improvement |
|---------|---------------------|------------------|-------------|
| 2       | 1.66 M ops/s        | 2.16 M ops/s     | +30%        |
| 4       | 1.73 M ops/s        | 3.01 M ops/s     | +74%        |
| 8       | 1.74 M ops/s        | 3.57 M ops/s     | +105%       |
| 16      | 1.91 M ops/s        | 3.67 M ops/s     | +92%        |

Throughput now scales near-linearly with thread count for read-heavy workloads.

`engine_bench` compares ByteCask against LevelDB and RocksDB across Put, Get, Del, Range50, Mixed, MixedBatch, PutMT, and MixedMT benchmarks at both NoSync and Sync durability levels. RocksDB compression is disabled (`kNoCompression`) and values are 1 KiB of random (incompressible) bytes so neither LevelDB nor RocksDB gains an advantage from Snappy/block-cache effects.

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

(This project uses the PostgreSQL term *vacuum*; other systems call it *compaction* or *merge*.)

ByteeCask implements a **conservative online vacuum**: the engine continues to serve reads and writes while vacuum rewrites sealed data files. *Conservative* means the design prioritises write-path non-interference and correctness over maximum space reclamation efficiency.

#### Constraints

- Only sealed (immutable) data files are considered — the active file is never touched.
- Only files whose waste ratio exceeds a configurable threshold are processed; files below the threshold are left alone.
- One file is processed per `vacuum()` call. Callers that want to process multiple files call in a loop.
- Tombstones (Delete entries) are never dropped during partial compaction (see **Tombstone handling** below).
- A new compacted file is fully written and `fdatasync`-ed before any old file is removed.

#### Waste ratio

The waste ratio of a sealed data file is the fraction of disk space occupied by dead entries:

```
waste_ratio = 1 - (live_bytes / total_bytes)
```

A file qualifies for vacuum when `waste_ratio >= VacuumOptions::waste_threshold` (default `0.5`).

#### Online waste stats

Rather than computing `live_bytes` at vacuum time (which would require a full key-directory traversal — O(total_keys)), the engine maintains `live_bytes` and `total_bytes` per file as a pair of `uint64_t` counters in a `std::map<uint32_t, FileStats>` (`file_stats_`), updated under `write_mu_` on every write operation:

- **On `put(key, value)`**: if the key already exists, subtract `kHeaderSize + key.size() + old_entry.value_size + kCrcSize` from `file_stats_[old_entry.file_id].live_bytes`. Add the new entry size to `file_stats_[active_file_id_].live_bytes` and to `.total_bytes`.
- **On `del(key)`**: subtract the old Put entry size from `file_stats_[old_entry.file_id].live_bytes`. Advance `.total_bytes` of the active file by the tombstone size (`kHeaderSize + key.size() + kCrcSize`). The tombstone itself is never added to `live_bytes` — tombstones are never referenced by the key directory.
- **On rotation**: the active file's `total_bytes` counter is already accurate; it becomes the frozen `total_bytes` for the new sealed file. A fresh `FileStats{0, 0}` is inserted for the new active file.

At vacuum time the waste ratio is an O(1) integer division per file — no scanning, no I/O, no additional lock contention.

The active file's `live_bytes` may be non-zero (it holds the current live writes), but the active file is never a vacuum candidate, so its waste ratio is never evaluated. The stats for the active file are used only to seed the new sealed file's `total_bytes` on rotation.

#### Vacuum procedure

1. **Acquire `vacuum_mu_`** — prevents two `vacuum()` calls from running concurrently.
2. **Snapshot the key directory** — call `read_snapshot()` to obtain the current `DirSnapshot{key_dir, files}`. This is the authoritative view of which entries are live.
3. **Select a target file** — iterate sealed files, compute `waste_ratio = 1 - (live_bytes / total_bytes)` from `file_stats_` (O(1) per file, no I/O), pick the highest-waste sealed file above `waste_threshold`. If no file qualifies, return immediately.
4. **Rewrite the target file** — open a new data file for writing (new timestamp stem, same directory). Scan the old data file entry by entry:
   - *Put entry*: check whether the snapshot's key directory entry for that key points to the old file at this offset with the same sequence number. If yes (live), write it to the new file at its new offset, recording `(key → new_file_id, new_offset)`. If no (dead), skip.
   - *Delete entry*: always copy to the new file verbatim (same sequence number, same key). See **Tombstone handling** below.
   - *BulkBegin / BulkEnd entries*: always skip — they are crash-recovery markers not needed in an already-committed sealed file.
5. **Durability** — `fdatasync` the new file. Write a hint file for it using the same temp-then-rename protocol as rotation.
6. **Atomic commit** (under `write_mu_`):
   a. Build a `TransientRadixTree` from the current `key_dir_`.
   b. For each live Put entry copied to the new file, look up the key in the current key directory. If the sequence number still matches (no concurrent write superseded it), update `KeyDirEntry` to the new `file_id` and `file_offset`. If the sequence number differs, skip — the concurrent writer's version takes precedence.
   c. Call `persistent()` to obtain the new immutable key directory.
   d. Build an updated `FileRegistry`: add the new compacted file, remove the old file.
   e. Update `file_stats_`: remove the old file's entry; insert a new entry for the compacted file where `live_bytes == total_bytes` (every surviving entry is live by construction) and `total_bytes` is the physical size of the new file.
   f. Call `publish_snapshot()` to atomically replace the observable state for all readers.
7. **Release `write_mu_`**.
8. **Delete the old file** — once published, the old sealed file is removed from disk. Any in-flight `EntryIterator` still holds a `shared_ptr` to the old `FileRegistry` snapshot, keeping the old `DataFile` fd alive until the iterator is destroyed.
9. **Release `vacuum_mu_`**.
10. Return `VacuumStats` summarising bytes examined and bytes reclaimed.

#### Tombstone handling

Tombstone (Delete) entries record that a key was explicitly removed. They must be copied to the compacted output during partial vacuum.

**Why**: if an older data file (not being compacted in this cycle) contains a Put for the same key, recovery would see that Put and resurrect the key — unless the Delete entry survives in some file to overrule it. Preserving the tombstone prevents this.

A deleted key is not present in the key directory, so no key-directory update is needed for tombstones — they are pure pass-through copies. The original sequence number is preserved verbatim so recovery's LSN comparison still works correctly on the compacted file.

The practical consequence is that space reclaimed by partial vacuum comes entirely from superseded Put entries. Tombstones occupy space in the compacted file until a full-vacuum pass eliminates them.

**Full tombstone elision** is only safe when compacting all sealed files in a single commit, guaranteeing that no unprocessed Put for any deleted key can survive in any remaining file. Full vacuum is a separate, user-triggered operation and is not yet implemented.

#### Space accounting

For each deleted key in the vacuumed file:
- **Reclaimed**: `kHeaderSize + key_size + value_size + kCrcSize` (the Put entry)
- **Residue**: `kHeaderSize + key_size + kCrcSize` (the copied tombstone; `value_size = 0`)

For 1 KiB values the tombstone residue (`~19 + key_size` bytes) is negligible relative to the value reclaimed.

#### Concurrency guarantee

Vacuum never holds `write_mu_` during file I/O. The only time `write_mu_` is held is the atomic commit step (6), which is a pure in-memory operation (transient tree update + pointer swaps). Write-path latency is unaffected by the size of the file being vacuumed.

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
- CRC-32C uses the Castagnoli polynomial `0x1EDC6F41` via the [google/crc32c](https://github.com/google/crc32c) library, which auto-detects hardware acceleration at runtime (SSE4.2 on x86-64, CRC instructions on AArch64) and falls back to a software implementation when neither is available.
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
- `bytecask.crc32`: CRC-32C accumulator (`Crc32`, backed by google/crc32c) and checked `narrow<To>(From)` conversion.
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

## GroupWriter (Removed)

`GroupWriter` was a leader/follower group-commit helper that coalesced concurrent `fdatasync` calls. It was implemented (BC-049), benchmarked, and removed (BC-051) because benchmarks showed it provided no measurable throughput benefit: on fast storage `fdatasync` is already cheap enough that grouping saves nothing; on slow storage LevelDB's internal WAL batching achieves similar amortisation without an extra abstraction.

One improvement originally introduced for `GroupWriter` was **retained** because it is independently valuable:

- **`rotate_active_file()` always fdatasyncs before sealing** — ensures prior `sync=false` writes are durable before the file becomes immutable.

The full design and implementation are preserved in git history (see BC-049 in the project plan).

---

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: bitsery v5.2.5 (header-only binary serialization), crc32c (google/crc32c, hardware-accelerated CRC-32C), immer (header-only persistent data structures)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: Full `Bytecask` SWMR engine with `open`, `get`, `put`, `del`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`. Key directory backed by `PersistentOrderedMap<Key, KeyDirEntry>`. `open()` always creates a fresh active data file; recovery is BC-019. 143 assertions, 34 test cases.

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/crc32.cppm`: C++23 module (`bytecask.crc32`) — `Crc32` accumulator (google/crc32c), `narrow<To>(From)` checked conversion
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
| D10 | **Vacuum**: Conservative online 1:1 rewrite. One sealed file per `vacuum()` call. Engine continues serving reads and writes. Write-path impact is zero — `write_mu_` is held only for the in-memory commit step. Tombstones are always copied (never elided) during partial vacuum to prevent key resurrection across unprocessed files. Full vacuum (drop tombstones, multi-file merge) is a separate future operation. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmssUUUUUU}` using microsecond precision. Gives lexicographic == chronological ordering and avoids the false precision of nanosecond timestamps whose sub-microsecond bits are often zero on Linux. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be discarded with a logged warning. No partial-batch entries enter the key directory. |

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints