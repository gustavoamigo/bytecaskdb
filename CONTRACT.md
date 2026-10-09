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

**Write**: a `put`, `del`, `del_range`, `apply_batch` or `ingest` call. A
write is **acknowledged** when the call returns normally.

**Sequence**: the number the engine assigns to every entry it appends,
batch markers included. A write's sequence is the highest one it was
assigned, reported in its `CommitResult`.

**Visible**: a write is visible when a `get`, `contains_key`, `snapshot` or
iterator taken after it can observe it.

**Durable**: a write is durable when it survives a process crash or a
power loss and is present after the next `open`. `durable_sequence()` is
the highest sequence confirmed durable.

**Committed unit**: a standalone entry, or a batch from its `BulkBegin`
through its `BulkEnd`. Recovery takes a unit whole or not at all.

**Recovery-equivalent**: the state readers see agrees with what `open` on
the current files would produce.

**Degraded**: the engine has met a failure it will not write past. Every
write throws `DbDegraded`, with the reason in `degraded_reason()`; reads,
snapshots and iterators keep working, on a recovery-equivalent state.
`resume()` recovers in process, and `open` after a restart recovers the
same way. Degrading never publishes the write that failed, with one
exception a caller can see: a write whose file rotation failed after the
write itself was synced is applied, visible at once, and still thrown
(*`apply_batch`*, Consistency).

---

## Conditions

What the engine assumes of its storage and requires of its caller. The
guarantees in this document hold when these do.

| Condition | What is required | If it does not hold | Proved by |
|---|---|---|---|
| **Storage keeps what it confirmed** | Bytes an `fdatasync` reported written stay as written, and so does a synced directory entry. A failed `fdatasync` is handled (*`apply_batch`*, I/O Failure Safety); one that reports success for bytes it did not write is not detected. | `open` and `resume()` recover what the device holds. Damage they can see is refused (*`open`*); damage shaped like a crash is not. | n/a: an assumption, not a behaviour |
| **Ranges are non-empty** | Every `[from, to)` — `del_range` on `DB` and `WritePlan`, `ensure_range_unchanged`, `Snapshot::count_keys` — has `from < to`. | `std::invalid_argument` before anything is written or read; a `WritePlan` is left as it was. A range-delete entry with `from >= to` written before this rule still recovers, and deletes nothing. | "Ranges: from >= to is refused, before anything is written" |
| **No vacuum during a manifest transfer** | `vacuum()` does not run between `create_manifest()` and the end of the file copy the manifest serves. | Vacuum unlinks files by path, so a copy in progress fails with `ENOENT`. Serialising the two is the caller's. | none |

---

## Engine-wide guarantees

### Limits

Every limit is checked before any I/O. Reaching one throws, leaves no
byte in a data file and does not degrade the engine.

