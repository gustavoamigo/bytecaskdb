# ByteCaskDB MariaDB Plugin — Backup Design

## Overview

ByteCaskDB's append-only architecture makes backup fundamentally simpler than traditional storage engines. Sealed data files are immutable — once rotated, they never change. There is no redo log to capture, no undo log to process, and no pages changing mid-copy. Backup reduces to: seal the active file, copy immutable files.

---

## Why backup is simple

| Concern | InnoDB | ByteCaskDB |
|---------|--------|------------|
| Files change during copy | Yes — requires redo log replay | No — sealed files are immutable |
| Prepare step after copy | Apply redo log, roll back uncommitted | Nothing — just open |
| Incremental backup | Track changed pages via LSN | Copy new file_ids only |
| Consistency guarantee | Redo log + checkpoint | Append-only + fdatasync |

ByteCaskDB's on-disk state is always consistent. Every committed write is followed by `fdatasync` before becoming visible. After a crash (or a backup snapshot), recovery replays hint files to rebuild the in-memory key directory — no fixup, no rollback, no log application.

---

## Architecture

The plugin uses a single global ByteCaskDB instance at `datadir/bytecaskdb/`. All tables, indexes, and catalog entries share this instance. Backup operates on this single directory — there are no per-table data files to coordinate.

```
datadir/bytecaskdb/
├── 000001.data   (sealed — immutable)
├── 000001.hint   (sealed — immutable)
├── 000002.data   (sealed — immutable)
├── 000002.hint   (sealed — immutable)
├── ...
├── 000047.data   (active — receives writes)
└── (no .hint for active file until sealed)
```

Sealed files (paired `.data` + `.hint`) are never modified after creation. Only the active file receives appends. Sealing the active file (rotating it) makes the entire directory a set of immutable files that can be copied safely.

---

## Backup procedure

### Full backup

1. **Pause vacuum.** Vacuum must not run during the copy — it can rewrite and delete sealed files that the backup needs.

2. **Seal the active file.** Call `create_manifest()` on the global DB instance. This rotates the active file, waits for hint file generation to complete, and returns the list of all sealed files plus a `through_sequence` marking the last committed entry. New writes continue into a fresh active file.

3. **Block commits and re-seal.** When using `BACKUP STAGE`, MariaDB blocks new commits globally. At this point, re-seal the active file to capture any writes that landed between step 2 and now. The final `through_sequence` after this seal matches the binlog position exactly.

4. **Copy all `.data` and `.hint` files.** Every sealed file is immutable. Copy them to the backup destination in any order, with any level of parallelism. No locking required during the copy — reads continue.

5. **Record `through_sequence`.** This is the backup's consistency point — every entry up to this sequence is included, and it matches the recorded binlog position.

6. **Resume vacuum.**

For script-based backup without binlog (no `BACKUP STAGE`), steps 2-3 collapse into a single seal under `FLUSH TABLES WITH READ LOCK` — no gap exists because no writes land while the lock is held.

### Incremental backup

Because sealed files are immutable, incremental backup is trivial:

1. Seal the active file (same as full backup).
2. Compare the file list against the previous backup's file list.
3. Copy only files with `file_id` greater than the previous backup's highest `file_id`.

No diffing, no log tailing. New files are the delta by definition.

### Restore

1. Copy all `.data` and `.hint` files back to the data directory.
2. Start MariaDB. ByteCaskDB's `DB::open()` replays hint files in parallel and rebuilds the key directory.
3. The catalog (`0x01` prefix keys) is recovered automatically — it lives in the same data files.

No prepare step, no log application, no rollback of uncommitted transactions.

---

## MariaDB integration

### Without binlog (simplest)

When the binary log is disabled, there is no binlog position to coordinate and no 2PC required. The backup is purely physical.

**Option A — `FLUSH TABLES FOR EXPORT` (handler-level)**

Implement `handler::extra(HA_EXTRA_FLUSH)` or the export handler:
1. MariaDB calls the flush handler.
2. The plugin seals the active file.
3. The operator copies the data directory.
4. MariaDB releases the flush.

**Option B — Plugin command or status table**

Expose `create_manifest()` through `SHOW ENGINE BYTECASKDB STATUS` or an `INFORMATION_SCHEMA` table that returns the file list and `through_sequence`. A backup script calls this, copies the listed files, and completes.

**Option C — Filesystem snapshot (LVM/ZFS/btrfs)**

Filesystem snapshots are crash-consistent, not application-consistent — they capture whatever was in the page cache at snapshot time, including partial `fdatasync` states. This means the active file may contain a torn entry (partial write with bad CRC).

**Recovery contract:** `DB::open()` scans the active file entry-by-entry with CRC-32C validation to generate its hint file. If it encounters a corrupt entry (bad CRC, truncated header), it throws — the database cannot be opened. `DB::open()` does not truncate trailing garbage; truncation only exists in `DB::resume()`, which is the in-process degraded-state recovery path and is not reachable during a cold open.

This means a raw filesystem snapshot is **not guaranteed to be recoverable** via `DB::open()` without an external repair step. To use filesystem snapshots safely, either:

1. **Seal first** (recommended): seal the active file before taking the snapshot. After sealing, the entire directory is immutable files and there is no recovery work. This is equivalent to option A with a snapshot instead of a file copy.
2. **Add a cold-open truncation path**: extend `DB::open()` to truncate the active file at the last valid entry when `fail_recovery_on_crc_errors = false`, similar to what `resume()` does. This is not implemented today.

Without one of these, filesystem snapshots are not a supported backup method.

### With binlog (replication-compatible)

When the binary log is enabled, backup must record the binlog position for point-in-time recovery via binlog replay.

**mariabackup integration via `BACKUP STAGE`:**

MariaDB's `BACKUP STAGE` protocol coordinates consistent snapshots across all engines. The plugin participates through `hton->backup_stage`:

| Stage | Plugin action |
|-------|---------------|
| `BACKUP_START` | Pause vacuum thread |
| `BACKUP_FLUSH` | Seal active file — rotates bulk data into immutable files for fast copy |
| `BACKUP_BLOCK_DDL` | No-op (DDL already blocked by MariaDB's MDL) |
| `BACKUP_BLOCK_COMMIT` | Re-seal active file to capture the tail written between FLUSH and now. Record final `through_sequence`. This matches the binlog position mariabackup records at this stage. |
| `BACKUP_END` | Resume vacuum thread |

After `BACKUP_FLUSH`, new writes continue landing in a fresh active file. Those writes are not in the FLUSH-time manifest. The re-seal at `BACKUP_BLOCK_COMMIT` — which runs under the global commit lock (no new commits can start) — captures this tail so the engine's `through_sequence` matches the binlog position exactly. Without the re-seal, the backup would be behind the recorded binlog position by the transactions that committed between FLUSH and BLOCK_COMMIT, and restore would depend on binlog replay being idempotent by primary key — an assumption we should not require.

---

## Crash safety and scope

This document covers **backup and restore** only. Crash-safe binlog replication (ensuring engine and binlog agree after a crash) requires 2PC (`prepare`/`commit` coordination with the binlog) and is a separate problem — see Phase 6 in the engine design doc.

Without 2PC, after a crash the engine and binlog may disagree by one transaction. This does not affect backup: the `BACKUP STAGE FLUSH` call seals the active file while the server is running normally (no crash involved). All sealed files are fully committed and consistent. The backup captures a perfect point-in-time snapshot regardless of whether 2PC is implemented.

**What works without 2PC:**
- Full and incremental physical backup (this document)
- Restore from backup
- Point-in-time recovery via binlog replay (from the recorded binlog position forward)

**What requires 2PC (out of scope):**
- Crash-safe binlog replication (engine/binlog consistency after unexpected crash)
- XA distributed transactions

---

## Without MariaDB (engine-level backup)

For users of ByteCaskDB outside of MariaDB (the C++ API directly), the same approach works:

```cpp
// Caller must ensure vacuum is not running during the copy.
auto manifest = db.create_manifest();

// Copy all files listed in manifest.files
for (auto& file : manifest.files) {
    copy_file(file.data_path, backup_dir / file.data_path.filename());
    copy_file(file.hint_path, backup_dir / file.hint_path.filename());
}

// Record the consistency point
save_metadata(manifest.through_sequence);
```

The snapshot inside `FileManifest` holds open all referenced file descriptors, preventing vacuum from deleting files during the copy. Release the manifest after the copy completes.

---

## Vacuum interaction

Vacuum reclaims space by rewriting sealed files that contain mostly dead entries, then deleting the originals. The `create_manifest()` contract requires that **vacuum must not run between `create_manifest()` and file transfer completion** — this is the caller's responsibility.

If vacuum rewrites and deletes a sealed file while the backup is copying it, the backup will contain a corrupt or missing file.

**Plugin implementation:**

The plugin's backup stage handler must pause the vacuum background thread before sealing:

1. `BACKUP_START`: pause vacuum thread.
2. `BACKUP_FLUSH`: call `create_manifest()`.
3. `BACKUP_BLOCK_COMMIT`: re-seal to capture tail.
4. Copy proceeds (driven by mariabackup or a script).
5. `BACKUP_END`: resume vacuum thread.

For script-based backup without `BACKUP STAGE` (e.g. `FLUSH TABLES WITH READ LOCK`), the plugin pauses vacuum internally when the flush/lock handler fires and resumes it on unlock. The operator does not need to manage vacuum state — the fence is always handled inside the plugin.

---

## Implementation plan

### Implemented: backup without binlog

The plugin registers two handlerton callbacks — `prepare_for_backup` (called at `BACKUP STAGE START`) and `end_backup` (called at `BACKUP STAGE END`). MariaDB does not expose per-stage engine callbacks; the intermediate stages (FLUSH, WAIT_FOR_FLUSH, LOCK_COMMIT) are coordinated by the SQL layer via MDL locks.

**`prepare_for_backup`:**

1. Pauses the background vacuum thread (reference-counted, waits for any in-progress vacuum pass to complete).
2. Seals the active file via `create_manifest()` — rotates it into a sealed `.data`+`.hint` pair, opens a fresh active file for new writes.
3. Writes `backup_manifest.txt` to the data directory listing all sealed files and the `through_sequence` consistency point.
4. Holds the `FileManifest` alive — its `Snapshot` keeps file descriptors open.

**`end_backup`:**

1. Releases the `FileManifest`.
2. Removes `backup_manifest.txt` from the data directory.
3. Decrements the vacuum pause counter. Resumes vacuum when counter reaches zero.

**Restore-time manifest cleanup:**

On startup, before `DB::open()`, the plugin checks for `backup_manifest.txt` in the data directory. If present:

1. Reads the allowed file list from the manifest.
2. Moves any `.data`/`.hint` files NOT in the manifest to a `discarded/` subfolder.
3. Deletes `backup_manifest.txt`.

This enables **mariabackup compatibility** without mariabackup-side changes: mariabackup copies the entire directory (including the post-seal active file that may have torn entries), and the manifest-based cleanup removes that file before `DB::open()` sees it.

**Crash safety of the manifest file:**

The manifest must be deleted in exactly two places:
- After `BACKUP STAGE END` — `end_backup()` removes it from the live data directory.
- On startup (restore) — `bytecaskdb_init()` removes it after filtering files.

If neither fires (e.g. crash between `BACKUP STAGE START` and `END`), the manifest remains. On next startup the plugin sees it and runs the discard logic — but since no restore happened (the files are the originals), the manifest lists exactly the files present, nothing gets discarded, and the manifest is deleted. Safe no-op.

### Future: production version (binlog-coordinated, incremental)

| Step | Scope |
|------|-------|
| 5. `BACKUP_BLOCK_COMMIT` re-seal | Second seal under global commit lock so `through_sequence` matches binlog position. Requires a per-stage engine callback or an alternative coordination mechanism. |
| 6. Internal vacuum fencing on flush/lock paths | Pause vacuum automatically on `FLUSH TABLES` and `BACKUP STAGE` without operator intervention |
| 7. Incremental metadata | Persist last-backup `file_id` so incremental backup copies only new files |

---

## Limitations

Backup protects against hardware failure, accidental `DROP TABLE`, and operational mistakes that destroy data files. It does **not** protect against:

- **Logical corruption.** A bad `DELETE` or `UPDATE` that executes successfully is faithfully captured in the backup. The backup contains what the engine committed — if what the engine committed is wrong, the backup is wrong. Point-in-time recovery via binlog replay (rolling back to a moment before the bad statement) is the mitigation, but requires binlog to be enabled.

- **Silent engine bugs.** If a bug in ByteCaskDB produces corrupt data that passes CRC checks (e.g. wrong value written for the right key), the backup copies the corruption. CRC only detects bit-rot and torn writes, not logic errors. The mitigation is `CHECK TABLE` and application-level validation — not backup.

- **Binlog/engine divergence after crash (without 2PC).** As noted above, without 2PC the engine and binlog may disagree by one transaction after a crash. A backup taken after such a crash faithfully captures the engine's state, but replaying the binlog forward from the recorded position may re-apply or skip one transaction. This is a replication concern, not a backup concern — the backup itself is always internally consistent.

- **Backup-time corruption.** If the copy tool introduces errors (disk failure during copy, network corruption during transfer), the backup is silently corrupt. Mitigation: verify backup integrity by running `DB::open()` against the backup copy (hint-file CRC verification confirms file-level integrity).

---

## Comparison with other engines

| | InnoDB | MyRocks | ByteCaskDB |
|---|---|---|---|
| Backup tool | mariabackup (complex, redo log capture) | mariabackup + checkpoint | File copy after seal |
| Incremental | LSN-based page tracking | SST file tracking | file_id comparison |
| Prepare step | Apply redo log, undo uncommitted | None (checkpoints are consistent) | None |
| Restore time | Depends on redo log size | Fast (SST restore) | Fast (hint file replay) |
| Requires 2PC for backup | No | No | No |
| Requires binlog for backup | No | No | No |
