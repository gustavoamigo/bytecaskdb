# ByteCaskDB — Correctness Contract

This document defines the behavioral guarantees of every write function
in the engine. It is the source of truth for both the implementation
and the proof infrastructure. The invariant checker and fault injection
harness prove this contract, not the implementation.

One section per write function, then one for the read path: what the
engine promises about the views and spans it lends out, and how long.
Plain language.

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
  key-directory changes are not published. `next_seq` advances to
  prevent sequence reuse, but no key-value changes become visible. With
  the commit pipeline a failed commit fdatasync (F) covers every writer
  appended since the last successful flush: all of them receive the error
  and none of their changes are published. `resume()` replays every valid
  entry it finds in the active file, so a writer that received the error
  can see its write persisted after recovery — true for the single batch
  of class F before the pipeline, and now for every batch since the last
  flush.
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

### Size preconditions

All keys and values in the `WritePlan` must satisfy the size limits
configured in `Options` (default: 4 KiB keys, 4 MiB values; hard
ceiling: 65,535 bytes keys, ~4 GiB values). Size validation happens
at the `WritePlan` API boundary (`put`, `del`, `del_range`, guard
methods) before any data is copied. `DB::put`, `DB::del`, and
`DB::del_range` also validate before creating their internal
`WritePlan`. Violations throw `std::invalid_argument`.

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
  after file rotation), the entry with the highest sequence determines
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
  `next_seq` must be advanced past all sequences consumed by appends that reached
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

### Sequence Numbers

Every append to disk must consume a sequence number from `next_seq`.
Every entry written to disk — including `BulkBegin` and `BulkEnd`
markers — must consume one sequence.

**On success**: `next_seq` in the published state must be advanced
past all consumed sequences. Each `KeyDirEntry` in `key_dir` must record
the sequence of its write.

**On failure**: the local working copy of `next_seq` must be discarded
along with the rest of the local state. The published `next_seq` must
be unchanged — the engine must only publish new state after all I/O
and mutations succeed. sequences consumed by the failed partial write are now
on disk but not reflected in the published counter.

**Must be true, always:**

- **Monotonicity for new entries.** New entries appended to the active
  file during `apply_batch` must always have a higher sequence
  number than any previous new entry in the same file session. The
  sequence must never go backwards or repeat for new writes.

- **Uniqueness per key.** Two entries for different logical writes
  must not share a sequence number for the same key with different
  values. Same key, same sequence, same value is harmless — it is a
  duplicate of the same write and recovery handles it. Same key, same
  sequence, different value is undefined behavior.

- **next_seq strictly greater than all on-disk sequences.** `next_seq`
  must always be greater than any sequence number that exists on disk.
  A future write must never reuse an sequence already present on disk.

- **next_seq advances past all consumed sequences.** If an append fails
  (classes B1, B2, B3), `next_seq` must be advanced past all sequences consumed
  by the failed call. Whether or not bytes reached disk is indeterminate from
  userspace — POSIX does not guarantee that `writev = -1` means no bytes were
  written (FUSE and network filesystems may write bytes and still return an
  error). The engine always advances conservatively: gaps are safe, reuse is
  not. If an append reaches disk but the subsequent `fdatasync` fails
  (classes F and G), `next_seq` must likewise be advanced past all consumed
  sequences. The bytes are in the page cache; reusing those sequences on the next write
  would create ambiguous sequence numbers for the same key. Key-directory
  changes are not published in these cases — the write is not visible to
  callers. The engine degrades on F and G: `resume()` is required before
  further writes are accepted. Gaps are safe. Reuse is not.

- **Recovery produces the same next_seq as a clean run.** Given the
  same committed writes, opening a fresh DB from disk must produce
  `next_seq == max(all committed sequences) + 1`, regardless of
  failed partial writes on disk.

- **Markers consume their own sequences.** `BulkBegin` and `BulkEnd`
  markers must each consume one sequence. A failed batch must not leave a marker
  sequence that gets reused by a data entry in a subsequent write.

