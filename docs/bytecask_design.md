# ByteCaskDB Design

## Purpose

ByteCaskDB is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf) implementation with a key architectural difference: it uses an immutable **persistent radix tree** for the Key Directory instead of a Hash Table. This design choice enables efficient **range queries** and **prefix searches** while maintaining Bitcask's core strengths of fast writes and simple recovery. The name "ByteCaskDB" reflects this hybrid approach: **Bitcask algorithm** + **tree index** = **ByteCaskDB**.

**Core design choice**: all keys live in memory at all times. This eliminates disk-bound index lookups entirely — every point read is O(1) from the radix tree, every range scan is a pure in-memory walk. Database size is bounded by available RAM: at ~70 bytes per unique key (key data + metadata + tree structure overhead), 10 million keys require around 700 MB.

This document is the living design reference for the repository. It should track the current implementation state, the intended architecture, and important constraints.

Canonical location: `docs/bytecask_design.md`.

For a first-pass overview focused on the main write loop, see `docs/bytecask_intro.md`.

## Repository Tooling Notes

`scripts/sys-info.sh` reports host hardware characteristics for the current execution context. The memory section is intentionally minimal and privilege-free: it reports the machine's installed RAM capacity from `/proc/meminfo` instead of attempting detailed DIMM inventory. The disk section resolves the filesystem mounted at the script's current working directory via `findmnt --target "$PWD"`, strips any bracketed subvolume suffix from the reported source, and then maps partitions/LVM-style block devices back to their parent physical disk with `lsblk -no PKNAME`. This keeps the output focused on the disk that backs the active repository path instead of listing every block device on the host.

The main CI workflow (`.github/workflows/ci.yml`) includes a dedicated `coverage` job. It runs `scripts/run_coverage.sh` in a Fedora + Clang environment, executes all C++ test binaries (`bytecask_tests`, `radix_tree_memory_tests`, `unordered_view_tests`) with LLVM profile instrumentation, merges profiles with `llvm-profdata`, generates HTML coverage (`coverage/html`), exports `lcov.info`, uploads both artifacts to GitHub Actions, and publishes `lcov.info` to Codecov for repository coverage tracking and badge rendering.

The wheel workflow (`.github/workflows/build-wheels.yml`) publishes the assembled wheel index to GitHub Pages after all platform wheel jobs succeed. The Pages deployment is retried once when the GitHub Pages API reports a transient failure; a second failure still fails the workflow.

Codecov policy is configured in `.codecov.yml`: project coverage status uses the `cpp` flag with an 85% target (1% threshold), patch coverage status uses the same flag with an 80% target (1% threshold), and CI pass is required before Codecov reports success.

The `build-and-test` CI job also exports Catch2 JUnit XML reports for all three C++ test binaries. It publishes them to GitHub Checks via `EnricoMi/publish-unit-test-result-action@v2` for PR-native test summaries, and uploads them to Codecov via `codecov/codecov-action@v5` with `report_type: test_results` (OIDC), enabling the Codecov Test Analytics dashboard (`/tests/new`). Before upload, `scripts/enrich_junit_sources.py` joins each JUnit file with Catch2 `--list-tests --reporter xml` metadata and injects `file`/`line` attributes per testcase so dashboards have source context. Both coverage and test-results uploads set `slug: gustavoamigo/bytecaskdb` explicitly to avoid repository auto-detection issues, include `if: ${{ always() && !cancelled() }}` guards, and pass `${{ secrets.CODECOV_TOKEN }}` as a fallback for environments where OIDC repository mapping is not yet active. The raw XML files are retained as a GitHub Actions artifact (`junit-test-results`) for debugging failed test uploads.

## Goals

- Provide a clean, minimal API surface for key-value operations.
- Support atomic multi-operation batches.
- Support ordered range iteration (enabled by the radix-tree key directory).
- Be idiomatic C++23: no raw pointers, no stringly-typed errors, move-only ownership.

## Non-Goals (for now)

- Multi-writer access, MVCC, or full transaction isolation. ByteCaskDB uses a SWMR model. Snapshot isolation via `snapshot()` and `apply_batch(WritePlan)` is available at Layer 1.
- TTL or expiry.
- Async I/O.
- Background (auto) vacuum. Vacuum is called explicitly by the user.

## Integration: MariaDB Storage Engine

ByteCaskDB is being integrated as a MariaDB pluggable storage engine (`ha_bytecask`). The plugin builds out-of-tree against MariaDB development headers and loads via `INSTALL PLUGIN`. The integration uses a MariaDB-internal L2 Transaction built directly on Layer 1 primitives (`snapshot()` + `apply_batch(WritePlan)`), separate from the public `Transaction` class in `transaction_design.md`. Full design: `docs/mariadb_engine_design.md`.

The plugin consumes the engine through the PIMPL C++ header `include/bytecask.hpp` (typed `bytecask::DB`, `Snapshot`, `WritePlan`, RAII iterators, throwing errors). The implementation lives in `bytecaskdb/bytecask_hpp.cpp` and is compiled into `libbytecask.a`, which the plugin links statically.

### C API / Shared Library Boundary

ByteCaskDB uses C++23 modules internally, which are not portable across compilation unit boundaries when linking external code. To cross this boundary (e.g. the MariaDB plugin), a stable `extern "C"` API is provided:

- **`include/bytecask_c.h`**: flat C header with opaque `bytecask_db_t*` / `bytecask_iter_t*` / `bytecask_snapshot_t*` / `bytecask_write_plan_t*` handles. No C++ types, no module imports. Covers: open/close, put/del/get, forward iteration, snapshots, conditional atomic writes (`apply_batch` via `WritePlan`), and vacuum.
- **`bytecaskdb/bytecask_c.cpp`**: implementation that imports `bytecask` (the C++23 module) and forwards calls through the C API. Compiled into `libbytecask.a`.
- **`xmake.lua` `bytecask` target**: static library combining all engine module objects plus `bytecask_c.cpp`.

Any out-of-tree consumer (not just the MariaDB plugin) should use this C API boundary rather than importing the C++23 modules directly.

### C++ Public Header (`include/bytecask.hpp`)

For C++ consumers that want the full typed API without importing C++23 modules, a PIMPL header is provided:

- **`include/bytecask.hpp`**: standard `#pragma once` header. Defines all public types (`WriteOptions`, `ReadOptions`, `Mode`, `Options`, `Snapshot`, `WritePlan`, `DB`, all iterator types, `DbDegraded`, `DbFollowerMode`) in namespace `bytecask::internal`, with `using` aliases in `namespace bytecask` at the bottom. Depends only on the C++ standard library — no module imports.
- **`bytecaskdb/bytecask_hpp.cpp`**: the only translation unit that `import bytecask;`. Contains all `Impl` struct definitions and out-of-line method bodies. `to_module()`/`from_module()` helpers in an anonymous namespace convert between `bytecask::internal::` (header-defined, plain mangling) and `bytecask::` (module-imported, module-attached mangling) types. `translate_exceptions()` re-throws `bytecask::DbDegraded` and `bytecask::DbFollowerMode` (module-attached) as the header-defined equivalents so callers that include only the header can catch them correctly.

**C++23 module type attachment**: types defined in a module's purview get a `@modulename` suffix in their mangled symbol names. This makes `bytecask::WriteOptions` (from `import bytecask;`) and a nominally identical `WriteOptions` in the header different types at link time. The header resolves this by putting all plain types in `bytecask::internal` namespace (unattached mangling), and using `BYTECASK_HPP_IMPL_MODE` to suppress the `namespace bytecask` aliases in the one TU that imports the module.

### Python Bindings

