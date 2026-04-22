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
state satisfies the **Degraded State Invariant** (below). The service
calls `resume()` to attempt in-process recovery; on success the engine
accepts writes again without a restart.

**Degraded State Invariant**: at the moment the engine enters a degraded
state, the published in-memory state equals what recovery would produce
from the current on-disk files. This is what makes reads safe during
degradation. The invariant holds because every failure class that
degrades the engine does so *without* publishing the failed transition:

- Classes B1, B2, B3 (append failures): the in-memory state is never
  updated — `apply_writes` is not reached because the I/O threw first.
- Class C (orphaned BulkBegin): same as B — the partial batch is never
  applied to in-memory state.
- Classes F, G (sync failures): bytes are in the page cache but the
  key-directory changes are not published. `next_lsn` advances to
  prevent LSN reuse, but no key-value changes become visible.
- Class H (rotation failure): the write succeeded and was published, but
  the rotation to a new file failed. The published state is consistent
  with what recovery would find — the committed entries are on disk.

Therefore: the published state at degradation time corresponds only to
fully-durable committed transitions, and reads against it are safe.

**Recovery-equivalent**: the in-memory state agrees with what opening
a fresh DB from the current on-disk files would produce.

**fdatasync trust assumption**: the engine trusts that a successful
`fdatasync` return means all preceding writes are durable on the
underlying storage device. On Linux, this trust is not fully earned in
all configurations — the "fsyncgate" issue (PostgreSQL, 2018) showed
that on some kernel/filesystem combinations, `fsync` can return success
after an earlier async writeback error was consumed by another fd.
PostgreSQL's response was to `PANIC` on any `fsync` error; RocksDB added
`track_and_verify_wals_in_manifest`. ByteCaskDB does not implement
writeback error tracking. This is a deliberate simplicity choice for an
embedded engine targeting local storage — the assumption is stated here
so that it can be revisited if the engine is deployed on storage
configurations where it does not hold.

---

## apply_batch

The single write path. Every mutation — insert, update, delete — goes
through `apply_batch`. `put` and `del` are convenience wrappers that
call it with a guardless `WritePlan`.

When called with guards, it is a conditional atomic batch write: apply
these writes, but only if the caller-specified conditions still hold
at commit time. Returns `false` with no side effects if they don't.
This is the primitive that makes read-modify-write sequences safe
without external locking.

### Atomicity

The engine must apply all writes in the plan and make them visible
after return, or apply none. Partial application must never be
observable by a subsequent `get()`, `contains_key()`, or `snapshot()`.

### Causality

If write A completes before write B begins, the observable state must
reflect B, not A. This holds regardless of whether A and B are
separate API calls, operations within the same batch, or a mix of
both.

Specifically:

- **Across calls**: if `put(k, v1)` returns, then `put(k, v2)`
  returns, a subsequent `get(k)` must return `v2`. If `put(k, v1)`
  returns, then `del(k)` returns, `get(k)` must return false.
- **Within a batch**: operations in a single `WritePlan` are applied
  in insertion order. If a batch contains `put(k, v1)`
  followed by `del(k)`, `get(k)` must return false. If it contains
  `del(k)` followed by `put(k, v2)`, `get(k)` must return `v2`.
- **Across files**: when a key exists in multiple data files (e.g.
  after file rotation), the entry with the highest LSN determines
  the key's state. Recovery must produce the same result regardless
  of which files are replayed first.

This ordering is preserved through recovery: hint file replay and
parallel merge must produce the same causal result as the original
writes.

### Durability

If `opts.sync == true` and no exception is thrown, all writes must be
durable on disk before they become visible to any caller.

If `opts.sync == false` and no exception is thrown, all writes must be
visible to subsequent reads but may not survive a crash. This is an
accepted trade-off chosen by the caller.

### Conflict Safety

If any precondition guard, range guard, or implicit W-W check fails,
`apply_batch` must return `false`. The engine must not attempt any
writes, perform I/O, or change state. The caller's snapshot must not
be invalidated.

A `WritePlan` with guards but no write operations (no `put` or `del`)
is not empty — guards are evaluated. If all guards pass, the plan
returns `true` with no I/O and no state change. If any guard fails,
the plan returns `false`. This is intentional: it allows callers to
validate preconditions without committing writes, using the same
conflict-detection mechanism.

