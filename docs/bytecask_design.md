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

Keys are stored as byte sequences within the radix tree's prefix-compressed nodes. The radix tree API accepts `std::span<const std::byte>` for all key parameters — no intermediate `Key` wrapper is needed for internal operations. The public `Key` class (backed by `std::vector<std::byte>`) is retained for the external iterator API (`KeyIterator`, `EntryIterator`) and for the recovery tombstone tracking map. `Key` provides `operator<=>` (lexicographic over raw byte values) and `begin()`/`end()`/`size()` accessors. Keys have a hard upper bound of 65 535 bytes (the `u16 key_size` field in the data file header).

**Historical note**: the original key directory used `PersistentOrderedMap<Key, KeyDirEntry>`, backed by `immer::flex_vector<Entry>`. The radix tree replacement (BC-030) delivers O(k) lookups vs O(n log n) binary search, lower memory overhead via prefix compression and intrusive refcounting, and faster batch mutations via the transient API's in-place path copying. `PersistentOrderedMap` is retained in the codebase for benchmarking purposes (`benchmarks/map_bench.cpp`).

### Concurrency Model

ByteCask follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time.
- Multiple readers may operate concurrently.
- MVCC and snapshot isolation are not supported.

#### Concurrency strategy

ByteCask's read path is designed so that **readers never acquire the write mutex**. The strategy combines two ideas:

1. **A single writer mutex** (`write_mu_`) that serialises mutations — readers are completely unaffected by it.
2. **An immutable, copy-on-write snapshot** (`EngineState`) published via `std::atomic<std::shared_ptr<EngineState>>` — readers capture the current snapshot without blocking the writer.

##### State publication via std::atomic<shared_ptr>

The engine state is published through `std::atomic<std::shared_ptr<EngineState>>`. Writers call `state_.store()` to publish a new immutable snapshot; readers call `state_.load()` to obtain a reference-counted copy. The atomic `shared_ptr` guarantees that `load()` always returns a valid, self-consistent snapshot. Old snapshots stay alive as long as any reader holds a reference.

`Bytecask` uses `std::atomic<std::shared_ptr<EngineState>>` for its published state. The `write_mu_` mutex serialises writers; `state_.load()` is the readers' only access point.

##### Shared state layout

```
  Bytecask object
  ┌─────────────────────────────────────────────────────────────┐
  │  write_mu_      std::mutex (heap-allocated)                 │  ← writers only
  │                                                             │
  │  file_stats_    map<uint32_t, FileStats>                    │  ← writers only (under write_mu_)
  │                                                             │
  │  state_         atomic<shared_ptr<EngineState>>             │  ← writer stores,
  │                                                             │    readers load (no write_mu_)
  └─────────────────────────────────────────────────────────────┘

  EngineState  (heap, reference-counted, never mutated in place)
  ┌─────────────────────────────────────────────────┐
  │  key_dir        PersistentRadixTree<KeyDirEntry> │  key → (file_id, offset, seq)
  │  files          shared_ptr<FileMap>              │  file_id → open DataFile fd
  │  active_file_id uint32_t                         │
  │  next_file_id   uint32_t                         │  writer-only; monotonic file counter
  │  next_lsn       uint64_t                         │  writer-only; monotonic sequence counter
  └─────────────────────────────────────────────────┘
```

`EngineState` bundles all mutable engine state into a single immutable value. Each write operation produces a new `EngineState` via pure transition methods (`apply_put`, `apply_del`, `apply_rotation`). The old state stays alive as long as any reader holds a `shared_ptr` reference — the writer always allocates a fresh state and never mutates one in place.

`next_file_id` and `next_lsn` are writer-only fields (readers never inspect them), but including them in `EngineState` makes transitions self-contained: `apply_put` bumps `next_lsn` internally, `apply_rotation` bumps `next_file_id`, so the writer never manages loose counters.

##### Write path

```
  Writer thread
  ─────────────
  1. acquire write_mu_
     └─ serialises concurrent writers; readers never touch this mutex

  2. append entry to active DataFile   (I/O, under write_mu_)

  3. produce new EngineState via pure transition (e.g. state.apply_put(...))

  4. state_.store( make_shared<EngineState>(new_state) )
     └─ atomically publishes the new immutable snapshot;
        any subsequent state_.load() on any thread is
        guaranteed to observe this or a later value

  5. release write_mu_

  6. sync_group_.sync(file)   ← group commit; see below
```

##### Group commit (`SyncGroup`)

After releasing `write_mu_`, the writer calls `sync_group_.sync(file)` instead
of `file->sync()` directly. `SyncGroup` uses a ticket-based protocol to batch
concurrent `fdatasync` calls:

1. **Phase 1 — Take a ticket.**  The writer increments a monotonic counter
   (`next_ticket_`) under the SyncGroup mutex, registering that its `writev`
   has completed and its data is in the page cache.

2. **Phase 2 — Wait or lead.**  The writer waits until either
   (a) `current_synced_ticket_ >= my_ticket` — a later sync already covered
   its data, so it returns immediately; or
   (b) `!syncing_` — no `fdatasync` is in flight, so it becomes the leader.

3. **Phase 3 — Sync.**  The leader snapshots `next_ticket_ - 1` as the batch
   watermark, releases the lock, and calls `fdatasync` (wrapped in a try/catch
   block to gracefully handle I/O exceptions by waking waiters before throwing).
   Every ticket issued up to that watermark has a completed `writev`, so one syscall
   covers them all. On return the leader advances `current_synced_ticket_` and
   wakes all waiters.

**Invariant**: a writer's data is never assumed durable until an `fdatasync`
that started *after* the writer's `writev` has completed. Writers can never
piggyback on an in-flight sync that may have started before their data reached
the page cache.

This amortises the ~2 ms `fdatasync` cost across N concurrent writers instead
of serialising N × 2 ms.

**Historical note**: BC-051 removed an earlier `GroupWriter` implementation
because benchmarks showed no benefit. Those benchmarks ran on tmpfs where
`fdatasync` is a no-op (~0 ns). Re-benchmarking on a real block device
(ext4 on NVMe) confirmed that `fdatasync` costs ~2 ms and serialised syncs are
the dominant bottleneck for concurrent writes.

##### Read path (no lock, no mutex)

```
  Reader thread N

  1. snap = load_state(opts)  // const ref to thread-local shared_ptr
     └─ no refcount bump — returns a const& to the TL cache.
        The shared_ptr stays alive until the same thread
        calls load_state again.

  2. tree lookup via raw const Node* pointers
     └─ no IntrusivePtr copies, no atomic refcount traffic.
        Safe: snap owns root IntrusivePtr → keeps all
        descendants alive. Transient (write path) clones
        shared nodes before mutating — old nodes stay intact.

  3. pread(fd, ...) for value retrieval
     └─ stateless, no synchronisation.
```

The read path never acquires `write_mu_`. `load_state` returns a `const&` to a thread-local `shared_ptr<const EngineState>`, avoiding the refcount increment/decrement that `atomic<shared_ptr>::load()` would impose on every read. The radix tree lookup uses raw `const Node*` pointers instead of `IntrusivePtr` copies, eliminating per-level `acq_rel` atomic traffic. Both optimisations are safe because the thread-local snapshot anchors the entire node tree for the duration of the `get()` call.

**Same-thread guarantee**: a `put` followed by a `get` on the **same thread** always observes the put — the `store()` in step [4] is sequenced before the `load()` in the subsequent `get()`.

#### Read-scaling behaviour

Benchmarks on a 22-vCPU instance (50k keys, 1 KiB random values):

| Threads | Before (mutex only) | After (atomic shared_ptr) | Improvement |
|---------|---------------------|--------------------------|-------------|
| 2       | 1.66 M ops/s        | 2.16 M ops/s             | +30%        |
| 4       | 1.73 M ops/s        | 3.01 M ops/s             | +74%        |
| 8       | 1.74 M ops/s        | 3.57 M ops/s             | +105%       |
| 16      | 1.91 M ops/s        | 3.67 M ops/s             | +92%        |

Throughput scales near-linearly with thread count for read-heavy workloads.

#### Read consistency (`ReadOptions`)

`ReadOptions` controls consistency behaviour for read operations (`get`, `contains_key`). ByteCask provides two read consistency modes, controlled by a single field:

```cpp
struct ReadOptions {
  std::chrono::milliseconds staleness_tolerance{0};
};
```

The two modes follow the same naming conventions used by Azure Cosmos DB and distributed systems literature:

| Mode | `staleness_tolerance` | Same-thread `put` → `get` | Cross-thread staleness | Hot-path cost |
|------|----------------------|--------------------------|------------------------|---------------|
| **Session** (default) | `0` | Always sees the put. Refreshes whenever `state_time_` changes — i.e. after any new write, including one just performed by this thread. | A nanosecond-scale window between the two writer stores (see below). | Single `MOV` + integer compare when cached |
| **Bounded staleness** | `> 0` | **May not see the put.** If a previous write occurred within `staleness_tolerance`, the cached snapshot is returned — even for the thread that just called `put()`. | Up to `staleness_tolerance` after each write. | Single `MOV` + integer compare when cached |

This is analogous to RocksDB's built-in `SuperVersion` thread-local caching, which always provides session consistency with no user-facing knob. ByteCask adds bounded staleness as an opt-in for write-heavy workloads where read throughput matters more than freshness.

##### Session consistency (`staleness_tolerance = 0`, default)

The default mode. The thread-local snapshot is refreshed whenever any write has occurred — the reader compares the writer's timestamp against its cached timestamp and refreshes if they differ. This guarantees **read-your-writes** on the same thread: a `put()` followed by a `get()` always observes the put.

Cross-thread writes are visible within nanoseconds (the two-store gap described below). For all practical purposes, this mode behaves like the latest committed state.

##### Bounded staleness (`staleness_tolerance > 0`)

The thread-local snapshot is refreshed only when the last write is older than `staleness_tolerance`. The same-thread read-your-writes guarantee does **not** hold: a `put()` immediately followed by a `get()` may return a stale snapshot.

Example with `staleness_tolerance = 100ms`:

```
t=0 ms   put("a", "v1")   → state_time_ = T0, tl refreshes, tl.last_write_time = T0
t=50 ms  put("b", "v2")   → state_time_ = T1 (T1 − T0 = 50ms)
t=50 ms  get("b")          → wt = T1, T1 − T0 = 50ms ≤ 100ms → condition false
                              ↳ returns stale snapshot; "b" not found
```

Use this mode when write throughput is high and readers can tolerate bounded staleness. At high thread counts, avoiding the `atomic<shared_ptr>` internal spinlock on every read yields significant throughput improvements.

##### Mechanism

The writer timestamps every state publication. After every `state_.store()`, the writer calls `steady_clock::now()` and stores the result in `atomic<int64_t> state_time_` with `memory_order_release`. The read hot path is a single `relaxed` load of `state_time_` (plain `MOV` on x86) — no clock call on the reader side, no locked instruction, no refcount traffic until a refresh is needed.

