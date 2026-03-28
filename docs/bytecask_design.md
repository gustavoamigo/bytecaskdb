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
| Value Data       | value_size bytes (0 if deleted)
+------------------+
| CRC32            | 4 bytes (trailing)
+------------------+
```

### Leading Header (15 bytes)

| Offset | Size | Field      | Type   | Description                              |
|--------|------|------------|--------|------------------------------------------|
| 0      | 8    | Sequence   | u64 LE | Monotonic sequence number                |
| 8      | 2    | Key Size   | u16 LE | Key length (max 65,535 bytes)            |
| 10     | 4    | Value Size | u32 LE | Value length (0 for tombstone, max 4GB)  |
| 14     | 1    | Flags      | u8     | Bit 0: deleted flag, Bits 1-7: reserved  |

### Trailing CRC (4 bytes)

| Offset from start of entry      | Size | Field | Type   | Description                    |
|---------------------------------|------|-------|--------|--------------------------------|
| 15 + key_size + value_size      | 4    | CRC32 | u32 LE | Checksum of all preceding bytes |

CRC is at the **end** of the entry so both write and read can be done in a single pass: write all fields and accumulate CRC in one loop, then append the checksum.

### Size constants

- `kHeaderSize = 15` — fixed leading fields (sequence + key_size + value_size + flags)
- `kCrcSize = 4` — trailing CRC
- Total entry size: `kHeaderSize + key_size + value_size + kCrcSize`

### Flags Specification

- Bit 0: `0x01` = Deleted (tombstone), `0x00` = Active
- Bits 1-7: Reserved (must be 0)

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
- **`append(key, value) -> std::uint64_t`**: Serializes a new entry with auto-incrementing sequence number, writes it to the OS page cache via `::write()`, and returns the byte offset where the entry starts. Does **not** guarantee durability on its own.
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

### Current scope boundaries

- Flags are always 0 (active) — tombstone writes are deferred.
- No file rotation or size limits.
- No read path — append-only for now.

## Current implementation state

- Language: C++23
- Build system: xmake
- Dependencies: bitsery v5.2.5 (header-only binary serialization)
- Primary target: `bytecask` (includes `src/*.cpp` + `src/engine/*.cppm`)
- Test target: `bytecask_tests` (includes `tests/*.cpp` + `src/engine/*.cppm`)
- Status: `DataFile` append-only writer with bitsery-backed serialization, trailing CRC32, and `CrcOutputAdapter` for reusable one-pass checksum computation

## Current repository structure

- `src/main.cpp`: temporary executable entry point
- `src/engine/data_entry.cppm`: C++23 module (`bytecask.data_entry`) — `DataFile`, `EntryHeader`, CRC32, serialization
- `tests/data_entry_test.cpp`: behavior tests for data entry serialization and file append
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