- **No caller obligation for sequence safety.** The engine must guarantee
  that after any failure, a subsequent `apply_batch` call with any
  valid `WritePlan` will not produce sequence reuse. The caller must be
  free to retry with any plan, modify the plan, or abandon it
  entirely.

**Gaps are safe, reuse is not.** Nothing in the engine requires sequence
contiguity. All operations that depend on sequence comparison must use strict
`<`, never equality for ordering or arithmetic on gaps. Duplicate
sequences are the actual risk.

### Durable Sequence (`durable_seq`)

`durable_seq` tracks the highest sequence number confirmed by `fdatasync`.
It is a field on `EngineState`, updated through the normal transient →
persistent → `store_state` path via `TransientEngineState::apply_sync`.

**Must be true, always:**

- **Monotonicity.** `durable_seq` must never regress. Enforced by
  `store_state` (same check as `next_seq`, `active_file_id`,
  `next_file_id`).

- **Only advanced after successful fdatasync.** `durable_seq` advances
  only after `file.sync()` returns successfully — in `flush_pending`,
  which publishes the prepared head it captured before the fdatasync
  with `durable_seq = next_seq - 1`, or through `apply_sync` on the
  rotation and ingest paths. If sync fails (classes F, G), the published
  `durable_seq` is unchanged.

- **Covers every entry appended before the fdatasync.** A flush covers
  the prepared head as it stood when the flush started: every batch
  appended since the previous flush, including nosync entries, is
  covered by the single `fdatasync`. Entries appended while the fdatasync
  was in flight are not claimed; the next flush covers them.

- **Never published ahead of its sync requests.** Every published state
  satisfies `durable_seq >= sync_requested_seq`, where
  `sync_requested_seq` is the highest sequence written by a `sync=true`
  slot. Enforced by `store_state`; a violation degrades the engine.

- **Recovery sets `durable_seq = next_seq - 1`.** All recovered entries
  were previously synced. After `DB::open()` and `resume()`,
  `durable_seq` reflects the full recovered state.

- **NoSync-only writes do not advance `durable_seq`.** If the prepared
  head owes no fdatasync, `flush_pending` publishes it with the previous
  `durable_seq` unchanged.

`durable_sequence(min_sequence, timeout)` exposes `durable_seq` to callers
(renamed from `current_sequence` — BC-231; no compatibility alias remains).
`min_sequence = 0`, an already-reached target, or a nonpositive `timeout`
return the current watermark immediately without blocking. Otherwise it
blocks on `durable_cv_` until `durable_seq >= min_sequence` or the timeout
expires, then returns the watermark. The condvar notification is
centralized in `store_state` — one place, one check.

