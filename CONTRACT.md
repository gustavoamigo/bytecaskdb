# ByteCaskDB — Correctness Contract

What the engine guarantees, and what it does not. *Definitions* and
*Conditions* come first, then one table per operation: each row is one
guarantee, and its second cell names what fails if the row is broken: the
Catch2 tests, by name or by tag, or a check script the repository runs,
or none.

The test for every sentence here: if it could change while every
guarantee stays the same, it is mechanism and belongs in
[`docs/bytecask_design.md`](docs/bytecask_design.md), not here. The
contract names only what `include/bytecask.hpp` declares, and CI
enforces it. A change to a guarantee is named in the pull request that
makes it: added, widened, narrowed or removed.

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
(*`apply_batch`*).

---

## Conditions

What the engine assumes of its storage and requires of its caller. The
guarantees in this document hold when these do.

| Condition | What is required | If it does not hold | Proved by |
|---|---|---|---|
| **Storage keeps what it confirmed** | Bytes an `fdatasync` reported written stay as written, and so does a synced directory entry. A failed `fdatasync` is handled (*`apply_batch`*); one that reports success for bytes it did not write is not detected. | `open` and `resume()` recover what the device holds. Damage they can see is refused (*`open`*); damage shaped like a crash is not. | n/a: an assumption, not a behaviour |
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
| A publication that would move a sequence, a file id or the durable sequence backwards, or make a `sync = true` write visible before it is durable. | Not published; the engine degrades, and reads keep the previous state. | "pipeline: publishing a state that moves a sequence or a file id backwards degrades the engine", "pipeline: publishing a state that owes an fdatasync degrades the engine" |
| A recovered state, after `open` or `resume()`, whose active file is missing, that owes an `fdatasync`, or whose per-file accounting misses a file or holds incoherent sequence bounds. | `std::runtime_error`: `open` fails; `resume()` fails and the engine stays degraded. | none |
| A data file created under a name this database has used before, as a data or a hint file. | The process aborts. This is the only failure that ends the process; every other one throws or degrades. | "createDataFileForWrite panics when the data file already exists", "createDataFileForWrite panics when the stem was already hinted", "renameDataFileExclusive panics rather than replacing a live file" |

---

## `apply_batch`

The one write path: `put`, `del` and `del_range` are `apply_batch` with a
plan of one operation and no guards. A plan applies all of its operations,
in order, if every guard holds at commit time, and none otherwise;
`nullopt` reports a conflict.

| Guarantee | Proved by |
|---|---|
| **Atomic.** A plan's operations become visible together or not at all. No read, snapshot or iterator sees part of a plan, and a reopen holds the plan whole or not at all. | "apply_batch succeeds with no conflict", "DB WriteOptions sync=false apply_batch results visible", "vacuum compact handles batch with mixed put/del", `[prove_recovery]`, `[model]` |
| **Ordered.** Of two writes, the one that returned before the other began is the older: the later one's state wins for every key both touch. Within a plan, operations apply in the order they were added. The order survives a reopen, whichever order recovery reads the files in. | "WritePlan: interleaved puts and del_range — correct causality", "Causality survives recovery", "DB recovery: order-independent tombstone", "DB recovery: cross-file tombstone suppresses stale put" |
| **Durable before visible.** With `sync = true` the plan is durable before it is visible to anyone, and before the call returns. With `sync = false` it is visible on return and may be lost to a crash; `durable_sequence()` says when it has become durable. | "class F: key not visible after commit sync failure", "pipeline: sync write is invisible until its fdatasync returns; a writer appended behind it lands in the next flush", "pipeline: nosync write behind an in-flight flush becomes visible after it, without an fdatasync of its own" |
| **A conflict leaves no trace.** When a guard, a range guard or the check on the plan's own write set fails, the call returns `nullopt`: nothing is appended, nothing becomes visible, and the plan's snapshot stays usable. Checking a guard may read the key's record; it never writes. | "apply_batch ensure_present fails when key absent", "apply_batch ensure_absent fails when key exists", "apply_batch ensure_unchanged fails on modification", "apply_batch ensure_range_unchanged fails on in-range modification", "CommitResult apply_batch conflict returns nullopt", "range conflicts match a model of the key range", "location tokens: plans checked and applied by location commit and conflict exactly as by key" |
| **Isolation is what the plan guards.** A plan with a snapshot is refused if any key it writes changed since the snapshot. A key it read and does not write takes part in the check only under `ensure_unchanged`, a range only under `ensure_range_unchanged`: guarded, the plan is serializable against every other write; unguarded, it has snapshot isolation. A plan without a snapshot is checked on `ensure_present` and `ensure_absent` alone. | `scripts/run_isolation_check.py`, nightly in `.github/workflows/isolation-nightly.yml`: Elle finds guarded plans strict-serializable and unguarded ones snapshot-isolated, with write skew as their only anomaly, under vacuum, injected `fdatasync` failures and SIGKILL (`docs/isolation_checking_design.md`). The pairwise checks: "validate_preconditions: del with snapshot triggers W-W check", "validate_preconditions: no snapshot skips W-W checks", "apply_batch ensure_unchanged fails on modification", "apply_batch ensure_range_unchanged fails on in-range modification", "range conflicts match a model of the key range" |
| **A conflict is reported once a retry can see what won.** A plan that lost to a write not yet published waits for that write to be published, at most one flush, before returning `nullopt`; a plan whose snapshot is already behind the published state is refused at once. This is not a promise that a fresh snapshot holds the winning value, which a later write may have superseded; under contention a retry can lose again, and backing off is the caller's. | "pipeline: a plan that loses to a write not yet published reports the conflict once a retry can see that write", "pipeline: a plan whose snapshot is already behind the published state reports its conflict at once", "a conflict waiting on a write whose flush fails returns when the engine degrades" |
| **A guard-only plan commits nothing.** Its guards are checked like any plan's. When they hold it returns `{sequence = 0, durable = true}` after appending nothing, and with `sync = true` it first makes every earlier write durable (*`CommitResult`*). | "apply_batch guards-only plan with no writes", "CommitResult guard-only plan returns durable zero sequence", "apply_batch: a guard-only sync plan makes earlier unsynced writes durable" |
| **A thrown write is unseen until `resume()`, indeterminate after.** On an I/O failure, an append or an `fdatasync`, the caller gets the exception, nothing from the plan is visible, and the engine degrades. After `resume()` or a reopen, every complete unit that reached the file is visible as if the write had returned, and anything incomplete is gone; a caller that needs to know re-reads. The bytes a failed write leaves are the engine's to clean up, never the caller's. One failure publishes at once: a write whose file rotation failed after the write itself was synced is visible and still thrown, because the write succeeded. | `[fault_sweep]`, "class F: key not visible after commit sync failure", "class G: key not visible after rotation sync failure", "pipeline: fdatasync failure fails every writer appended since the last flush and resume() recovers them", "resume() replays unpublished entries from active file" |
| **Out of memory is a failure like any other.** Before anything is appended: `std::bad_alloc`, and nothing changed. After the entries reached the file: the write throws and the engine refuses writes until `resume()`, which replays what reached the file. Refusing allocates nothing, so `is_degraded()`, `degraded_reason()` and every write report it even when the degraded state itself cannot be published, and no writer waits forever on a flush that threw. Dropping the last handle of a `Snapshot`, an iterator or any other key directory version never throws: what cannot be freed for want of memory is freed later, or leaked, never freed while a version reaches it. `degraded_reason()` returns a copy and may throw `std::bad_alloc`. | `[alloc_sweep]`, "alloc sweep: a version is released when nothing can be allocated", "alloc sweep: a failed publication leaves the base derivable", "refuse writes: a throw after the append refuses writes until resume()", "refuse writes: the refusal holds when the degraded state cannot be published", "refuse writes: a flush that throws releases the flush role" |
| **Readers always see a recovery-equivalent state**, with `sync = false` writes as the one exception: they can be lost to a crash, or without one when a failed `fdatasync`'s pages are evicted before `resume()` reads them back, in which case `resume()` refuses and a reopen recovers without them. Where the engine cannot keep this, it degrades rather than serve a state recovery would not produce. | `[prove_recovery]`, "reads work on a degraded DB", "resume() refuses when a failed fdatasync's pages were evicted, and a reopen recovers what the device holds" |

---

## `resume`

Recovers a degraded engine in process. A no-op when the engine is not
degraded.

| Guarantee | Proved by |
|---|---|
| **What reached the file is kept; what did not is cut.** After it returns, every complete unit in the active file is visible and durable: a failed write's units that landed are in, a batch that failed before its `BulkEnd` is cut whole, and a torn or garbage tail is gone. The engine accepts writes again, into a fresh active file. | `[prove_resume]`, "resume() recovers from degraded state", "resume() replays unpublished entries from active file", "resume() discards pending batch on CRC error in active file", "durable_sequence correct after resume" |
| **Nothing it trusts is unsynced.** Before it builds anything from the active file, it writes the file back to the device and syncs it, so what it publishes is durable whether or not an earlier `fdatasync` failed. | "resume() makes durable what it publishes after a failed fdatasync", "resume() stays degraded when its rewrite's fdatasync fails, and leaves the file as it was" |
| **Acknowledged data is refused, never repaired.** It cuts only above the published extent, the bytes the last published state already held. Damage below it, or `sync = false` writes a failed `fdatasync` left off the device and the kernel evicted before the read-back, throws `std::runtime_error`, leaves the file exactly as found and keeps the engine degraded on every retry; a reopen recovers what the device holds. An I/O error during the scan is rethrown, never read as the end of the file. | "resume() refuses when a failed fdatasync's pages were evicted, and a reopen recovers what the device holds", "resume() rethrows an I/O error from its scan and truncates nothing", "resume() does not trust an entry size that runs past the file", `[prove_corruption]` |
| **It can fail and be retried.** A failure at any step leaves the engine degraded and the file no worse than it found it; the caller may call again. A failed sync after the cut, a failed directory sync, or no file id left for the new active file are such failures. | "resume() stays degraded when the sync after its truncate fails", "directory sync: a failed sync in resume() stays degraded", "Limits: resume without a file id leaves the engine degraded and untouched", "fault sweep: resume after a failed sync", "fault sweep: resume after a torn append", "alloc sweep: resume after a failed sync", "alloc sweep: resume after a torn append" |
| **Readers are undisturbed.** Snapshots, iterators and spans taken before stay valid (*View and span lifetimes*). | "resume() with live snapshot on degraded DB", "mmap: resume() keeps reader spans valid" |

---

## `ingest`

Applies entries a leader produced, with their sequences, to a follower.

| Guarantee | Proved by |
|---|---|
| **Mode.** Throws `std::logic_error` unless `mode()` is `Mode::Follower`, and `DbDegraded` on a degraded engine. | "ingest throws in leader mode", "refuse writes: ingest is refused by the flag alone" |
| **Idempotent.** Entries at or below `durable_sequence()` are skipped, so a slice can be re-delivered after any failure. | "ingest idempotency: re-ingesting is a no-op" |
| **Limits are checked before any I/O**, and nothing is written on a refusal (*Limits*). A slice that ends inside an atomic batch throws `std::invalid_argument` the same way: published, it would expose part of a batch, and a rotation after it would leave a `BulkBegin` that recovery drops (#188). The caller cuts slices after a `BulkEnd` or a standalone entry. | "ingest refuses a slice that ends inside an atomic batch", "Limits: ingest refuses an atomic batch past the per-write byte limit", "Limits: ingest takes the last packable sequence and refuses the next", "Limits: ingest refuses a slice that needs a file id it does not have, before any I/O" |
| **Atomic and durable per slice.** A slice is visible whole or not at all, and durable before it is visible: every file it fills is synced before the next is started, and the last before the slice is published. A batch never spans two files. | "ingest with batches: BulkBegin/BulkEnd preserved", "ingest triggers file rotation", "ingest post-loop rotation: last entry tips file past threshold", "ingest sequence continuity: durable_sequence matches max ingested" |
| **Same order, same state.** Entries apply in the leader's sequence order, so the follower's state, and what it holds after a reopen, is the leader's through the last sequence ingested. The first write after a promotion takes the next sequence. | "basic ingest: entries from changes_since are ingested correctly", "ingest with range delete", "ingest recovery equivalence: survives close and reopen", "promotion continuity: first put after ingest gets next sequence", `[prove_repl]` |
| **A thrown slice is unseen until `resume()`.** On an I/O failure the engine degrades and nothing from the slice is visible. `resume()` keeps the complete units that reached the file, so the follower may then hold the first units of the slice, never part of a batch; re-delivery from `durable_sequence()` skips those and applies the rest. | "fault sweep: ingest", "alloc sweep: ingest", `[prove_repl]` |

---

## `vacuum`

Reclaims the space of overwritten and deleted values by rewriting one
sealed file without them, or by removing a file with nothing live in it.
Returns `true` when a file was reclaimed.

| Guarantee | Proved by |
|---|---|
| **No value is lost and none comes back.** Every key readable before the call reads the same after it, and after any later reopen. A tombstone is dropped only when no other file holds an older value it hides, decided at the last `open`; a tombstone written since is kept. A file with no live value is removed without a rewrite only if it holds no tombstone. | "vacuum compact removes dead entries", "vacuum compact preserves tombstones", "vacuum keeps a tombstone-only file that shadows an older put", "vacuum keeps a tombstone written since the open", "vacuum drops a tombstone no older file needs", "vacuum keeps a range tombstone over a Put in an older file", "Vacuum model-based: dropping tombstones never resurrects a key", "vacuum remaps only the keys no write changed during its copy", "DB recovery: vacuum keeps a data file whose hint was rebuilt" |
| **A superseded value is dropped only once its replacement is durable.** Before the old file goes, every write the vacuum judged by, `sync = false` ones included, is made durable: a power cut may lose an unsynced write, never the durable value it replaced. If that `fdatasync` fails, the engine degrades, the call throws, and the old file stays. On a degraded engine the call throws `DbDegraded`. | "vacuum does not drop a durable record that only an unsynced write supersedes", "vacuum whose fdatasync fails degrades, keeps the source file, and the durable values survive a power cut" |
| **Retention.** Nothing with a sequence above `VacuumOptions::retain_after` is dropped: a dead value or droppable tombstone above it is copied, and a file is removed whole only when everything in it is at or below. `kNoRetention`, the default, restricts nothing. A follower resuming `changes_since` at or above `retain_after` misses nothing. | "vacuum keeps dead entries above retain_after", "vacuum keeps tombstones above retain_after", "vacuum with retain_after 0 drops nothing", "Vacuum model-based: a follower resuming at retain_after converges" |
| **Every `true` reclaims bytes.** A file qualifies by its dead value bytes alone: one holding only live values, tombstones and batch markers is never selected, and a rewrite that would not make the file smaller is discarded and the next candidate tried. So `while (db.vacuum({.fragmentation_threshold = 0.0})) {}` terminates. | "vacuum loop reclaims all fragmentation", "vacuum does not select a file holding only tombstones", "vacuum is not stalled by a file whose only dead bytes are markers", "vacuum is not stalled by a file whose dead entries are all above retain_after", "vacuum no-op when nothing exceeds threshold" |
| **The swap is atomic.** The compacted file replaces the old one in a single publication; no reader sees a key that points to neither. Batch markers and sequences are preserved, so `changes_since` reads the compacted file as it read the original. | "vacuum preserves BulkBegin/BulkEnd markers", "vacuum compact tracks sequences", "vacuum compact handles batch entries", `[prove_vacuum_compact]` |
| **A failure leaves nothing behind.** If any step fails, scan, copy, sync, rename, hint or commit, the caller gets the exception, the old file stays published and readable, the copy is removed before the exception propagates, and the engine stays usable. A removal that fails under the same fault is left to the next `open`, which deletes it. | "vacuum that fails between its rename and its commit removes its copy", "open removes staged .data and .hint files and nothing else", "directory sync: a failed sync in vacuum keeps the source", "fault sweep: vacuum", "alloc sweep: vacuum", `[prove_vacuum_compact]` |
| **A kill between the rename and the unlink is undone.** The next `open` finds the old file and its compacted copy, both holding the same entries under the same sequences, deletes the copy once it has checked that every entry of the copy is in the old file, and opens with every key. Two files that share sequences and fail that check make `open` throw, with nothing deleted. | "recovery undoes a vacuum killed before the source was unlinked", `[prove_vacuum_compact]`, `[prove_corruption]` |
| **Readers keep the old file.** A snapshot or iterator holding the old file reads it to the end, after the unlink (*View and span lifetimes*). | "vacuum unlinks stale file immediately, snapshot reads via open fd" |

---

## `create_manifest`

Seals the active file and returns the sealed files, with a snapshot, for
a follower bootstrap or a backup.

| Guarantee | Proved by |
|---|---|
| **Complete and durable through `through_sequence`.** The manifest lists every sealed data file. Every write acknowledged before the call, `sync = false` ones included, is synced before the active file is sealed, so `through_sequence` is durable and the snapshot holds exactly the writes through it. No write slips between the seal and the snapshot. | "basic manifest contains sealed files with hints", "empty db manifest", "writes continue after manifest", "manifest after vacuum", `[prove_manifest]` |
| **Every listed data file exists; a listed hint may not.** Hint writes are waited for before the list is built, but a hint whose write failed is only logged. A missing hint costs no data: opening the copied directory rebuilds it (#349). A caller copies a hint when it exists and skips it when it does not. | `[prove_manifest]` |
| **A failed sync degrades, and nothing is sealed.** The exception propagates, no manifest is produced, and writes throw `DbDegraded` until `resume()`. A seal that fails after the sync, because the next active file cannot be created, degrades the same way; `resume()` opens a fresh active file. | "create_manifest whose fdatasync fails degrades, and a power cut keeps everything below durable_sequence", "create_manifest after resume() from its failed fdatasync makes every write durable", "refuse writes: create_manifest's rotation that fails in memory refuses writes", "fault sweep: create_manifest" |

---

## `set_mode` / `mode`

| Guarantee | Proved by |
|---|---|
| **No write straddles a mode change.** A write completes before `set_mode` or starts after it. `mode()` is lock-free. | "set_mode transitions: leader -> follower -> leader" |
| **Stepping down is durable.** `set_mode(Mode::Follower)` on a leader syncs first: on return `durable_sequence()` covers every write acknowledged before the call, `sync = false` ones included, so `changes_since` can ship them all to the next leader. A failed `fdatasync` degrades the engine, throws, and leaves the mode unchanged. A degraded leader steps down without a sync. | "set_mode(Follower) makes unsynced acknowledged writes durable", "set_mode(Follower): a failed fdatasync degrades and keeps the mode", "set_mode(Follower) on a degraded leader steps down without a sync" |
| **Leader**: `put`, `del`, `del_range` and `apply_batch` are allowed, `ingest` throws `std::logic_error`. **Follower**: those four throw `DbFollowerMode`, `ingest` is allowed. Reads, snapshots, `vacuum()` and `resume()` work in both. The initial mode is `Options::initial_mode`. | "mode enforcement: put/del/apply_batch throw in follower mode", "ingest throws in leader mode" |

---

## Hint files

A hint is an index of a sealed data file that `open` reads instead of the
file. It is never the record; the data file is.

| Guarantee | Proved by |
|---|---|
| **A hint never costs a key.** A hint that is missing, fails its CRC, that a read fails on, or that a crash left half written is rebuilt from its data file at the next `open`. A hint is built only from bytes the device holds, so it never points past what a power loss keeps. | "DB recovery: a corrupt hint is rebuilt from its data file", "DB::open rebuilds a hint file a read fails on", "DB::open leaves a hint alone on an error that says nothing about its bytes", "DB recovery: a crash while open writes a hint leaves no hint behind it, and the next open recovers", "open fails when the rewrite of a hint-less file cannot be synced, and writes no hint for it", "directory sync: a hint rebuilt at open is synced", `[hintfile]` |
| **A hint indexes exactly the committed entries of its file**: every complete entry outside a batch, every entry of a batch that has its `BulkEnd`, range tombstones included, and nothing of a batch that lacks one. A key that appears more than once appears each time; recovery keeps the highest sequence. | "recovery checks the layout of a hint file's sorted run", "DB recovery: hint file path after rotation", "Recovery model-based: hints split into many frames", `[model]` |
| **Hints never block a write, and the work they leave is bounded.** They are written in the background after a file is sealed. At most `max_hint_backlog` sealed files wait for one; a rotation past that waits for the writer, stalling writes, not reads. `close()` writes every hint owed, the active file's included, and reports a failure (*`close`*). `max_hint_backlog = 0` never waits. | "hint backlog: a rotation past max_hint_backlog waits for the worker", "hint backlog: max_hint_backlog = 0 never waits", "DB close writes hint file for sealed file", "a clean close writes the active file's hint", "DB destructor flushes hint files" |
| **`fail_recovery_on_crc_errors = false`** is the one opt-out: a data file whose hint is bad and that cannot be rescanned is skipped with a warning, and the database opens without its keys (*`open`*). | "DB recovery: strict mode throws when a hint cannot be rebuilt", "DB recovery: lenient mode opens with partial recovery when a hint cannot be rebuilt" |

---

## `open`

Recovers the database a crash or power loss left, on storage that keeps
its promises (*Conditions*), and refuses any other.

| Guarantee | Proved by |
|---|---|
| **Every durable write is there.** Every write acknowledged as durable is in the opened database, under its sequence. `sync = false` writes made after the last sync may be missing; those that survive are a prefix of the write order, and a batch is in whole or not at all. The database that opens is recovery-equivalent to the one that closed (*Definitions*). | "DB recovery: puts survive restart", "DB recovery: batch survives restart", "DB recovery: incomplete batch is discarded", "pipeline: many concurrent sync writers, every commit durable and visible, recovery agrees", "Recovery model-based: random workload matches oracle", `[model]` |
| **One file at most is cut.** Only the file being written when the process stopped can hold a torn record. `open` cuts that file at its last committed record, whole if its first page was lost, and syncs the cut. A tail of zeros, preallocated space, is trimmed in any file. | "DB recovery: a hint-less file's tail is truncated only in the newest file", "DB recovery: open fails when the cut of a torn tail cannot be synced, and the next open recovers", "Preallocated tail: sealed files shrink to their logical size", "fault sweep: open after a crash" |
| **Nothing it indexes is unsynced.** A data file without a hint is written back to the device and synced before anything is built from it, so what `open` publishes is durable whether or not the last process synced it. | "open makes a hint-less file durable before it indexes it: close after a failed fdatasync", "open makes a hint-less file durable before it indexes it: a process killed before its sync", "open fails when the rewrite of a hint-less file cannot be synced, and writes no hint for it" |
| **Damage is refused, not opened around.** A state no crash leaves on storage that keeps its promises is damage. Where `open` can see it, it throws and cuts nothing: data past the last committed record in a file that is not the newest, such data in more than one file, two files sharing sequences other than an interrupted vacuum's pair. It never opens a best-effort subset of a damaged database, which to the caller would be data loss with no error. The one opt-out is `fail_recovery_on_crc_errors = false` (*Hint files*), and it does not relax the cutting rules. | "DB recovery: a hint-less file's tail is truncated only in the newest file", "recovery refuses two different writes under one sequence", "recovery keeps the full file over a copy of its prefix", "recovery undoes a vacuum killed before the source was unlinked", "DB vacuum: a damaged sealed file is not compacted away" |
| **One process at a time.** A directory another process holds open is refused with `std::system_error`. `close` releases it. | "DB rejects concurrent open on same directory", "DB directory unlocked after close" |
| **Options are checked first.** `max_key_bytes`, `max_value_bytes` or `max_file_bytes` above its hard ceiling (*Limits*), `recovery_threads = 0`, or a buffer pool under `2 x max_file_bytes`, throws `std::invalid_argument` before the directory is created or locked. `max_file_bytes = 0` is valid: every write seals the file it went into. | "Limits: open rejects options above the hard ceilings", "Options: recovery_threads = 0 is refused at open", "Options: a buffer pool of exactly 2 x max_file_bytes is accepted, one byte less is not", "Options: max_file_bytes = 0 seals a file after every write" |

---

## `close`

Shuts the engine down and reports whether the shutdown kept every
acknowledged write. `~DB` calls it when the caller did not, and swallows
its errors.

| Guarantee | Proved by |
|---|---|
| **It returns only if everything acknowledged is durable.** Every write the DB acknowledged, `sync = false` ones included, is on the device, and every hint write succeeded. On a healthy engine the sync and the trim of the active file succeeded too; on a degraded one the trim is best effort, since nothing it could sync can be trusted. | "close() makes unsynced writes durable and writes the active file's hint", "a clean close writes the active file's hint", "DB close writes hint file for sealed file", "fault sweep: close" |
| **A failure is reported, once.** On a healthy engine a failed `fdatasync`, trim or hint write throws `std::system_error`. On a degraded engine `close` syncs nothing, since a sync after a failed one proves nothing, and throws `DbDegraded` if a write was acknowledged above `durable_sequence()`; one whose acknowledged writes are all durable closes normally. A second `close` returns at once and reports nothing: a retry cannot make lost pages durable. | "close() throws when its fdatasync fails, closes all the same, and a reopen recovers what was durable", "close() on a degraded engine reports acknowledged writes that are not durable", "close() on a degraded engine whose acknowledged writes are all durable returns normally", "after close() every operation throws DbClosed; snapshots taken before stay readable" |
| **Closed either way.** Whatever it throws, the engine is closed and the directory lock released. A failed `close` leaves on disk what a crash would, and the next `open` recovers it the same way. | "close() throws when its fdatasync fails, closes all the same, and a reopen recovers what was durable", "DB directory unlocked after close" |
| **A racing operation completes or throws `DbClosed`.** `close` waits for a running vacuum and for every in-flight write to be published. A write that races it either commits first, and is covered by its durability verdict, or throws `DbClosed`; a read returns a result or throws `DbClosed`. | "writers and readers racing close() either complete or throw DbClosed, and every acknowledged write survives" |
| **Afterwards every call throws `DbClosed`**, a `std::logic_error`, except `close()`, which returns at once, and `mode()`, `is_degraded()` and `degraded_reason()`, which keep answering. Views taken before, a `Snapshot`, an iterator and the spans it lent, stay valid and readable (*View and span lifetimes*). | "after close() every operation throws DbClosed; snapshots taken before stay readable" |
| **`~DB` closes too.** It calls `close` if the caller did not and swallows what it throws, so a caller that only destroys the `DB` cannot tell a clean shutdown from one that lost its unsynced writes; call `close` to know. No operation may be in flight when `~DB` runs: that is a caller precondition, since it ends the object's lifetime. An operation that races it is undefined. The undefined part does not extend to data already written: the worst on-disk outcome is a shorter valid prefix of the committed history, which the next `open` recovers, and the directory lock is released only once teardown has stopped writing, so no second process opens the directory while it still does. | "DB destructor flushes hint files" |

---

## `changes_since`

A lazy iterator over the committed, durable entries of a `Snapshot`, in
ascending sequence order, for replication.

| Guarantee | Proved by |
|---|---|
| **Durable entries only.** It yields the entries that were durable when the `Snapshot` was taken, and nothing a `sync = false` write had left unsynced, even where the `Snapshot` shows that write. | "changes_since stops at the snapshot's durable sequence" |
| **Complete and exactly once, above the retention point.** Every durable entry with a sequence above `from_sequence` is yielded once, provided every vacuum since that sequence was written ran with `retain_after <= from_sequence`. A vacuum with a higher `retain_after`, or the default, may have dropped dead values and tombstones above `from_sequence`; the stream then skips them silently, and the caller re-bootstraps from a manifest (#168). | "Vacuum model-based: a follower resuming at retain_after converges", "vacuum keeps dead entries above retain_after", "vacuum keeps tombstones above retain_after", "basic ingest: entries from changes_since are ingested correctly", "changes_since empty iterator when no new entries" |
| **Ascending sequence order**, strictly. | "changes_since iterator yields entries in sequence order", "leader-to-follower replication round-trip" |
| **Whole batches, markers included.** `BulkBegin` and `BulkEnd` are yielded with the entries between them, so `ingest` can keep the batch atomic. Nothing of a batch that lacks its `BulkEnd` is yielded. | "ingest with batches: BulkBegin/BulkEnd preserved"; the incomplete-batch half: none |
| **Vacuum is invisible to it.** An entry keeps its sequence and its batch markers when its file is compacted. Above the vacuum's `retain_after`, the stream over a compacted file is the stream over the original. | "Vacuum model-based: a follower resuming at retain_after converges" |
| **It is a view.** The iterator holds what it reads, independently of the `Snapshot` it was given and of the `DB`; a vacuum or a `close` under it changes nothing it yields (*View and span lifetimes*). | "after close() every operation throws DbClosed; snapshots taken before stay readable" |

---

## View and span lifetimes

Every read API either copies bytes out or lends a view of them. A lent
view, a `std::span` or a reference into an iterator, is correct only
while the memory behind it is still owned and still holds what it held.
This section says, for each view the engine hands out, what it survives
and what ends it.

Its scope is memory safety and byte stability, not visibility. A view is
frozen at the moment it was taken: a concurrent write never changes what
a live view shows, and never invalidates it either.

### The views

| View | Handed out by | What it is |
|------|---------------|------------|
| `Bytes& out` | `DB::get`, `Snapshot::get` | A copy into the caller's own vector, not a view |
| `EntryView` (`key`, `value`) | `EntryIterator`, `ReverseEntryIterator`: `iter_from`, `riter_from` | Spans lent by the iterator |
| `const Bytes&` | `KeyIterator`, `ReverseKeyIterator`: `keys_from`, `rkeys_from` | A reference to a key the iterator owns |
| `DataEntryView` (`key`, `value`) | `ChangeIterator`: `changes_since` | Spans lent by the iterator |
| `Snapshot` | `DB::snapshot`, `create_manifest` | A frozen state |

### What a view survives

| Guarantee | Proved by |
|---|---|
| **Every engine event.** A live view stays valid and keeps its bytes across `resume()`, `vacuum()`, a file rotation, `set_mode()`, a concurrent write, `close()` and the destruction of the `DB`. Nothing the engine does to its files, including deleting one, reaches a view already lent, under any `io_backend`. | "mmap: resume() keeps reader spans valid", "vacuum unlinks stale file immediately, snapshot reads via open fd", "io_backend=BufferPool: an iterator outlives the DB with its spans", "io_backend=BufferPool: lent entry spans hold while readers evict", "resume() with live snapshot on degraded DB", "after close() every operation throws DbClosed; snapshots taken before stay readable" |
| **Frozen.** A view shows what it showed when it was taken. A later write changes nothing it shows and invalidates nothing. | "Snapshot get is frozen at snapshot time", "Snapshot iter_from is frozen at snapshot time", "Snapshot riter_from is frozen at snapshot time", "count_keys on a snapshot ignores later writes", "DB snapshot isolation under concurrent writes" |
| **Independent of its `Snapshot`.** An iterator taken from a `Snapshot`, or the one `changes_since` is given, holds what it needs itself. Destroying or moving the `Snapshot` mid-iteration is safe, and so is returning the iterator from the scope the `Snapshot` was local to. | "chaos soak: concurrent readers, writers and lifecycle" (walks `iter_from`, `riter_from` and `keys_from` after destroying their `Snapshot`) |
| **Bound to its iterator, and no further.** A lent span or key reference belongs to the iterator that produced it: the next `operator++` replaces it, and the iterator's destruction ends it. Moving the iterator keeps it valid; the storage travels with the spans. `EntryIterator`, `ReverseEntryIterator` and `ChangeIterator` are move-only, so a copy can never separate a span from the storage it addresses. `ReverseKeyIterator` is copyable, as its sentinel is itself; a copy owns its own key, and a reference binds to the iterator it was taken from. | "entry iterators are move-only and still model input_iterator" |
| **A copy is the caller's.** The `Bytes` that `get` fills is the caller's; no event reaches it, and the next `get` into the same vector overwrites it. | "DB get output-param round-trip" |
| **Under `IoBackend::BufferPool`** a lent `EntryView` whose record lies in one resident cache frame pins that frame until the iterator advances; otherwise the bytes are copied into the iterator and nothing is pinned. Eviction skips a pinned frame and never waits for it, so a span held across a long loop body costs at most one frame, never a stall. | "BufferPool: a leased frame survives eviction pressure", "io_backend=BufferPool: lent entry spans hold while readers evict" |
| **Under `IoBackend::Mmap`** a read the kernel cannot complete, a media error or a file truncated by something other than the engine, is `SIGBUS`, not `std::system_error`. The other back-ends throw. | none |