`bytecaskdb-python/` provides Python bindings via [nanobind](https://github.com/wjakob/nanobind). The extension includes `include/bytecask.hpp` and links against `libbytecask.a` — it does not import the C++23 module directly. The extension exposes DB, Snapshot, WritePlan, all iterator types, and Options. The GIL is released on all I/O paths so multiple Python threads can perform concurrent reads.

**Free-threaded Python (PEP 703)**: the bindings support free-threaded Python 3.13+ (`Py_GIL_DISABLED=1`). The build system auto-detects free-threading via `sysconfig.get_config_var('Py_GIL_DISABLED')` and defines `NB_FREE_THREADED`, which declares `Py_mod_gil = Py_MOD_GIL_NOT_USED` and activates nanobind's locking primitives.

`DataEntry` is constructible from Python (`DataEntry(sequence, entry_type, key, value)`) with bytes-like key/value inputs. This enables network replication transports to deserialize wire payloads back into `DataEntry` objects before calling `ingest()`.

The locking strategy respects the engine's existing thread model:

- **DB reads and snapshot reads are unlocked** — the C++ engine provides lock-free reads via immutable snapshots; adding Python-level locks would destroy read scaling.
- **`PySnapshot::take()`** is protected by `nb::ft_mutex` — the move-out operation is not atomic and concurrent double-move would be UB.
- **Iterator `__next__`** uses `nb::lock_self()` — mutable cursor state must be serialized per-instance.
- **WritePlan mutation methods** use `nb::lock_self()` — the check-then-mutate pattern is not atomic.

Under GIL Python, all nanobind locking primitives (`nb::ft_mutex`, `nb::lock_self()`) are no-ops — zero overhead.

## Design Principles

The design follows these core tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Predictable latency over peak throughput**: Write-path operations must have bounded, predictable latency. Work that can be deferred without compromising correctness must be deferred. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes. This directly influences decisions like deferring hint file writes out of the rotation path.
4. **Performance**: Optimizations require a real use case. Without one, correctness and simplicity take priority.

## System Architecture

### Key Directory

ByteCaskDB uses `PersistentRadixTree<KeyDirEntry>` as the in-memory key directory. All keys reside in memory at all times.

The key directory is a persistent (immutable) radix tree with path-compressed nodes, intrusive reference counting, and structural sharing across versions. It provides O(k) get/set/erase (where k = key length), bidirectional ordered iteration via DFS, `lower_bound()` / `upper_bound()` for range queries, `rbegin()` / `rend()` for safe reverse iteration via `ReverseRadixTreeIterator`, and a `transient()` / `persistent()` API for batch mutations. `RadixTreeIterator` satisfies `std::bidirectional_iterator` — `operator--` walks the tree in reverse with O(1) amortized cost per step. `ReverseRadixTreeIterator` wraps `RadixTreeIterator` with pre-decrement at construction so `operator*` always returns a span into a live iterator buffer — no dangling references. It also tracks a `past_rend_` flag so `++rend()` is a no-op (never wraps back to the last element) and `rend().base() == begin()` holds (standard reverse_iterator semantics). Implemented in `bytecask.radix_tree` (`bytecaskdb/radix_tree.cppm`).

`TransientRadixTree` is intentionally single-use. `persistent() &&` retires the builder, and any later read or write on a consumed or moved-from transient throws `std::logic_error` in release builds instead of relying on debug-only assertions.

`Node::release()` uses iterative tail-release: when the last child's refcount drops to zero, the loop continues with that child instead of recursing through `~IntrusivePtr`. This avoids O(depth) recursive destructor frames for chains of single-child nodes — the dominant pattern in compressed radix trees. Profiling (perf, MergeOverlapping/100K) measured the recursive `~IntrusivePtr` cascade at 29% of total merge time; the iterative version reduces MergeOverlapping by ~6% and parallel recovery at 16T by ~13%.

`TransientRadixTree::upsert(key, val, should_replace)` performs a single-traversal conditional insert-or-replace: it walks from root to leaf once, inserting if the key is absent, or replacing if `should_replace(existing, incoming)` returns true. Returns the displaced old value when a replacement occurs, enabling callers to track side-effects (e.g. file_stats adjustment). Used by parallel recovery's `recovery_build_from_hints` to eliminate the separate `get()` + `set()` dual traversal that profiling (perf, Recovery/Parallel/16) showed consuming ~49% of recovery time.

Child storage remains behind the `Node` / `InternalNode` split. `child_count`, `child_at`, `find_child`, and the mutation helpers now funnel through checked accessors that return safe defaults for leaf nodes instead of repeating flag checks plus open-coded downcasts at each call site.

Keys are stored as byte sequences within the radix tree's prefix-compressed nodes. The radix tree API accepts `std::span<const std::byte>` for all key parameters — no intermediate `Key` wrapper is needed for internal operations. The public `Key` class (backed by `std::vector<std::byte>`) is retained for the external iterator API (`KeyIterator`, `EntryIterator`) and for the recovery tombstone tracking map. `Key` provides `operator<=>` (lexicographic over raw byte values) and `begin()`/`end()`/`size()` accessors. Keys have a hard upper bound of 65 535 bytes (the `u16 key_size` field in the data file header).

**Historical note**: the original key directory used `PersistentOrderedMap<Key, KeyDirEntry>`, backed by `immer::flex_vector<Entry>`. The radix tree replacement (BC-030) delivers O(k) lookups vs O(n log n) binary search, lower memory overhead via prefix compression and intrusive refcounting, and faster batch mutations via the transient API's in-place path copying. `PersistentOrderedMap` is retained in the codebase for benchmarking purposes (`benchmarks/map_bench.cpp`).

### Size Limits

The on-disk entry header imposes hard ceilings: keys are limited to 65,535 bytes (u16 `key_size` field) and values to 4,294,967,295 bytes (u32 `value_size` field). These cannot be raised without a format change.

The in-memory `KeyDirEntry` is bit-packed into two 64-bit words (16 bytes) to reduce radix tree node size. Field limits enforced by the packing:

| Field | Bits | Max value |
|---|---|---|
| sequence | 48 | 281 trillion (~8.9 years at 1M ops/sec) |
| file_id | 20 | 1,048,575 (split across word0 and word1) |
| file_offset | 32 | 4 GiB per file |
| value_size | 28 | 256 MiB per value |

These limits are validated at construction time (`KeyDirEntry::make`). The on-disk format is unaffected — packing is in-memory only. All field access goes through accessor methods so the internal layout can be changed without touching call sites.

Configurable limits are enforced at the API boundary — before any data is copied into a `WritePlan` or written to disk:

| Limit | Default | Hard ceiling | Rationale |
|-------|---------|-------------|-----------|
| `Options::max_key_bytes` | 4,096 (4 KiB) | 65,535 | Keys live in memory (radix tree). Large keys bloat RAM and slow traversal. |
| `Options::max_value_bytes` | 4,194,304 (4 MiB) | 4,294,967,295 | Values go to disk. Oversized values cause pathological file rotation. |

Violations throw `std::invalid_argument`. `WritePlan` carries the limits from `Snapshot` (which inherits them from `DB`) or uses the defaults when constructed without a snapshot. `DB::put`, `DB::del`, `DB::del_range`, and `DB::ingest` all validate before proceeding.

### Concurrency Model

ByteCaskDB follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time.
- Multiple readers may operate concurrently.
- MVCC and snapshot isolation are not supported.

#### Directory lock

A single process may hold a database directory open at a time. `DB::open()` acquires an exclusive advisory lock (`flock(LOCK_EX | LOCK_NB)`) on `dir/.lock` before recovery begins. A second process attempting to open the same directory receives a `std::system_error`. The lock is released when the `DB` is destroyed. The `.lock` file remains on disk as a harmless sentinel.

#### Concurrency strategy

ByteCaskDB's read path is designed so that **readers never acquire the write mutex**. The strategy combines two ideas:

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
  │  state_         atomic<shared_ptr<EngineState>>             │  ← writer stores,
  │                                                             │    readers load (no write_mu_)
  └─────────────────────────────────────────────────────────────┘

  EngineState  (heap, reference-counted, never mutated in place)
  ┌──────────────────────────────────────────────────┐
  │  key_dir         PersistentRadixTree<KeyDirEntry> │  key → (file_id, offset, seq)
  │  files           shared_ptr<FileMap>              │  file_id → open DataFile fd
  │  file_stats      map<uint32_t, FileStats>         │  per-file live/total bytes
  │  active_file_id  uint32_t                         │
  │  next_file_id    uint32_t                         │  writer-only; monotonic file counter
  │  next_seq         uint64_t                         │  writer-only; monotonic sequence counter
  │  durable_seq      uint64_t                         │  highest sequence confirmed by fdatasync
  └──────────────────────────────────────────────────┘
```

`EngineState` bundles all engine state into a single immutable value. Writers never mutate an `EngineState` in place; they create a `TransientEngineState` working copy, apply mutations, and publish the result via `persistent()`. The old state stays alive as long as any reader holds a `shared_ptr` reference.

`file_stats` lives inside `EngineState` so that the transient/persistent discipline covers all mutable state uniformly. The map is shallow-copied into `TransientEngineState` on each write — acceptable because the number of open files is small (typically < 100).

##### Write path — group commit

All writes (`put`, `del`, `apply_batch`) route through a single coordinator: `DB::apply_batch`. The public methods `put` and `del` are thin wrappers that build a `WritePlan` and delegate. Each write is packaged into an `EngineSlot` and submitted to either `SoloWriter` (single-slot, no batching) or `WriteGroup` (leader-applies-all batching).

`EngineSlot::result` is `std::optional<CommitResult>` (BC-231): `execute_slot` sets it to `nullopt` on a validation/conflict failure, or to `CommitResult{.sequence = entries.back().sequence}` on success (`{.sequence = 0}` for an empty or guard-only plan). `durable` is filled in at the end of Phase 3 once the batch's sync/rotation outcome is known: `slot->result->durable = (t.durable_seq() >= slot->result->sequence)` for every committed slot — this naturally covers group-coalesced and rotation syncs. A batch made of only empty/guard-only plans produces no entries at all and returns from `execute_slots` right after Phase 1, before Phase 3 ever runs; that early-return path explicitly sets `durable = true` on every committed slot, since a `{sequence = 0}` result has nothing to wait for and needed no I/O. See `docs/commit_result_api_design.md` for the full `CommitResult` contract, including the C/Python/Node bindings.

**Routing**: a write goes to `SoloWriter` when `WriteOptions::solo` is set (benchmarking) or when the plan's byte size exceeds `kGroupWriteMaxBytes` (256 KiB). All other writes go to `WriteGroup`.

**WriteGroup**: the first writer to arrive becomes the leader, drains the queue of pending slots, and executes them all under one lock hold. N concurrent sync writes share a single `fdatasync` instead of paying N separate calls. This is the dominant performance win — fdatasync is ~2 ms; pwritev is ~microseconds.

**SoloWriter**: same `submit(Slot&)` interface, no internal batching. The executor acquires `write_mu_` and processes the single slot. Provides a uniform interface for routing.

Both executors follow a three-phase pattern:

```
  Phase 1: pure in-memory (per-slot, sequential)
  ──────────────────────────────────────────────
  For each slot:
    1. validate_preconditions(plan)
       └─ point guards, range guards, W-W checks — pure reads on transient
    2. prepare_write(plan) → vector<DataEntryView>
       └─ assigns sequences, inserts BulkBegin/End markers — pure computation
    3. pre-compute offsets from running_offset
       └─ entry sizes are deterministic (header + key + value + CRC)
    4. apply_writes(plan, offsets)
       └─ updates key_dir, file_stats, sequence — pure in-memory mutations
    5. collect entries into all_entries

  Phase 2: one I/O call
  ─────────────────────
  file.append_entries(all_entries)
    └─ single pwritev for all entries across all slots in the batch

  Phase 3: sync / rotate / publish
  ────────────────────────────────
  1. if rotation needed: sync + seal + rotate
  2. if any_sync: fdatasync            ← amortized across all slots
  3. state_.store(persistent())        ← atomically publishes new snapshot
```

The sequential per-slot processing in Phase 1 preserves the serial correctness model exactly. Each slot's `validate_preconditions` sees the key_dir after all previous slots' writes. A `del` with `ensure_present` in slot 2 correctly sees slot 1's `put`.

##### Range deletion (`del_range`)

`del_range(opts, from, to)` deletes all keys in `[from, to)` with a single data file append. The on-disk entry reuses the standard layout: `entry_type = RangeDel (0x05)`, `key = start_key`, `value = end_key`. No new header fields.

On the write path, `prepare_write` emits one `DataEntryView` per range delete. `apply_writes` iterates the key directory from `lower_bound(from)` to the first key `>= to`, decrements `live_bytes` on each affected file, and erases the keys. The RangeDel entry itself contributes only to `total_bytes` (same as point Delete — tombstones are not live).

Range deletes are supported on `DB::del_range` and `WritePlan::del_range`. Inside a batch, they are framed by `BulkBegin`/`BulkEnd` like other operations. Existing guards (`ensure_unchanged`, `ensure_range_unchanged`, implicit W-W check) detect concurrent range deletes without changes — erased keys produce sequence mismatches.

##### TransientEngineState

`TransientEngineState` is the mutable working copy for all write-path state transitions. It follows the same `transient()` / `persistent()` pattern as `PersistentRadixTree` / `TransientRadixTree`.

The coordinator (step 5 above) only performs IO. It never touches `key_dir`, `file_stats`, or sequence directly. The transient owns all state logic:

- `validate_preconditions(plan)` — reads snapshot + current state, returns bool
- `prepare_write(plan)` — assigns sequences, returns `vector<DataEntryView>` for the IO loop
- `apply_writes(plan, io_result)` — updates key_dir, file_stats, advances sequence
- `apply_rotate_file(new_file)` — registers new file, updates active_file_id
- `apply_vacuum(old_file_id, scan, new_file)` — remaps keys, updates registry + stats
- `apply_sync(batch_max_seq)` — records that fdatasync confirmed durability up to batch_max_seq
- `persistent() &&` — produces `shared_ptr<EngineState>` for publishing

**Three-phase discipline**: Phase 1 is pure in-memory (validate, prepare, compute offsets, apply_writes). Phase 2 is a single IO call. Phase 3 is sync/rotate/publish. If IO throws, the transient's state mutations are already applied but never published — the transient is discarded and the engine degrades.

##### Rotate on batch failure

If `append()` throws mid-batch after `BulkBegin` has been written, the active file contains an orphaned `BulkBegin` with no matching `BulkEnd`. Without intervention, subsequent writes to the same file extend the region that `flush_hints_for` (and `vacuum_scan_and_copy`) treat as part of the incomplete batch — those entries would be silently discarded on recovery.

The fix: the IO loop (step 5) is wrapped in `try/catch`. When the batch is multi-entry and IO fails, the catch block attempts to sync the tainted file and rotate to a fresh active file. If the isolation sync fails, the orphaned batch markers may not be durable — a crash could leave an unresolvable `BulkBegin` — so the engine is poisoned immediately instead of proceeding. If sync succeeds but the isolation rotation fails, the engine is also poisoned (see below). When isolation succeeds, the rotated state is published immediately so subsequent writes go to the clean file, and the exception is rethrown to the caller.

##### Degraded state (`DbDegraded`) and `resume()`

If the isolation sync or rotation after an orphaned `BulkBegin` fails (e.g. `fdatasync` error on the tainted file, or filesystem error creating the new data file), the active file may retain the orphaned marker in a non-durable or inconsistent state. Any subsequent write to that file would appear to succeed in-process but be silently discarded by recovery.

When this happens, the engine calls `deem_as_degraded(reason)` with a diagnostic string identifying the failed file, then rethrows the original exception. From that point:

- **Writes blocked**: `put`, `del`, `apply_batch`, and `vacuum` throw `DbDegraded` immediately. The reason string is available via `degraded_reason()`.
- **Reads available**: `get`, `contains_key`, `snapshot`, `iter_from`, `keys_from`, `riter_from`, `rkeys_from` continue to work. The in-memory state was correctly rolled back (the transient was never persisted), so reads reflect the last successfully committed state.
- **Recovery via `resume()`**: the degraded flag is in-memory only. The service calls `resume()` to attempt in-process recovery without a restart. `resume()` runs a universal recovery process:
  1. Acquires the write lock.
  2. Scans the active file using `CommittedEntryIterator` to find the last valid committed offset. Orphaned `BulkBegin` batches are excluded — if a `BulkBegin` has no matching `BulkEnd`, `committed_offset` is reset to before the batch start.
  3. Replays valid committed entries into the key directory using sequence-wins resolution: for each Put whose sequence exceeds the current key_dir entry (or the key is absent), update key_dir and file_stats; for each Delete whose sequence exceeds the current entry, erase the key. This recovers entries that were written to the data file but never published to EngineState (e.g. sync-failure paths where only next_seq was advanced, or degraded-state transitions that occurred between IO and state publication). Also advances `next_seq` past the highest sequence seen on disk.
  4. Calls `ftruncate` to remove garbage bytes and orphaned batch markers up to `valid_offset`.
  5. Calls `fdatasync` to persist the truncation.
  6. Seals the active file and dispatches hint generation (both idempotent).
  7. Creates a new active file and publishes the new engine state.
  8. Clears the `degraded_` flag.
  If any step throws, the engine stays degraded and the caller may retry. Recovery is idempotent.

Implementation: `degraded_` is an `atomic<bool>` (release on write, acquire on read). The non-atomic `degraded_reason_` string is safely published via the release/acquire pair — the string is written before `degraded_.store(true, release)` and read after `degraded_.load(acquire)`.

##### Runtime invariant enforcement

The engine validates structural invariants at runtime before publishing state, not just in tests. `store_state` compares old and new `EngineState` on every publication: `next_seq`, `active_file_id`, `next_file_id`, and `durable_seq` must never regress. On violation the engine degrades (nothing published, writes blocked, reads remain available). Cost: four integer comparisons per write — unmeasurable against `pwritev` + `fdatasync`. Debug builds add a full `next_seq > max(key_dir sequences)` walk. When `durable_seq` advances, `store_state` notifies `durable_cv_` — the condvar used by `durable_sequence(min_sequence, timeout)` for long-poll.

On cold paths (`DB::open()`, `resume()`), `validate_state_consistency` runs the full O(n) structural check: active file in registry, no dangling file references, `next_seq` ahead of all sequences, `file_stats` covers all files, `live_bytes` matches `key_dir`. On violation it throws — the DB does not open or `resume()` fails.

##### Partial write detection (tainted file)

If `pwritev` returns a short write (0 < written < total), the append method sets a `tainted_` flag before throwing. A tainted file has bytes on disk that `offset_` does not account for. The next `pwritev` writes at the tracked `offset_` — past the partial data — so subsequent append offsets would be wrong if `offset_` were advanced.

For multi-entry batches this is safe: the isolation rotation moves to a new file, abandoning the tainted one. For single-entry writes, the `apply_batch` catch block checks `file.is_tainted()` and degrades the DB if set. `resume()` then truncates the partial entry and restores a clean state.

##### Durable sequence tracking

`durable_seq` is a field on `EngineState` that tracks the highest sequence number confirmed by `fdatasync`. Updated via `TransientEngineState::apply_sync(batch_max_seq)` — a named state transition, same as `apply_writes`, `apply_rotate_file`, etc.

In `execute_slots`, `batch_max_seq = next_seq - 1` is computed once after Phase 1. At each successful `file.sync()`, the transient calls `apply_sync(batch_max_seq)`. If sync fails, `apply_sync` is never called — the transient carries forward the previous `durable_seq` unchanged. The monotonicity guard in `apply_sync` ensures idempotent calls (rotation sync + commit sync on the same batch).

`durable_sequence(min_sequence, timeout)` exposes `durable_seq` to callers (renamed from `current_sequence` — BC-231, since "current" was ambiguous between the highest *allocated* and highest *durable* sequence). It is the single sequence primitive: `min_sequence = 0`, an already-reached target, or a nonpositive `timeout` all return the current watermark immediately without blocking. Otherwise it blocks on `durable_cv_` (notified by `store_state` when `durable_seq` advances) until `durable_seq >= min_sequence` or the timeout expires, then returns the current watermark. The condvar notification is centralized in `store_state` — one place, one check. This single target-based primitive covers polling (`min_sequence = 0`), the replication wake-up (`min_sequence = follower.durable_sequence() + 1`), and RYOW waits (`min_sequence = result.sequence` from a `CommitResult`) — see `docs/commit_result_api_design.md` and `docs/replication_primitives_design.md`.

After recovery (`DB::open`, `resume`), `durable_seq` is set to `next_seq - 1` because all recovered entries were previously synced.

##### Post-write rotation failure

After all appends succeed and mutations are applied, the engine may rotate the active file if it exceeds the size threshold. Rotation syncs the file, seals it, and creates a new active file. Two distinct failures can occur:

- **Sync fails before seal**: the file is not sealed. The engine captures the exception, advances `next_seq` past consumed sequences, and rethrows without publishing key changes. The DB is not degraded — the next write retries rotation or continues appending.
- **File creation fails after seal**: `rotate_active_file` calls `seal()` before creating the new file. If creation fails, the active file is sealed and cannot accept further appends. The engine degrades the DB and publishes state. `resume()` creates a fresh active file.

##### Sync failure: advance sequence, discard key changes

If `fdatasync` fails (step 8 or the rotation sync at step 7a), the write is not
confirmed durable. Key-directory changes are not published — the written key is not
visible to callers. `next_seq` is advanced past the consumed sequence numbers to
prevent sequence reuse for bytes now in the page cache. The caller receives the
exception and must retry. This matches the contract of every other peer engine.

##### Durability before visibility

`state_.store()` happens **after** `fdatasync` in all cases. A write is never
visible to readers until it is durable on disk (or explicitly chosen as
`sync=false` by the caller).

Consequences:
- `write_mu_` is held across the entire write including `fdatasync`. Concurrent writers are serialised.
- `sync = false` writes still have no durability guarantee, but visibility is immediate after the write completes under the lock.
- After recovery, in-memory state is consistent with what is on disk.

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

`ReadOptions` controls consistency behaviour for read operations (`get`, `contains_key`). ByteCaskDB provides two read consistency modes, controlled by a single field:

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

This is analogous to RocksDB's built-in `SuperVersion` thread-local caching, which always provides session consistency with no user-facing knob. ByteCaskDB adds bounded staleness as an opt-in for write-heavy workloads where read throughput matters more than freshness.

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

`engine_bench` compares ByteCaskDB against LevelDB and RocksDB across Put, Get, Del, Range50, Mixed, MixedBatch, PutMT, and MixedMT benchmarks at both NoSync and Sync durability levels. RocksDB compression is disabled (`kNoCompression`) and values are 1 KiB of random (incompressible) bytes so neither LevelDB nor RocksDB gains an advantage from Snappy/block-cache effects.

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
- **Sequence-disjoint files**: vacuum must preserve the invariant that all data files have non-overlapping sequence ranges. Compacted files maintain disjoint sequence ranges from other files.

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
  std::uint64_t min_sequence{0};  // lowest sequence in this file (0 = no entries)
  std::uint64_t max_sequence{0};  // highest sequence in this file (0 = no entries)
};
```

`file_stats` is a `std::map<uint32_t, FileStats>` inside `EngineState`. It is copied into `TransientEngineState` on each write and updated as part of the state transition. This keeps all mutable state under the transient/persistent discipline.

The helper `entry_size(key_size, value_size)` returns `kHeaderSize + key_size + value_size + kCrcSize` and is used everywhere stats are updated.

##### Write-path updates

All stats updates happen inside `TransientEngineState::apply_writes`:

- **On Put**: if the key already exists (overwrite), subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats[old_entry.file_id].live_bytes`. Add `entry_size(key.size(), value.size())` to `file_stats[active_file_id].live_bytes` and to `.total_bytes`.
- **On Del**: if the key exists, subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats[old_entry.file_id].live_bytes`. Add the tombstone size (`kHeaderSize + key.size() + kCrcSize`) to `file_stats[active_file_id].total_bytes`. The tombstone is never added to `live_bytes` — tombstones are never referenced by the key directory.
- **BulkBegin/BulkEnd markers**: each add `kHeaderSize + kCrcSize` to the active file's `total_bytes` only — they are never referenced by the key directory.
- **On rotation**: `apply_rotate_file` inserts `FileStats{0, 0}` for the new active file.

At vacuum time fragmentation is an O(1) integer division per file — no scanning, no I/O, no additional lock contention.

The active file's `live_bytes` may be non-zero (it holds the current live writes), but the active file is never a vacuum candidate, so its fragmentation is never evaluated.

##### Recovery stats reconstruction

`file_stats` must be rebuilt on startup. Both `total_bytes` and `live_bytes` are reconstructed without scanning data files or traversing the key directory:

- **`total_bytes`**: computed via `std::filesystem::file_size(path)` per sealed file in `open_and_prepare_files()`. Exact for append-only files. O(1) per file, no I/O beyond a `stat` call.
- **`live_bytes`**: reconstructed as a side-effect of the existing hint-file recovery pass. The hint entry carries `key_size` (from `key.size()`) and `value_size`, so `entry_size(key_size, value_size)` is computable without touching the data file.

The displacement logic mirrors write-path updates:

```
for each hint entry h (processed in arbitrary file order, sequence wins):
  if h.type == Put and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    file_stats[h.file_id].live_bytes += entry_size(h.key_size, h.value_size)

  if h.type == Delete and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    // tombstone never enters live_bytes
```

Processing order across files does not matter — the canonical key-ownership comparator always picks the same winner regardless of merge order, so `live_bytes` converges to the right values.

##### Canonical key-ownership comparator

Sequence numbers are unique per logical write. When two `KeyDirEntry` values claim the same key, the one with the higher sequence wins. If two entries share the same sequence number, they must point to the same physical record (`file_id`, `file_offset`); if they don't, the database is corrupt and recovery throws `std::runtime_error`. This comparator is commutative, so merge results are independent of worker count, completion order, or file iteration order. Both serial and parallel recovery use this comparator.

##### Sequence bounds

`min_sequence` and `max_sequence` track the range of sequence numbers written to each file. They enable `changes_since` to skip irrelevant files during replication and are maintained alongside `live_bytes`/`total_bytes` in every write path:

- **`apply_writes`**: captures the batch's start and end sequence once per write batch.
- **Vacuum**: `vacuum_scan_and_copy` tracks sequences of all copied entries (including batch markers). `apply_vacuum` propagates bounds to the compacted file.
- **`apply_resume`**: resets bounds on truncation, rebuilds from committed entries.
- **Recovery**: both serial and parallel paths track min/max per file during hint replay.

Invariant: both are zero (no entries yet) or both are non-zero with `min_sequence <= max_sequence`. Enforced by `validate_state_consistency`.

**Parallel recovery**: each worker tracks bounds for its own files. Bounds carry through the Phase 3 merge unchanged (file IDs are disjoint across workers). Phase 4 does not modify sequence bounds.: `RecoveryResult` includes a `file_stats` map alongside `key_dir`, `tombstones`, and `max_seq`. Each worker builds its own `file_stats` during Phase 2 using the algorithm above. During Phase 3 sequential accumulator merge, `file_stats` maps are unioned (file IDs are disjoint across workers due to round-robin). The merge does **not** recompute `live_bytes` — instead, a single `live_bytes` pass runs once after the final merge in Phase 4 (assembly), iterating the fully-merged tree exactly once.

#### Vacuum primitives

Vacuum uses two self-contained, independently testable paths. The `vacuum()` caller picks exactly one per target file based on whether the file has live entries.

##### `vacuum_compact_file(file_id)` — rewrite a sealed file, dropping dead entries

Used when the file's live data is too large to fit into the active file. Produces a new, sealed, compacted file.

1. **Snapshot the key directory** — call `state_.load()` to obtain the current `EngineState`. This is the authoritative view of which entries are live.
2. **Rewrite the target file** — open a new data file at a `.data.tmp` path for writing (new timestamp stem, same directory). Scan the old data file entry by entry using `CommittedEntryIterator` (see below). For each emitted entry:
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
6. **Unlink old file** — the old file is removed from the registry and its `.data` and `.hint` files are unlinked from the filesystem immediately after the commit. Existing readers continue via their open file descriptors — POSIX guarantees that `pread` on an unlinked file succeeds as long as the fd is open. Disk blocks are freed when the last `shared_ptr<DataFile>` is destroyed, closing the fd.

##### `vacuum_remove_file(file_id)` — delete a file with zero live entries

Used when `live_bytes == 0` — the file contains only dead entries (tombstones or superseded puts). No I/O is required.

1. **Snapshot the key directory** — call `state_.load()` to obtain the current `EngineState`.
2. **Commit the removal** under `write_mu_`:
   a. Remove the file from the registry.
   b. Remove the file's entry from `file_stats_`.
   c. Publish the updated `EngineState`.
3. **Unlink the files** — remove the `.data` and `.hint` files from the filesystem.

This is a pure metadata operation — no scanning, no copying, no syncing. The file contained no live data, so removing it has no effect on readable keys.

#### Committed entry scanning

`vacuum_compact_file` uses `CommittedEntryIterator` (`scan_committed`) when reading the sealed data file. The iterator yields entries one at a time, including `BulkBegin`/`BulkEnd` markers. Internally, entries between `BulkBegin` and `BulkEnd` are buffered and only yielded when `BulkEnd` is reached. If the file ends mid-batch (crash during `apply_batch`), the buffered entries are silently discarded — they were never committed.

**Why**: without committed-entry filtering, vacuum would copy uncommitted entries (including tombstones from incomplete batches) into the output. Those tombstones could incorrectly shadow live Puts in other files, causing data loss on recovery. This matches `flush_hints_for`'s hint generation.

(`vacuum_remove_file` performs no scanning — it simply removes file metadata.)

#### Crash safety

`vacuum_compact_file` writes the new data file to `.data.tmp` and renames it atomically to `.data` after `fdatasync`. If the engine crashes mid-write, recovery ignores `.data.tmp` files (it only processes `.data` extensions) and cleans them up in `open_and_prepare_files`. The hint file uses the same `.hint.tmp` → `.hint` protocol.

`vacuum_remove_file` has no crash safety concerns — it performs no I/O, only removes file references from engine state.

#### Vacuum caller (`vacuum()`)

The public `vacuum()` method orchestrates file selection and dispatches to exactly one primitive:

1. **Acquire `vacuum_mu_`** — prevents two `vacuum()` calls from running concurrently.
2. **Select a target file** — copy `file_stats_` under a brief `write_mu_` acquisition (O(sealed files), then release). Iterate sealed files, compute `fragmentation = 1 − live_bytes / total_bytes` (O(1) per file, no I/O), pick the highest-fragmentation sealed file above `fragmentation_threshold`. If no file qualifies, return immediately.
3. **Branch**:
   - If `file_stats_[target].live_bytes == 0` → call `vacuum_remove_file(target)` (fast path, no I/O).
   - Otherwise → call `vacuum_compact_file(target)` (sealed→sealed compaction).
4. **Release `vacuum_mu_`**.

#### Tombstone handling

Tombstone (Delete) entries record that a key was explicitly removed. They must be copied to the compacted output during partial vacuum.

**Why**: if an older data file (not being compacted in this cycle) contains a Put for the same key, recovery would see that Put and resurrect the key — unless the Delete entry survives in some file to overrule it. Preserving the tombstone prevents this.

A deleted key is not present in the key directory, so no key-directory update is needed for tombstones — they are pure pass-through copies. The original sequence number is preserved verbatim so recovery's sequence comparison still works correctly on the compacted file.

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
| 0      | 8    | Sequence   | u64 LE | Monotonic sequence number                      |
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

### Sequence Number

The sequence is a **globally monotonic** counter across all data files and all engine sessions — not a per-file counter. This is a correctness invariant: recovery determines which of two entries for the same key is fresher by comparing sequences from potentially different files. Any per-file counter reset would silently allow stale data to overwrite live data.

- The engine owns and increments the global sequence. `DataFile` is a passive consumer: the caller passes `sequence` to every `append` call.
- `DataFile` does not start its own counter; it does not know or care what value the sequence starts at.
- On startup, the engine scans all hint files and the active data file to find `max_seq`, then seeds the new active `DataFile` at `max_seq + 1`.

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

`DataFile` (in `bytecask.data_file` module) is the abstract base for all data file types. It defines the read interface (`scan`, `read_value`, `read_entry`, `read_entry_unverified`, `size`). Concrete writable implementations extend `WritableDataFile`, which adds `append_entry`, `append_entries`, `sync`, and `truncate` as pure virtual methods. Two concrete writable implementations are provided:

- **`WritableMmapDataFile`**: Pre-allocates the file to the rotation threshold via `ftruncate`, then maps it with `mmap(PROT_READ, MAP_SHARED)`. Reads within the mapped region are zero-syscall memcpy from the mmap region. Reads beyond the mmap region (rare: overflow past pre-allocated size) fall back to `pread`. Writes go through `pwritev` at a tracked `offset_`. MAP_SHARED is required so that `pwritev` writes through the fd are visible to mmap readers.
- **`WritablePosixDataFile`**: Pure `pread`-based reads, `pwritev` writes. Uses `fallocate(FALLOC_FL_KEEP_SIZE)` in 4 MiB chunks for block preallocation. This is the fallback for platforms without mmap support (Emscripten/WASM).

The factory function `openDataFileForWrite(path, capacity, use_mmap)` selects the implementation. Both implementations open the file with `O_RDWR | O_CREAT | O_CLOEXEC` — no `O_APPEND`, since pre-allocated files require positioned writes.

- **`append_entry(sequence, entry_type, key, value) -> Offset`**: Serializes a new entry with the given sequence number and `EntryType`, writes it via `pwritev()` at the tracked `offset_`, and returns the byte offset where the entry starts. `BulkBegin`/`BulkEnd` entries pass empty key and value spans. Does **not** guarantee durability on its own. The 15-byte header and 4-byte CRC are serialized into a fixed member buffer (`hdr_crc_buf_`); the key and value spans are passed directly as iovecs — no heap allocation and no copy of key/value data occurs on the write path.
- **`sync()`**: Calls `::fdatasync()` to flush all pending writes to physical storage. Must be called explicitly to guarantee crash-safety. Decoupled from `append_entry()` to enable Group Commit: callers can batch multiple `append_entry()` calls before a single `sync()`.
- **`read_entry(offset, key_size, value_size, io_buf)`**: Single-pread read primitive. Resizes `io_buf` (reusing existing capacity) and preads the full entry into it. Callers then pass `io_buf` to `deserialize_entry()` (recovery, scan) depending on what they need. `scan()` uses this internally after its header pread.
- **`read_value(offset, key_size, value_size, io_buf, out)`**: High-level read primitive. Calls `read_entry` then `extract_value_into` to pread, CRC-verify, and extract only the value into `out`. Both `io_buf` (scratch) and `out` reuse existing capacity across calls. Used by `Bytecask::get()` and `EntryIterator`.
- Key and value are accepted as `std::span<const std::byte>` for binary safety.

### I/O Back-end Rationale

- **POSIX over `std::ofstream`**: `::pwritev()` issues a single syscall per entry using scatter-gather I/O at a specified offset, skipping the buffering layers and locale state overhead of C++ streams.
- **`fdatasync` over `fflush`/`flush()`**: `fdatasync` syncs data to physical media while skipping inode metadata updates (access time etc.), making it faster than `fsync` for a pure append-only log.
- **Group Commit pattern**: Separating `append_entry()` (writes to page cache) from `sync()` (forces to disk) lets future code batch hundreds of writes before a single expensive `fdatasync`, which is the primary lever for high write throughput on NVMe hardware (see `io_uring` paper reference).
- **Zero-copy write path**: `append_entry()` builds only the 15-byte header and 4-byte CRC in a fixed member buffer, then calls `::pwritev()` with four iovecs — `[header(15), key, value, crc(4)]`. The kernel gathers the scattered buffers into one atomic write without any intermediate heap allocation or memcpy of key/value data. For 1 KiB values this eliminates ~250 MB/s of unnecessary copying at 244k puts/s.
- **Block preallocation**: On Linux, `WritablePosixDataFile` preallocates disk blocks in 4 MiB chunks using `fallocate(FALLOC_FL_KEEP_SIZE)` before each `pwritev()`. This eliminates per-write filesystem extent-allocation overhead without changing the file's logical size — `offset_`, scanning, and recovery are all unaffected. The call is a no-op on filesystems that do not support it (tmpfs, NFS). On non-Linux platforms (macOS) the feature is compiled out. `WritableMmapDataFile` pre-allocates the full file via `ftruncate` instead.

### Source Code Module Architecture

We use fine-grained C++20 modules:
- `bytecask.util`: CRC-32C accumulator (`Crc32`, backed by google/crc32c) and checked `narrow<To>(From)` conversion.
- `bytecask.serialization`: Core serialization primitives (`ByteWriter`, `ByteReader`, `read_le`, `write_le`) and re-exports `bytecask.util`.
- `bytecask.data_entry`: Logical entry definition, `write_header_and_crc()` (fills a fixed 19-byte buffer with LE header + CRC for zero-copy I/O), `serialize_entry()` (complete in-memory entry for tests/recovery), `parse_header_and_verify()` / `deserialize_entry()` / `extract_value_into()` — CRC verification is factored into `parse_header_and_verify()` and shared by both extraction functions.
- `bytecask.data_file`: Disk I/O — `DataFile` (abstract base), `WritableDataFile` (pure interface), `WritableMmapDataFile`, `WritablePosixDataFile`, `ReadOnlyMmapDataFile`, `ReadOnlyPosixDataFile`, `Offset`.
- `bytecask.hint_entry`: `HintEntry`, `serialize_entry()`, `deserialize_entry()` — symmetric read/write for hint entries.
- `bytecask.hint_file`: Hint file writer and reader (`HintFile`).
- `bytecask.persistent_ordered_map`: Immutable sorted map (`PersistentOrderedMap<K,V>`, `OrderedMapTransient<K,V>`) backed by `immer::flex_vector`; retained for benchmarking.
- `bytecask.radix_tree`: Persistent radix tree (`PersistentRadixTree<V>`, `TransientRadixTree<V>`, `RadixTreeIterator<V>`) with path compression and intrusive refcounting; used as the key directory.
- `bytecask.engine`: Public engine API (`Bytecask`, `EngineState`, `KeyIterator`, `EntryIterator`, `FileRegistry`, type aliases).

### Current scope boundaries

- `EntryType` is written and read back on `DataFile::read()`; atomic bulk semantics are enforced at a layer above `DataFile`.
- No file rotation or size limits.
- No read path for the key directory — append-only for now.

### DataFile fd mode after rotation

Writable data files open with `O_RDWR | O_CREAT | O_CLOEXEC`. After a data file is rotated it is logically immutable — no new entries should be appended. The engine enforces this at a higher level; the fd mode is not downgraded to `O_RDONLY` after rotation. Rationale:

- `DataFile` is an internal class; the engine exclusively controls when `append()` is called.
- Re-opening the fd purely for semantic enforcement adds syscall overhead and complexity without improving correctness for the production path.
- A `sealed_` flag with `assert(!sealed_)` at the top of `append()` is sufficient: it catches programming errors in debug builds at zero production cost.

Contrast with `HintFile`, which uses `OpenForWrite` / `OpenForRead` factory functions. That split models externally visible, non-overlapping lifecycles at different call sites: one site writes during `flush_hints()`, a completely separate site reads during recovery. Encoding that distinction in the type prevents mixing them up. `DataFile` has no equivalent external semantic split.

### DataFile mmap

Two mmap strategies are used depending on the file's lifecycle:

**Active (writable) file — `WritableMmapDataFile`**: When `Options::use_mmap` is set, the active file is pre-allocated to the rotation threshold via `ftruncate` and mapped with `mmap(PROT_READ, MAP_SHARED)`. MAP_SHARED is required so that `pwritev` writes through the fd update the same pages that mmap readers see. Reads within the mapped region are zero-syscall memcpy; reads beyond (rare: file grew past pre-allocated size) fall back to `pread`. On `truncate()` (called by `resume()`), the mapping is released via `munmap`, the file is truncated, and a new mapping is created — avoiding SIGBUS from accessing pages beyond the new EOF.

**Sealed (read-only) files**: When `seal()` is called, the file is memory-mapped with `mmap(PROT_READ, MAP_PRIVATE)` and `MADV_RANDOM`. Subsequent `read_entry()` and `read_value()` serve directly from the mapped region, eliminating `pread` syscalls on the hot read path. If `mmap` fails (e.g. address space exhaustion), the class silently falls back to `pread`. The mapping is released by `munmap` in the destructor.

On WASM/Emscripten builds, mmap is disabled (`#ifndef __EMSCRIPTEN__`). Emscripten's mmap emulation allocates a heap buffer and copies the file contents into it — functionally identical to `pread` but with doubled memory consumption. WASM builds use the `pread` fallback exclusively.

### HintFile I/O model

Write and read modes have different I/O strategies.

- **`OpenForWrite(path)`** — opens the file immediately. Each `append()` serializes one entry and writes it directly to the fd, updating a running CRC-32C accumulator. `close()` writes the 4-byte CRC trailer, calls `fdatasync()`, and closes the fd. If the HintFile is destroyed without calling `close()` (e.g. exception path), the fd is closed without writing the CRC — the `.hint.tmp` file is cleaned up on next startup.
- **`OpenForRead(path)`** — reads the entire file into an in-memory buffer via a single `pread` and immediately closes the fd. `scan()` operates on the buffer.

`HintEntry.key` is a `std::span<const std::byte>` into the backing buffer — zero allocation per entry, valid for the lifetime of the `HintFile`. All callers (recovery and tests) use `scan()`.

## Hint File Format (.hint)

### Purpose

Hint files are compact companion files to sealed (rotated) data files. Each hint entry summarises one data file entry — just enough metadata and the full key — so that the in-memory Key Directory can be rebuilt at startup by scanning the smaller hint files instead of the raw data files. Only sealed data files have a corresponding hint file; the active data file is recovered by scanning its raw bytes if needed.

### Entry Structure

`flush_hints_for()` writes entries in data-file append order. Keys are stored in full — no prefix compression.

```
+------------------+
| Hint Header      | 23 bytes
+------------------+
| Key Data         | key_len bytes
+------------------+
     ...repeated for each entry...
+------------------+
| File CRC32       | 4 bytes (file trailer)
+------------------+
```

Total fixed overhead per entry: **23 bytes** (header). A single 4-byte CRC-32C trailer at the end of the file covers all entry bytes.

> `BulkBegin`/`BulkEnd` data file entries are **never** written to hint files — only `Put` and `Delete` entries are included.

### Hint Header (23 bytes)

| Offset | Size | Field        | Type   | Description                                    |
|--------|------|--------------|--------|------------------------------------------------|
| 0      | 8    | Sequence     | u64 LE | Entry sequence number                          |
| 8      | 1    | EntryType    | u8     | Entry kind: `Put` (0x01), `Delete` (0x02), or `RangeDel` (0x05) |
| 9      | 8    | File Offset  | u64 LE | Byte offset of the entry in the data file      |
| 17     | 4    | Value Size   | u32 LE | Value length in bytes                          |
| 21     | 2    | Key Len      | u16 LE | Length of the key bytes that follow             |

`Value Size` is stored so the reader can compute the full on-disk entry size in the data file without reading it.

### File Trailer (4 bytes)

| Offset from file start | Size | Field  | Type   | Description                                       |
|------------------------|------|--------|--------|---------------------------------------------------|
| end - 4                | 4    | CRC32  | u32 LE | CRC-32C (Castagnoli) over all preceding entry bytes |

`OpenForRead` verifies the trailer CRC eagerly before parsing any entries. On mismatch the entire hint file is rejected by throwing `std::runtime_error`. Corrupt hint files cause the engine to regenerate the hint from the raw data file during recovery.

### Size Constants

- `kHintHeaderSize = 23` — fixed header fields (no per-entry CRC)
- Total entry size: `kHintHeaderSize + key_len` (variable)
- File overhead: 4 bytes (file CRC trailer)

### Scanner API

`HintFile::make_scanner()` returns a `Scanner` object that iterates over entries sequentially. `HintEntry.key` is a `span<const byte>` into the backing file buffer, valid for the lifetime of the `HintFile`.

```cpp
auto scanner = hint.make_scanner();
while (auto he = scanner.next()) { /* use he->key, he->sequence, … */ }
```

### Recovery

On engine startup:

1. Discard any `.hint.tmp` files — incomplete hint files from a crash mid-rotation.
2. Open all `.data` files and seal them. For any data file without a companion `.hint`, generate one via `flush_hints_for()` (uses `CommittedEntryIterator`: buffers entries between BulkBegin/BulkEnd, discards incomplete batches, logs a warning). `BulkBegin`/`BulkEnd` markers are written to hint files with their sequence numbers (for accurate `next_seq` computation). Other hint entries are sorted by key (for prefix compression). Recovery's sequence-aware upsert handles duplicate keys across entries.
3. Recover exclusively from hint files. For each hint entry:
   - `Put`: insert `(key → {sequence, file_id, file_offset, value_size})` only if `entry.sequence > dir[key].sequence` (skip if a fresher entry is already present).
   - `Delete`: remove the key from the tree if `entry.sequence > dir[key].sequence`; otherwise skip.
4. Record `max_seq` — the largest sequence number seen across all hint entries.
5. Create a new active data file seeded at `max_seq + 1`.

This is a single code path: `flush_hints_for()` is the same function used by rotation and background hint writes. No raw-scan recovery logic exists — recovery always goes through hints.

### Parallel Recovery

`Bytecask::open(dir, max_file_bytes, recovery_threads)` accepts an optional `recovery_threads` parameter (default 4). A single unified code path handles all thread counts — there is no separate serial implementation. When `recovery_threads == 1`, the same algorithm runs on the calling thread without spawning workers:

1. **Phase 1 (serial, shared)**: same as above — open files, generate missing hints. Factored into `open_and_prepare_files()`, shared by both paths.
2. **Phase 2 (parallel build)**: round-robin assign files to W workers. Each builds a `RecoveryResult{key_dir, tombstones, max_seq, file_stats}` independently.
3. **Phase 3 (sequential accumulator merge)**: workers push finished `RecoveryResult`s into a thread-safe queue. A single merge thread pops results and merges each into a growing accumulator using `PersistentRadixTree::merge(acc, incoming, seq_resolver)`, then cross-applies tombstones. Each ~N/W-key tree is merged once; disjoint subtrees are shared O(1) by the persistent tree, so total merge work is proportional to overlap, not N × log₂(W).
4. **Phase 4 (serial assembly)**: `s.key_dir = final.key_dir; s.next_seq = final.max_seq + 1`.

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

**Hint files are a correctness-carrying artifact for recovery.** On startup, any data file without a companion `.hint` has one generated via `flush_hints_for()` (using `CommittedEntryIterator`). Recovery then reads only hint files — there is no separate raw-scan fallback path.

Hint files are written **deferred**: never inline on the write path. During normal operation, `rotate_active_file()` dispatches hint writes to the `BackgroundWorker`. At engine close, `~DB()` seals the active file and calls `flush_hints()`, ensuring every data file has a companion `.hint` after a clean shutdown. This keeps write-path latency flat and bounded. The cost is that a crash before shutdown causes recovery to generate missing hint files on startup, which is always correct and only slower.

### Hint File Atomicity

To guarantee hint files are either complete or absent, writing uses a temp-then-rename protocol:

1. Write the complete hint file to `data_{timestamp}.hint.tmp`.
2. Call `fdatasync` to flush all bytes to physical storage.
3. Atomically `rename(2)` to `data_{timestamp}.hint` — POSIX guarantees this rename is atomic on the same filesystem.

Any `.hint.tmp` file found at startup is discarded (it represents an incomplete write interrupted by a crash). Recovery will re-scan the corresponding `.data` file instead.

Because hint file writes are deferred (see above), this protocol is exercised at engine close or during an explicit `flush_hints()` call — not inside the rotation critical path.

### Module Plan

`HintFile` lives in the `bytecask.hint_file` C++20 module (`bytecaskdb/hint_file.cppm`), symmetric with `DataFile`:

Construction uses named static factory functions to make intent explicit at the call site:

- **`HintFile::OpenForWrite(path) -> HintFile`** — opens the file immediately with `O_WRONLY | O_CREAT | O_TRUNC`. Each `append()` serializes one entry and writes it directly to the fd, updating a running CRC-32C accumulator.
- **`HintFile::OpenForRead(path) -> HintFile`** — reads the entire file into an in-memory buffer via a single `pread`, verifies the file-level CRC eagerly, and closes the fd.

Write API:
- **`append(sequence, entry_type, file_offset, key, value_size) -> void`**: Serializes one hint entry and writes it to the fd. Only `Put` and `Delete` are valid entry types; passing `BulkBegin` or `BulkEnd` is a programming error.
- **`close() -> void`**: Writes the 4-byte CRC-32C trailer, calls `fdatasync()`, and closes the fd. If the HintFile is destroyed without calling `close()`, the fd is closed without writing the CRC — the `.hint.tmp` file is cleaned up on next startup.

Read API:
- **`make_scanner() -> Scanner`**: Returns a forward-only scanner. `HintEntry.key` is a `span<const byte>` into the backing buffer — valid for the lifetime of the `HintFile`.

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

### WritePlan

`WritePlan` is the unified type for all write operations submitted to `apply_batch`. It carries both **writes** (`put`, `del`, `del_range`) and optional **guards** (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`). When constructed with a `Snapshot`, guards and implicit W-W checks are available; without a snapshot, only unconditional writes and `ensure_present`/`ensure_absent` guards are available.

`WritePlan` is move-only and single-use; `DB::apply_batch` consumes it by move. See the Layer 1 section and `docs/transaction_design.md` for the full type definition and guard semantics.

### Iterators

Both `KeyIterator` and `EntryIterator` satisfy `std::bidirectional_iterator`. They yield entries in ascending key order when advanced with `operator++` and descending order with `operator--`. This matches the underlying `RadixTreeIterator`, which is itself bidirectional. Backward traversal uses a `retreat()` method that backtracks through the parent stack and `descend_rightmost()` to enter the rightmost subtree of prior siblings — O(1) amortized per step, matching forward iteration.

Forward scans use `iter_from` / `keys_from` (keys >= `from`); reverse scans use `riter_from` / `rkeys_from` (keys <= `from` in descending order). Both are available on `DB` and `Snapshot`. RocksDB equivalents: `Seek` + `Next` ↔ `iter_from`; `SeekForPrev` + `Prev` ↔ `riter_from`.

Reverse iteration is provided by a generic `ReverseIterator<Iter>` template that wraps `KeyIterator` or `EntryIterator`. `std::reverse_iterator` cannot be used because its `operator*` dereferences a temporary copy of the underlying iterator — when the underlying iterator caches its result internally (as both `KeyIterator` and `EntryIterator` do), the returned reference dangles. `ReverseIterator` holds the inner iterator directly and pre-decrements once in the constructor, so the reference remains valid.

```cpp
// Yields (key, value) pairs — bidirectional.
class EntryIterator {
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type      = std::pair<Bytes, Bytes>;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> EntryIterator&;
    auto operator--() -> EntryIterator&;
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};

// Yields keys only (no value I/O) — bidirectional.
class KeyIterator {
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type      = Bytes;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> KeyIterator&;
    auto operator--() -> KeyIterator&;
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};
```

Both integrate with `std::ranges::subrange` so callers can use range-for directly:

```cpp
// Forward scan (ascending).
for (auto& [key, value] : db.iter_from(opts, start_key)) { ... }
for (auto& key : db.keys_from(opts, prefix))              { ... }

// Reverse scan (descending).
for (auto& [key, value] : db.riter_from(opts, start_key)) { ... }
for (auto& key : db.rkeys_from(opts, prefix))              { ... }
```

- **Lazy**: each dereference reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries.
- **`KeyIterator` is in-memory only**: walks the radix tree key directory without touching any data file.
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

    // Atomically applies all operations in `plan` wrapped in BulkBegin/BulkEnd entries.
    // `plan` is consumed (move-only). No-op if plan is empty and has no guards.
    // When the plan carries a snapshot, guards and implicit W-W checks are evaluated.
    // Returns true if committed, false on conflict.
    // Throws std::system_error on I/O failure or DbDegraded if degraded.
    [[nodiscard]] auto apply_batch(const WriteOptions& opts, WritePlan plan) -> bool;

    // ── Range iteration ───────────────────────────────────────────────────

    // Forward: keys >= `from` in ascending order.
    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    // Reverse: keys <= `from` in descending order.
    // Empty `from` starts from the last key.
    [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;
    [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

    // ── Manifest ──────────────────────────────────────────────────────────

    // Rotates the active file, waits for all hint files, and returns a
    // manifest of sealed files with a snapshot. Forces file rotation.
    // Vacuum must not run between create_manifest() and file transfer
    // completion (caller responsibility).
    [[nodiscard]] auto create_manifest() -> FileManifest;

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
WritePlan plan;
plan.put(as_bytes("user:2"), as_bytes("bob"));
plan.put(as_bytes("user:3"), as_bytes("carol"));
plan.del(as_bytes("user:1"));
db.apply_batch({}, std::move(plan));

// Range scan (forward).
for (auto& [key, value] : db.iter_from(as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan (no disk I/O — radix tree walk only).
for (auto& key : db.keys_from(as_bytes("user:"))) { ... }

// Reverse scan (descending).
for (auto& [key, value] : db.riter_from(as_bytes("user:~"))) {
    // Iterates all keys <= "user:~" in descending order.
}
for (auto& key : db.rkeys_from(as_bytes("user:~"))) { ... }
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

The `~DB()` destructor seals the active file, then calls `flush_hints()` which drains the background worker and writes hint files for all sealed files. This guarantees that after a clean shutdown every data file has a companion `.hint` — the next `DB::open()` recovers purely from hint files with no raw `.data` scanning. `flush_hints()` is a private method; callers rely on the destructor for clean shutdown.

---

## Fault Injection Test Seam

To directly test correctness paths that only activate on IO failures (BC-131, BC-133), `bytecask.data_file` exposes a thread-local fault injection API compiled exclusively when `BYTECASK_TESTING` is defined.

### API

```cpp
// Fail the Nth next append() call. n=0 fails immediately; n=1 lets one through, etc.
void fault_inject_nth_append(int n) noexcept;

// Fail the Nth next sync() call. n=0 fails immediately.
void fault_inject_nth_sync(int n) noexcept;

// Reset all fault state to disabled. Call after each test (or use FaultGuard).
void fault_inject_reset() noexcept;
```

Fault state is per-thread (`thread_local FaultHooks`), so tests on different threads don't interfere. Both functions throw `std::system_error(EINVAL)` when the countdown reaches zero, identical in type to a real IO failure.

### Intended use

The `FaultGuard` RAII helper in `bytecask_test.cpp` calls `fault_inject_reset()` on destruction, so the fault state is always cleared even when a test assertion fails.

Tests enabled by this seam:
- **`[fault_inject]` mid-batch append failure** (BC-131): confirms that a failed append after `BulkBegin` triggers rotation and that the orphaned batch is discarded by recovery.
- **`[f_visibility]`/`[g_visibility]`** (BC-155): confirms that key changes are not published on sync failure — written key is absent after F or G failure, engine not degraded.

### Design notes

The seam is intentionally minimal:
- Zero production overhead: the `#ifdef BYTECASK_TESTING` blocks compile away entirely in non-test builds.
- Thread-local (not global) state: each thread has an independent countdown, avoiding cross-test interference in multi-threaded test runners.
- No virtual dispatch, no interface changes: `DataFile` remains a concrete class.

---

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: crc32c (google/crc32c, hardware-accelerated CRC-32C)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: Full `Bytecask` SWMR engine with `open`, `get`, `put`, `del`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`, `riter_from`, `rkeys_from`. Key directory backed by `PersistentRadixTree<KeyDirEntry>`. Per-file fragmentation tracking via `FileStats` (`live_bytes`, `total_bytes`) maintained on every write and reconstructed during recovery. `open()` always creates a fresh active data file; unified recovery path from hint files (single-threaded or parallel). 1.2M+ assertions, 103 test cases.

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/util.cppm`: C++23 module (`bytecask.util`) — `Crc32` accumulator (google/crc32c), `narrow<To>(From)` checked conversion
- `src/engine/serialization.cppm`: C++23 module (`bytecask.serialization`) — `ByteWriter`, `ByteReader`, `read_le`, `write_le`
- `src/engine/data_entry.cppm`: C++23 module (`bytecask.data_entry`) — `EntryType`, `EntryHeader`, `DataEntry`, serialization helpers
- `src/engine/data_file.cppm`: C++23 module (`bytecask.data_file`) — `DataFile`, `WritableDataFile`, `WritableMmapDataFile`, `WritablePosixDataFile`, `ReadOnlyMmapDataFile`, `ReadOnlyPosixDataFile`, `Offset`
- `src/engine/hint_entry.cppm`: C++23 module (`bytecask.hint_entry`) — `HintEntry`, `serialize_entry`, `deserialize_entry`
- `src/engine/hint_file.cppm`: C++23 module (`bytecask.hint_file`) — `HintFile`, `OpenForWrite`/`OpenForRead`
- `src/engine/radix_tree.cppm`: C++23 module (`bytecask.radix_tree`) — `PersistentRadixTree<V>`, `RadixTreeIterator<V>`
- `src/engine/concurrency.cppm`: C++23 module (`bytecask.concurrency`) — `SyncGroup`, `BackgroundWorker`
- `src/engine/internals.cppm`: internal partition `bytecask.engine:internals` — `EngineState`, `FileStats`, `KeyDirEntry`, `FileRegistry`, `Key`, `StaleFile`, `VacuumMapping`, `VacuumScanResult`, `RecoveredFile`, `RecoveryResult`, `entry_size`
- `src/engine/bytecask.cppm`: primary interface unit `bytecask.engine` — public types (`Bytes`, `BytesView`, `VacuumOptions`, `WritePlan`, `WriteOptions`, `ReadOptions`, `Options`, `KeyIterator`, `EntryIterator`) and `Bytecask` class declaration
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

## Layer 1: `snapshot()` and `apply_batch(WritePlan)`

Two primitives added to `DB` in BC-103 providing snapshot isolation without any mandatory transaction wrapper.

### `DB::snapshot() → Snapshot`

Returns a `Snapshot` — a move-only, read-only value wrapping a `shared_ptr<const EngineState>`. The entire state at that moment is frozen: the key directory, the file registry, and open file descriptors. Reads on `Snapshot` are lock-free; no mutex is ever acquired.

The `shared_ptr` keeps all data files referenced at snapshot time alive — their file descriptors remain open. Vacuum unlinks files immediately, but POSIX guarantees that `pread` on an unlinked file succeeds as long as the fd is open. Disk blocks are freed when the last `shared_ptr<DataFile>` is destroyed, closing the fd.

`Snapshot` exposes the same read API as `DB`: `get`, `contains_key`, `iter_from`, `keys_from`, `riter_from`, `rkeys_from`.

### `DB::apply_batch(opts, plan) -> bool`

Applies `plan` atomically only if all guards pass and no key in the write set was modified since the plan's snapshot was taken (when the plan carries a snapshot). Both the conflict check and the apply run under `write_mu_`, serialised with all other writers.

Conflict is detected by comparing `KeyDirEntry::sequence` between the snapshot state and the current state for each key in the write set. Three conflict cases are detected:

1. Key absent in snapshot but present now (key appeared after snapshot).
2. Key present in snapshot but absent now (key deleted after snapshot).
3. Key present in both but with a different `sequence` (key modified after snapshot).

On the first conflict detected, `apply_batch` returns `false` before any I/O is performed. If no conflict is found, the writes are applied atomically.

#### Implicit W-W check on write keys

When a `WritePlan` carries a snapshot, `apply_batch` automatically checks every key in the write set (put or del) for concurrent modification — the caller does not need to call `ensure_unchanged` on keys they intend to write. This closes a common concurrency hole: without it, a plan could read a key, compute a new value, and write it back without noticing a concurrent writer already changed that key.

`ensure_unchanged` is for read-only dependencies: keys whose value influenced the plan's decisions but that the plan does not modify. For example, reading a price to compute an order total — the price key is a read dependency but not in the write set, so it needs an explicit guard.

### Conflict signalling

`apply_batch` returns `std::optional<CommitResult>`: engaged with the assigned `CommitResult{sequence, durable}` if committed, `nullopt` on conflict (W-W or guard violation) — nothing was written, no sequence assigned. Conflicts are expected outcomes in concurrent workloads — not errors. I/O failures remain exceptions (`std::system_error`). `put` and `del_range` cannot conflict (no guards, no snapshot) and return `CommitResult` directly; `del` returns `nullopt` if the key was absent. See `docs/commit_result_api_design.md` for the full `CommitResult` contract (BC-231).

### Single-entry batch optimization

When the write set contains exactly one operation, `apply_batch` skips the `BulkBegin`/`BulkEnd` marker writes entirely. A single data entry is CRC-protected and self-describing — the markers add no recovery benefit. See D14.

## Immediate engineering constraints

- Tests must remain runnable from the repository with a single clear command.
- Architectural decisions should prefer small, composable units over logic embedded in `main.cpp`.
- Design notes in `docs/old_bytecask_design.md` are historical reference material, not the current source of truth.
- The living design and project tracker live under `docs/`.

## Operational Counters

`DB::stats()` returns a `std::map<std::string, std::int64_t>` containing all operational counters and gauges. Designed for pull-based metrics integration (Prometheus, logging, debugging).

**Monotonic counters** (increment only, reset on restart):

| Counter | Path | Description |
|---------|------|-------------|
| `bytecask.bytes_written` | Write | On-disk bytes appended (header + key + value + CRC) |
| `bytecask.group_writer_batches` | Write | `execute_slots()` calls (one per batch) |
| `bytecask.group_writer_coalesced` | Write | Total writers coalesced across all batches |
| `bytecask.file_rotations` | Write | Active file rotations |
| `bytecask.fsyncs` | Write | `fdatasync` calls |
| `bytecask.disk_reads` | Read | `pread` calls from `get()` |
| `bytecask.disk_read_bytes` | Read | Bytes read from disk |
| `bytecask.vacuum_bytes_reclaimed` | Vacuum | Bytes freed by vacuum |
| `bytecask.vacuum_files_unlinked` | Vacuum | Data files physically removed |
| `bytecask.files_opened` | Lifecycle | `DataFile` opens (recovery, rotation, vacuum) |
| `bytecask.crc_failures` | Error | CRC mismatches on the read path |
| `bytecask.io_errors` | Error | `std::system_error` from I/O operations |
| `bytecask.degraded_transitions` | Error | Times the engine entered degraded state |

**Recovery counters** (set once at open, immutable for DB lifetime):

| Counter | Description |
|---------|-------------|
| `bytecask.recovery_files` | Data files replayed during recovery |
| `bytecask.recovery_keys` | Keys recovered into the key directory |
| `bytecask.recovery_duration_us` | Wall-clock recovery time in microseconds |

**Gauges** (current state, read from `EngineState` at call time):

| Gauge | Description |
|-------|-------------|
| `bytecask.degraded` | 1 if degraded, 0 otherwise |
| `bytecask.open_files` | Number of data files in the file registry |

All atomic counters use `std::memory_order_relaxed` — sufficient for monotonic counters where cross-counter consistency is not required. Write-path counters are incremented under the existing `write_mu_` (zero contention). Read-path counters use relaxed `fetch_add` (~8 ns on x86), negligible next to the `pread` syscall.

Counters are per-DB instance (`Counters` struct owned by `DB`). Two open databases have independent counter sets.

## Design Decisions

| # | Decision |
|---|----------|
| D1 | **Error handling**: Throw (`std::system_error` for I/O, `std::runtime_error` for corruption). These are panic-level events the caller cannot meaningfully recover from inline. `std::optional` covers the key-not-found case for `get`. No `std::expected` at this boundary — there are no anticipated recoverable error conditions in normal operation. |
| D2 | **Config**: Deferred — removed from the initial API scope. |
| D3 | **WritePlan ownership**: `WritePlan` is move-only (copy constructor and copy assignment deleted). Single-use by design. |
| D4 | **WritePlan size limit**: None — the caller is responsible. |
| D5 | **Iterator strategy**: Lazy — each `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries. |
| D6 | **`KeyIterator` source**: In-memory only — walks the B-Tree key directory without opening any data file. |
| D7 | **`del` on missing key**: Returns `bool` — `true` if the key existed and was removed, `false` if it was absent. Consistent with `std::set::erase` returning a count. |
| D8 | **Error handling during iteration**: Throw `std::system_error` on I/O failure (consistent with D1 and standard C++ practice). |
| D9 | **Concurrency model**: SWMR — exactly one writer at a time; reads are concurrent. MVCC and snapshot isolation are not provided. |
| D10 | **Vacuum**: Two independently testable paths — `vacuum_compact_file` (rewrite sealed file into a new sealed file, dropping dead entries) and `vacuum_remove_file` (delete files with zero live entries, no I/O required). `vacuum()` selects a target file above `fragmentation_threshold`, then branches: `vacuum_remove_file` if `live_bytes == 0`, otherwise `vacuum_compact_file`. Returns `true` if a file was processed, `false` if nothing qualified. No compound paths. All vacuum-related identifiers use a `vacuum_` prefix. One sealed file per `vacuum()` call. Engine continues serving reads and writes. For `vacuum_compact_file`, `write_mu_` is held only for the commit step (I/O writes to a private temp file). For `vacuum_remove_file`, `write_mu_` is held only for the brief metadata update. `vacuum_commit` itself does not acquire `write_mu_` — the caller is responsible for holding it. Tombstones are always copied (never elided) during partial vacuum. File selection uses `fragmentation >= fragmentation_threshold` computed from incrementally maintained `FileStats` — O(1) per file. Stats are reconstructed during recovery as a side-effect of the hint-file pass. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmss}_{RRRRRRRR}_V{XX}`. Timestamp is UTC second precision — a human-readable creation-time hint, not content age (compaction produces new files with old entries). `RRRRRRRR` is a 4-byte random hex salt for collision avoidance. `V{XX}` is the file format version (`V01` initially). Filename ordering carries no semantic meaning; entry sequence numbers are authoritative. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be discarded with a logged warning. No partial-batch entries enter the key directory. |
| D14 | **Single-entry batch optimization**: When `apply_batch` is called with exactly one operation, the `BulkBegin`/`BulkEnd` marker writes are skipped. A single data entry is self-describing and CRC-protected, so the markers add no recovery benefit for a write set of size 1. |
| D15 | **C ABI / shared-library link constraint**: `libbytecask.a` is compiled with `-fPIC` so it can be linked into a shared object (e.g. `ha_bytecaskdb.so`). Without `-fPIC`, clang emits `R_X86_64_TPOFF32`/`R_X86_64_32S` relocations illegal in a DSO. xmake syntax: `add_cxxflags("-fPIC", {force = true})` on the `bytecask` static target. |
| D16 | **MariaDB plugin header ordering**: Server-internal headers require `server/my_global.h` before `handler.h`. The client-side stub does not define `MY_GLOBAL_INCLUDED`/`uchar`/`unlikely()`. Fedora layout: base `/usr/include/mysql`, server `/usr/include/mysql/server`, private `/usr/include/mysql/server/private`. CMake include order must be `server/private` → `server` → base. `-DMYSQL_SERVER` is required. `handlerton::state` does not exist in this MariaDB ABI; use `PLUGIN_LICENSE_GPL` (no MIT constant). |
| D17 | **Directory locking**: One process per directory, enforced by `flock()` on `dir/.lock`. Advisory only — does not protect against uncooperative processes that bypass `DB::open()`. |
| D18 | **Sequence-disjoint files**: All data files must have non-overlapping sequence ranges — no two files contain entries with the same sequence number. Active file rotation naturally preserves this (sealed files have contiguous sequence ranges). Vacuum compact must ensure compacted files maintain disjoint ranges. This invariant enables efficient replication (linear scan instead of min-heap merge), supports future file merging operations, and allows skipping entire files based on sequence bounds. |

## Replication Primitives

ByteCaskDB exposes a minimal set of primitives for building leader-follower replication on top of the engine. The design avoids embedding a replication protocol — instead it provides the read and write building blocks that an external coordinator composes.

See [`replication_primitives_design.md`](replication_primitives_design.md) for the full design reference.

### Mode

```cpp
enum class Mode { Leader, Follower };
```

`Mode` controls which write paths are available:

- **Leader** (default): normal writes (`put`, `del`, `del_range`, `apply_batch`) are allowed; `ingest` is rejected.
- **Follower**: normal writes throw `DbFollowerMode`; only `ingest` is allowed. Reads, snapshots, vacuum, and `resume()` work in both modes.

`set_mode(Mode)` acquires the write mutex to ensure no in-flight write straddles the transition. `mode()` is a lock-free atomic read (acquire semantics), same pattern as `is_degraded()`.

### Leader-side: `durable_sequence`, `create_manifest`, `changes_since`

- `durable_sequence(min_sequence, timeout)` — the single sequence primitive (renamed from `current_sequence` — BC-231). Blocks until the durable sequence reaches at least `min_sequence` or the timeout expires, then returns the durable sequence; `min_sequence = 0`/an already-reached target/a nonpositive timeout return immediately without blocking (useful for polling replicas or waking a replication loop only when the leader is genuinely ahead).
- `create_manifest()` — rotates the active file, waits for all hint files, and returns a `FileManifest` of sealed files with a snapshot. Used for initial bootstrap.
- `changes_since(seq, snap)` — returns a lazy `ChangeIterator` that walks sealed files in sequence order, yielding `DataEntryView` entries with `sequence > seq`. Constant memory — scans one entry at a time.

### Follower-side: `ingest`

```cpp
void ingest(std::span<const DataEntryView> entries);
```

`ingest` applies pre-sequenced entries from a leader to the follower's storage. Key properties:

- **Idempotent**: entries with `sequence <= durable_seq` are silently skipped.
- **Batch-safe rotation**: `BulkBegin`/`BulkEnd` pairs always land in the same data file. Rotation only occurs at boundaries where no batch is open.
- **Chunked I/O**: entries are written in chunks separated by rotation boundaries — one `pwritev` + one `fdatasync` per chunk, mirroring the leader's group-commit batching.
- **Durability before visibility**: `store_state` (publishing to readers) happens only after the final `fdatasync`.
- **Degraded-state on failure**: same pattern as the normal write path — on I/O failure, the engine goes degraded and `resume()` recovers.

Correctness is validated by 211 generated proof tests (178 ingest + 33 manifest) covering the full (StateShape × OpsShape × FailureClass) matrix. See [`correctness_validation.md`](correctness_validation.md) for the proof framework and [`replication_primitives_design.md`](replication_primitives_design.md) for the invariants.

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints