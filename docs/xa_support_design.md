# ByteCaskDB XA Support Design

> **Status: very early design.** This document describes a design for generic two-phase commit primitives. Implementation and API surface are subject to change.

## Purpose

This document describes ByteCaskDB's generic two-phase commit (2PC) primitives. These allow an external coordinator to prepare, commit, or rollback batches in two distinct steps — without embedding any distributed transaction logic inside the engine.

The design supports any coordinator that follows the XA model: MariaDB's binlog group commit, an application-level outbox, or a custom distributed transaction manager. ByteCaskDB does not interpret the coordinator's metadata — it stores and returns it opaquely.

Canonical location: `docs/xa_support_design.md`.

---

## Background

ByteCaskDB's standalone write path uses `apply_batch` to atomically apply a `WritePlan`. On disk, a multi-entry batch is framed by `BulkBegin` and `BulkCommit` markers written in a single `writev` call — the batch is either fully visible or fully absent.

This works when the engine is the sole arbiter of commit. But when an external coordinator needs to interpose a decision between "data is durable" and "data is visible" — as in binlog-based replication or distributed transactions — a two-phase protocol is required.

The challenge: ByteCaskDB's append-only data file uses positional framing (`BulkBegin`...`BulkCommit`). If a batch is left open (no closing marker) while waiting for the coordinator's commit decision, no other writes can append to the file. The 2PC design solves this by introducing `BulkPrepare`, which closes the batch on disk without making it visible, freeing the data file for subsequent writes immediately.

---

## Design Principles

1. **ByteCaskDB is passive.** The engine provides prepare/commit/rollback primitives. The coordinator decides when to call them.
2. **Generic, not coordinator-specific.** `BulkPrepare` carries an opaque value blob that the engine does not interpret. The coordinator stores whatever it needs for recovery (e.g. an XID for MariaDB, a transaction ID for a custom protocol).
3. **No write blocking between prepare and commit.** `BulkPrepare` closes the batch, freeing the data file for other writers immediately.
4. **Prepared entries participate in conflict detection.** W-W conflicts are detected at prepare time, not deferred to commit. A concurrent writer touching the same keys is rejected immediately.
5. **The data file is the durable store.** No separate prepare log or WAL. Prepared entries live in the same append-only data files as committed entries.

---

## Batch Marker Types

Five marker types cover both standalone and 2PC paths:

| Marker | Payload | Written with | Purpose |
|--------|---------|--------------|---------|
| `BulkBegin` | — | `writev` (atomic with entries + closing marker) | Start a batch |
| `BulkCommit` | — | `writev` (atomic with `BulkBegin` + entries) | End batch, make visible (standalone) |
| `BulkPrepare` | opaque value blob | `writev` (atomic with `BulkBegin` + entries) | End batch, W-W conflicts active, not visible (2PC prepare) |
| `Bulk2PCCommit` | `u64 begin_sequence` | separate append | Make a prepared batch visible (2PC commit) |
| `Bulk2PCRollback` | `u64 begin_sequence` | separate append | Revert a prepared batch (2PC rollback) |

`BulkBegin`, `BulkCommit`, and `BulkPrepare` are always written together in a single `writev` — no interleaving with other writes is possible. `Bulk2PCCommit` and `Bulk2PCRollback` are written separately, after the coordinator's decision.

### EntryType values

Extends the existing `EntryType` enum (see `docs/file_format.md`):

| Value | Name | Key | Value |
|-------|------|-----|-------|
| 0x01 | `Put` | key bytes | value bytes |
| 0x02 | `Delete` | key bytes | empty |
| 0x03 | `BulkBegin` | empty | empty |
| 0x04 | `BulkCommit` | empty | empty |
| 0x05 | `RangeDel` | start_key | end_key |
| 0x06 | `BulkPrepare` | empty | opaque coordinator data |
| 0x07 | `Bulk2PCCommit` | empty | `u64 LE begin_sequence` (8 bytes) |
| 0x08 | `Bulk2PCRollback` | empty | `u64 LE begin_sequence` (8 bytes) |

`BulkCommit` (0x04) is the renamed `BulkEnd`. The on-disk byte value is unchanged — only the name changes in the codebase.

---

## On-Disk Layout

### Standalone path (unchanged)

```
BulkBegin          (seq=100)  ──┐
Put  key_a val_a   (seq=101)    │ single writev
Put  key_b val_b   (seq=102)    │
BulkCommit         (seq=103)  ──┘  ← batch visible immediately
```

### 2PC path

```
BulkBegin          (seq=200)  ──┐
Put  key_x val_x   (seq=201)    │ single writev (prepare)
Put  key_y val_y   (seq=202)    │
BulkPrepare(xid)   (seq=203)  ──┘  ← block closed, W-W conflicts active, not visible
-- fdatasync --
...                                 ← other standalone or 2PC writes can proceed here
Bulk2PCCommit(200) (seq=210)        ← batch now visible (commit)
```

### 2PC rollback

```
BulkBegin          (seq=300)  ──┐
Put  key_z val_z   (seq=301)    │ single writev (prepare)
BulkPrepare(xid)   (seq=302)  ──┘  ← prepared, not visible
-- fdatasync --
...
Bulk2PCRollback(300) (seq=315)      ← batch reverted, entries become dead
```

After rollback, the key directory changes from the prepared batch are undone. The on-disk entries remain (append-only file) but are dead — reclaimable by vacuum.

---

## Conflict Detection

Prepared entries update the key directory immediately at prepare time. This means:

