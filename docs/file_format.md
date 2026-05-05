# ByteCaskDB File Format Reference — V01

This document specifies the V01 on-disk format for ByteCaskDB databases. It is
the authoritative reference for anyone who needs to read, write, or validate
ByteCaskDB files outside the engine itself.

## Database Directory Layout

A ByteCaskDB database is a directory containing pairs of files sharing the same
stem:

```
my_db/
├── data_20260407123456_a7f2b31e_V01.data   ← active data file (append-only)
├── data_20260407123455_3b1e04c9_V01.data   ← sealed data file (read-only)
├── data_20260407123455_3b1e04c9_V01.hint   ← companion hint file (read-only)
├── data_20260407123454_c904f182_V01.data
├── data_20260407123454_c904f182_V01.hint
└── ...
```

| Extension    | Role |
|--------------|------|
| `.data`      | Stores the full key and value for every write operation. Append-only during normal operation. |
| `.hint`      | Compact index for a sealed data file. Stores the key and file offset, but not the value. Used to rebuild the in-memory key directory at startup without re-reading values. |
| `.data.tmp`  | Partial data file being written by vacuum. Discarded at startup. |
| `.hint.tmp`  | Partial hint file being written. Discarded at startup. |

### File Naming

```
data_{YYYYMMDDHHmmss}_{RRRR}_V{XX}
```

| Field              | Description |
|--------------------|-------------|
| `YYYYMMDDHHmmss`   | UTC timestamp at second precision. Records when the file was created on disk — **not** the age of its content. After vacuum, a file may contain entries much older than its timestamp. |
| `RRRRRRRR`           | 4-byte random hex salt (8 characters, `00000000`–`ffffffff`). Prevents collisions when multiple files are created within the same second. |
| `V{XX}`            | File format version. `V01` is the initial version. The engine uses this to select the correct parser at open time. |

The timestamp is a human-readable debug hint. Filename ordering carries no
semantic meaning for content ordering — entry sequence numbers inside the files
are the authoritative ordering mechanism. Each data file has at most one
companion hint file with the same stem.

Examples:
- `data_20260407123456_a7f2b31e_V01.data`
- `data_20260407123456_a7f2b31e_V01.hint`

---

## EntryType

Both file formats share the same entry type discriminant:

| Value | Name        | Description                                    |
|-------|-------------|------------------------------------------------|
| 0x01  | `Put`       | Standard key-value write.                      |
| 0x02  | `Delete`    | Tombstone — the key is present, value is empty. |
| 0x03  | `BulkBegin` | Start-of-batch marker — key and value are empty. |
| 0x04  | `BulkEnd`   | End-of-batch marker — key and value are empty.  |
| 0x05  | `RangeDel`  | Range tombstone — key is start_key, value is end_key. Deletes all keys in [start, end). |

A zero byte in the `EntryType` field always means corrupt or uninitialized
storage. No valid type maps to 0, so the scanner can detect truncated writes
without a separate magic number.

`BulkBegin` and `BulkEnd` appear only in data files. Hint files contain only
`Put`, `Delete`, and `RangeDel` entries.

---

## Data File Format (`.data`)

Data files are pure append-only logs. Every `put`, `del`, and `apply_batch`
call appends one or more entries to the active data file. Entries are never
modified or removed in place.

### Entry Layout

```
 ┌─────────────────────────────────────────────────────┐
 │  0 –  7   sequence     u64 LE   8 bytes             │
 ├─────────────────────────────────────────────────────┤
 │  8        entry_type   u8       1 byte              │
 ├─────────────────────────────────────────────────────┤
 │  9 – 10   key_size     u16 LE   2 bytes             │
 ├─────────────────────────────────────────────────────┤
 │  11 – 14  value_size   u32 LE   4 bytes             │
 ├─────────────────────────────────────────────────────┤  ← 15-byte fixed header
 │  15 – …   key data              key_size bytes      │
 ├─────────────────────────────────────────────────────┤
 │  … – …    value data            value_size bytes    │  (0 for Delete/BulkBegin/BulkEnd)
 ├─────────────────────────────────────────────────────┤
 │  last 4   crc32c       u32 LE   4 bytes             │
 └─────────────────────────────────────────────────────┘
```

Total entry size: `15 + key_size + value_size + 4` bytes.

### Header Fields

| Offset | Size | Field        | Type   | Constraints | Description |
|--------|------|--------------|--------|-------------|-------------|
| 0      | 8    | `sequence`   | u64 LE | Globally monotonic, never 0 | Log Sequence Number (LSN) |
| 8      | 1    | `entry_type` | u8     | One of the values in the EntryType table | Entry kind |
| 9      | 2    | `key_size`   | u16 LE | 0 for `BulkBegin`/`BulkEnd`; 1–65535 otherwise | Key length in bytes |
| 11     | 4    | `value_size` | u32 LE | 0 for `Delete`/`BulkBegin`/`BulkEnd`; for `RangeDel`, holds `end_key` length | Value length in bytes |

**Hard limits**: The wire format imposes absolute ceilings of 65,535 bytes for keys (u16) and 4,294,967,295 bytes for values (u32). The engine enforces configurable limits via `Options::max_key_bytes` (default 4 KiB) and `Options::max_value_bytes` (default 4 MiB), validated at the API boundary before any data is written to disk.

### Trailing CRC

| Offset from entry start              | Size | Type   | Description |
|--------------------------------------|------|--------|-------------|
| `15 + key_size + value_size`         | 4    | u32 LE | CRC-32C (Castagnoli) over all preceding bytes of this entry (header + key + value) |

The CRC is placed at the **end** of the entry so both write and read can proceed
in a single pass: write all fields and accumulate the checksum in one loop, then
append it.

### Log Sequence Number (LSN)

The LSN is a **globally monotonic** counter across all data files and engine
sessions. It is never per-file. This is a correctness invariant: recovery
determines which of two entries for the same key is fresher by comparing LSNs
from potentially different data files. A per-file counter reset would allow
stale data to silently overwrite live data.

On startup, the engine scans all hint files to find `max_lsn`, then seeds new
writes at `max_lsn + 1`.

### Atomic Batch Framing

`apply_batch` wraps its writes in `BulkBegin`/`BulkEnd` markers. Both markers
have empty key and value (`key_size = 0`, `value_size = 0`). A batch that is
not closed by a matching `BulkEnd` before a crash is discarded entirely during
recovery — no partial-batch entries enter the key directory.

### Range Tombstone (RangeDel)

A `RangeDel` entry reuses the standard entry layout. The `key` field holds the
start key (inclusive bound) and the `value` field holds the end key (exclusive
bound). The semantics are `[start, end)` — a key `k` is deleted iff
`start <= k < end`.

On disk: `entry_type = 0x05`, `key_size = start_key length`,
`value_size = end_key length`. Entry size: `15 + start_key_size + end_key_size + 4` bytes.

### Size Constants

| Constant       | Value | Meaning |
|----------------|-------|---------|
| `kHeaderSize`  | 15    | Fixed leading fields |
| `kCrcSize`     | 4     | Trailing CRC |

---

## Hint File Format (`.hint`)

Hint files are compact companion files to sealed data files. Each entry stores
enough metadata and the full key to reconstruct the in-memory key directory
without reading value bytes. Only `Put` and `Delete` entries are written —
`BulkBegin` and `BulkEnd` are never included.

Entries are written in data-file append order (the order they appear in the
companion `.data` file). Keys are stored in full — no prefix compression.

### File Layout

```
 ┌──────────────────────────────────────────────────────┐
 │  Entry 0  (23-byte header + key_len bytes)           │
 ├──────────────────────────────────────────────────────┤
 │  Entry 1  (23-byte header + key_len bytes)           │
 ├──────────────────────────────────────────────────────┤
 │  ...                                                 │
 ├──────────────────────────────────────────────────────┤
 │  File CRC-32C  (4 bytes, file trailer)               │
 └──────────────────────────────────────────────────────┘
```

The 4-byte trailer covers all entry bytes. It is verified eagerly by
`OpenForRead` before any parsing begins.

### Entry Layout

```
 ┌─────────────────────────────────────────────────────┐
 │  0 –  7   sequence     u64 LE   8 bytes             │
 ├─────────────────────────────────────────────────────┤
 │  8        entry_type   u8       1 byte              │
 ├─────────────────────────────────────────────────────┤
 │  9 – 16   file_offset  u64 LE   8 bytes             │
 ├─────────────────────────────────────────────────────┤
 │  17 – 20  value_size   u32 LE   4 bytes             │
 ├─────────────────────────────────────────────────────┤
 │  21 – 22  key_len      u16 LE   2 bytes             │
 ├─────────────────────────────────────────────────────┤  ← 23-byte fixed header
 │  23 – …   key data              key_len bytes       │
 └─────────────────────────────────────────────────────┘
```

Total entry size: `23 + key_len` bytes.

### Header Fields

| Offset | Size | Field         | Type   | Description |
|--------|------|---------------|--------|-------------|
| 0      | 8    | `sequence`    | u64 LE | LSN copied from the data file entry |
| 8      | 1    | `entry_type`  | u8     | `Put` (0x01), `Delete` (0x02), or `RangeDel` (0x05) |
| 9      | 8    | `file_offset` | u64 LE | Byte offset of the entry in the companion `.data` file |
| 17     | 4    | `value_size`  | u32 LE | Value length in bytes (0 for `Delete`) |
| 21     | 2    | `key_len`     | u16 LE | Length of the key bytes that follow this header |

### File Trailer

| Offset from file start | Size | Type   | Description |
|------------------------|------|--------|-------------|
| `file_size - 4`        | 4    | u32 LE | CRC-32C (Castagnoli) over all bytes that precede this field |

Reading a hint file with a mismatched trailer CRC is a hard error. The engine
discards the hint file and regenerates it from the raw data file during recovery.

### Size Constants

| Constant          | Value | Meaning |
|-------------------|-------|---------|
| `kHintHeaderSize` | 23    | Fixed header fields per entry |
| File trailer      | 4     | CRC-32C trailer (one per file, not per entry) |

### RangeDel Hint Entry Extension

When `entry_type == RangeDel` (0x05), the hint entry appends the full end key
after the start key:

```
 [normal hint header: 23 bytes]
 [start_key:          key_len bytes]
 [end_key_len:        u16 LE, 2 bytes]
 [end_key:            end_key_len bytes]
```

The `value_size` field in the hint header holds the end key length (same as in
the data file).

Total RangeDel hint entry size: `23 + key_len + 2 + end_key_len` bytes.

---

## Checksums

All checksums use **CRC-32C** (Castagnoli polynomial `0x1EDC6F41`), implemented
by the [google/crc32c](https://github.com/google/crc32c) library. The library
auto-detects hardware acceleration at runtime: SSE 4.2 on x86-64, dedicated CRC
instructions on AArch64, and a software fallback otherwise.

### Data file entries — per-entry CRC

Covers: the 15-byte header + key data + value data.  
Does not cover: the 4-byte CRC field itself.

### Hint files — per-file CRC

Covers: all entry bytes from the start of the file up to (but not including)
the 4-byte trailer.  
There is no per-entry CRC in hint files.

---

## Byte Order

All multi-byte integer fields in both file formats are **little-endian**.

---

## Hint File Atomicity

Hint files are written using a temp-then-rename protocol to guarantee they are
either complete or absent — a partial hint file never exists from the engine's
perspective:

1. Write the complete hint file to `data_{stem}.hint.tmp`.
2. Call `fdatasync` to flush all bytes to physical storage.
3. Atomically `rename(2)` to `data_{stem}.hint`.

Any `.hint.tmp` file found at startup is discarded as an incomplete write
interrupted by a crash. The engine regenerates the hint from the raw data file
during that recovery run.
