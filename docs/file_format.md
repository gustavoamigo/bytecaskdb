# ByteCask File Format Reference

This document specifies the on-disk format for ByteCask databases. It is the
authoritative reference for anyone who needs to read, write, or validate
ByteCask files outside the engine itself.

## Database Directory Layout

A ByteCask database is a directory containing pairs of files sharing the same
timestamp stem:

```
my_db/
├── data_20260407123456000001.data   ← active data file (append-only)
├── data_20260407123455000001.data   ← sealed data file (read-only)
├── data_20260407123455000001.hint   ← companion hint file (read-only)
├── data_20260407123454000001.data
├── data_20260407123454000001.hint
└── ...
```

| Extension    | Role |
|--------------|------|
| `.data`      | Stores the full key and value for every write operation. Append-only during normal operation. |
| `.hint`      | Compact index for a sealed data file. Stores the key and file offset, but not the value. Used to rebuild the in-memory key directory at startup without re-reading values. |
| `.data.tmp`  | Partial data file being written by vacuum. Discarded at startup. |
| `.hint.tmp`  | Partial hint file being written. Discarded at startup. |

### File Naming

Files are named with a timestamp stem:

```
data_{YYYYMMDDHHmmssUUUUUU}
```

where `UUUUUU` is the microsecond sub-second component (zero-padded, 6 digits).

Examples:
- `data_20260407123456000001.data`
- `data_20260407123456000001.hint`

Lexicographic sort equals chronological order. Each data file has at most one
companion hint file with the same stem.

---

## EntryType

Both file formats share the same entry type discriminant:

| Value | Name        | Description                                    |
|-------|-------------|------------------------------------------------|
| 0x01  | `Put`       | Standard key-value write.                      |
| 0x02  | `Delete`    | Tombstone — the key is present, value is empty. |
| 0x03  | `BulkBegin` | Start-of-batch marker — key and value are empty. |
| 0x04  | `BulkEnd`   | End-of-batch marker — key and value are empty.  |

A zero byte in the `EntryType` field always means corrupt or uninitialized
storage. No valid type maps to 0, so the scanner can detect truncated writes
without a separate magic number.

`BulkBegin` and `BulkEnd` appear only in data files. Hint files contain only
`Put` and `Delete` entries.

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
| 11     | 4    | `value_size` | u32 LE | 0 for `Delete`/`BulkBegin`/`BulkEnd` | Value length in bytes |

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

`flush_hints_for()` writes entries in **ascending key order** so that each entry
can share a prefix with the previous key (prefix compression).

### File Layout

```
 ┌──────────────────────────────────────────────────────┐
 │  Entry 0  (24-byte header + suffix_len bytes)        │
 ├──────────────────────────────────────────────────────┤
 │  Entry 1  (24-byte header + suffix_len bytes)        │
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
 │  21       prefix_len   u8       1 byte              │
 ├─────────────────────────────────────────────────────┤
 │  22 – 23  suffix_len   u16 LE   2 bytes             │
 ├─────────────────────────────────────────────────────┤  ← 24-byte fixed header
 │  24 – …   suffix data           suffix_len bytes    │
 └─────────────────────────────────────────────────────┘
```

Total entry size: `24 + suffix_len` bytes.

### Header Fields

| Offset | Size | Field         | Type   | Description |
|--------|------|---------------|--------|-------------|
| 0      | 8    | `sequence`    | u64 LE | LSN copied from the data file entry |
| 8      | 1    | `entry_type`  | u8     | `Put` (0x01) or `Delete` (0x02) only |
| 9      | 8    | `file_offset` | u64 LE | Byte offset of the entry in the companion `.data` file |
| 17     | 4    | `value_size`  | u32 LE | Value length in bytes (0 for `Delete`) |
| 21     | 1    | `prefix_len`  | u8     | Bytes shared with the previous entry's key (0 for the first entry; capped at 255) |
| 22     | 2    | `suffix_len`  | u16 LE | Length of the suffix bytes that follow this header |

### Key Reconstruction (Prefix Compression)

The full key for entry N is:

```
key[N] = key[N-1][0 .. prefix_len) || suffix_data
```

For the first entry `prefix_len` is always 0, so `key[0] = suffix_data`.

A reader must maintain a rolling key buffer (`key_buf`), resize to
`prefix_len + suffix_len` bytes, and overwrite only the suffix portion:

```
key_buf.resize(prefix_len + suffix_len)
key_buf[prefix_len .. prefix_len + suffix_len) = suffix_data
```

The `key_buf` contents are valid only until the next entry is read.

### File Trailer

| Offset from file start | Size | Type   | Description |
|------------------------|------|--------|-------------|
| `file_size - 4`        | 4    | u32 LE | CRC-32C (Castagnoli) over all bytes that precede this field |

Reading a hint file with a mismatched trailer CRC is a hard error. The engine
discards the hint file and regenerates it from the raw data file during recovery.

### Size Constants

| Constant          | Value | Meaning |
|-------------------|-------|---------|
| `kHintHeaderSize` | 24    | Fixed header fields per entry |
| File trailer      | 4     | CRC-32C trailer (one per file, not per entry) |

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