| Limit | Value | Reached by | What happens | Proved by |
|---|---|---|---|---|
| Key size | `max_key_bytes` (default 4 KiB; ceiling 65,535 bytes) | a key or range bound in any write or guard | `std::invalid_argument` | "Size limits: put rejects oversized key", "Size limits: del rejects oversized key", "Size limits: del_range rejects oversized boundary", "Size limits: guard methods validate key size", "Size limits: WritePlan validates from snapshot limits" |
| Value size | `max_value_bytes` (default 4 MiB; ceiling 268,435,455 bytes) | a value in any write | `std::invalid_argument` | "Size limits: put rejects oversized value" |
| Option ceilings | `max_key_bytes` 65,535; `max_value_bytes` 268,435,455; `max_file_bytes` 3 GiB; `recovery_threads` at least 1 | `open` | `std::invalid_argument` before the directory is created or locked. `max_file_bytes = 0` is valid: every write seals the file it went into. | "Limits: open rejects options above the hard ceilings", "Options: recovery_threads = 0 is refused at open", "Options: max_file_bytes = 0 seals a file after every write" |
| Bytes per write | 1 GiB, batch markers included | a `WritePlan`; an atomic batch in an `ingest` slice | `std::invalid_argument` | "Limits: a plan larger than the per-write byte limit is refused before any I/O", "Limits: ingest refuses an atomic batch past the per-write byte limit" |
| File size | `max_file_bytes` | the active file reaching it | The file is sealed and a new one started. A write never spans two files, so a value larger than `max_file_bytes` gets a file of its own. Writers that commit together can carry a file past `max_file_bytes`, never past 4 GiB. | "Options: a value larger than max_file_bytes gets a file of its own", "Limits: a commit group ends before a slot that would carry the file past the group limit" |
| File ids | 1,048,575 per process | every rotation, vacuum compaction and `resume()` takes one; `open` renumbers | A write that would need a new file throws `std::runtime_error` and appends nothing; smaller writes still commit. `vacuum()`, `resume()` and an `ingest` slice that needs a new file refuse the same way before touching a file, and `resume()` leaves the engine degraded. `open` refuses a directory with more data files than ids. | "Limits: the last file id is used, then a write that needs another is refused until a reopen", "Limits: ingest refuses a slice that needs a file id it does not have, before any I/O", "Limits: resume without a file id leaves the engine degraded and untouched" |
| Sequence | 2^48 − 1 | `ingest` of a higher sequence; a leader's write after it | `ingest` throws `std::invalid_argument` for any entry above it, whatever its type. A write that would need a sequence above it throws `std::runtime_error` and appends nothing. At a million writes a second a leader reaches it after about 8.9 years. | "Limits: ingest takes the last packable sequence and refuses the next", "Limits: a KeyDirEntry packs each field at its ceiling and refuses one past" |

### Sequences

| Guarantee | Proved by |
|---|---|
| Every entry appended, batch markers included, carries its own sequence, and sequences strictly increase in commit order within a process: a later write's sequence is above every earlier one's. | "CommitResult sequence is monotonic across write operations", "batch write covers marker sequences" |
| A sequence is never reused, failure or not. After a write that threw, the next write's sequences lie above every sequence the failed one may have put in the file, so no two entries for one key ever share a sequence with different values. Gaps are normal, and nothing a caller sees depends on contiguity. | `[fault_sweep]`, "a write after a restart does not reuse a batch marker's sequence", "recovery refuses two different writes under one sequence" |
| The highest sequence wins. For any key, the entry with the highest sequence is its state, across files and whichever order recovery reads them in. | `[model]`, "Causality survives recovery" |
| After `open` or `resume()`, every new sequence is above every sequence on disk, those of failed writes included. | "apply_resume: advances next_seq past highest seen sequence", "a write after a restart does not reuse a batch marker's sequence" |

### Durable sequence

`durable_sequence()` is the highest sequence confirmed durable.

| Guarantee | Proved by |
|---|---|
| It never decreases while the engine runs. After `open` or `resume()` it equals the highest recovered sequence: every recovered entry is durable. | "durable_sequence correct after recovery", "durable_sequence correct after resume" |
| It advances only once an `fdatasync` has confirmed every entry up to it, `sync = false` writes that landed before that sync included. Writes made with `sync = false` alone do not advance it. | "durable_sequence reflects sync writes", "durable_sequence stays at zero for nosync-only writes", "CommitResult nosync writer coalesced with sync writer is durable" |
| A `sync = true` write is never visible before it is durable. | "class F: key not visible after commit sync failure", "class G: key not visible after rotation sync failure" |
| A reader, local or a follower fed by `changes_since`, whose `durable_sequence()` is at or above a write's sequence sees every entry of that write. | `[prove_repl]`, "ingest sequence continuity: durable_sequence matches max ingested" |
| `durable_sequence(min_sequence, timeout)` returns at once when `min_sequence` is 0, already reached, or `timeout` is not positive. Otherwise it waits until the durable sequence reaches `min_sequence`, the timeout passes or the DB closes, then returns the durable sequence. `std::chrono::milliseconds::max()` waits with no deadline. | "durable_sequence returns immediately for reached targets", "durable_sequence long-poll wakes on sync write", "durable_sequence long-poll times out on idle DB", "durable_sequence with no timeout returns an unreached target at once", "durable_sequence wakes a waiter when the DB closes", "Limits: durable_sequence waits with a timeout the clock cannot represent" |

### `CommitResult`

Every write that commits returns one; `del` and `apply_batch` return
`std::optional<CommitResult>` and report `nullopt` for an absent key or a
conflict. [`docs/commit_result_api_design.md`](docs/commit_result_api_design.md)
has the API across the bindings.

| Guarantee | Proved by |
|---|---|
| `sequence` is the highest sequence the write was assigned: the `BulkEnd` marker's for a batch of more than one operation. It is 0 when nothing was appended, as for an empty or guard-only plan. | "CommitResult sequence equals BulkEnd sequence for multi-op batch", "CommitResult empty plan returns durable zero sequence", "CommitResult guard-only plan returns durable zero sequence" |
| `durable` is true when `fdatasync` confirmed the write before the call returned: always for `sync = true`; for `sync = false` when a sync writer in the same group, or a rotation, covered it. | "CommitResult sync write is durable", "CommitResult solo nosync write is not durable", "CommitResult nosync writer coalesced with sync writer is durable" |
| A `sync = true` write that appends nothing still returns only once every earlier write is durable, so `apply_batch({.sync = true}, WritePlan{})` bounds what a crash can take from a caller that writes with `sync = false`. With `sync = false` it returns at once. If the `fdatasync` it waits for fails, the engine degrades. | "apply_batch: an empty sync plan makes earlier unsynced writes durable with one fdatasync", "apply_batch: a guard-only sync plan makes earlier unsynced writes durable", "apply_batch: an empty sync plan on a fresh DB does not sync", "apply_batch: an empty sync plan whose fdatasync fails degrades the engine" |

### What the engine refuses

Checks that run in every build, on every publication and at every `open`
and `resume()`. They are a backstop: a violation means a bug, and the
engine stops rather than publish it.

| Refused | What happens | Proved by |
|---|---|---|
| A publication that would move a sequence, a file id or the durable sequence backwards, or make a `sync = true` write visible before it is durable. | Not published; the engine degrades, and reads keep the previous state. | none |
| A recovered state, after `open` or `resume()`, whose active file is missing, that owes an `fdatasync`, or whose per-file accounting misses a file or holds incoherent sequence bounds. | `std::runtime_error`: `open` fails; `resume()` fails and the engine stays degraded. | none |
| A data file created under a name this database has used before, as a data or a hint file. | The process aborts. This is the only failure that ends the process; every other one throws or degrades. | "createDataFileForWrite panics when the data file already exists", "createDataFileForWrite panics when the stem was already hinted", "renameDataFileExclusive panics rather than replacing a live file" |

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