- A concurrent `apply_batch` that touches any key in the prepared batch sees a sequence mismatch and returns `false` (W-W conflict). **Conflicts are detected at prepare time, not deferred to commit.**
- Reads do not see prepared entries — visibility requires `Bulk2PCCommit`.
- If the prepared batch is rolled back, the key directory is restored to its pre-prepare state. Any concurrent batch that was rejected due to conflict with the prepared batch would need to be retried — this is consistent with OCC semantics.

---

## Recovery

On startup, recovery processes batch markers to determine the state of each batch:

### Algorithm

1. Scan all data files forward. For each file, track:
   - `BulkBegin` → open a pending batch (keyed by `begin_sequence`).
   - `BulkCommit` → close the pending batch, apply entries to key directory.
   - `BulkPrepare` → close the pending batch, mark as **prepared** (not yet applied).
   - No closing marker before end of file → **incomplete**, discard all entries since `BulkBegin`.

2. After scanning all files, collect resolution markers:
   - `Bulk2PCCommit(begin_seq)` → add `begin_seq` to the **committed** set.
   - `Bulk2PCRollback(begin_seq)` → add `begin_seq` to the **rolled-back** set.

3. Resolve each prepared batch:
   - `begin_seq` in committed set → apply entries to key directory.
   - `begin_seq` in rolled-back set → discard entries.
   - `begin_seq` in neither set → **unresolved**. Report to the external coordinator (e.g. via `recover()`) so it can decide.

### Recovery outcome table

| On-disk state | Recovery action |
|---------------|-----------------|
| `BulkBegin` + entries + `BulkCommit` | Apply (standalone commit) |
| `BulkBegin` + entries + `BulkPrepare` + `Bulk2PCCommit` | Apply (2PC commit) |
| `BulkBegin` + entries + `BulkPrepare` + `Bulk2PCRollback` | Discard (2PC rollback) |
| `BulkBegin` + entries + `BulkPrepare`, no resolution | Unresolved — report to coordinator |
| `BulkBegin` + entries, no closing marker | Incomplete write — discard |

### Hint files

`BulkPrepare`, `Bulk2PCCommit`, and `Bulk2PCRollback` are **never** written to hint files — same as `BulkBegin` and `BulkCommit`. Hint files contain only `Put`, `Delete`, and `RangeDel` entries. Recovery of 2PC state requires scanning the data files for these markers (only on startup, not during normal hint-based recovery — unless unresolved prepared batches exist).

---

## Integration Points

The engine exposes these operations to the coordinator. The exact API is deferred — this section describes the semantic contract.

### `prepare(plan, value) -> begin_sequence`

Atomically writes `BulkBegin` + entries + `BulkPrepare(value)` and calls `fdatasync`. Returns the `begin_sequence` that identifies the prepared batch. The batch is durable, triggers W-W conflict detection, but is not visible to readers.

The `value` is opaque — the engine stores it as-is and returns it during `recover()`. The coordinator stores whatever it needs for crash recovery (e.g. an XID, a transaction ID, a correlation key).

### `commit_prepared(begin_sequence)`

Appends `Bulk2PCCommit(begin_sequence)`. Makes the prepared batch visible to readers. May be called with or without `fdatasync` — the coordinator decides based on its own durability model (e.g. MariaDB's binlog group fsync covers durability, so the engine skips its own sync).

### `rollback_prepared(begin_sequence)`

Appends `Bulk2PCRollback(begin_sequence)`. Reverts the key directory changes from the prepared batch. On-disk entries become dead (reclaimable by vacuum).

### `recover() -> list of (begin_sequence, value)`

Returns all unresolved prepared batches — those with `BulkPrepare` but no `Bulk2PCCommit` or `Bulk2PCRollback`. The coordinator reads the opaque `value` to identify the transaction and decides whether to commit or rollback.

Called during startup, before the engine accepts writes.

---

## MariaDB Integration

MariaDB is the first consumer of these primitives. The MariaDB storage engine plugin maps them to the handler 2PC interface:

| Handler method | Engine primitive | Notes |
|----------------|-----------------|-------|
| `prepare_ordered()` | `prepare(plan, xid)` | XID stored in `BulkPrepare` value |
| `commit_ordered()` | `commit_prepared(begin_seq)` | `sync=false` — binlog group fsync covers durability |
| `ha_recover()` | `recover()` | Returns XIDs from unresolved `BulkPrepare` blocks |
| `commit_by_xid(xid)` | `commit_prepared(begin_seq)` | Maps XID → begin_sequence |
| `rollback_by_xid(xid)` | `rollback_prepared(begin_seq)` | Maps XID → begin_sequence |

See `docs/mariadb_engine_design.md` Phase 6 for the full MariaDB-specific integration details including binlog group commit interaction and handler lifecycle wiring.

---

## Interaction with Other Features

### Vacuum

Prepared-but-uncommitted entries occupy space in data files. After rollback (or after commit + subsequent overwrites), these entries become dead and are reclaimable by vacuum. Vacuum must not reclaim entries from unresolved prepared batches — the coordinator may still commit them.

### Replication (`changes_since`)

The `changes_since` iterator (see `docs/replication_primitives_design.md`) only yields entries from fully committed batches. Prepared batches without `Bulk2PCCommit` are filtered out. This ensures followers only ingest committed state.

### Snapshots

A snapshot taken while a batch is prepared-but-not-committed does not include the prepared entries (they are not visible). A snapshot taken after `Bulk2PCCommit` includes them.
