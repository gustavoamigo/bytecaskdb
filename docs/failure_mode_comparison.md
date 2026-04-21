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
| **B1** — write call returns an error before any bytes reach disk | Throws; degrades (`DbDegraded`); `resume()` recovers. Any `writev` failure — including a possible partial write on FUSE/network filesystems — is treated as indeterminate. No in-flight recovery is attempted. | `seen_error` latch on writer; `SetBGError` at severity level; write not visible | `bg_error_` permanently set; DB poisoned | `PAGER_ERROR`; transaction rolled back; recoverable without reopen | Transaction aborted; error code returned; no poison unless meta-page write involved | `WT_RET_PANIC`; connection immediately poisoned |
| **B2** — partial bytes on disk (short write) | Throws; degrades unconditionally (`DbDegraded`); `resume()` recovers | Undetectable at write time (`Append` returns `Status`, not byte count); caught at recovery via CRC mismatch | Same structural blind spot as RocksDB; tail corruption tolerated at recovery | Cumulative checksum + commit-frame requirement; partial frame invisible by construction | Data-page partial: safe (not reachable from valid meta); meta-page partial: old meta intact | Pre-zeroed files + trailing-zero heuristic + CRC; recovery stops at partial record |
| **B3** — all bytes on disk, valid CRC, error returned | Throws; degrades unconditionally. A best-effort sync before degrading maximises the chance that page-cache bytes reach durable storage, so `resume()` can replay valid committed entries. | Treated as B1 at write time (poison); valid CRC record replayed in `kPointInTimeRecovery` | Treated as B1 (poison); valid record replayed on recovery | `PAGER_ERROR` at write time; if commit frame present, full transaction replayed from WAL | Data-page: error returned, no poison; on next open, committed meta found and replayed | Treated as B1 (panic); valid CRC → record replayed during `__wt_log_scan` |
| **F** — commit `fdatasync` fails (bytes in page cache, not confirmed durable) | **Throws, degraded (`DbDegraded`)**; key changes not published; `next_lsn` advanced past consumed LSNs; `resume()` required before further writes. If page-cache bytes survive to `resume()`, the write is committed at that point. | `SyncInternal` fails → `SetBGError(kHardError+)`; write not visible (sync precedes sequence publish); `DB::Resume()` or close+reopen | `logfile_->Sync()` fails → `bg_error_` permanently set; write not visible; must close+reopen | `PAGER_ERROR`; recoverable within same connection; under `NORMAL` sync, per-commit sync is deferred | `MDB_FDATASYNC` failure → meta write blocked → transaction aborted; likely `MDB_FATAL_ERROR`; close+reopen | `ENOTSUP` silently disables dirty sync; other errors propagated; log server error → panic |
| **G** — post-write rotation `fdatasync` fails | **Throws, degraded (`DbDegraded`)**; key changes not published; `next_lsn` advanced; `resume()` required before further writes. | `SwitchMemtable` sync failure → `SetBGError(Corruption)` → close+reopen required | Old `logfile_->Close()` failure → `bg_error_` permanently set; close+reopen | Checkpoint sync failure: `SQLITE_IOERR`; connection remains open; pager error state, recoverable | N/A — no log rotation | Rotation sync failure: propagated error; subsequent write on stalled file → `WT_RET_PANIC` |
| **H** — active file sealed, new file creation fails | Degrade (`DbDegraded`); `resume()` creates new active file without restart | Before any writes: recoverable per-write error. After committed writes: `SetBGError(Corruption)` → close+reopen | New log creation fails: recoverable (file number reclaimed). Old log close fails: permanent poison | WAL created once at mode activation; creation failure → mode reverts to prior journaling; not analogous | N/A — single-file environment | File creation: propagated error, no immediate panic; after ~10K yield attempts: `EBUSY`; subsequent write failure → panic |

---

## Key Observations

### 1. Class F/G: ByteCaskDB degrades on `fdatasync` failure

All six engines agree that a failed `fdatasync` must not make a write
visible. Key changes are not published on sync failure.
`next_lsn` is still advanced past consumed sequence numbers to prevent LSN
reuse for bytes now in the page cache.

ByteCaskDB now degrades on F and G: writes throw `DbDegraded` until
`resume()` is called. When `fdatasync` fails, bytes are in the page cache
but not confirmed durable, and the key directory does not reflect those
bytes. Continuing to accept writes in this state would leave data on disk
that recovery could replay, but that is invisible to the in-memory state —
a divergence. `resume()` scans the active file, replays any valid committed
entries (including any F/G bytes that survived in the page cache), and
creates a fresh active file. If the page-cache bytes survive to `resume()`,
they are committed at that point.

LevelDB permanently latches `bg_error_`; RocksDB sets `kHardError`
(requiring `DB::Resume()` or close+reopen); LMDB escalates to
`MDB_FATAL_ERROR`; WiredTiger panics the connection. SQLite WAL is the
closest: `PAGER_ERROR` is recoverable within the same connection without
restart — analogous to ByteCaskDB's `DbDegraded` + `resume()`. The
`DbDegraded` + `DB::resume()` design extends this to all failure classes
(B1/B2/B3/C/F/G/H): the engine degrades rather than permanently blocking,
and `resume()` restores normal operation without a restart.

### 2. The `fdatasync` trust assumption and fsyncgate

All engines in this comparison trust `fdatasync`'s return value: success
means the data is durable. On Linux, this trust has a known gap — the
"fsyncgate" issue (2018) showed that on some kernel/filesystem
combinations, `fsync` can return success after an earlier async writeback
error was consumed by another fd, because the kernel clears the error
flag on the first `fsync` call against the inode.

Engine responses vary:

| Engine | Response |
|--------|----------|
| PostgreSQL | `PANIC` on any `fsync` error; writeback error tracking added post-2018 |
| RocksDB | `track_and_verify_wals_in_manifest` option; verify WAL checksums against manifest at open |
| WiredTiger | `WT_RET_PANIC` on any write error — fsyncgate is subsumed by the general policy |
| LevelDB | No specific mitigation; permanent `bg_error_` latch on any error |
| SQLite WAL | Relies on the VFS abstraction; platform-specific VFS can add writeback tracking |
| LMDB | COW + meta-page flip sidesteps — a successful `msync`/`fdatasync` of the meta page is the only sync that matters |
| ByteCaskDB | Trusts `fdatasync` return value; no writeback error tracking |

ByteCaskDB's position: this is a deliberate simplicity choice for an
embedded engine targeting local storage with reliable `fdatasync`
semantics (ext4, XFS, ZFS on Linux ≥ 4.13). The assumption is documented
in `CONTRACT.md` so it can be revisited for deployments on storage
configurations where it may not hold.

### 3. Partial write detection varies widely

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

### 4. Degraded/poison semantics diverge significantly on failure

| Engine | Permanent block? | In-session recovery? | Resume API? |
|---|---|---|---|
| ByteCaskDB | No — `DbDegraded` (B1/B2/B3/C/F/G/H); `resume()` restores writes | Yes — `resume()` | `DB::resume()` |
| RocksDB | Severity-dependent | `DB::Resume()` for kHardError | Yes |
| LevelDB | Yes — one-way latch | No | No |
| SQLite WAL | No — `PAGER_ERROR` is recoverable | Yes (discard page cache) | Implicit |
| LMDB | Yes (meta-page failures) | No | No |
| WiredTiger | Yes (write failures panic immediately) | No | No |

ByteCaskDB's `DbDegraded` + `resume()` model aligns it
with RocksDB's severity tiers: all IO failure classes (B1/B2/B3/C/F/G/H)
enter a degraded state, and `resume()` performs a universal recovery
(scan → truncate → sync → rotate) to restore normal operation.

### 5. Rotation failure asymmetry in LevelDB

LevelDB distinguishes within the same rotation event: failing to create the
*new* log file is recoverable (the old file is still intact and writable);
failing to *close* the old file permanently poisons the DB. ByteCaskDB's class
H follows the same conservative logic — when the active file is sealed but the
new file cannot be created, the sealed file cannot accept further appends.
ByteCaskDB degrades rather than permanently blocking: `resume()` creates a
fresh active file and restores writes without a restart.

### 6. Recovery mode configurability

RocksDB uniquely exposes four explicit WAL recovery modes:

- `kTolerateCorruptedTailRecords` — ignores errors at the log tail (crash-truncated writes tolerated)
- `kPointInTimeRecovery` (default since v6.6) — halts replay at first CRC mismatch; consistent point-in-time state
- `kAbsoluteConsistency` — any replay error → recovery fails → DB unusable
- `kSkipAnyCorruptedRecords` — ignores all errors; disaster recovery only

All other engines have fixed recovery behavior. ByteCaskDB's append+hint-file
recovery is structurally closest to WiredTiger's `__wt_log_scan`: scan forward,
CRC-verify each record, stop at the first invalid entry. Hint files provide a
parallel-reconstruction acceleration path on top of that.

### 7. No engine retries hardware IO errors — fail-fast is the universal strategy

