# ByteCaskDB — Correctness Contract

This document defines the behavioral guarantees of every write function
in the engine. It is the source of truth for both the implementation
and the proof infrastructure. The invariant checker and fault injection
harness prove this contract, not the implementation.

One section per write function. Plain language.

---

## Definitions

**Visible**: a key-value pair is visible when a subsequent `get()`,
`contains_key()`, or `snapshot()` can observe it.

**Durable**: a key-value pair is durable when it will survive a process
crash and be present after recovery.

**Degraded**: the engine has detected an internal state divergence that
it cannot resolve on its own. Continuing to accept writes would risk
persisting data that recovery would not reproduce. A degraded DB must
refuse all write operations with a `DbDegraded` exception carrying a
diagnostic reason. Read operations remain available — the in-memory
state is recovery-equivalent at the time of degradation. The service
calls `resume()` to attempt in-process recovery; on success the engine
accepts writes again without a restart.

**Recovery-equivalent**: the in-memory state agrees with what opening
a fresh DB from the current on-disk files would produce.

---

## apply_batch_if

The single write path. Every mutation — insert, update, delete — goes
through `apply_batch_if`. `put`, `del`, and `apply_batch` are
convenience wrappers that call it with no guards.

When called with guards, it is a conditional atomic batch write: apply
these writes, but only if the caller-specified conditions still hold
at commit time. Returns `false` with no side effects if they don't.
This is the primitive that makes read-modify-write sequences safe
without external locking.

### Atomicity

The engine must apply all writes in the plan and make them visible
after return, or apply none. Partial application must never be
observable by a subsequent `get()`, `contains_key()`, or `snapshot()`.

### Durability

If `opts.sync == true` and no exception is thrown, all writes must be
durable on disk before they become visible to any caller.

If `opts.sync == false` and no exception is thrown, all writes must be
visible to subsequent reads but may not survive a crash. This is an
accepted trade-off chosen by the caller.

### Conflict Safety

If any precondition guard, range guard, or implicit W-W check fails,
`apply_batch_if` must return `false`. The engine must not attempt any
writes, perform I/O, or change state. The caller's snapshot must not
be invalidated.

### I/O Failure Safety

If any I/O operation (append, sync) throws during execution:

- The caller must receive the exception.
- The published key directory must reflect zero operations from this call.
  `next_lsn` must be advanced past all LSNs consumed by appends that reached
  the file, to prevent reuse of those sequence numbers on the next write.
- The disk may contain none, some, or all of the bytes from this
  write. The engine must not assume what was written. For a single-entry
  write where the file is tainted (bytes may have landed), the engine
  must attempt a read-back: `pread` the entry at the known offset and
  CRC-verify it. Valid CRC → the write is durable; publish state normally.
  Invalid CRC or short read → the engine must guarantee that a valid entry
  is subsequently written so recovery will reach a consistent state from
  it. If that is not possible, the engine must degrade — `resume()` will
  scan and truncate the active file to restore a consistent state.
- The DB must remain operational for subsequent calls.

### Rotation Safety

If a multi-entry batch fails mid-write, an orphaned `BulkBegin` marker
may exist on the active file. The engine must isolate it by rotating
the active file so that subsequent writes are not silently discarded
on recovery.

If the isolation rotation itself fails:

- The active file still contains the orphaned `BulkBegin`.
- Any subsequent write to that file would appear to succeed but would
  be silently discarded by recovery (it falls after an unmatched
  `BulkBegin`).
- This is a consistency divergence: the caller believes the write is
  committed, but recovery will not produce it.
- The engine must degrade itself. `resume()` scans the active file,
  truncates the orphaned batch, and creates a fresh active file.

### Consistency

The in-memory state visible to callers must always be recovery-
equivalent, with one permitted exception: writes completed with
`sync=false` may be visible in memory but lost on crash.

The specific guarantees:

- **Write succeeds, `sync=true`**: the data must be on disk AND
  visible to subsequent reads.
- **Write succeeds, `sync=false`**: the data must be visible to
  subsequent reads. It may or may not survive a crash.
- **Write throws**: the data may or may not be partially on disk. It
  must not be visible to subsequent reads. Recovery must reach a
  consistent state.

If the engine cannot maintain recovery-equivalence — because a commit-
phase failure (rotation, state publication) leaves in-memory state in
a configuration that recovery would not produce — the engine must
degrade the DB.

Degrading is an acceptable outcome. The engine must not:

1. Allow a caller to read a key-value pair that recovery would not
   produce.
2. Prevent a caller from reading a key-value pair that recovery would
   produce (excluding the `sync=false` crash-loss case).
3. Continue operating with inconsistent in-memory state without
   signaling the divergence.

### Sequence Numbers (LSN)

Every append to disk must consume a sequence number from `next_lsn`.
Every entry written to disk — including `BulkBegin` and `BulkEnd`
markers — must consume one LSN.

**On success**: `next_lsn` in the published state must be advanced
past all consumed LSNs. Each `KeyDirEntry` in `key_dir` must record
the LSN of its write.

**On failure**: the local working copy of `next_lsn` must be discarded
along with the rest of the local state. The published `next_lsn` must
be unchanged — the engine must only publish new state after all I/O
and mutations succeed. LSNs consumed by the failed partial write are now
on disk but not reflected in the published counter.

**Must be true, always:**

- **Monotonicity for new entries.** New entries appended to the active
  file during `apply_batch_if` must always have a higher sequence
  number than any previous new entry in the same file session. The
  sequence must never go backwards or repeat for new writes.
  `vacuum_absorb` appends old entries with their original LSNs —
  these are not new writes and do not follow this ordering.

- **Uniqueness per key.** Two entries for different logical writes
  must not share a sequence number for the same key with different
  values. Same key, same LSN, same value is harmless — it is a
  duplicate of the same write and recovery handles it. Same key, same
  LSN, different value is undefined behavior.

- **next_lsn strictly greater than all on-disk LSNs.** `next_lsn`
  must always be greater than any sequence number that exists on disk.
  A future write must never reuse an LSN already present on disk.

- **next_lsn advances past all consumed LSNs.** If an append fails
  before any bytes reach disk (classes B1, A), `next_lsn` is unchanged —
  no bytes were written. If an append reaches disk but the subsequent
  `fdatasync` fails (classes F and G), `next_lsn` must be advanced past
  all consumed LSNs. The bytes are in the page cache; reusing those LSNs
  on the next write would create ambiguous sequence numbers for the same
  key. Key-directory changes are not published in these cases — the write
  is not visible to callers. Gaps are safe. Reuse is not.

- **Recovery produces the same next_lsn as a clean run.** Given the
  same committed writes, opening a fresh DB from disk must produce
  `next_lsn == max(all committed sequences) + 1`, regardless of
  failed partial writes on disk.

- **Markers consume their own LSNs.** `BulkBegin` and `BulkEnd`
  markers must each consume one LSN. A failed batch must not leave a marker
  LSN that gets reused by a data entry in a subsequent write.

- **No caller obligation for LSN safety.** The engine must guarantee
  that after any failure, a subsequent `apply_batch_if` call with any
  valid `WritePlan` will not produce LSN reuse. The caller must be
  free to retry with any plan, modify the plan, or abandon it
  entirely.

**Gaps are safe, reuse is not.** Nothing in the engine requires LSN
contiguity. All operations that depend on LSN comparison must use strict
`<`, never equality for ordering or arithmetic on gaps. Duplicate
LSNs are the actual risk.

---

## vacuum_compact

Rewrites a sealed data file, discarding dead entries. The old file
must be deferred for deletion when no readers reference it.

### Data Preservation

Every key-value pair readable before `vacuum_compact` is called must
be readable after it returns, with the same value. The engine must not
lose data or introduce phantom entries. Tombstones must be preserved.

### Atomicity

The compacted file must replace the old file in the published state
in a single atomic state publication. A reader must never see a state
where
a key points to neither the old nor the new file.

### I/O Failure Safety

If any I/O operation throws during scan, copy, sync, or rename:

- The caller must receive the exception.
- The old file must remain in the published state, unchanged and
  readable.
- Any partial temporary file (`.data.tmp`) must be cleaned up on next
  recovery or vacuum call.
- The DB must remain operational.

If the commit step (`vacuum_commit`) fails after the new file is
written and renamed:

- The old file must remain in the published state.
- The new file exists on disk but is unreferenced by `key_dir`.
- Recovery or next vacuum must handle it.

### Stale File Safety

The old data file must not be deleted while any in-flight reader
(snapshot or iterator) holds a shared reference to it. Deletion must
be deferred until no external references remain.

### Consistency

Same as `apply_batch_if`: in-memory state must be recovery-equivalent.
If `vacuum_commit` would publish a state where a key references a file
that does not contain the expected entry, the engine must degrade
itself.

---

## vacuum_absorb

Appends live entries from a sealed file into the current active file,
then removes the sealed file from the published state.

### Data Preservation

Same as `vacuum_compact`: every key-value pair readable before must be
readable after, with the same value.

### Atomicity

The absorbed entries and the removal of the old file must be published
in a single atomic state publication. The key directory must
transition all
absorbed keys from the old file to the active file atomically — no
reader must see a state where some absorbed keys still resolve from
the old file while others already resolve from the active file.

### I/O Failure Safety

If any I/O throws during scan, copy, or sync:

- The old file must remain in the published state, unchanged.
- The active file must be left in the same state it was in before the
  absorb began. The engine must attempt to truncate any bytes appended
  during the failed copy; if the truncate also fails, the active file
  may contain dead bytes that are not referenced by `key_dir`.
- The DB must remain operational.

### Locking

`vacuum_absorb` must block all other writes for its entire duration
because it appends to the shared active `DataFile`. Writes are blocked during
absorb. This is acceptable because absorb only targets small files
(below `absorb_threshold`).

### Stale File Safety

Same as `vacuum_compact`: the old file must be deferred for deletion
until no external references remain.

### Consistency

Same as `vacuum_compact`.

---

## Hint Files

A hint file is a compact index of a sealed data file. It allows
recovery to rebuild the key directory without scanning raw data
entries. One hint file per sealed data file.

### Correctness

The hint file for a sealed data file must be a faithful summary of
that file's committed content. Every entry that recovery would accept
from a raw data scan must be present in the hint file. An entry that
recovery would reject must not be present.

Specifically:

- Every complete Put and Delete entry outside a batch must be included.
- Entries inside a `BulkBegin`..`BulkEnd` pair must be included only
  if the `BulkEnd` is present. If `BulkEnd` is missing (crash mid-
  batch), all entries after the unmatched `BulkBegin` must be
  discarded.
- If the same key appears multiple times in the file, only the entry
  with the highest sequence number must be kept. This is a per-file
  deduplication — cross-file conflict resolution happens during
  recovery.

### Atomicity

Hint files must be written atomically via a temp-then-rename protocol.
The file must be written to `.hint.tmp`, synced, then renamed to
`.hint`. A reader must see either the complete hint file or no hint
file. A partial `.hint.tmp` left by a crash must be cleaned up on next
recovery.

### Idempotency

If a `.hint` file already exists for a data file, hint generation
must be skipped. Generating the same hint file twice must produce the
same result. The operation must be safe to retry or run concurrently
from different code paths (rotation dispatch, shutdown, recovery).

### Timing

Hint generation must be dispatched to a background worker when the
active file is sealed during rotation. It must not block the write
path.

At shutdown, the engine must drain the background worker, then write
hint files for any sealed files that do not yet have one.

During recovery, any sealed data file without a `.hint` file must get
one generated before recovery proceeds. Recovery must never build the
key directory directly from raw data files. If a sealed data file has
no hint file, recovery must generate one from the raw data file before
proceeding. The key directory must always be built from hint files.

### I/O Failure Safety

If hint generation fails (I/O error during scan, write, sync, or
rename):

- A `.hint` file must not be produced. The `.hint.tmp` may remain on
  disk.
- The data file must not be affected.
- The next recovery must clean up the `.hint.tmp` and regenerate the
  hint file from the data file.
- The engine must not lose data.