Every committed write (`put`, `del`, `del_range`, `apply_batch`) returns a
`CommitResult{sequence, durable}` (`std::optional<CommitResult>` for `del`
and `apply_batch`, which can report `nullopt` on an absent key or a
conflict). `sequence` is the highest sequence assigned to the write (the
`BulkEnd` marker's sequence for a multi-op batch); `0` means nothing was
written (empty plan, guard-only plan, or empty-range `del_range`) —
`durable` is always `true` in that case. `durable` reports whether
`fdatasync` confirmed the write before return: always `true` for
`sync=true`, and possibly `true` for `sync=false` writes coalesced with a
sync writer in the same group-commit batch or a rotation sync. A reader —
local or follower — whose `durable_sequence() >= sequence` is guaranteed to
see every entry of that write. See
[`docs/commit_result_api_design.md`](docs/commit_result_api_design.md) for
the full contract.

---

## vacuum_compact

Rewrites a sealed data file, discarding dead entries. The old file
must be deferred for deletion when no readers reference it.

### Data Preservation

Every key-value pair readable before `vacuum_compact` is called must
be readable after it returns, with the same value. The engine must not
lose data or introduce phantom entries. Tombstones must be preserved.

### Size Accounting

A published file's `total_bytes` equals its size on disk. Recovery
seeds `total_bytes` from the file's length, so anything the compacted
file contains has to be counted as it is written — including the
`BulkBegin` / `BulkEnd` markers, which compaction preserves like any
other entry. A `file_stats()` reading must not change across a restart.

### Progress

`vacuum()` returns `true` only when a file was actually reclaimed.
A file whose every byte is live data, a tombstone or a batch marker
cannot be made smaller, because compaction must preserve all three;
the engine discards the staged copy and returns `false` rather than
publishing an identical file.

This is a termination guarantee, not only an efficiency one. Fragmentation
is measured against `live_bytes`, and tombstones and markers can never
count towards it — hint files have no marker concept, so recovery could
not reproduce a `live_bytes` that included them. Without this rule a
file holding either one stays eligible at `fragmentation_threshold = 0`
forever, and `while (db.vacuum({.fragmentation_threshold = 0.0})) {}`
never terminates.

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

The old data file must remain readable through any in-flight reader
(snapshot or iterator) that holds a shared reference to it. Vacuum
unlinks the path as soon as the new file is published; the inode
survives because the `DataFile` object — and with it the open
descriptor and any mapping — is reference-counted and outlives the
unlink. Readers continue through their open descriptor; POSIX keeps an
unlinked file readable until the last one closes. What must never
happen is the object being destroyed while a reader still references
it.

This is one row of a general rule. See **View and span lifetimes** for
every other event that touches a file under a live reader.

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
| `next_seq` must not regress | Prevents sequence reuse (§Sequence Numbers). |
| `active_file_id` must not regress | File IDs are monotonically assigned; rotation only moves forward. |
| `next_file_id` must not regress | Same monotonicity as `active_file_id`. |
| `durable_seq` must not regress | Confirmed-durable sequences cannot un-sync. |

On violation: the engine degrades (publishes nothing, writes blocked,
reads remain available). Cost: three integer comparisons per write.

### Hot-path checks (debug builds only)

Under `NDEBUG=0`, `store_state` additionally walks the key directory
to verify `next_seq > max(all key_dir sequences)`. This is O(n) and
too expensive for release builds.

### Cold-path checks (open, resume — always on)

`validate_state_consistency` runs the full structural check on the
published state after `DB::open()` and `resume()`. The O(n) key
directory walk is too expensive for release builds, so the invariants
that need it run in test builds only:

| Invariant | Cost | Build |
|-----------|------|-------|
| Active file exists in files registry | O(1) | always |
| `sync_requested_seq <= durable_seq` | O(1) | always |
| `file_stats` covers all files | O(f) | always |
| `min_sequence` / `max_sequence` coherence | O(f) | always |
| No dangling file references in key_dir | O(n) | test builds |
| `next_seq > max(all key_dir sequences)` | O(n) | test builds |
| `live_bytes` matches key_dir | O(n) | test builds |
| Every entry lies inside its file's committed extent (**P**) | O(n) | test builds |

The same containment check runs in `store_state`'s debug walk, so it
covers every publication — rotation and vacuum included — not only the
two cold paths.

P is the invariant the mmap read path depends on — see *Offset
containment* under **View and span lifetimes**. It is checked after
`resume()` specifically because `resume()` is the operation that
shortens a file that published offsets point into.

On violation: throws `std::runtime_error`. The DB does not open or
`resume()` fails. This is intentional — if recovery produces
inconsistent state, the engine should not run.

### Fatal invariants (always on, in release builds too)

**A writable data file must never be created under a name this database
has already used**, either as `<stem>.data` or as `<stem>.hint`. Reusing
one would append past a sealed file's end, and those entries would be
invisible to recovery, because hint generation skips a file whose hint
already exists. The engine checks this on every data file creation
(`O_EXCL`) and on vacuum's final placement (which claims the name
atomically rather than replacing it).

A violation is not an I/O failure and is not degradable: it aborts the
process rather than degrading. Degrading would be caught by the write
path, which calls `resume()`, which mints a fresh stem and continues —
recovering from the symptom while the broken generator that caused it
stays broken. It is also not reachable by chance: stems carry a 64-bit
salt drawn per file, so a repeat within one timestamp second is ~1e-13
at 2,000 files. Reaching this check means the generator itself is
broken, which no retry fixes.

This is the only class of failure that terminates the process. Every
other failure either throws or degrades, per the sections above.

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
| **I/O failure safety** | If any I/O operation throws, the published key directory reflects zero entries from this call. The engine degrades; `resume()` restores normal operation. After resume, re-delivery from `follower.durable_sequence()` proceeds normally. |

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

---

## View and span lifetimes

Every read API either copies bytes out or lends a view of them. A lent
view — `std::span`, a reference into an iterator's cache — is correct
only while the memory behind it is still owned and still holds what it
held. This section says, for each view the engine hands out and each
event that can move a file underneath it, whether the view stays valid.

Its scope is memory safety and byte stability, not visibility. A view
is frozen at the moment it was taken: a concurrent write never changes
what a live view shows, and never invalidates it either.

### The views

| View | Handed out by | The bytes live in |
|------|---------------|-------------------|
| `Bytes& out` | `DB::get`, `Snapshot::get` | The caller's own vector — a copy, not a view |
| `EntryView` (`key`, `value`) | `EntryIterator`, `ReverseEntryIterator` — `iter_from`, `riter_from` | The producing iterator's `io_buf_`, or a data file's mapping under `io_backend = mmap` |
| `const Key&` | `KeyIterator`, `ReverseKeyIterator` — `keys_from`, `rkeys_from` | An owning `Key` member of the producing iterator |
| `DataEntryView` (`key`, `value`) | `ChangeIterator` — `changes_since` | The producing iterator's scan buffer — owning `DataEntry` storage |
| `Snapshot` | `DB::snapshot`, `create_manifest` | A reference-counted engine state |

### What holds a view up

Four mechanisms account for every answer below. Where a cell is valid,
it is valid because of one of these, and the grid names which.

**A — Owned copy.** The bytes are in storage the view's owner
allocated. `get` copies the value into the caller's `Bytes`.
`KeyIterator` materialises an owning `Key`. `ChangeIterator` reads
through `CommittedEntryIterator`, whose scan buffer holds owning
`DataEntry` values. No file event can reach any of them.

**B — Iterator-pinned state.** `EntryIterator`,
`ReverseEntryIterator` and `ChangeIterator` each hold their own
`shared_ptr<const EngineState>`, which holds a `shared_ptr<DataFile>`
per file. Every descriptor and mapping the iterator can reach stays
open for as long as the iterator lives — independently of the `DB`, of
the `Snapshot` it came from, and of what the engine publishes next.

**C — Tree-pinned nodes.** `KeyIterator` holds a reference-counted
pointer to the key-directory root it was created from. A write mutates
a node in place only when it holds the sole reference to it, so no node
reachable from a published root is ever modified. The subtree an
iterator walks is immutable for the iterator's life.

**D — Fixed mapping address.** The active file's mapping is
established once, in `WritableMmapDataFile`'s constructor, and released
once, in its destructor. No engine operation unmaps and remaps it. Only
`mmap_end_` — the prefix still backed by the file — moves, and only
downwards, and only in step with the file's length.

### Offset containment

One invariant carries the mmap answers, and is stated here so the rest
can refer to it:

> **P.** Every offset published in `key_dir` lies below the committed
> extent of the file it names.

P holds by construction: an entry becomes visible only after its bytes
are complete on disk, and the scan that establishes a committed extent
stops at the first incomplete or corrupt entry, which is always past
everything already published. P is what lets `resume()` shorten the
active file under a live reader without taking anything away from it.

Test builds check P rather than assume it, on every published state:
`store_state` verifies that each entry ends at or before its file's
`total_bytes` as part of the key-directory walk it already performs,
and `validate_state_consistency` repeats the check at `DB::open()` and
after `resume()` — the latter being the operation that moves a
committed extent downwards.

### Sealed `MAP_PRIVATE` versus active `MAP_SHARED`

The two mappings give different guarantees, and a span into one is not
governed by the same rules as a span into the other.

**Sealed files** (`ReadOnlyMmapDataFile`, `MAP_PRIVATE`) are immutable
for their whole life. Nothing rewrites them, nothing shortens them, and
vacuum's unlink does not disturb the mapping. A span into a sealed file
is bounded by one thing only: the lifetime of the `DataFile` object,
which mechanism B pins.