```
  Writer:  state_.store(S1, seq_cst)             ← new immutable snapshot
           state_time_.store(now_ns(), release)   ← timestamp the publication

  Reader:  wt = state_time_.load(relaxed)         ← single MOV on x86
           if wt - tl.last_write_time > tolerance:
               tl.snapshot = state_.load()        ← refresh (refcount bump)
               tl.last_write_time = wt
           return tl.snapshot
```

##### Cross-thread staleness window (session mode)

The writer performs two separate stores in sequence:

```
  state_.store(S1)                ← step A: publish new state
  state_time_.store(T1, release)  ← step B: publish new timestamp
```

A reader that samples `state_time_` between steps A and B sees the old timestamp `T0`, computes `T0 − T0 = 0 > 0` as false, and returns the old snapshot — even though `S1` is already visible in `state_`. This window is a few nanoseconds wide (two consecutive stores on the same CPU). `memory_order_release` on step B ensures that once a reader observes `T1`, `state_.load()` is guaranteed to see `S1` (via acquire semantics). This window can only be observed by reader threads running concurrently on other CPUs, not by the writer thread itself.

With `staleness_tolerance > 0` the window is irrelevant: the snapshot is held for at least `staleness_tolerance` regardless.

**Benchmark results** (22-vCPU, 50k keys, 1 KiB values, 1 background writer, bounded staleness with `tolerance = 100ms` vs session with `tolerance = 0`):

| Threads | Session ops/s | Bounded Staleness ops/s | Speedup | p99 Session | p99 Bounded Staleness |
|---------|--------------|------------------------|---------|-------------|----------------------|
| 2       | 812k         | 791k                   | −3%     | 16.6 µs     | 21.9 µs              |
| 4       | 1.54 M       | 1.39 M                 | −10%    | 33.0 µs     | 47.2 µs              |
| 8       | 1.39 M       | 1.98 M                 | +42%    | 193 µs      | 129 µs               |
| 16      | 846k         | 2.44 M                 | +188%   | 1809 µs     | 536 µs               |

The benefit is pronounced at high thread counts where session-mode readers contend for the internal spinlock inside `atomic<shared_ptr>` on every refresh.

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
| **Rotating** | Sealed (`fdatasync` + `seal()`); a companion `.hint` file is being written by the background worker. |
| **Immutable** | The `.hint` file exists; the data file is sealed and read-only. |

A new active data file is always created on engine startup.

### Vacuum

(This project uses the PostgreSQL term *vacuum*; other systems call it *compaction* or *merge*.)

ByteeCask implements a **conservative online vacuum**: the engine continues to serve reads and writes while vacuum rewrites sealed data files. *Conservative* means the design prioritises write-path non-interference and correctness over maximum space reclamation efficiency.

#### Constraints

- Only sealed (immutable) data files are considered — the active file is never touched.
- Only files whose fragmentation exceeds a configurable threshold are processed; files below the threshold are left alone.
- One file is processed per `vacuum()` call. Callers that want to process multiple files call in a loop.
- Tombstones (Delete entries) are never dropped during partial compaction (see **Tombstone handling** below).
- A new compacted file is fully written and `fdatasync`-ed before any old file is removed.

#### Fragmentation

The fragmentation of a sealed data file is the fraction of disk space occupied by dead entries:

```
fragmentation = 1 − live_bytes / total_bytes
```

- `total_bytes` — physical file size: all appended bytes, including dead puts, tombstones, and BulkBegin/BulkEnd markers.
- `live_bytes` — sum of entry sizes (`kHeaderSize + key_size + value_size + kCrcSize`) for Put entries currently referenced by the key directory.
- Tombstones always contribute to `total_bytes` but never to `live_bytes` — they genuinely increase fragmentation. BulkBegin/BulkEnd markers likewise contribute only to `total_bytes`; vacuum strips them from the compacted output.

A file qualifies for vacuum when `fragmentation >= VacuumOptions::fragmentation_threshold` (default `0.5`).

#### Live fragmentation tracking

Rather than computing `live_bytes` at vacuum time (which would require a full key-directory traversal — O(total_keys) — or scanning sealed data files), the engine maintains per-file stats updated incrementally.

```cpp
struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
};
```

`file_stats_` is a `std::map<uint32_t, FileStats>` member of `Bytecask` (not inside `EngineState`). It is updated under `write_mu_` on every write operation. Keeping it outside `EngineState` avoids an O(files) map copy on every write — `EngineState` is an immutable snapshot published via `atomic<shared_ptr>`, so embedding a `std::map` in it would force a deep copy on every transition.

The helper `entry_size(key_size, value_size)` returns `kHeaderSize + key_size + value_size + kCrcSize` and is used everywhere stats are updated.

##### Write-path updates

- **On `put(key, value)`**: if the key already exists (overwrite), subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats_[old_entry.file_id].live_bytes`. Add `entry_size(key.size(), value.size())` to `file_stats_[active_file_id].live_bytes` and to `.total_bytes`.
- **On `del(key)`**: if the key exists, subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats_[old_entry.file_id].live_bytes`. Add the tombstone size (`kHeaderSize + key.size() + kCrcSize`) to `file_stats_[active_file_id].total_bytes`. The tombstone is never added to `live_bytes` — tombstones are never referenced by the key directory.
- **On `apply_batch`**: same per-operation logic as `put`/`del`. BulkBegin and BulkEnd markers each add `kHeaderSize + kCrcSize` to the active file's `total_bytes` only — they are never referenced by the key directory.
- **On rotation**: the active file's stats are already accurate. Insert `FileStats{0, 0}` for the new active file.

At vacuum time fragmentation is an O(1) integer division per file — no scanning, no I/O, no additional lock contention.

The active file's `live_bytes` may be non-zero (it holds the current live writes), but the active file is never a vacuum candidate, so its fragmentation is never evaluated.

##### Recovery stats reconstruction

`file_stats_` must be rebuilt on startup. Both `total_bytes` and `live_bytes` are reconstructed without scanning data files or traversing the key directory:

- **`total_bytes`**: computed via `std::filesystem::file_size(path)` per sealed file in `open_and_prepare_files()`. Exact for append-only files. O(1) per file, no I/O beyond a `stat` call.
- **`live_bytes`**: reconstructed as a side-effect of the existing hint-file recovery pass. The hint entry carries `key_size` (from `key.size()`) and `value_size`, so `entry_size(key_size, value_size)` is computable without touching the data file.

The displacement logic mirrors write-path updates:

```
for each hint entry h (processed in arbitrary file order, LSN wins):
  if h.type == Put and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    file_stats[h.file_id].live_bytes += entry_size(h.key_size, h.value_size)

  if h.type == Delete and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    // tombstone never enters live_bytes
```

Processing order across files does not matter — LSN comparison always picks the correct winner, so `live_bytes` converges to the right values.

**Parallel recovery**: `RecoveryResult` includes a `file_stats` map alongside `key_dir`, `tombstones`, and `max_lsn`. Each worker builds its own `file_stats` during Phase 2 using the algorithm above. During Phase 3 fan-in: file IDs are disjoint across workers (files are partitioned by round-robin), so `file_stats` maps are unioned without summing. When the LSN resolver discards a losing entry during tree merge or tombstone cross-application, the loser's size is subtracted from its file's `live_bytes`.

#### Vacuum primitives

Vacuum is decomposed into two self-contained, independently testable primitives. The `vacuum()` caller picks exactly one per target file — there is no compound "compact then maybe absorb" path.

##### `vacuum_compact_file(file_id)` — rewrite a sealed file, dropping dead entries

Used when the file's live data is too large to fit into the active file. Produces a new, sealed, compacted file.

1. **Snapshot the key directory** — call `state_.load()` to obtain the current `EngineState`. This is the authoritative view of which entries are live.
2. **Rewrite the target file** — open a new data file at a `.data.tmp` path for writing (new timestamp stem, same directory). Scan the old data file entry by entry using **batch-aware scanning** (see below). For each emitted entry:
   - *Put entry*: check whether the snapshot's key directory entry for that key points to the old file at this offset with the same sequence number. If yes (live), write it to the new file at its new offset, recording `(key → new_file_id, new_offset)`. If no (dead), skip.
   - *Delete entry*: always copy to the new file verbatim (same sequence number, same key). See **Tombstone handling** below.
3. **Seal and durability** — `fdatasync` the tmp file, close it. Rename `.data.tmp` → `.data` atomically. Open a new `DataFile` at the final path and seal it. Write a hint file by scanning the compacted file (no batches in the output), using the temp-then-rename protocol (`.hint.tmp` → `.hint`).
4. **Atomic commit** (under `write_mu_`):
   a. Build a `TransientRadixTree` from the current `key_dir_`.
   b. For each live Put entry copied to the new file, look up the key in the current key directory. If the sequence number still matches (no concurrent write superseded it), update `KeyDirEntry` to the new `file_id` and `file_offset`. If the sequence number differs, skip — the concurrent writer's version takes precedence.
   c. Call `persistent()` to obtain the new immutable key directory.
   d. Build an updated `FileRegistry`: add the new compacted file, remove the old file.
   e. Update `file_stats_`: remove the old file's entry. Insert a new entry for the compacted file using the exact `compacted_live_bytes` tracked during step 2 — for each entry whose key-dir sequence no longer matched in step 4b (concurrent write won), its `entry_size` was subtracted from the running total. `total_bytes` is the physical size of the new file. (Note: `compacted_live_bytes` may be less than `total_bytes` because the new file also contains tombstones that are never counted as live, and entries superseded by concurrent writes during the I/O phase.)
   f. Publish the new `EngineState` via `state_.store()`.
5. **Release `write_mu_`**.
6. **Defer file cleanup** — the old file is removed from the registry but may still be referenced by in-flight readers via their `EngineState` snapshot. The old `shared_ptr<DataFile>` is stashed in `stale_files_` (protected by `vacuum_mu_`). At the start of each `vacuum()` call, entries whose `use_count() == 1` (only `stale_files_` holds the reference, no readers) are purged: the fd is closed and the `.data` + `.hint` files are deleted. The destructor also purges all remaining stale files.

##### `vacuum_absorb_file(file_id)` — fold a sealed file's live entries into the active file

Used when the file's live data is small enough to append to the active file without exceeding the rotation threshold. Eliminates a file descriptor and reduces recovery time.

Precondition: `file_stats_[file_id].live_bytes + active_file.size() <= rotation_threshold`. The caller (`vacuum()`) verifies this before calling.

1. **Snapshot the key directory** — call `state_.load()`.
2. **Acquire `write_mu_`** — the active `DataFile` is NOT thread-safe, so the entire scan-copy-sync-commit sequence must be serialised against concurrent `put`/`del`/`apply_batch` calls that also append to the active file.
3. **Append live entries to the active file** — scan the old data file entry by entry using **batch-aware scanning** (see below). For each emitted entry:
   - *Put entry*: if live (same check as `compact_file`), append to the active file at its new offset. The original LSN is preserved verbatim — no new sequence numbers are allocated. Record `(key → active_file_id, new_offset)`.
   - *Delete entry*: copy verbatim to the active file (same LSN, same key).
4. **Durability** — `fdatasync` the active file.
5. **Commit** (still under `write_mu_`):
   a–c. Same key-directory remapping as `vacuum_compact_file`, but pointing entries to `active_file_id` instead of a new compacted file.
   d. Build an updated `FileRegistry`: remove the old file (no new file added).
   e. Update `file_stats_`: remove the old file's entry. Add the absorbed entries' sizes to `file_stats_[active_file_id]` (both `live_bytes` and `total_bytes` for live Puts; `total_bytes` only for tombstones). Subtract sizes for entries superseded by concurrent writes (same logic as `vacuum_compact_file`).
   f. Publish the new `EngineState`.
6. **Release `write_mu_`**.
7. **Defer file cleanup** — same as `vacuum_compact_file`: stash in `stale_files_`, purge when `use_count() == 1`.

Note: absorbed entries preserve their original LSNs, which are all less than `next_lsn`. This is safe because LSN ordering is only used for recovery conflict resolution across files, and the absorbed entries now live in the active file alongside newer writes. Recovery will see all entries and LSN comparison still picks the correct winner.

#### Batch-aware scanning

Both vacuum primitives use batch-aware scanning when reading the old data file. Entries between `BulkBegin` and `BulkEnd` are buffered and only emitted when `BulkEnd` is reached. If the file ends mid-batch (crash during `apply_batch`), the buffered entries are silently discarded — they were never committed.

**Why**: without batch-aware scanning, vacuum would copy uncommitted entries (including tombstones from incomplete batches) into the output. Those tombstones could incorrectly shadow live Puts in other files, causing data loss on recovery. This matches `flush_hints_for`'s batch-aware hint generation.

#### Crash safety

`vacuum_compact_file` writes the new data file to `.data.tmp` and renames it atomically to `.data` after `fdatasync`. If the engine crashes mid-write, recovery ignores `.data.tmp` files (it only processes `.data` extensions) and cleans them up in `open_and_prepare_files`. The hint file uses the same `.hint.tmp` → `.hint` protocol.

`vacuum_absorb_file` appends to the already-open active file. If the engine crashes mid-absorb, the old file still exists on disk (deferred cleanup). Recovery opens both files and processes all entries. Absorbed entries appear in both the old file and the active file with identical LSNs — recovery's conflict resolution handles duplicates correctly.

#### Vacuum caller (`vacuum()`)

The public `vacuum()` method orchestrates file selection and dispatches to exactly one primitive:

1. **Acquire `vacuum_mu_`** — prevents two `vacuum()` calls from running concurrently.
2. **Purge stale files** — check `stale_files_` for entries whose `shared_ptr<DataFile>` has `use_count() == 1` (no in-flight readers). For those, close the fd and delete the `.data` + `.hint` files.
3. **Select a target file** — copy `file_stats_` under a brief `write_mu_` acquisition (O(sealed files), then release). Iterate sealed files, compute `fragmentation = 1 − live_bytes / total_bytes` (O(1) per file, no I/O), pick the highest-fragmentation sealed file above `fragmentation_threshold`. If no file qualifies, return immediately.
4. **Branch**:
   - If `file_stats_[target].live_bytes <= absorb_threshold` **and** `file_stats_[target].live_bytes + active_file.size() <= rotation_threshold` → call `vacuum_absorb_file(target)`.
   - Otherwise → call `vacuum_compact_file(target)`.

   `absorb_threshold` (default 1 MiB) limits absorption to genuinely small files. A file with more live data than `absorb_threshold` is always compacted into a new sealed file, keeping the active file from bloating.
5. **Release `vacuum_mu_`**.

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
- Serialization uses an internal cursor-based API (`ByteWriter` / `ByteReader` in `serialization.cppm`). Both wrap the low-level `write_le` / `read_le` helpers with an auto-advancing offset so callers never compute byte positions by hand. `ByteWriter` optionally accepts a `Crc32*`; when non-null every `put()` / `put_bytes()` call also feeds the written bytes into the CRC accumulator, giving one-pass write + checksum with zero ceremony.
- CRC-32C uses the Castagnoli polynomial `0x1EDC6F41` via the [google/crc32c](https://github.com/google/crc32c) library, which auto-detects hardware acceleration at runtime (SSE4.2 on x86-64, CRC instructions on AArch64) and falls back to a software implementation when neither is available.
- CRC32 is computed over **all bytes of the entry except the trailing CRC field itself** (i.e., the leading header + key data + value data).

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
- **`append(sequence, entry_type, key, value) -> Offset`**: Serializes a new entry with the given sequence number and `EntryType`, writes it to the OS page cache via `::writev()`, and returns the byte offset where the entry starts. `BulkBegin`/`BulkEnd` entries pass empty key and value spans. Does **not** guarantee durability on its own. The 15-byte header and 4-byte CRC are serialized into a fixed member buffer (`hdr_crc_buf_`); the key and value spans are passed directly as iovecs — no heap allocation and no copy of key/value data occurs on the write path.
- **`sync()`**: Calls `::fdatasync()` to flush all pending writes to physical storage. Must be called explicitly to guarantee crash-safety. Decoupled from `append()` to enable Group Commit: callers can batch multiple `append()` calls before a single `sync()`.
- **`read_entry(offset, key_size, value_size, io_buf)`**: Single-pread read primitive. Resizes `io_buf` (reusing existing capacity) and preads the full entry into it. Callers then pass `io_buf` to `deserialize_entry()` (recovery, scan) depending on what they need. `scan()` uses this internally after its header pread.
- **`read_value(offset, key_size, value_size, io_buf, out)`**: High-level read primitive. Calls `read_entry` then `extract_value_into` to pread, CRC-verify, and extract only the value into `out`. Both `io_buf` (scratch) and `out` reuse existing capacity across calls. Used by `Bytecask::get()` and `EntryIterator`.
- **`read_value_into`** was removed — `read_entry` + `extract_value_into` replaced it; `read_value` now provides the combined convenience API.
- Key and value are accepted as `std::span<const std::byte>` for binary safety.

### I/O Back-end Rationale

- **POSIX over `std::ofstream`**: `::writev()` with `O_APPEND` issues a single syscall per entry using scatter-gather I/O, skipping the buffering layers and locale state overhead of C++ streams.
- **`fdatasync` over `fflush`/`flush()`**: `fdatasync` syncs data to physical media while skipping inode metadata updates (access time etc.), making it faster than `fsync` for a pure append-only log.
- **Group Commit pattern**: Separating `append()` (writes to page cache) from `sync()` (forces to disk) lets future code batch hundreds of writes before a single expensive `fdatasync`, which is the primary lever for high write throughput on NVMe hardware (see `io_uring` paper reference).
- **Zero-copy write path**: `append()` builds only the 15-byte header and 4-byte CRC in a fixed member buffer, then calls `::writev()` with four iovecs — `[header(15), key, value, crc(4)]`. The kernel gathers the scattered buffers into one atomic write without any intermediate heap allocation or memcpy of key/value data. For 1 KiB values this eliminates ~250 MB/s of unnecessary copying at 244k puts/s.

### Source Code Module Architecture

We use fine-grained C++20 modules:
- `bytecask.util`: CRC-32C accumulator (`Crc32`, backed by google/crc32c) and checked `narrow<To>(From)` conversion.
- `bytecask.serialization`: Core serialization primitives (`ByteWriter`, `ByteReader`, `read_le`, `write_le`) and re-exports `bytecask.util`.
- `bytecask.data_entry`: Logical entry definition, `write_header_and_crc()` (fills a fixed 19-byte buffer with LE header + CRC for zero-copy I/O), `serialize_entry()` (complete in-memory entry for tests/recovery), `parse_header_and_verify()` / `deserialize_entry()` / `extract_value_into()` — CRC verification is factored into `parse_header_and_verify()` and shared by both extraction functions.
- `bytecask.data_file`: Disk I/O, writing streams sequentially to `.data` files.
- `bytecask.hint_entry`: `HintEntry`, `serialize_entry()`, `deserialize_entry()` — symmetric read/write for hint entries.
- `bytecask.hint_file`: Hint file writer and reader (`HintFile`).
- `bytecask.persistent_ordered_map`: Immutable sorted map (`PersistentOrderedMap<K,V>`, `OrderedMapTransient<K,V>`) backed by `immer::flex_vector`; retained for benchmarking.
- `bytecask.radix_tree`: Persistent radix tree (`PersistentRadixTree<V>`, `TransientRadixTree<V>`, `RadixTreeIterator<V>`) with path compression and intrusive refcounting; used as the key directory.
- `bytecask.engine`: Public engine API (`Bytecask`, `EngineState`, `Batch`, `KeyIterator`, `EntryIterator`, `FileRegistry`, type aliases).

### Current scope boundaries

- `EntryType` is written and read back on `DataFile::read()`; atomic bulk semantics are enforced at a layer above `DataFile`.
- No file rotation or size limits.
- No read path for the key directory — append-only for now.

### DataFile fd mode after rotation

`DataFile` opens with `O_RDWR | O_CREAT | O_APPEND`. After a data file is rotated it is logically immutable — no new entries should be appended. The engine enforces this at a higher level; the fd mode is not downgraded to `O_RDONLY` after rotation. Rationale:

- `DataFile` is an internal class; the engine exclusively controls when `append()` is called.
- Re-opening the fd purely for semantic enforcement adds syscall overhead and complexity without improving correctness for the production path.
- A `sealed_` flag with `assert(!sealed_)` at the top of `append()` is sufficient: it catches programming errors in debug builds at zero production cost.

Contrast with `HintFile`, which uses `OpenForWrite` / `OpenForRead` factory functions. That split models externally visible, non-overlapping lifecycles at different call sites: one site writes during `flush_hints()`, a completely separate site reads during recovery. Encoding that distinction in the type prevents mixing them up. `DataFile` has no equivalent external semantic split.

### HintFile I/O model

Both write and read modes operate on an in-memory `std::vector<std::byte>` buffer. No OS file descriptor is held open across calls.

- **`OpenForWrite(path)`** — creates an empty in-memory buffer. `append()` serializes entries into this buffer. `sync()` opens the file, writes the entire buffer in a single `write()`, calls `fdatasync()`, and closes the fd — all within the `sync()` call.
- **`OpenForRead(path)`** — reads the entire file into the buffer via a single `pread` and immediately closes the fd. `scan()` operates on the buffer.

This symmetry eliminates fd lifetime management from the class: no destructor close, no move-constructor fd transfer, defaulted special members.

`HintFile::scan()` returns a `HintEntry` whose `key` is a `std::span<const std::byte>` into the backing buffer — zero allocation per entry. All callers (recovery and tests) use `scan()`.

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

### Recovery

On engine startup:

1. Discard any `.hint.tmp` files — incomplete hint files from a crash mid-rotation.
2. Open all `.data` files and seal them. For any data file without a companion `.hint`, generate one via `flush_hints_for()` (batch-aware: buffers entries between BulkBegin/BulkEnd, discards incomplete batches, logs a warning).
3. Recover exclusively from hint files. For each hint entry:
   - `Put`: insert `(key → {sequence, file_id, file_offset, value_size})` only if `entry.sequence > dir[key].sequence` (skip if a fresher entry is already present).
   - `Delete`: remove the key from the tree if `entry.sequence > dir[key].sequence`; otherwise skip.
4. Record `max_lsn` — the largest sequence number seen across all hint entries.
5. Create a new active data file seeded at `max_lsn + 1`.

This is a single code path: `flush_hints_for()` is the same function used by rotation and background hint writes. No raw-scan recovery logic exists — recovery always goes through hints.

### Parallel Recovery

`Bytecask::open(dir, max_file_bytes, recovery_threads)` accepts an optional `recovery_threads` parameter (default 1 = serial). When `recovery_threads > 1`, recovery uses `parallel_recover_existing_files`:

1. **Phase 1 (serial, shared)**: same as above — open files, generate missing hints. Factored into `open_and_prepare_files()`, shared by both paths.
2. **Phase 2 (parallel build)**: round-robin assign files to W workers. Each builds a `RecoveryResult{key_dir, tombstones, max_lsn, file_stats}` independently.
3. **Phase 3 (parallel fan-in)**: pairwise merge in ⌈log₂(W)⌉ rounds. Each merge uses `PersistentRadixTree::merge(a, b, lsn_resolver)` then cross-applies tombstones from both sides to suppress stale PUTs, unions tombstone maps and `file_stats` maps for subsequent rounds. Losing entries' sizes are subtracted from their file's `live_bytes`.
4. **Phase 4 (serial assembly)**: `s.key_dir = final.key_dir; s.next_lsn = final.max_lsn + 1`.

See `docs/parallel_recovery_design.md` §11 for the full v1 algorithm.

**Benchmark** (`BM_RecoveryParallel`, prefixed keys, 1-byte values, 256 KiB rotation, hint-only, disk-backed TMPDIR):

| Threads | 1M keys (ms) | Speedup | 10M keys (ms) | Speedup |
|---------|-------------|---------|--------------|---------|
| 1       | 262         | 1.00×   | 2858         | 1.00×   |
| 2       | 153         | 1.71×   | 1647         | 1.73×   |
| 4       | 91          | 2.88×   | 962          | 2.97×   |
| 8       | 58          | 4.51×   | 567          | 5.04×   |
| 16      | 52          | 5.04×   | 503          | 5.68×   |

Scaling is sub-linear due to fan-in merge overhead and memory bandwidth saturation beyond 8 threads. At 10M keys, recovery drops from 2.86s (serial) to 503ms (16 threads).

### Incomplete Batch Recovery

If the engine crashes after writing a `BulkBegin` but before the matching `BulkEnd`, the data file contains an incomplete batch. `flush_hints_for()` detects this (BulkBegin with no matching BulkEnd), discards the buffered entries, and logs a warning. No partial-batch entries appear in the generated hint file, so they are never inserted into the key directory.

### Relationship to Data Files

| Property | Data file | Hint file |
|---|---|---|
| Extension | `.data` | `.hint` |
| Contents | Full key + value | Key + location metadata only |
| Created | At engine open / rotation | When a data file is sealed (rotated), or on startup for files missing hints |
| Read at startup | Never directly — hints are generated first if absent | For all sealed files |

One hint file corresponds to exactly one data file (same timestamp stem, different extension).

**Hint files are a correctness-carrying artifact for recovery.** On startup, any data file without a companion `.hint` has one generated via the batch-aware `flush_hints_for()`. Recovery then reads only hint files — there is no separate raw-scan fallback path.

Hint files are written **deferred**: at engine close, or via an explicit `flush_hints()` call — never inline on the write path. This keeps write-path latency flat and bounded regardless of file size at the time of rotation. The cost is that a crash after rotation but before `flush_hints()` causes recovery to generate the hint file on startup, which is always correct and only slower.

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
- **`scan(offset) -> std::optional<std::pair<HintEntry, uint64_t>>`**: Returns a zero-copy view of the hint entry at `offset` bytes. The key is a `std::span<const std::byte>` into the backing buffer — valid while the `HintFile` is alive. Returns `std::nullopt` at end-of-file. Panics on CRC mismatch. Typical usage: `while (auto result = file.scan(offset)) { auto& [entry, next] = *result; offset = next; }`.

`HintEntry` is a plain struct holding `{uint64_t sequence, EntryType entry_type, uint64_t file_offset, std::span<const std::byte> key, uint32_t value_size}`.

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

## Background Worker

`BackgroundWorker` is a non-exported internal class in `bytecask.engine`. It maintains a single persistent background thread that processes tasks in FIFO order.

### API

```cpp
class BackgroundWorker {
public:
  BackgroundWorker();
  ~BackgroundWorker();                        // drains, then joins thread
  void dispatch(std::function<void()> task);  // non-blocking enqueue
  void drain();                               // block until idle
};
```

### Lifetime invariant

`BackgroundWorker` is declared as the **last member** of `Bytecask`. C++ destruction order is reverse of declaration order, so `worker_` destructs first — its destructor sets `stop_ = true`, notifies the worker thread, and joins it. This guarantees the background thread has finished all tasks before any other `Bytecask` member (`dir_`, `state_`, `write_mu_`, etc.) is destroyed. There is no risk of the background thread accessing freed memory.

### Exception handling

Exceptions thrown by background tasks are caught per-task, logged to `stderr`, and swallowed. Hint file writes are correctness-safe to drop: recovery always falls back to scanning the raw `.data` file if no `.hint` companion exists.

### BC-026: deferred hint file writes

After `rotate_active_file()` seals a data file, it captures a `shared_ptr<DataFile>` to the sealed file and dispatches `flush_hints_for(file, dir)` to the worker. The `shared_ptr` keeps the `DataFile` fd alive for the duration of the background task regardless of what the engine state does subsequently.

The synchronous path in `flush_hints(EngineState&)` (called by the public `flush_hints()` method) reuses the same `flush_hints_for` helper, guaranteeing consistent hint-writing behaviour between the background and synchronous paths.

The `~Bytecask()` destructor no longer calls `flush_hints()` explicitly. Because `worker_` destructs first and drains all pending tasks, every sealed file that was rotated during the engine's lifetime will have had its hint file written (or the write attempted) before the engine shuts down.

---

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: crc32c (google/crc32c, hardware-accelerated CRC-32C)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: Full `Bytecask` SWMR engine with `open`, `get`, `put`, `del`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`. Key directory backed by `PersistentRadixTree<KeyDirEntry>`. Per-file fragmentation tracking via `FileStats` (`live_bytes`, `total_bytes`) maintained on every write and reconstructed during recovery. `open()` always creates a fresh active data file; recovery from hint files (serial and parallel). 1.2M+ assertions, 103 test cases.

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/util.cppm`: C++23 module (`bytecask.util`) — `Crc32` accumulator (google/crc32c), `narrow<To>(From)` checked conversion
- `src/engine/serialization.cppm`: C++23 module (`bytecask.serialization`) — `ByteWriter`, `ByteReader`, `read_le`, `write_le`
- `src/engine/data_entry.cppm`: C++23 module (`bytecask.data_entry`) — `EntryType`, `EntryHeader`, `DataEntry`, serialization helpers
- `src/engine/data_file.cppm`: C++23 module (`bytecask.data_file`) — `DataFile` POSIX I/O, `Offset`
- `src/engine/hint_entry.cppm`: C++23 module (`bytecask.hint_entry`) — `HintEntry`, `serialize_entry`, `deserialize_entry`
- `src/engine/hint_file.cppm`: C++23 module (`bytecask.hint_file`) — `HintFile`, `OpenForWrite`/`OpenForRead`
- `src/engine/radix_tree.cppm`: C++23 module (`bytecask.radix_tree`) — `PersistentRadixTree<V>`, `RadixTreeIterator<V>`
- `src/engine/concurrency.cppm`: C++23 module (`bytecask.concurrency`) — `SyncGroup`, `BackgroundWorker`
- `src/engine/internals.cppm`: internal partition `bytecask.engine:internals` — `EngineState`, `FileStats`, `KeyDirEntry`, `FileRegistry`, `Key`, `StaleFile`, `VacuumMapping`, `VacuumScanResult`, `RecoveredFile`, `RecoveryResult`, `entry_size`
- `src/engine/bytecask.cppm`: primary interface unit `bytecask.engine` — public types (`Bytes`, `BytesView`, `VacuumOptions`, `Batch`, `WriteOptions`, `ReadOptions`, `Options`, `KeyIterator`, `EntryIterator`) and `Bytecask` class declaration
- `src/engine/bytecask.cpp`: implementation unit `bytecask.engine` — all `Bytecask` method bodies, recovery, vacuum, hint, rotation logic
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
| D10 | **Vacuum**: Two independently testable primitives — `vacuum_compact_file` (rewrite sealed file into a new sealed file, dropping dead entries) and `vacuum_absorb_file` (append live entries to the active file, preserving original LSNs, then delete the old file). `vacuum()` selects a target file above `fragmentation_threshold`, then branches: `vacuum_absorb_file` if `live_bytes <= absorb_threshold` and the data fits in the active file, otherwise `vacuum_compact_file`. Returns `true` if a file was processed, `false` if nothing qualified. No compound paths. All vacuum-related identifiers use a `vacuum_` prefix. One sealed file per `vacuum()` call. Engine continues serving reads and writes. For `vacuum_compact_file`, `write_mu_` is held only for the commit step (I/O writes to a private temp file). For `vacuum_absorb_file`, `write_mu_` is held for the entire scan-copy-sync-commit phase because the active `DataFile` is not thread-safe and concurrent `put`/`del`/`apply_batch` also append to it. `vacuum_commit` itself does not acquire `write_mu_` — the caller is responsible for holding it. Tombstones are always copied (never elided) during partial vacuum. File selection uses `fragmentation >= fragmentation_threshold` computed from incrementally maintained `FileStats` — O(1) per file. Stats are reconstructed during recovery as a side-effect of the hint-file pass. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmssUUUUUU}` using microsecond precision. Gives lexicographic == chronological ordering and avoids the false precision of nanosecond timestamps whose sub-microsecond bits are often zero on Linux. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be discarded with a logged warning. No partial-batch entries enter the key directory. |

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints