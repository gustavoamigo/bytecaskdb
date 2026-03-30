# ByteCask Project Plan

This file is the repository's simple issue tracker.

Canonical location: `docs/bytecask_project_plan.md`.

## How to use it

- Add new work as a short item with an ID, status, title, and note.
- Move active work to `In Progress`.
- Move finished work to `Done`.
- Keep follow-ups in `Backlog`.

## In Progress

_nothing currently in progress_

## Backlog

| ID | Title | Note |
| --- | --- | --- |
| BC-002 | Shared engine library target | xmake C++23 module BMI sharing across static-lib targets needs investigation; currently engine sources are compiled per-target. |
| BC-008 | File naming + rotation | Implement `data_{YYYYMMDDHHmmssUUUUUU}` microsecond timestamp naming and file rotation (active → rotating → immutable lifecycle). |
| BC-019 | Recovery and startup | Startup procedure: discard `.hint.tmp`, read hint files oldest-to-newest, scan active data file, discard incomplete batches (warn), seed LSN from max seen sequence. |


## Done

| ID | Title | Note |
| --- | --- | --- |
| BC-022 | PersistentOrderedMap wrapper | `immer::btree_map` does not exist; replaced with `PersistentOrderedMap<K,V>` backed by `immer::flex_vector<Entry>` (RBT). Provides sorted-map API (`get`, `contains`, `lower_bound`, `set`, `erase`) and `OrderedMapTransient<K,V>` for batch mutations. Module `bytecask.persistent_ordered_map`, 13 test cases. |
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