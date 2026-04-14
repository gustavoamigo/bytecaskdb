# Write-Path Failure Mode Comparison

Comparison of how ByteCaskDB handles write-path failure modes versus five
other storage engines: RocksDB, LevelDB, SQLite WAL, LMDB, and WiredTiger.

Failure classes follow the taxonomy defined in
[`correctness_validation.md`](correctness_validation.md).

---

## Architecture Summary

The commit primitive and visibility gate determine what "partial write" and
"state publish" mean in each engine.

| Engine | Commit primitive | Visibility gate | Durability before visibility? |
|---|---|---|---|
| **ByteCaskDB** | `writev` append + CRC per entry | `state_.store()` atomic pointer swap | Yes — `fdatasync` before `state_.store()` |
| **RocksDB** | WAL append + CRC32C per record | Sequence number publish after memtable insert | Yes — in sync mode |
| **LevelDB** | WAL append + CRC per record | `mem_->Add()` — memtable insert | Yes — sync mode: sync before `Add()` |
| **SQLite WAL** | WAL frames + cumulative CRC + commit-frame marker | WAL-index `mxFrame` update | Yes — `FULL` mode; deferred in `NORMAL` |
| **LMDB** | COW B-tree pages + meta-page flip | Meta-page atomic write | Yes — meta written only after data flush |
| **WiredTiger** | Append-only log + CRC32C + slot consolidation | LSN advancement after slot release | Yes — in sync mode |

---

## Comparative Matrix

| Failure class | ByteCaskDB | RocksDB | LevelDB | SQLite WAL | LMDB | WiredTiger |
|---|---|---|---|---|---|---|
| **B1** — `writev = -1`, nothing on disk | Throws; no state change; no poison | `seen_error` latch on writer; `SetBGError` at severity level; write not visible | `bg_error_` permanently set; DB poisoned | `PAGER_ERROR`; transaction rolled back; recoverable without reopen | Transaction aborted; error code returned; no poison unless meta-page write involved | `WT_RET_PANIC`; connection immediately poisoned |
| **B2** — partial bytes on disk (short write) | Multi-entry: isolate via isolation rotation. Single-entry: poison | Undetectable at write time (`Append` returns `Status`, not byte count); caught at recovery via CRC mismatch | Same structural blind spot as RocksDB; tail corruption tolerated at recovery | Cumulative checksum + commit-frame requirement; partial frame invisible by construction | Data-page partial: safe (not reachable from valid meta); meta-page partial: old meta intact | Pre-zeroed files + trailing-zero heuristic + CRC; recovery stops at partial record |
| **B3** — all bytes on disk, valid CRC, error returned | Multi: orphaned, isolation-rotated. Single: poison; recovery **will** find the write | Treated as B1 at write time (poison); valid CRC record replayed in `kPointInTimeRecovery` | Treated as B1 (poison); valid record replayed on recovery | `PAGER_ERROR` at write time; if commit frame present, full transaction replayed from WAL | Data-page: error returned, no poison; on next open, committed meta found and replayed | Treated as B1 (panic); valid CRC → record replayed during `__wt_log_scan` |
| **F** — `fdatasync` fails after state publish | **Throws, NOT poisoned**; transition in page cache; no retry obligation | `SyncInternal` fails → `SetBGError(kHardError+)`; write not visible (sync precedes sequence publish); `DB::Resume()` or close+reopen | `logfile_->Sync()` fails → `bg_error_` permanently set; write not visible; must close+reopen | `PAGER_ERROR`; recoverable within same connection; under `NORMAL` sync, per-commit sync is deferred | `MDB_FDATASYNC` failure → meta write blocked → transaction aborted; likely `MDB_FATAL_ERROR`; close+reopen | `ENOTSUP` silently disables dirty sync; other errors propagated; log server error → panic |
| **G** — post-write rotation `fdatasync` fails | **Throws, NOT poisoned**; transition persisted in page cache; retry on next write | `SwitchMemtable` sync failure → `SetBGError(Corruption)` → close+reopen required | Old `logfile_->Close()` failure → `bg_error_` permanently set; close+reopen | Checkpoint sync failure: `SQLITE_IOERR`; connection remains open; pager error state, recoverable | N/A — no log rotation | Rotation sync failure: propagated error; subsequent write on stalled file → `WT_RET_PANIC` |
| **H** — active file sealed, new file creation fails | Poison; restart required | Before any writes: recoverable per-write error. After committed writes: `SetBGError(Corruption)` → close+reopen | New log creation fails: recoverable (file number reclaimed). Old log close fails: permanent poison | WAL created once at mode activation; creation failure → mode reverts to prior journaling; not analogous | N/A — single-file environment | File creation: propagated error, no immediate panic; after ~10K yield attempts: `EBUSY`; subsequent write failure → panic |