None of the five comparison engines retry a failed `write()` or `fdatasync()`
that returns a hardware error (`EIO`). The dominant pattern is **fail-fast and
propagate**: report the error to the caller immediately and let the application
or orchestrator decide the response.

Some engines do retry *transient* interruptions: PostgreSQL's `pg_pwrite()`
wraps `pwrite()` in a retry loop for `EINTR`, and WAL writes continue on short
writes (writing remaining bytes). SQLite's VFS retries `EINTR` on `xWrite`.
These are not IO retries in the hardware-error sense — they handle signals and
partial completions that are expected under normal operation. The universal rule
is: **don't retry `EIO`**, not "don't retry any IO."

The reasoning is consistent across all engines:

- **Masking hardware problems.** A transient `EIO` may be the first symptom
  of a dying disk. Retrying and succeeding once does not mean the next write
  will land.
- **Violating ordering guarantees.** If a retry changes the order of writes
  relative to visibility, durability invariants break.
- **Unbounded latency.** POSIX provides no timeout mechanism for regular file
  IO (see observation 9), so retrying disk IO means the engine has no way to
  bound how long a caller waits.

Engines differ only in *how aggressively* they react after propagation:

| Reaction | Engines | Behavior |
|---|---|---|
| Instant panic / poison | WiredTiger, LevelDB | One IO error → entire DB unusable until restart |
| Severity-tiered poison | RocksDB | Soft errors recoverable via `DB::Resume()`; hard errors require restart |
| Recoverable in-session | SQLite WAL, ByteCaskDB | `PAGER_ERROR` → discard dirty pages → next transaction clean; `DbDegraded` → `resume()` → writes restored |
| Propagate, don't degrade (sync failures) | ByteCaskDB (class F/G) | Throw to caller; don't latch error state |

WiredTiger is the extreme: `WT_RET_PANIC` on *any* write failure. The MongoDB
team's position is that crashing the node and letting the replica set elect a
new primary is preferable to serving reads from a potentially inconsistent
state. LevelDB's one-way `bg_error_` latch is only slightly less aggressive.

PostgreSQL is a notable external reference: despite retrying `EINTR` and short
writes (see above), `fdatasync` failures are **not** retried — they trigger a
`PANIC` and crash recovery.

No production storage engine uses exponential backoff for disk IO. Backoff is
a network/contention strategy. Disk IO either works or it doesn't — retrying
a `write()` that returned `EIO` after a delay will not help because the failure
indicates a hardware or filesystem problem, not transient contention. The only
backoff-like behavior in this space is lock-contention retry: SQLite's
`sqlite3_busy_timeout` (stepped retry for lock contention, not IO errors) and
WiredTiger's log-slot yield loop (~10K iterations waiting for a consolidation
slot under high concurrency — spin-wait for a lock, not IO retry).

### 8. Idempotency strategies for write-path recovery

After a failed IO, the engine often cannot determine whether the operation
partially completed. Each engine's idempotency strategy determines how safely
the same write can be re-attempted.

| Strategy | Used by | Mechanism |
|---|---|---|
| Append-only + CRC | ByteCaskDB, RocksDB, LevelDB, WiredTiger | Appending is naturally idempotent for failure: a partial append is detected by CRC on recovery and truncated. The write can be re-appended. |
| COW + meta-page flip | LMDB | Data pages are written to new locations; the meta-page flip is the atomic commit. A failed write leaves no trace in the committed state — retry the entire transaction. |
| Cumulative CRC + commit-frame | SQLite WAL | The commit frame is the idempotency boundary. Without it, all preceding frames are invisible. Retry = re-run entire transaction. |
| Temp-then-rename | ByteCaskDB (hint files), PostgreSQL (control file) | Write to a temp file, `fdatasync`, then `rename()`. The rename is the atomic visibility gate. If any step fails, the temp file is orphaned and harmless. |

The common pattern: **make the visibility gate atomic and separate from the
data write**, so a failed write is either invisible (retry the whole thing) or
fully visible (no retry needed).

### 9. POSIX provides no timeout for regular file IO

All engines in this comparison block the calling thread unconditionally during
disk IO. No engine implements IO timeouts at the engine level.

`read()`, `write()`, `pwrite()`, `fdatasync()`, and `fsync()` on regular files
are unconditionally blocking with no timeout parameter. The multiplexing
mechanisms (`select()`, `poll()`, `epoll()`) do not work on regular files —
they always report regular files as immediately ready. `O_NONBLOCK` has no
effect on regular files per POSIX.

The practical consequence: if a disk hangs (bad sector, NFS stall, cgroup
throttle), the calling thread hangs indefinitely in kernel space (`D` state on
Linux — uninterruptible sleep). This is not a bug in any engine; it is a
fundamental POSIX limitation. Mitigations exist but are not widely adopted:

- **Watchdog threads** that detect hung IO and trigger a process restart
  (PostgreSQL's `recovery_timeout`, though that targets replication, not local
  IO).
- **`io_uring` with `IORING_OP_LINK_TIMEOUT`** — the closest mechanism to a
  real IO timeout on Linux. No major storage engine uses `io_uring` for
  write-path IO in production as of 2026 (RocksDB has experimental support
  via `PosixRandomRWFile`). Even with `io_uring`, a "timed out" IO is still
  in-flight in the kernel — a disk write submitted to the block layer cannot
  be cancelled.
- **Filesystem-level timeouts** (e.g. NFS `timeo` mount option, or ext4's
  `errors=panic` which triggers a kernel panic on IO error rather than
  hanging).

Because every engine shares this constraint, the per-write blocking behavior
is consistent across the comparison: RocksDB's `WriteImpl()` holds writers in
a write group that blocks until `fdatasync` completes; LevelDB's `Write()` holds
the mutex and blocks; SQLite blocks in `xWrite` / `xSync` VFS calls;
WiredTiger's log-slot consolidation means multiple threads block waiting for a
single slow IO; and ByteCaskDB holds the write lock for the entire `writev` +
state publish.

### 10. Correctness contract documentation is rare

ByteCaskDB's [`CONTRACT.md`](../CONTRACT.md) is a single plain-language
document that specifies the behavioral guarantees of every write function:
atomicity, durability, IO failure safety, LSN invariants, and poisoning
conditions. No major embedded storage engine publishes an equivalent
single-document correctness contract.

| Engine | What exists | Gap vs. CONTRACT.md |
|---|---|---|
| **RocksDB** | Wiki pages on WAL recovery modes, `ErrorHandler` behavior, sync semantics. Scattered across `options.h` comments and GitHub wiki. | No unified per-function failure contract. IO failure behavior is documented post-hoc in bug reports and design docs, not as a binding spec. |
| **LevelDB** | `include/leveldb/db.h` header comments (brief). `doc/impl.md` describes the LSM architecture. | Almost no failure-mode documentation. `bg_error_` behavior is discoverable only by reading source. No stated invariants for partial write. |
| **SQLite** | `atomiccommit.html` (10K+ words on the commit protocol), the VFS interface spec, "How SQLite Is Tested." | Closest peer in ambition. Spread across multiple documents; no single per-function spec. The VFS boundary is well-specified; the pager's internal failure handling is documented as implementation detail, not as a guarantee. |
| **LMDB** | `lmdb.h` header — detailed Doxygen comments on every API function. Two-meta-page design described in the source header. | Precise about return codes and preconditions. Does not cover internal failure-mode reasoning (e.g. what happens if `mdb_page_flush` short-writes). |
| **WiredTiger** | `src/docs/` architecture guides. MongoDB maintains an internal "Storage Engine Technical Specification." | Public docs describe normal operation. Failure-mode behavior (`WT_RET_PANIC` conditions, recovery expectations) lives in source comments. |
| **PostgreSQL** | `src/backend/access/transam/README` — detailed WAL protocol, checkpoint, and recovery invariant description. | Closest peer in spirit. Architecture explanation rather than per-function behavioral spec. Does not state "if `XLogWrite` short-writes, then X must hold." |

Three properties distinguish CONTRACT.md from what exists elsewhere:

1. **Per-function failure contracts.** Each write function (`apply_batch`,
   `vacuum_compact`, hint files) gets its own section with
   explicit guarantees for atomicity, durability, IO failure, and consistency.
   Other engines scatter equivalent information across header comments, wiki
   pages, and source code.

2. **Explicit IO failure classes as contract terms.** The contract specifies
   behavior for "some bytes on disk," "all bytes on disk but error returned,"
   "fdatasync fails" — the taxonomy from `correctness_validation.md`. Other
   engines document the happy path well; failure behavior is typically
   reverse-engineered from source.

3. **LSN invariants as first-class guarantees.** Monotonicity, uniqueness,
   gap safety, and no-reuse are stated as binding properties. In other
   engines, sequence-number invariants are implicit in the implementation —
   understanding what happens to RocksDB's sequence number after a failed
   write requires reading `WriteImpl()`.

The industry norm is implicit contracts: the code is the spec, tests are the
proof, and failure behavior surfaces through bug reports and post-mortems.
The engines that approach similar rigor are the ones that have been burned
badly enough by subtle correctness bugs to invest in formal documentation
after the fact — SQLite being the prime example.

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