### I/O Failure Safety

If any I/O operation (append, sync) throws during execution:

- The caller must receive the exception.
- The published key directory must reflect zero operations from this call.
  `next_lsn` must be advanced past all LSNs consumed by appends that reached
  the file, to prevent reuse of those sequence numbers on the next write.
- The disk may contain none, some, or all of the bytes from this write.
  The engine must not assume what was written. Any append failure —
  whether nothing reached disk, a partial write, or a full write that
  returned an error — degrades the engine unconditionally. Any `fdatasync`
  failure (commit sync or rotation sync) likewise degrades the engine:
  bytes are in the OS page cache but durability is not confirmed, and the
  key directory does not reflect those bytes. `resume()` scans the active
  file, truncates garbage, replays valid committed entries, and creates a
  fresh active file, restoring normal operation.
- The DB must remain operational for subsequent calls.

### Rotation Safety

If a multi-entry batch fails mid-write, an orphaned `BulkBegin` marker
may exist on the active file. The engine degrades: `resume()` scans the
active file, truncates the orphaned batch (no `BulkEnd` found), and creates
a fresh active file.

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
  file during `apply_batch` must always have a higher sequence
  number than any previous new entry in the same file session. The
  sequence must never go backwards or repeat for new writes.

- **Uniqueness per key.** Two entries for different logical writes
  must not share a sequence number for the same key with different
  values. Same key, same LSN, same value is harmless — it is a
  duplicate of the same write and recovery handles it. Same key, same
  LSN, different value is undefined behavior.

- **next_lsn strictly greater than all on-disk LSNs.** `next_lsn`
  must always be greater than any sequence number that exists on disk.
  A future write must never reuse an LSN already present on disk.

- **next_lsn advances past all consumed LSNs.** If an append fails
  (classes B1, B2, B3), `next_lsn` must be advanced past all LSNs consumed
  by the failed call. Whether or not bytes reached disk is indeterminate from
  userspace — POSIX does not guarantee that `writev = -1` means no bytes were
  written (FUSE and network filesystems may write bytes and still return an
  error). The engine always advances conservatively: gaps are safe, reuse is
  not. If an append reaches disk but the subsequent `fdatasync` fails
  (classes F and G), `next_lsn` must likewise be advanced past all consumed
  LSNs. The bytes are in the page cache; reusing those LSNs on the next write
  would create ambiguous sequence numbers for the same key. Key-directory
  changes are not published in these cases — the write is not visible to
  callers. The engine degrades on F and G: `resume()` is required before
  further writes are accepted. Gaps are safe. Reuse is not.

- **Recovery produces the same next_lsn as a clean run.** Given the
  same committed writes, opening a fresh DB from disk must produce
  `next_lsn == max(all committed sequences) + 1`, regardless of
  failed partial writes on disk.

- **Markers consume their own LSNs.** `BulkBegin` and `BulkEnd`
  markers must each consume one LSN. A failed batch must not leave a marker
  LSN that gets reused by a data entry in a subsequent write.

- **No caller obligation for LSN safety.** The engine must guarantee
  that after any failure, a subsequent `apply_batch` call with any
  valid `WritePlan` will not produce LSN reuse. The caller must be
  free to retry with any plan, modify the plan, or abandon it
  entirely.

**Gaps are safe, reuse is not.** Nothing in the engine requires LSN
contiguity. All operations that depend on LSN comparison must use strict
`<`, never equality for ordering or arithmetic on gaps. Duplicate
LSNs are the actual risk.

### Durable Sequence (`durable_seq`)

`durable_seq` tracks the highest sequence number confirmed by `fdatasync`.
It is a field on `EngineState`, updated through the normal transient →
persistent → `store_state` path via `TransientEngineState::apply_sync`.

**Must be true, always:**

- **Monotonicity.** `durable_seq` must never regress. Enforced by
  `store_state` (same check as `next_seq`, `active_file_id`,
  `next_file_id`).

- **Only advanced after successful fdatasync.** `apply_sync` must only
  be called after `file.sync()` returns successfully. If sync fails
  (classes F, G), the transient's `durable_seq` must not be updated.

- **Covers all entries in the batch.** When group commit syncs a batch,
  `durable_seq` advances to `next_seq - 1` (the highest sequence
  consumed by Phase 1). All entries in the batch — including earlier
  nosync entries — are covered by the single `fdatasync`.

- **Recovery sets `durable_seq = next_seq - 1`.** All recovered entries
  were previously synced. After `DB::open()` and `resume()`,
  `durable_seq` reflects the full recovered state.

- **NoSync-only writes do not advance `durable_seq`.** If no `fdatasync`
  occurs in `execute_slots`, the transient carries forward the previous
  `durable_seq` unchanged.

`current_sequence(timeout)` exposes `durable_seq` to callers.
`timeout=0` is a non-blocking load. `timeout>0` blocks on `durable_cv_`
until `durable_seq` advances or timeout expires. The condvar notification
is centralized in `store_state` — one place, one check.

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

Same as `apply_batch`: in-memory state must be recovery-equivalent.
If `vacuum_commit` would publish a state where a key references a file
that does not contain the expected entry, the engine must degrade
itself.

---

## `create_manifest`

Rotates the active file, waits for all hint files, and returns a manifest
of sealed files with a snapshot. Provides a consistent point-in-time view
for follower bootstrap, backup, or federation.

### Completeness

The manifest includes every sealed data file and its hint companion. The
`through_sequence` value equals the highest sequence that was durable at
the time of rotation. The snapshot reflects exactly the state through
`through_sequence` — no more, no less.

### Rotation atomicity

`create_manifest` syncs the active file before rotation. The sync advances
`durable_seq` so that `through_sequence` is fully durable. The state is
captured under `write_mu_` immediately after `store_state`, preventing a
concurrent write from slipping between state publication and snapshot
capture.

### File list accuracy

Every file in `FileManifest::files` exists on disk with both `.data` and
`.hint` paths at the time of return. `worker_.drain()` ensures hint
generation has completed before building the file list.

### Caller responsibility

Vacuum must not run between `create_manifest()` and file transfer
completion. Vacuum unlinks files by path; an in-progress file transfer
(rsync, cp) would get ENOENT. Serializing vacuum with file transfer is
the caller's responsibility.

### I/O failure safety

If the pre-rotation sync fails, the exception propagates and no manifest
is produced. The active file is not sealed; the leader continues normally.

If `rotate_active_file` fails (active file sealed but new file creation
fails), the engine degrades — same pattern as `execute_slots` and
`ingest`. The sealed active file cannot accept further appends;
degrading forces `resume()` before the next write. `resume()` creates a
fresh active file and clears the degraded state.

---

## `FileStats` sequence bounds

`FileStats::min_sequence` and `max_sequence` track the lowest and highest
sequence numbers of entries written to each file.

### Invariants

- Both are zero (no entries) or both are non-zero. A state where one is
  zero and the other is not is a consistency violation.
- When both are non-zero: `min_sequence <= max_sequence`.
- These invariants are enforced by `validate_state_consistency` on every
  state transition.

### Tracking

Sequence bounds are updated in every path that writes entries:

- `apply_writes`: captures the batch's start and end sequence.
- `vacuum_scan_and_copy`: tracks sequences of all entries copied to the
  destination file, including `BulkBegin`/`BulkEnd` markers.
- `apply_vacuum`: propagates scan bounds to the compacted file's `FileStats`.
- `apply_resume`: resets bounds on truncation, then rebuilds from the
  committed entries.
- Recovery (serial and parallel): tracks bounds per file during hint
  replay.

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

---

## Runtime Invariant Enforcement

The engine validates structural invariants at runtime, not just in
tests. Violations are detected before corrupted state becomes visible
to readers.

### Hot-path checks (every state publication, always on)

`store_state` compares old and new `EngineState` before publishing:

| Invariant | Rationale |
|-----------|-----------|
| `next_lsn` must not regress | Prevents LSN reuse (§Sequence Numbers). |
| `active_file_id` must not regress | File IDs are monotonically assigned; rotation only moves forward. |
| `next_file_id` must not regress | Same monotonicity as `active_file_id`. |
| `durable_seq` must not regress | Confirmed-durable sequences cannot un-sync. |

On violation: the engine degrades (publishes nothing, writes blocked,
reads remain available). Cost: three integer comparisons per write.

### Hot-path checks (debug builds only)