---

## Key Observations

### 1. Class F/G: ByteCaskDB publishes before durability — an intentional outlier

Every other engine gates reader visibility behind a successful `fdatasync`.
ByteCaskDB publishes state before the exception propagates and does not poison
on F or G, accepting "page-cache durability" as sufficient. The rationale is
coherent for an append-only file: a clean process shutdown flushes the page
cache; the next successful `fdatasync` on the same file covers all preceding
pages; and a power failure between the failed sync and one of those events is
the only remaining risk. No other engine among the five takes this position.

This is a documented, deliberate tradeoff: a transition in state F or G has
`writev`-level persistence (survives clean shutdown) but not `fdatasync`-level
durability (may be lost on power failure or kernel crash before the next sync).

### 2. Partial write detection varies widely

SQLite WAL is the strongest: its cumulative checksum across all prior frames
plus the commit-frame requirement makes partial writes or uncommitted
transactions invisible by construction, even without OS-level notification.
WiredTiger reinforces CRC with a pre-zeroed file heuristic (trailing zero byte
signals a possible partial record). RocksDB and LevelDB share a structural gap:
`FSWritableFile::Append()` / `WritableFile::Append()` returns a `Status`, not
a byte count, so a short write that appears successful to the OS is undetectable
at write time and surfaces only as a CRC mismatch on recovery. LMDB avoids the
partial-write problem structurally for data pages — COW B-tree pages are only
reachable after the meta-page flip, so partial data-page writes are invisible
to any reader.

### 3. Poison semantics diverge significantly on sync failure

| Engine | Permanent poison? | In-session recovery? | Resume API? |
|---|---|---|---|
| ByteCaskDB | Yes (`DbPoisoned`), but not on F/G | No | No |
| RocksDB | Severity-dependent | `DB::Resume()` for kHardError | Yes |
| LevelDB | Yes — one-way latch | No | No |
| SQLite WAL | No — `PAGER_ERROR` is recoverable | Yes (discard page cache) | Implicit |
| LMDB | Yes (meta-page failures) | No | No |
| WiredTiger | Yes (write failures panic immediately) | No | No |

ByteCaskDB's future `DbDegraded → DbPoisoned` two-tier model would align it
more closely with RocksDB's severity tiers than with LevelDB's one-way latch.
See the project memory on this design direction.

### 4. Rotation failure asymmetry in LevelDB

LevelDB distinguishes within the same rotation event: failing to create the
*new* log file is recoverable (the old file is still intact and writable);
failing to *close* the old file permanently poisons the DB. ByteCaskDB's class
H follows the same conservative logic — when the active file is sealed but the
new file cannot be created, the sealed file cannot accept further appends, so
the only safe response is to poison.

### 5. Recovery mode configurability

RocksDB uniquely exposes four explicit WAL recovery modes:

- `kTolerateCorruptedTailRecords` — ignores errors at the log tail (crash-truncated writes tolerated)
- `kPointInTimeRecovery` (default since v6.6) — halts replay at first CRC mismatch; consistent point-in-time state
- `kAbsoluteConsistency` — any replay error → recovery fails → DB unusable
- `kSkipAnyCorruptedRecords` — ignores all errors; disaster recovery only

All other engines have fixed recovery behavior. ByteCaskDB's append+hint-file
recovery is structurally closest to WiredTiger's `__wt_log_scan`: scan forward,
CRC-verify each record, stop at the first invalid entry. Hint files provide a
parallel-reconstruction acceleration path on top of that.

---

## Sources and Methodology

Behavior described above is derived from engine source code and documentation
as of early 2026. Specific mechanisms cited:

- **RocksDB**: `WritableFileWriter::WriteBuffered`, `ErrorHandler::SetBGError`,
  `SwitchMemtable`, WAL recovery mode enum in `options.h`
- **LevelDB**: `log::Writer::AddRecord`, `DBImpl::MakeRoomForWrite`,
  `RecordBackgroundError` comment ("We may have lost some data written to the
  previous log file")
- **SQLite WAL**: pager state machine (`PAGER_ERROR` → `PAGER_OPEN`),
  WAL-index `mxFrame` update under `FULL` sync, WAL frame checksum algorithm
  (Fibonacci-weighted cumulative CRC)
- **LMDB**: `mdb_page_flush`, `MDB_FDATASYNC`, `MDB_FATAL_ERROR` flag
  (`0x80000000` on `me_flags`), two-meta-page design header comment
- **WiredTiger**: `__log_fs_write` / `WT_RET_PANIC`, `__log_check_partial_write`
  trailing-zero heuristic, `__wt_log_scan` salvage mode