A conflict is reported no earlier than the moment a retry could see
what the plan lost to. Plans are validated against writes that are
applied but not yet published (the commit pipeline's head); a snapshot
never contains those, so a plan that lost to one is held until that
write is published — at most one flush — and then reported. A plan whose
snapshot is already behind the published state is reported at once. This
is the engine's guarantee against retries that cannot succeed; it is
*not* a guarantee that a fresh snapshot after a conflict holds the value
that won, which a later write may already have superseded. Under genuine
contention — several writers on one key — a retry can still lose; backing
off between retries is the caller's responsibility, and a tight retry
loop is not a supported client pattern.

A `WritePlan` with guards but no write operations (no `put` or `del`)
is not empty — guards are evaluated. If all guards pass, the plan
returns `true` with no I/O and no state change. If any guard fails,
the plan returns `false`. This is intentional: it allows callers to
validate preconditions without committing writes, using the same
conflict-detection mechanism.

### I/O Failure Safety

If any I/O operation (append, sync) throws during execution:

- The caller must receive the exception.
- The published key directory must reflect zero operations from this call
  until `resume()` or a reopen; what either brings back is under
  *Consistency*. `next_seq` must be advanced past all sequences consumed by appends that reached
  the file, to prevent reuse of those sequence numbers on the next write.
- The disk may contain none, some, or all of the bytes from this write.
  The engine must not assume what was written. Any append failure —
  whether nothing reached disk, a partial write, or a full write that
  returned an error — degrades the engine unconditionally. Any `fdatasync`
  failure (commit sync or rotation sync) likewise degrades the engine:
  bytes are in the OS page cache but durability is not confirmed, and the
  key directory does not reflect those bytes. `resume()` rewrites and
  syncs the active file, scans it, truncates garbage, replays valid
  committed entries, and creates a fresh active file, restoring normal
  operation.
- The DB must remain operational for subsequent calls.

### Allocation Failure Safety

An operation that cannot allocate fails; the engine does not try to go on
without the memory. What it guarantees is how it fails:

- Before anything is appended: the operation throws (`std::bad_alloc`)
  and nothing changed. The engine stays writable.
- After its entries reached the file and before a state covering them was
  published — or on any other throw on that stretch that no I/O handler
  degrades for: the operation throws and the engine refuses writes as
  degraded, with the published state unchanged. `resume()` replays what
  reached the file, as after a failed `fdatasync`. Refusing allocates
  nothing: if the degraded state cannot be published, `is_degraded()`,
  `degraded_reason()` and every write still report it.
- A writer never waits forever on a flush that threw, and the process is
  never ended by the engine for lack of memory. Dropping the last handle
  of a snapshot, an iterator or any other key directory version never
  throws: what cannot be freed for want of memory is freed later, or
  leaked, never freed while a version reaches it (#390).
- `degraded_reason()` returns a copy and may throw `std::bad_alloc`.

The counted allocation-failure sweep fails every allocation of each swept
operation, once and from then on, and checks these
(`docs/correctness_validation.md`).

### Consistency

The in-memory state visible to callers must always be recovery-
equivalent, with one permitted exception: writes completed with
`sync=false` may be visible in memory but lost on crash — or, without a
crash, after a failed `fdatasync` whose pages the kernel evicts before
`resume()` reads them back. `resume()` then refuses, and a reopen
recovers the state without them.

The specific guarantees:

- **Write succeeds, `sync=true`**: the data must be on disk AND
  visible to subsequent reads.
- **Write succeeds, `sync=false`**: the data must be visible to
  subsequent reads. It may or may not survive a crash.
- **Write throws**: the data may or may not be partially on disk, and
  nothing from the write is visible before `resume()` or a reopen. After
  either, the write is indeterminate, as a timed-out write is: every
  complete unit that reached the file — a standalone entry, or a batch
  through its `BulkEnd` — is replayed and becomes visible as if the write
  had returned, and anything incomplete is cut. A caller that needs to
  know re-reads after `resume()`. One case publishes at once: a rotation
  that fails after the write was synced (class H under *Definitions*)
  makes the write visible and still throws, because the write itself
  succeeded. Recovery must reach a consistent state either way.

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

---

## `resume`

- Garbage is only ever above the **published extent** — the active file's
  `total_bytes` in the last published state. Damage below it is not a
  failed write's leftovers but corruption of acknowledged data, and has no
  recovery contract: `resume()` throws `std::runtime_error` before
  truncating anything, the file is left exactly as found, and the engine
  stays degraded on every retry. The same happens when `sync=false` writes
  a failed `fdatasync` left off the device were evicted from the page
  cache before `resume()` read them back: they are gone, and the key
  directory holds no older version of the keys they overwrote. Reopening
  the database recovers the state the device holds. An I/O error during
  the scan is rethrown unchanged rather than read as the end of the file.

### Rotation Safety

If a multi-entry batch fails mid-write, an orphaned `BulkBegin` marker
may exist on the active file. The engine degrades: `resume()` scans the
active file, truncates the orphaned batch (no `BulkEnd` found), and creates
a fresh active file.

---

## `ingest`

Applies pre-sequenced entries from a leader to a follower's storage.

| Property | Contract |
|----------|----------|
| **Mode requirement** | Throws `std::logic_error` if `mode() != Mode::Follower`. |
| **Degraded check** | Throws `DbDegraded` if the engine is degraded. |
| **Idempotency** | Entries with `sequence <= durable_seq` are silently skipped. Safe for restart-on-failure semantics. |
| **Limits** | Checked before any I/O, and nothing is written on a refusal: a key or value over the configured size limit, a sequence above 2^48 − 1, or an atomic batch over `kMaxBatchBytes` throws `std::invalid_argument`; a slice that needs more new files than there are file ids left throws `std::runtime_error`. See *Limits*. |
| **Batch-safe rotation** | `BulkBegin`/`BulkEnd` pairs always land in the same data file. File rotation only occurs at boundaries where no batch is open. |
| **Durability** | Every chunk is `fdatasync`'d before rotation. The final chunk is `fdatasync`'d before `store_state` publishes. |
| **Sequence advancement** | After ingest, `next_seq = max(next_seq, max(ingested sequences) + 1)`. Monotonically non-decreasing. |
| **Degraded on I/O failure** | Same pattern as `apply_batch`: on `writev`/`fdatasync` failure, advance sequence to prevent reuse, go degraded, rethrow. |
| **Atomicity** | If ingest throws, no partial state is published to readers. |
| **Causality** | Entries are applied in the sequence order provided by `changes_since`. If entry A has a lower sequence than entry B, A is applied before B. The follower's state reflects the same causal ordering as the leader's write history. |
| **I/O failure safety** | If any I/O operation throws, the published key directory reflects zero entries from this call. The engine degrades; `resume()` restores normal operation. `resume()` replays every complete unit it finds in the active file — a standalone entry, or a batch through its `BulkEnd` — so after it the follower may hold the first units of the failed slice, never part of a batch. Re-delivery from `follower.durable_sequence()` then skips those and applies the rest. |
| **Slices end at batch boundaries** | A slice that ends between a `BulkBegin` and its `BulkEnd` throws `std::invalid_argument` before anything is written. Published, it would expose part of an atomic batch, and a rotation after it would seal the `BulkBegin` into a file with no `BulkEnd`, which recovery drops: the follower would lose entries it had synced and published ([#188](https://github.com/gustavoamigo/bytecaskdb/issues/188)). The caller cuts slices after a `BulkEnd` or a standalone entry. |

---

## vacuum_compact

Rewrites a sealed data file, discarding dead entries. The old file
must be deferred for deletion when no readers reference it.

### Data Preservation

Every key-value pair readable before `vacuum_compact` is called must
be readable after it returns, with the same value, and after any later
reopen. The engine must not lose data or introduce phantom entries.

A tombstone may be dropped only when no other file can hold an older Put
of a key it deletes: otherwise that Put comes back at the next open.
Recovery decides this once per open and compaction applies it; a
tombstone written since the open is always kept. A file whose Puts are all
dead may be removed without a scan only if it holds no tombstone.

A Put may be dropped as dead only once the write that superseded it is
durable. Before the old file can be unlinked, vacuum makes every write it
judged liveness by durable, even `sync = false` ones: a power cut may lose
a write the caller did not sync, never the durable value it replaced.

### Retention

Vacuum drops no entry with a sequence above `VacuumOptions::retain_after`:
a dead Put or a droppable tombstone above it is copied into the compacted
file, and a file is removed whole only when its `max_sequence` is at or
below it. `kNoRetention` (−1), the default, restricts nothing. The value is
the caller's: the replication service passes the lowest position a follower
it counts on could resume from, on every node.

### Size Accounting

A published file's `total_bytes` equals its size on disk. Recovery
seeds `total_bytes` from the file's length, so anything the compacted
file contains has to be counted as it is written — including the
`BulkBegin` / `BulkEnd` markers, which compaction preserves like any
other entry. `tombstone_bytes` and `marker_bytes` count the tombstones
and markers the file holds; recovery rebuilds both from the hint files.
A `file_stats()` reading must not change across a restart.

### Progress

`vacuum()` returns `true` only when a file was actually reclaimed.
A file is eligible only for its dead Put bytes (`total_bytes` minus
live, tombstone and marker bytes), so a file that is live data,
tombstones and markers and nothing else is never selected. If a
selected file's compaction still cannot make it smaller, the engine
discards the staged copy and tries the next eligible file, returning
`false` when none gets smaller, rather than publishing an identical file.
A file whose every entry is above `retain_after` is not eligible.

This is a termination guarantee, not only an efficiency one: every
`true` removes bytes, so
`while (db.vacuum({.fragmentation_threshold = 0.0})) {}` terminates.

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
- The temporary file (`.data.tmp`) must be removed before the exception
  propagates. A removal that fails too is left to the next recovery and
  must not replace the original exception.
- The DB must remain operational.

If the `fdatasync` that makes those superseding writes durable fails, the
engine degrades and vacuum throws before committing; the old file stays in
the published state and on disk. On an engine that is already degraded,
vacuum throws `DbDegraded`.

If the commit step (`vacuum_commit`) fails after the new file is
written and renamed:

- The old file must remain in the published state.
- The new file exists on disk but is unreferenced by `key_dir`.
- Recovery or next vacuum must handle it.

If the process is killed anywhere between the rename and the unlink of the
old file — before or after the commit:

- Both files are on disk with the same entries under the same sequences.
- The next open must succeed with every key the old file held, and must
  leave sequence-disjoint files: recovery deletes the new file, undoing the
  vacuum, once it has checked that every entry of the new file is in the
  old one.
- Two files that share sequences and fail that check must make `DB::open`
  throw, with nothing deleted.

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

Every `data_path` in `FileManifest::files` exists on disk at the time of
return. `worker_.drain()` waits for hint generation to finish before the
list is built, but a `hint_path` may name a file that does not exist: the
worker only logs a failed hint write. A missing hint costs no data. A hint
is a rebuildable index, and the open of a directory holding the copied
files writes the hint of any data file that has none (#349). A caller
copies a hint when it exists and skips it when it does not.

### I/O failure safety

If the pre-rotation sync fails, the engine degrades, the exception
propagates and no manifest is produced. The active file is not sealed.
As after any failed `fdatasync`, a later sync cannot be trusted to write
what the failed one did not (#231), so writes throw `DbDegraded` until
`resume()` rewrites and syncs the file (#281).

If `prepare_rotation` fails (active file sealed but new file creation
fails), the engine degrades — same pattern as `execute_slots` and
`ingest`. The sealed active file cannot accept further appends;
degrading forces `resume()` before the next write. `resume()` creates a
fresh active file and clears the degraded state.

---

## `set_mode` / `mode`

Controls which write paths are available.

| Property | Contract |
|----------|----------|
| **`set_mode(Mode)`** | Acquires `write_mu_` to ensure no in-flight write straddles the transition. Stores mode with release semantics. |
| **Stepping down is durable** | `set_mode(Follower)` on a leader first `fdatasync`s the active file, so on return `durable_sequence()` covers every write acknowledged before the call, `sync=false` ones included, and `changes_since` can ship them all to the next leader. A failed `fdatasync` degrades the engine and throws; the mode is unchanged. |
| **`mode()`** | Lock-free atomic read with acquire semantics. Same pattern as `is_degraded()`. |
| **Leader mode** | Normal writes allowed; `ingest` throws `std::logic_error`. |
| **Follower mode** | Normal writes (`put`, `del`, `del_range`, `apply_batch`) throw `DbFollowerMode`; `ingest` allowed. Reads, snapshots, vacuum, and `resume()` work in both modes. |
| **Initial mode** | Set from `Options::initial_mode` (default `Mode::Leader`) after recovery completes. |

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

### Ordering

A hint indexes only bytes the device holds. Its data file is `fdatasync`ed
before the hint is written — at rotation, before the sealed file is handed
to the hint worker; at vacuum, before the compacted copy is renamed; at
open, by rewriting and syncing a hint-less file before scanning it — so a
power loss can lose a hint, never leave one pointing into bytes that are
gone. `PageCacheModel::hint_written` checks this at every hint the tests
write.

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

## `open`

Recovers the database a crash or power loss left, on storage that keeps
its promises, and refuses any other. Proved by the `[recovery]` tests.

| Property | Contract |
|----------|----------|
| **Storage assumption** | Bytes an `fdatasync` reported durable stay as written, and so does a synced directory entry. `open` recovers what a crash or power loss leaves on storage that honours this. It does not recover from storage that does not. |
| **Durability** | Every write acknowledged as durable is in the opened database. `sync = false` writes made after the last sync may be missing; those that survive form a prefix of the write order, and a batch is in whole or not at all. |
| **Only the active file is cut** | Every other file was synced whole before the next was started, so a crash can tear only the file being written. `open` cuts that file at its last committed record — whole, if its first page was lost — and syncs the cut. A tail of zeros, preallocated space, is trimmed in any file. |
| **Damage is refused** | A state no crash can leave on such storage is damage. Where `open` can see it, it throws and cuts nothing: data past the last committed record in a file that is not the newest, such data in more than one file, two files sharing sequences other than an interrupted vacuum's pair. `open` does not open a best-effort subset of a damaged database: to the caller that is data loss with no error. |
| **Damage that cannot be seen** | Damage that leaves exactly what a crash would is outside this contract: `open` cannot refuse what it cannot tell from a torn write. `docs/bytecask_design.md`, *Recovering a Hint-less File*, lists the known shapes. |
| **Options** | `max_key_bytes`, `max_value_bytes` or `max_file_bytes` above its hard ceiling (see *Limits*), or `recovery_threads = 0`, throws `std::invalid_argument` before the directory is created or locked. `max_file_bytes = 0` is valid: every write seals the file it went into. |
| **Hint files** | A hint is a rebuildable index. One that fails its CRC, or that a read fails on, is rebuilt from its data file and costs no keys. |
| **`fail_recovery_on_crc_errors = false`** | The operator's explicit opt-out from refusal, for one case: a data file whose hint is bad and that cannot be rescanned is skipped with a warning on stderr, and the database opens without its keys. It does not relax the rules on cutting above. |

---

## `close`

Shuts the engine down and reports whether the shutdown kept every
acknowledged write. `~DB` calls it when the caller did not, and swallows
its errors. Proved by the `[close]` tests.

| Property | Contract |
|----------|----------|
| **Durability** | Returns normally only if every write the DB acknowledged — `sync = false` ones included — is durable, and the sync, trim and hint writes all succeeded. |
| **Failure reporting** | Healthy engine: a failed `fdatasync`, trim or hint write throws `std::system_error`. Degraded engine: throws `DbDegraded` when writes were acknowledged above `durable_seq`; it does not sync, since a sync after a failed one proves nothing (#231). A degraded engine whose acknowledged writes are all durable closes normally. |
| **Closed either way** | Whatever it throws, the engine is closed and the directory lock released. A failed close leaves on disk what a crash would, and the next `open` recovers it the same way. |
| **Concurrency** | Waits for a running vacuum and for every in-flight write to be published. A write racing `close` either commits before it — and is covered by its durability verdict — or throws `DbClosed`. A read racing it returns a result or throws `DbClosed`. |
| **Afterwards** | Every operation throws `DbClosed` (a `std::logic_error`), except `mode()`, `is_degraded()` and `degraded_reason()`, which keep answering. `Snapshot`s, iterators and spans taken before stay valid and readable. |
| **Idempotency** | A second `close` returns at once and reports nothing: a retry cannot make lost pages durable. |

---

## `changes_since`

Returns a lazy iterator over committed, durable entries in ascending
sequence order. Validated implicitly through the E2E ingest pipeline
tests, not through standalone `changes_since` proof tests.

| Property | Contract |
|----------|----------|
| **Durable boundary** | Only entries confirmed by `fdatasync` are yielded. The upper bound is `min(snap.sequence(), durable_sequence)`. Entries from NoSync writes not yet covered by a subsequent `fdatasync` are excluded, even if visible via snapshots. |
| **Completeness** | Every committed durable entry with `sequence > from_sequence` at snapshot time is yielded exactly once, provided every vacuum since `from_sequence` was written ran with `retain_after <= from_sequence`. A vacuum with a higher `retain_after` (or none) may have dropped dead Puts and tombstones above `from_sequence`; the stream then skips them silently, and the caller must re-bootstrap instead (#168). |
| **Ordering** | Entries are yielded in strictly ascending sequence order. |
| **Batch integrity** | Incomplete batches (orphaned `BulkBegin` without `BulkEnd`) are excluded. `BulkBegin`/`BulkEnd` markers are preserved in the output. |
| **Vacuum transparency** | After `vacuum_compact_file`, entries retain original sequences and batch markers. Above the vacuum's `retain_after`, `changes_since` over a vacuumed file yields the same entries as over the pre-vacuum file. |
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
  Neither touches the mapping; both lower `mmap_end_` with the file,
  `resume()` before it cuts, so an `ftruncate` that cuts the file and
  then reports an error leaves no page past the new end readable. By
  P, neither can take away a page a published offset points into, so no
  live span loses its backing. A read at or past the new bound takes
  the `pread` fallback and fails as a clean short read rather than
  faulting on a page beyond end of file.

Under `pread` neither case arises: every span an iterator hands out
points into that iterator's own `io_buf_`, which no file event can
reach. Under the buffer pool an iterator is lent spans straight into a
pool frame when the entry sits inside one resident frame, and holds a
`FrameLease` — a pin on that frame — until it advances. A pinned frame
is immutable and cannot be evicted or refilled, and the pool itself is
held by the data file, which the iterator holds, so no file event and
not even `DB` destruction can reach the frame while the span is live.
Call that **L**.

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

#### `EntryView` spans, `io_backend` = `pread`

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

#### `EntryView` spans, `io_backend` = `buffer_pool`

Spans point into a pool frame under lease when the whole entry lies in
one resident frame, and into the iterator's `io_buf_` otherwise. Both
are covered below.

| Event | Verdict | Why |
|-------|---------|-----|
| `resume()` | Valid | B + L — truncation lowers the active file's size; the lent entry lies below it and its frame is pinned |
| `vacuum()` | Valid | B + L — the frame outlives the unlink; file ids are never reused, so nothing looks the frame up again |
| File rotation | Valid | B + L — the frames of the old active file become evictable, but not while pinned |
| Seal | Valid | B + L |
| `set_mode()` | Valid | In-memory transition only |
| Originating `Snapshot` destroyed | Valid | B |
| Concurrent write | Valid | B + L — an append extends a frame only past the size the reader is bounded by; a fill never touches a pinned frame |
| Eviction under memory pressure | Valid | L — CLOCK passes over a pinned frame; it never waits for it |
| `DB` destruction | Valid | B + L — the pool and its counters are held by the data file, which the iterator holds |
| — | | |
| Next `operator++()` | Invalid | The lease is released and the buffer reused |
| Iterator destroyed | Invalid | The lease and the buffer go with it |
| Iterator moved | Valid | The lease and the buffer move with the iterator |
| Iterator copied | Not possible | Move-only, same as above |

A lease held across a long loop body keeps one frame out of the pool's
hands for that long. That is one frame, not a stall: eviction skips it.

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
| `DB::close()` / destruction | Valid | B + D + P — close's `shrink_to_fit` releases only the zero tail |
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

### `DB` close and destruction

`close()` may race other operations; see *`close`*. No operation may be
in flight when `~DB` runs: that is a caller precondition, not something
the engine enforces, since it ends the object's lifetime.

Views already taken stay valid. An iterator, a `Snapshot`, and the
spans they hand out hold their own references to the engine state and
to every data file it names, so they remain valid and remain readable
after the `DB` is closed or gone.

An operation *racing* the destructor is undefined: it uses an object
whose lifetime is ending. Undefined here does not extend to data
already written. Data files are append-only and the close's only
destructive act is truncating the active file to its logical end, so
the worst on-disk outcome is a shorter valid prefix of the committed
history — every surviving entry keeps its CRC, and a cut that lands
mid-entry is cleaned at the next `open`, which rescans a hint-less data
file and resizes it to its committed end. The directory lock is
released only after that truncation completes, so no second process can
open the directory while teardown is still writing.