**The active file** (`WritableMmapDataFile`, `MAP_SHARED`) is written
while it is mapped, so `MAP_SHARED` is required for a reader to see
what `pwritev` wrote. Two consequences follow, and both are guarantees,
not accidents:

- *Contents are stable.* Data files are append-only. `pwritev` writes
  at the logical end and zero-fill only ever writes at or past the
  zeroed end, both of which are at or above every published offset. No
  byte under a live span is ever rewritten.
- *The file can shorten under the mapping.* `resume()` truncates to the
  last committed offset and sealing releases the zero-filled tail.
  Neither touches the mapping; both lower `mmap_end_` with the file. By
  P, neither can take away a page a published offset points into, so no
  live span loses its backing. A read at or past the new bound takes
  the `pread` fallback and fails as a clean short read rather than
  faulting on a page beyond end of file.

The other two back-ends raise neither case: under `pread`, and under
the buffer pool — which copies each value out of its frames rather than
lending one — every span an iterator hands out points into that
iterator's own `io_buf_`, which no file event can reach.

### The grid

One row per (view, event). **Valid** means the view still addresses
live memory and still holds the bytes it held. **Invalid** means it
does not, and nothing reports that. **Invalid, detected** means the
engine reports it rather than letting the caller read freed memory.

No cell below is *invalid, detected*. Nothing in the engine notices
that a lent view has gone stale — where a view becomes invalid it does
so silently. That is what makes these rules worth writing down rather
than relying on a check to catch a mistake.

The rows below the rule in each table are the producing iterator's own
lifecycle. They are the only rows that invalidate anything.

*Seal* is not a public call. It is the step that turns the active file
into a read-only one — release the zero-filled tail, reopen the path
read-only, swap the registry entry — and it runs inside file rotation,
`create_manifest`, `resume()` and vacuum's staging copy. It appears as
its own row because it shortens a file, which is the property that
matters here.

#### `Bytes& out` from `get`

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | A — the caller owns the bytes |
| `vacuum()` | Valid | A |
| File rotation | Valid | A |
| Seal | Valid | A |
| `set_mode()` | Valid | A |
| Originating `Snapshot` destroyed | Valid | A |
| Concurrent write | Valid | A — a later write never reaches a value already copied out |
| `DB` destruction | Valid | A — the value outlives the engine |
| — | | |
| Next `get` into the same `Bytes` | Overwritten | The vector is reused by design; copy it out first if the previous value is still needed |

#### `EntryView` spans, `io_backend` = `pread` or `buffer_pool`

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | B — the span is in the iterator's `io_buf_`; truncation cannot reach it |
| `vacuum()` | Valid | B — the file object outlives the unlink |
| File rotation | Valid | B — rotation registers a new object; the old one stays alive for anyone holding it |
| Seal | Valid | B |
| `set_mode()` | Valid | In-memory transition only |
| Originating `Snapshot` destroyed | Valid | B — the iterator carries its own state reference |
| Concurrent write | Valid | B |
| `DB` destruction | Valid | B — see **`DB` destruction** below |
| — | | |
| Next `operator++()` | Invalid | The buffer is reused for the next entry |
| Iterator destroyed | Invalid | The buffer goes with it |
| Iterator moved | Valid | The buffer moves with the iterator; the span keeps addressing it |
| Iterator copied | Not possible | The iterators are move-only. A copy would carry spans addressing the source's buffer while deep-copying that buffer, so the copy is deleted rather than documented |

#### `EntryView` spans, `io_backend` = `mmap`

Spans point into a file's mapping when the entry is inside it, and into
the iterator's `io_buf_` otherwise. Both are covered below.

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | B + D + P — the mapping is never replaced; truncation lowers `mmap_end_` but cannot reach a published offset |
| `vacuum()` | Valid | B + D — the mapping outlives the unlink, sealed bytes are immutable |
| File rotation | Valid | B + D — the sealed reopen is a second, independent mapping; the old one is untouched |
| Seal | Valid | B + D + P — releasing the zero tail removes no published byte |
| `set_mode()` | Valid | In-memory transition only |
| Originating `Snapshot` destroyed | Valid | B |
| Concurrent write | Valid | B + D — appends and zero-fill never rewrite a published byte |
| `DB` destruction | Valid | B + D + P — the destructor's `shrink_to_fit` releases only the zero tail |
| — | | |
| Next `operator++()` | Invalid | Same as above |
| Iterator destroyed | Invalid | Same as above |
| Iterator moved | Valid | Same as above |
| Iterator copied | Not possible | Move-only, same as above |

#### `const Key&` from `keys_from` / `rkeys_from`

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | A + C — key iteration touches no file |
| `vacuum()` | Valid | A + C |
| File rotation | Valid | A + C |
| Seal | Valid | A + C |
| `set_mode()` | Valid | A + C |
| Originating `Snapshot` destroyed | Valid | C — the iterator pins the key-directory root itself |
| Concurrent write | Valid | C — a write path-copies; it never mutates a published node |
| `DB` destruction | Valid | A + C |
| — | | |
| Next `operator++()` | Contents replaced | The reference stays bound to the iterator's member; the bytes in it change |
| Iterator destroyed | Invalid | The member goes with it |

#### `DataEntryView` from `changes_since`

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | A + B — the spans are in owning scan storage |
| `vacuum()` | Valid | A + B |
| File rotation | Valid | A + B |
| Seal | Valid | A + B |
| `set_mode()` | Valid | A + B |
| `Snapshot` passed to `changes_since` destroyed | Valid | B — the iterator copies the state reference out of it |
| Concurrent write | Valid | A + B — the stream's upper bound is fixed at construction |
| `DB` destruction | Valid | A + B |
| — | | |
| Next `operator++()` | Invalid | The scan buffer is cleared before the next entry is read |
| Iterator destroyed | Invalid | The buffer goes with it |
| Iterator copied | Not possible | `ChangeIterator` is move-only |

#### `Snapshot`

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | Holds a reference-counted state; a later publication does not disturb it |
| `vacuum()` | Valid | Pins every file it names — vacuum's unlink leaves them readable |
| File rotation | Valid | Pins the pre-rotation file set |
| Seal | Valid | Reads published offsets only |
| `set_mode()` | Valid | The snapshot carries the mode it captured |
| Concurrent write | Valid | A snapshot is a frozen state; writes publish a new one |
| `DB` destruction | Valid | Carries no reference to the `DB` |
| — | | |
| Moved from | Invalid | A moved-from `Snapshot` holds no state. Only destruction and assignment are supported on it — the same rule the standard library applies to its own move-only types |

### A view may outlive the `Snapshot` it came from

`Snapshot::iter_from`, `keys_from`, `riter_from` and `rkeys_from` hand
the iterator its own reference to the engine state, and `changes_since`
copies the reference out of the `Snapshot` it is given. The binding is
to the iterator, not to the snapshot. Destroying or moving the
`Snapshot` mid-iteration is safe, and so is returning an iterator from
a scope where the `Snapshot` was a local.

The reverse is not true: a span belongs to the iterator that produced
it, and does not outlive it. That binding is enforced, not just stated:
`EntryIterator`, `ReverseEntryIterator` and `ChangeIterator` are
move-only, so a span can never be separated from the buffer it
addresses by copying the iterator. Moving is safe — the buffer travels
with the spans.

### `DB` destruction

No operation may be in flight when `~DB` runs. That is a caller
precondition, not something the engine enforces.

Views already taken stay valid. An iterator, a `Snapshot`, and the
spans they hand out hold their own references to the engine state and
to every data file it names, so they remain valid and remain readable
after the `DB` is gone.

An operation *racing* the destructor is undefined: a read may throw,
and a write may be truncated away by the destructor's final release of
the zero-filled tail and lost. Undefined here does not extend to data
already written. Data files are append-only and the destructor's only
destructive act is truncating the active file to its logical end, so
the worst on-disk outcome is a shorter valid prefix of the committed
history — every surviving entry keeps its CRC, and a cut that lands
mid-entry is cleaned at the next `open`, which rescans a hint-less data
file and resizes it to its committed end. The directory lock is
released only after that truncation completes, so no second process can
open the directory while teardown is still writing.