Under `NDEBUG=0`, `store_state` additionally walks the key directory
to verify `next_lsn > max(all key_dir sequences)`. This is O(n) and
too expensive for release builds.

### Cold-path checks (open, resume — always on)

`validate_state_consistency` runs the full structural check on the
published state after `DB::open()` and `resume()`:

| Invariant | Cost |
|-----------|------|
| Active file exists in files registry | O(1) |
| No dangling file references in key_dir | O(n) |
| `next_lsn > max(all key_dir sequences)` | O(n) |
| `file_stats` covers all files | O(f) |
| `live_bytes` matches key_dir | O(n) |

On violation: throws `std::runtime_error`. The DB does not open or
`resume()` fails. This is intentional — if recovery produces
inconsistent state, the engine should not run.

## `ingest`

Applies pre-sequenced entries from a leader to a follower's storage.

| Property | Contract |
|----------|----------|
| **Mode requirement** | Throws `std::logic_error` if `mode() != Mode::Follower`. |
| **Degraded check** | Throws `DbDegraded` if the engine is degraded. |
| **Idempotency** | Entries with `sequence <= durable_seq` are silently skipped. Safe for restart-on-failure semantics. |
| **Batch-safe rotation** | `BulkBegin`/`BulkEnd` pairs always land in the same data file. File rotation only occurs at boundaries where no batch is open. |
| **Durability** | Every chunk is `fdatasync`'d before rotation. The final chunk is `fdatasync`'d before `store_state` publishes. |
| **Sequence advancement** | After ingest, `next_seq = max(next_seq, max(ingested sequences) + 1)`. Monotonically non-decreasing. |
| **Degraded on I/O failure** | Same pattern as `apply_batch`: on `writev`/`fdatasync` failure, advance sequence to prevent reuse, go degraded, rethrow. |
| **Atomicity** | If ingest throws, no partial state is published to readers. |
| **Causality** | Entries are applied in the sequence order provided by `changes_since`. If entry A has a lower sequence than entry B, A is applied before B. The follower's state reflects the same causal ordering as the leader's write history. |
| **I/O failure safety** | If any I/O operation throws, the published key directory reflects zero entries from this call. The engine degrades; `resume()` restores normal operation. After resume, re-delivery from `follower.current_sequence()` proceeds normally. |

## `set_mode` / `mode`

Controls which write paths are available.

| Property | Contract |
|----------|----------|
| **`set_mode(Mode)`** | Acquires `write_mu_` to ensure no in-flight write straddles the transition. Stores mode with release semantics. |
| **`mode()`** | Lock-free atomic read with acquire semantics. Same pattern as `is_degraded()`. |
| **Leader mode** | Normal writes allowed; `ingest` throws `std::logic_error`. |
| **Follower mode** | Normal writes (`put`, `del`, `del_range`, `apply_batch`) throw `DbFollowerMode`; `ingest` allowed. Reads, snapshots, vacuum, and `resume()` work in both modes. |
| **Initial mode** | Set from `Options::initial_mode` (default `Mode::Leader`) after recovery completes. |

---

## `changes_since`

Returns a lazy iterator over committed, durable entries in ascending
sequence order. Validated implicitly through the E2E ingest pipeline
tests, not through standalone `changes_since` proof tests.

| Property | Contract |
|----------|----------|
| **Durable boundary** | Only entries confirmed by `fdatasync` are yielded. The upper bound is `min(snap.sequence(), durable_sequence)`. Entries from NoSync writes not yet covered by a subsequent `fdatasync` are excluded, even if visible via snapshots. |
| **Completeness** | Every committed durable entry with `sequence > from_sequence` at snapshot time is yielded exactly once. |
| **Ordering** | Entries are yielded in strictly ascending sequence order. |
| **Batch integrity** | Incomplete batches (orphaned `BulkBegin` without `BulkEnd`) are excluded. `BulkBegin`/`BulkEnd` markers are preserved in the output. |
| **Vacuum transparency** | After `vacuum_compact_file`, entries retain original sequences and batch markers. `changes_since` over a vacuumed file yields the same logical content as the pre-vacuum file. |
| **Snapshot safety** | The iterator holds a `Snapshot` reference, keeping file descriptors open. Safe to run concurrently with vacuum (reads via fd, not path). |
