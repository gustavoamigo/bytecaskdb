# ByteCask Design

## Purpose

ByteCask is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf) implementation with a key architectural difference: it uses an immutable **B-Tree** for the Key Directory instead of a Hash Table. This design choice enables efficient **range queries** and **prefix searches** while maintaining Bitcask's core strengths of fast writes and simple recovery. The name "ByteCask" reflects this hybrid approach: **Bitcask algorithm** + **B-Tree index** = **ByteCask**.

**Fundamental Trade-off**: ByteCask keeps **all keys in memory** at all times. This enables extremely fast lookups and range queries but limits database size to available RAM. Considering a memory requirement of approximately 100 bytes per unique key (key data + metadata + tree structure overhead), 10 million keys would require around 1 GB RAM.

This document is the living design reference for the repository. It should track the current implementation state, the intended architecture, and important constraints.

Canonical location: `docs/bytecask_design.md`.

## Design Principles

The design follows these core tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Performance**: Optimizations are pursued only when they don't compromise correctness or simplicity.

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

Every entry has a monotonic sequence number to determine data freshness. The `DataFile` class manages this internally, starting at 1 and incrementing per append.

### Log-Structured Naming Convention

File naming uses timestamp-based names: `data_{YYYYMMDDhhmmssnnnn}.data` and `data_{YYYYMMDDhhmmssnnnn}.hint`. Not yet implemented — callers currently provide the full file path.

### DataFile API

`DataFile` (in `bytecask.data_file` module) is the primary storage-engine component. It owns a single data file and provides:

- **Constructor**: Takes a `std::filesystem::path`. Opens (or creates) the file via POSIX `open(O_WRONLY | O_CREAT | O_APPEND)`. Throws `std::system_error` on failure.
- **`append(sequence, entry_type, key, value) -> Offset`**: Serializes a new entry with the given sequence number and `EntryType`, writes it to the OS page cache via `::write()`, and returns the byte offset where the entry starts. `BulkBegin`/`BulkEnd` entries pass empty key and value spans. Does **not** guarantee durability on its own.
- **`sync()`**: Calls `::fdatasync()` to flush all pending writes to physical storage. Must be called explicitly to guarantee crash-safety. Decoupled from `append()` to enable Group Commit: callers can batch multiple `append()` calls before a single `sync()`.
- Key and value are accepted as `std::span<const std::byte>` for binary safety.

### I/O Back-end Rationale

- **POSIX over `std::ofstream`**: `::write()` with `O_APPEND` issues a single system call per entry, skipping the buffering layers and locale state overhead of C++ streams.
- **`fdatasync` over `fflush`/`flush()`**: `fdatasync` syncs data to physical media while skipping inode metadata updates (access time etc.), making it faster than `fsync` for a pure append-only log.
- **Group Commit pattern**: Separating `append()` (writes to page cache) from `sync()` (forces to disk) lets future code batch hundreds of writes before a single expensive `fdatasync`, which is the primary lever for high write throughput on NVMe hardware (see `io_uring` paper reference).

### Source Code Module Architecture

We use fine-grained C++20 modules:
- `bytecask.crc32`: General purpose mathematical utilities (`Crc32`, checked `narrow<To>(From)` conversion).
- `bytecask.serialization`: Core bitsery abstractions (`CrcOutputAdapter`, legacy memory wrappers).
- `bytecask.data_entry`: Logical entry definition and single-entry memory formatting.
- `bytecask.data_file`: Disk I/O, writing streams sequentially to `.data` files.
- `bytecask.hint_file`: Hint file writer and reader (`HintFile`, `HintEntry`).

### Current scope boundaries

- `EntryType` is written and read back on `DataFile::read()`; atomic bulk semantics are enforced at a layer above `DataFile`.
- No file rotation or size limits.
- No read path for the key directory — append-only for now.

## Hint File Format (.hint)

### Purpose

Hint files are compact companion files to sealed (rotated) data files. Each hint entry summarises one data file entry — just enough metadata and the full key — so that the in-memory Key Directory (B-Tree) can be rebuilt at startup by scanning the smaller hint files instead of the raw data files. Only sealed data files have a corresponding hint file; the active data file is recovered by scanning its raw bytes if needed.

### Entry Structure

```
+------------------+
| Hint Header      | 22 bytes
+------------------+
| Key Data         | key_size bytes
+------------------+
| CRC32            | 4 bytes (trailing)
+------------------+
```

Total fixed overhead per entry: **26 bytes** (22-byte header + 4-byte trailing CRC).

> **Note on the 26-byte claim in the spec**: the four header fields sum to 22 bytes (u64 + u64 + u16 + u32). The "26 bytes" refers to the total fixed overhead including the trailing CRC, not the header alone. This matches the same accounting used in the data file, where `kHeaderSize + kCrcSize` is always cited together.

### Hint Header (22 bytes)

| Offset | Size | Field       | Type   | Description                                    |
|--------|------|-------------|--------|------------------------------------------------|
| 0      | 8    | Sequence    | u64 LE | Entry sequence number (LSN)                    |
| 8      | 8    | File Offset | u64 LE | Byte offset of the entry in the data file      |
| 16     | 2    | Key Size    | u16 LE | Key length in bytes                            |
| 18     | 4    | Value Size  | u32 LE | Value length in bytes (to rebuild the key directory without the data file) |

`Value Size` is stored so the reader can compute the full on-disk entry size in the data file without reading it, enabling future space-accounting features.

### Trailing CRC (4 bytes)

| Offset from entry start | Size | Field | Type   | Description                          |
|-------------------------|------|-------|--------|--------------------------------------|
| 22 + key_size           | 4    | CRC32 | u32 LE | Checksum of all preceding entry bytes |

CRC placement mirrors data file entries: accumulate header + key data in one sequential pass, then append the checksum at the end.

### Size Constants

- `kHintHeaderSize = 22` — fixed header fields
- `kHintCrcSize = 4` — trailing CRC
- Total fixed overhead: `kHintHeaderSize + kHintCrcSize = 26`
- Total entry size: `kHintHeaderSize + key_size + kHintCrcSize`

### Key Directory Rebuild (planned)

On startup, for each hint file (processed in file-name order, i.e., oldest-to-newest):
1. Read each hint entry sequentially.
2. Verify the trailing CRC; **abort (panic) on any CRC mismatch** — a bad checksum indicates corruption and the engine must not silently skip it. Truncation-based recovery will be layered on top later.
3. Insert `(key → {sequence, file_id, file_offset, value_size})` into the B-Tree. If a key already exists in the tree with an **equal or higher** sequence number, skip the insert (the stored entry is fresher).

After loading all hint files, scan the active data file (the one with no companion hint) to replay any entries written since the last rotation.

### Relationship to Data Files

| Property | Data file | Hint file |
|---|---|---|
| Extension | `.data` | `.hint` |
| Contents | Full key + value | Key + location metadata only |
| Created | At engine open / rotation | When a data file is sealed (rotated) |
| Read at startup | Only for the active file | For all sealed files |

One hint file corresponds to exactly one data file (same timestamp stem, different extension). Creation of hint files is deferred to when file rotation is implemented (BC-008).

### Module Plan

`HintFile` will live in a new `bytecask.hint_file` C++20 module (`src/engine/hint_file.cppm`), symmetric with `DataFile`:

Construction uses named static factory functions to make intent explicit at the call site:

- **`HintFile::OpenForWrite(path) -> HintFile`** — opens (or creates) the file with `O_WRONLY | O_CREAT | O_APPEND`.
- **`HintFile::OpenForRead(path) -> HintFile`** — opens an existing file with `O_RDONLY`. Read and write descriptors are kept independent so scanning a sealed hint file never interferes with an active writer.

Write API:
- **`append(sequence, file_offset, key, value_size) -> void`**: Serializes one hint entry using `CrcOutputAdapter` and writes it to the file.
- **`sync() -> void`**: `::fdatasync()` flush, decoupled from `append()` for Group Commit consistency.

Read API:
- **`read(offset) -> std::optional<std::pair<HintEntry, uint64_t>>`**: Reads the hint entry at `offset` bytes from the start of the file. Returns the parsed entry and the offset of the next entry (`offset + kHintHeaderSize + key_size + kHintCrcSize`). Pass `0` to start scanning from the beginning. Returns `std::nullopt` when `offset` equals file size (end-of-file). Panics on CRC mismatch. Typical usage: `while (auto result = file.read(offset)) { auto [entry, next] = *result; offset = next; }`.

`HintEntry` is a plain struct holding `{uint64_t sequence, uint64_t file_offset, std::vector<std::byte> key, uint32_t value_size}`.

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: bitsery v5.2.5 (header-only binary serialization)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: `DataFile` append-only writer + `HintFile` writer/reader, both backed by bitsery serialization with trailing CRC32. 57 assertions, 8 test cases.

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/data_entry.cppm`: C++23 module (`bytecask.data_entry`) — `EntryHeader`, `ReadResult`, serialization helpers
- `src/engine/data_file.cppm`: C++23 module (`bytecask.data_file`) — `DataFile` POSIX I/O
- `src/engine/hint_file.cppm`: C++23 module (`bytecask.hint_file`) — `HintFile`, `HintEntry`
- `tests/data_entry_test.cpp`: behavior tests for data entry serialization and file append
- `tests/hint_file_test.cpp`: behavior tests for hint file append, round-trip, and CRC panic
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

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints