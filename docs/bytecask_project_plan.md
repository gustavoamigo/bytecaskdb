# ByteCask Project Plan

This file is the repository's simple issue tracker.

Canonical location: `docs/bytecask_project_plan.md`.

## How to use it

- Add new work as a short item with an ID, status, title, and note.
- Move active work to `In Progress`.
- Move finished work to `Done`.
- Keep follow-ups in `Backlog`.

## In Progress

| ID | Title | Note |
| --- | --- | --- |
| BC-046 | Conservative online vacuum — design | Design documented in `bytecask_design.md` (Vacuum section, VacuumOptions/VacuumStats API, D10). No implementation yet. |

## Backlog

| ID | Title | Note |
| --- | --- | --- |
| BC-047 | Implement conservative online vacuum | Implement `Bytecask::vacuum(VacuumOptions)` per design in `bytecask_design.md`. Requires: waste-ratio calculation from key-dir snapshot, new data-file + hint-file write, atomic key-dir + file-registry commit under `write_mu_`, old-file deletion. |
| BC-048 | Auto vacuum after rotation | After each file rotation, dispatch vacuum on a background thread if any sealed file exceeds `waste_threshold`. Depends on BC-047 (vacuum) and BC-045 (background thread infrastructure). |
| BC-002 | Shared engine library target | xmake C++23 module BMI sharing across static-lib targets needs investigation; currently engine sources are compiled per-target. |
| BC-024 | Implement PMR - Memory Allocation described in design | Note: There is a draft proposal in the bytecask_design.md
| BC-026 | Run `flush_hints` in background after file rotate | After rotation, dispatch `flush_hints()` on the sealed file to a background thread so it does not block the write path. |
| BC-045 | `fdatasync` sealed file after rotation (async) | When a data file is rotated to immutable, schedule an `fdatasync` on it in a background thread. Ensures sealed files are durable without accumulating a large dirty-page backlog that would penalise the next `sync=true` write. Requires a background thread or thread pool (not yet implemented). |
| BC-041 | `ReadOptions::verify_checksums` flag | Allow skipping CRC verification on bulk scans for ~5% win. Mirrors LevelDB/RocksDB `verify_checksums` option. |
| BC-027 | Add Mixed/Sync benchmark | Added BC_Mixed_Sync and LDB_Mixed_Sync (1000 iter, 3 reps); results recorded 2026-04-02. Moved to backlog — no open work remaining. |


## Done

| ID | Title | Note |
| --- | --- | --- |
| BC-043 | Move fdatasync outside write lock | `put`, `del`, `apply_batch` now release the exclusive lock before calling `fdatasync`. Readers are no longer blocked during sync I/O. Captures `shared_ptr<DataFile>` under the lock to keep the file alive post-rotation. Correctness: durability guarantee preserved (caller still blocks until fdatasync returns). 87 tests pass (1M+ assertions). Mixed/Sync ~10% improvement in single-threaded bench; primary benefit is multi-threaded concurrency. |
| BC-044 | Replace shared_mutex with split write_mu_ + snapshot_mu_ | Eliminated `shared_mutex` reader-preference writer-starvation. Writers hold `write_mu_` (plain mutex) for I/O + key-dir update, then publish a `DirSnapshot` under `snapshot_mu_` (~10 ns). Readers only acquire `snapshot_mu_`. MixedMT/Sync/threads:2 improved from 179 ms → 89 µs (~2000×); MixedMT/NoSync/threads:4 improved ~2.4×. 88 tests pass (1M+ assertions). |
| BC-034 | Intrusive reference counting for radix tree nodes | Replace `shared_ptr<Node>` with `IntrusivePtr<Node>` embedding `atomic<uint32_t> refcount_` in each node. Eliminates ~32 B `make_shared` control block per node; shrinks child slot from 24 B to 16 B. Per-node cost: ~80 B (down from ~104 B). Measured: 108 B/key generic (−17%), 116 B/key prefixed (−17%) at 100k keys. 82 tests pass (1M+ assertions). ASan clean. |
| BC-039 | Single-pread read path (`DataFile::read_entry`) | Use `KeyDirEntry.value_size` + key size to issue one `pread` instead of two. Get +53% (1.18M ops/µs), Range50 +66% (28.2k scans/µs). ByteCask Get now 12% faster than LevelDB. Chunked I/O strategy (BC-038) removed — offset-sort overhead exceeded benefit on warm cache. 86 tests pass (1M+ assertions). |
| BC-038 | iter_from: chunked I/O strategy (removed) | Attempted chunked strategy (collect KeyDirEntries, sort by offset, read in order). Benchmarked worse than lazy path on warm cache due to sort + collect overhead. Removed; `ReadOptions` simplified to empty struct. |
| BC-042 | Buffer-reusing `get()` and iterator | Added `Bytes&` output-param `get()` overload (convenience `get` delegates to it), `DataFile::read_entry(io_buf)` as single read primitive, `verify_entry()` factored from duplicated CRC logic. Removed `read_value_into` (replaced by `read_entry` + `extract_value_into`). `EntryIterator` reuses internal I/O + value buffers across advances. Get: ~flat (pread-dominated). Range50: **−16%** CPU time (37 µs → 31 µs at 50K keys). 87 tests pass (1M+ assertions). |
| BC-030 | Radix tree engine integration | Replaced `PersistentOrderedMap<Key, KeyDirEntry>` with `PersistentRadixTree<KeyDirEntry>` as the in-memory key directory. `KeyIterator` and `EntryIterator` now wrap `RadixTreeIterator<KeyDirEntry>`. All engine operations (`get`, `put`, `del`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`) and recovery pass byte spans directly to the radix tree API. 82 tests pass (1M+ assertions). ASan clean. |
| BC-033 | Radix tree hardening + memory layout optimisation | Phase 1: linear scan kept, 3 comments added, 10 new tests (33 total, ~1M assertions). Phase 2: children N=8→N=4 (−31% memory). Phase 3: packed node layout — `uint64_t edit_tag` + `optional<V>` replaced by `uint32_t packed_tag_` (high bit = has_value) + bare `V value_`; children N=4→N=1. Combined: 184→112 B/node; measured 209→129 B/key (−38%) at 100k generic keys, 225→139 B/key (−38%) with prefixed UUIDv7 keys. 33 tests pass. |
| BC-036 | jemalloc as global allocator | `add_requires("jemalloc")` + `add_packages` on all four targets. Replaces glibc ptmalloc2 via symbol interposition. No regression in `engine_bench` (results within noise; load avg was 5.56 on the after run). Fragmentation benefit visible only in long-running workloads. |
| BC-035 | ByteCask vs LevelDB benchmark suite | `benchmarks/engine_bench.cpp` (target `engine_bench`). Benchmarks: put/get/del/range-50/mixed(80%get 10%put 10%del) at nosync and sync durability levels, prefixed UUIDv7 keys, 1 KiB values. Reports ops/s rate + p50/p99 latency counters per benchmark. LevelDB added as xmake dependency. |
| BC-031 | Benchmark harness: OrderedMap vs RadixTree | Google Benchmark suite in `benchmarks/map_bench.cpp` (target `bytecask_bench`, `set_default(false)`). Compares persistent set, transient set, get, iteration, lower_bound, and memory footprint at 1k/10k/100k keys. RadixTree transient set ~2.4× faster than its persistent set at 100k keys; RadixTree get ~8× faster than OrderedMap. |
| BC-032 | Sanitizer build mode + memory/concurrency verification | `xmake f --sanitizer=address` / `--sanitizer=thread` applies ASan/TSan to all targets via `on_load` callback. Full test suite clean under ASan+LSan (70 tests, 47,529 assertions, zero leaks). Concurrent reader/writer test (`[concurrency]` tag) verifies persistent snapshot immutability across threads. TSan blocked in Codespaces by ASLR sandbox; documented workaround. Memory footprint benchmark (`BM_MemoryFootprint`) added to `map_bench.cpp`. |
| BC-029 | Persistent Radix Tree — core implementation | `bytecask.radix_tree` module: `PersistentRadixTree<V>`, `TransientRadixTree<V>`, DFS iterator with `lower_bound`, `SmallVector<T,N>` inline storage. Fixed SmallVector insert-at-end corruption bug, rewrote iterator `seek()` for correct lower_bound. 35 test cases, 10k-round model-based test (47,522 total assertions pass). |
| BC-028 | Key type: immer::array backing | Replaced `Key = std::vector<std::byte>` with a value class wrapping `immer::array<std::byte>`. O(1) copies inside PersistentOrderedMap instead of O(n) deep copies. 191 assertions pass. |
| BC-019 | Recovery and startup | `recover_existing_files()` in `Bytecask` constructor: removes stale `.hint.tmp` files, scans each `.data` file via its hint companion (if present) or raw otherwise, applies BulkBegin/BulkEnd batch state machine, uses LSN as sole freshness authority (no file ordering relied upon). Seeds `next_lsn_` from max seen sequence. 5 new test cases; 187 assertions pass. |
| BC-025 | Add WriteOptions / ReadOptions to engine API | LevelDB/RocksDB-style option structs. `WriteOptions::sync` (default `true`) controls `fdatasync` on `put`, `del`, and `apply_batch`. `ReadOptions` is empty (reserved). 3 new test cases; 170 assertions pass. |
| BC-023 | Copy-on-write file registry + safe EntryIterator | Replaced `std::map<uint32_t,DataFile>` with `FileRegistry` (`shared_ptr<map<uint32_t,shared_ptr<DataFile>>>`). Rotation clones inner map and replaces outer `shared_ptr` — in-flight iterators hold a snapshot with independent lifetime. `EntryIterator` holds `FileRegistry` by value instead of a raw pointer; no dangling-pointer risk on engine move. 160 assertions pass. |
| BC-008 | File naming + rotation | Size-triggered rotation: `uint32_t` file IDs, `FileRegistry` COW registry, `sealed_` flag in `DataFile`, multi-file `get()`/`iter_from()`, deferred `flush_hints()` at close. |
| BC-018 | Bytecask engine class | `Bytecask` SWMR engine: `open`, `get`, `insert`, `remove`, `contains_key`, `apply_batch`, `iter_from`, `keys_from`. Key directory: `PersistentOrderedMap<Key, KeyDirEntry>`. `open()` always creates a fresh active data file. `fdatasync` after every write; transient pattern in `apply_batch`. Module `bytecask.engine`. 10 new test cases, 143 total assertions. |
| BC-016 | Atomic Bulk Put | Superseded by BC-018: `apply_batch` wraps operations in `BulkBegin`/`BulkEnd` inside `Bytecask`. |

| ID | Title | Note |
| --- | --- | --- |
| BC-014 | POSIX I/O + Group Commit in DataFile | Replaced `std::ofstream` with POSIX `open`/`write`/`fdatasync`. Extracted `sync()` from `append()` to enable Group Commit batching. |
| BC-013 | Modularize source structure | Split `data_entry.cppm` into finer-grained C++20 modules: `crc32.cppm`, `serialization.cppm`, `data_file.cppm`, and `data_entry.cppm`. |
| BC-000 | Establish Copilot workflow | Added always-on repo instructions, living design doc, project plan, and test wiring. |
| BC-001 | Define storage engine MVP | Defined data entry format and DataFile API as the first storage-engine component. |
| BC-003 | Replace smoke test with behavior tests | Replaced `smoke_test.cpp` with `data_entry_test.cpp` covering serialization, append, and CRC verification. |
| BC-004 | Move living docs into docs folder | Relocated the design doc and task tracker to `docs/` and updated workflow references. |
| BC-005 | Data entry append to file | `DataFile` appends key-value entries with 19-byte LE header, CRC32, and auto-incrementing sequence numbers. Zero warnings. All tests pass. |
| BC-009 | Bitsery serialization + CRC-at-end | Replaced manual LE serialization with bitsery. Moved CRC to trailing position for one-pass write. Extracted `CrcOutputAdapter<TAdapter>` (reusable bitsery wrapper) in `data_entry.cppm`. `kHeaderSize` = 15, `kCrcSize` = 4. Zero warnings. All tests pass. |
| BC-011 | C++20/23 modernization & readability | Applied `std::span`, `std::as_bytes`, `std::to_integer`, `std::in_range`, `std::ssize`, `std::filesystem::file_size`, `is_integral_v`, `auto` consistency. Added `narrow<To>()` checked-narrowing helper and `write_bytes()` wrapper to hide `reinterpret_cast` from call sites. Updated copilot-instructions with coding guidelines. All tests pass. |
| BC-017 | HintFile writer + reader | `bytecask.hint_file` module: `OpenForWrite`/`OpenForRead` factories, `append`/`sync`/`scan` API, 22-byte header + trailing CRC. `scan()` returns `std::optional<pair<HintEntry, Offset>>`, panics on CRC mismatch. 4 new test cases, 57 total assertions pass. |
| BC-021 | Update HintFile format: add EntryType | Hint header 22→23 bytes: `EntryType` at offset 8; `file_offset` shifts to offset 9. Updated `kHintHeaderSize`, `append`, `HintEntry`, `serialize_hint_entry`, `scan`, and all tests. `hint_entry` now `export import`s `bytecask.data_entry` to share the enum. 60 assertions pass. |
| BC-006 | Data file read path | `DataFile::scan()` and `read()` implemented via `deserialize_entry`. CRC verified via direct `Crc32::update()` over full buffer — no streaming adapter needed. Covered by Test 3 in `data_entry_test.cpp`. |
| BC-010 | CrcInputAdapter for read path | Not implemented — read path uses direct `Crc32::update()` on the full entry buffer in `deserialize_entry`; no bitsery input adapter was required. |
| BC-015 | EntryType replaces Flags in header | `flags` u8 replaced by `EntryType` enum; layout seq(8)+type(1)+key_size(2)+val_size(4); `append(seq,type,key,val)` API. 36 assertions pass. |
| BC-012 | Migrate tests to Catch2 | Replaced hand-rolled `fail()`/`expect()` harness with Catch2 v3. `TEST_CASE`/`CHECK`/`REQUIRE` macros, Catch2's own `main()`, randomized test order. 3 test cases, 28 assertions. |
| BC-035 | Leaf node optimization: pointer-to-children | Replace `SmallVector<..., 1> children` (32 B always) with `unique_ptr<ChildVec>` (8 B null for leaves). 94% of nodes are leaves and pay only 8 B pointer instead of 32 B empty SmallVector. Node struct: 80→56 B. Measured: 86 B/key generic (−20% from intrusive, −33% from original), 92 B/key prefixed (−21% from intrusive, −34% from original). 82 tests pass (1M+ assertions). ASan clean. |
| BC-027 | Write lock to enforce SWMR contract | `std::shared_mutex` (heap-allocated via `unique_ptr`) in `Bytecask`. Writers take `unique_lock`; readers take brief `shared_lock` to snapshot key_dir_ + files_ then release before I/O. `WriteOptions::try_lock` (default `false`) enables non-blocking attempts that throw `system_error(resource_unavailable_try_again)` on contention. |