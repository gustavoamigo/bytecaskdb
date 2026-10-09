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
| A publication that would move a sequence, a file id or the durable sequence backwards, or make a `sync = true` write visible before it is durable. | Not published; the engine degrades, and reads keep the previous state. | none |
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
| **Vacuum must not run until the copy is done** (*Conditions*). | none |

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
