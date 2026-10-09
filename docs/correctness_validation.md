# ByteCaskDB — Correctness Validation

> This document describes the correctness validation framework for
> ByteCaskDB's write path. It should be read alongside
> [`CONTRACT.md`](../CONTRACT.md).

---

## Conceptual Model

The append-only data file is a persistence mechanism for `EngineState`
transitions. Conceptually, we can think of the database as *being* `EngineState`. 
The file exists only to make `EngineState` recoverable after a crash.

A write operation is a state transition. It is correct if and only if:

1. **If committed** — the transition is persisted to disk AND applied
   to in-memory `EngineState`. Recovery produces the same `EngineState`.

2. **If not committed** — the transition is neither persisted to disk
   NOR applied to in-memory `EngineState`. Recovery does not produce
   the transition.

There is no valid intermediate state. A transition is either fully
committed or fully absent. This is the atomicity guarantee expressed
as a persistence invariant.

### What each component is

```
EngineState      — the truth. The database.
DataFile         — the persistence mechanism for EngineState transitions
key_dir          — an index into the persistence mechanism
LSN              — a transition identifier, not a sequence number
Recovery         — reconstructing EngineState from persisted transitions
Vacuum           — rewriting the persistence log keeping only the latest
                   transition per key. Produces identical EngineState
                   from a smaller file.
```

### What the append does

```
append(lsn, key, value)  ≡  persist(StateTransition{lsn, Set(key, value)})
append(lsn, key, {})     ≡  persist(StateTransition{lsn, Remove(key)})
BulkBegin/BulkEnd        ≡  persist(BatchTransition{lsn, [T1, T2, ... Tn]})
```

### What recovery does

```
Recovery ≡ replay(all committed state transitions in LSN order)
```

The hint file is an optimization — it collapses all transitions for a
key into the latest one so replay is O(unique keys) not O(total
transitions). Conceptually it is still replaying transitions.

### The two-phase protocol

```
Phase 1 — persist the transition to disk
Phase 2 — apply the transition to in-memory EngineState
```

The invariant:

```
A transition must be persisted before it is applied.
A transition that is not fully persisted must not be applied.
```

If Phase 1 fails, the transition is not persisted. Phase 2 does not
run. The in-memory `EngineState` does not change. Recovery replays
only fully persisted transitions. Consistency is maintained.

### LSN as transition identifier

```
Every committed transition has exactly one LSN     — identity
Two committed transitions never share an LSN       — uniqueness
Transitions are replayed in LSN order              — ordering
Uncommitted transitions are not replayed           — isolation
Gaps in LSN are safe — contiguity is not required  — gaps
```

---

## Problem Statement

Given a storage engine with a single write path, validate that for any
I/O failure at any point during a state transition, the resulting state
satisfies all invariants and the observed delta matches the expected
delta defined by the model.

```
Given:
  S1  — a valid EngineState (structural description, not concrete values)
  P   — a WritePlan (K operations: Puts, Deletes, with or without guards)
  F   — an I/O failure class (one of A..H, or SUCCESS)

Validate:
  transition(S1, P, F) → S2
  where S2 satisfies all invariants
  and delta(S1, S2) == expected_delta(P, F)
```

The model does not validate specific values. It validates the structure
of the transition. Concrete key bytes, LSN numbers, and file sizes are
irrelevant to whether the transition is correct.

The binary question for every case is:

```
Was the transition fully persisted?
  Yes → it must appear in both in-memory EngineState and in recovery
  No  → it must appear in neither
```

---

## Key Principle

The state space is infinite — keys, values, and LSNs can be anything.
But the transition delta is finite and bounded by the operation. The
goal is to validate properties of the transition, not properties of
specific values.

What matters is not all values that `EngineState` can have, but the
difference between `S1` and `S2`, which is finite and determined
entirely by `P` and `F`.

This is model-based testing with a transition-based model, and the finite
set is covered by enumeration rather than sampling: bounded exhaustive
testing. [*Prior Art*](#prior-art) places each layer of this document
against the work it follows.

---

## Failure Classes

Every I/O failure in `apply_batch` falls into exactly one structural
class. The class determines the expected delta — not which specific
operation failed, not what the key bytes were.

```python
class FailureClass(Enum):
    SUCCESS = "success"               # no failure — transition fully persisted
    NOSYNC  = "success_nosync"        # no failure, sync = false — persisted in process; a power cut may take it whole
    A  = "before_any_io"              # conflict check fails — no I/O attempted
    B1 = "append_fails_nothing_written"   # writev returns -1
    B2 = "append_fails_partial_write"     # writev returns short, file tainted
    B3 = "append_fails_after_full_write"  # writev ok, post-write fault, file tainted
    C  = "on_bulk_end_append"         # BulkEnd write fails — degrade
    F  = "commit_sync_fails"          # commit fdatasync fails — bytes in page cache, not confirmed durable
    G  = "rotation_sync_fails"        # pre-rotation fdatasync fails — bytes in page cache, not confirmed durable
    H  = "rotation_file_creation_fails"  # post-write rotation fails after seal — degrade
```

### Write outcome subclasses (B1, B2, B3)

`writev` can produce three outcomes when it fails:

- **B1 — error return**: `writev` returns -1. POSIX does not guarantee that
  no bytes reached the page cache — FUSE and network filesystems may not
  follow the Linux regular-file convention. The engine treats any `writev`
  failure as indeterminate: `tainted_` is always set, and the engine degrades.
- **B2 — partial write**: `writev` returns 0 < N < total, and sets no
  `errno`; the append reports it as `EIO` with the byte counts (#221). N
  bytes are on disk, kernel fd position advanced by N. `offset_` is not advanced (the
  throw in `append()` skips `offset_ +=`). The file is `tainted`. This
  class also covers sub-entry torn writes at the sector level: a power
  loss mid-flush can land some 512-byte sectors on disk and not others,
  even though `writev` never returned to the caller. On recovery, the
  entry fails CRC verification and, in the newest data file, is truncated
  — the same mechanism that handles B2 at the `writev` boundary (see
  `bytecask_design.md`, *Recovering a Hint-less File*). Until #138 it was
  refused instead, and a power cut during a write left the database
  unopenable.
- **B3 — full write + error return**: `writev` writes all bytes to disk
  successfully, but an error is returned to the caller (simulated by
  `PostWriteMode::throw_after`). The entry is structurally complete on disk
  with a valid CRC. The file is `tainted`.

B1, B2, and B3 are distinguished for modeling clarity — they represent
different `writev` outcomes at the POSIX level. The engine's runtime
response is identical for all three: set `tainted_`, degrade
unconditionally, best-effort sync, then `resume()` scans and truncates.
The distinction matters when reasoning about what bytes `resume()` may
find on disk (nothing for B1, a partial entry for B2, a structurally
complete entry for B3), but it does not affect the code path taken.

All three subclasses degrade the engine unconditionally. A best-effort
`sync()` is called before degrading so that any bytes already in the page
cache reach durable storage — `resume()` can then replay valid committed
entries written before the failure. `resume()` scans the active file,
truncates any incomplete or orphaned content, and creates a fresh active
file.

### Post-write rotation failure subclasses (G, H)

After all appends succeed and in-memory state is applied, the engine
checks whether the active file has reached the rotation threshold. If
so, it syncs the file and rotates to a new one. Two distinct failures
can occur at this point:

- **G — rotation sync fails**: The pre-rotation `fdatasync` fails.
  The file is not sealed. Bytes are in the page cache but durability is
  not confirmed. Key-directory changes are NOT published. `next_lsn`
  advances past all consumed LSNs. The engine degrades; `resume()` scans
  the active file, replays any valid committed entries, and creates a
  fresh active file.
- **H — rotation file creation fails**: The sync succeeded but
  `prepare_rotation` fails after sealing the active file. The sealed
  file cannot accept further appends (`assert(!sealed_)` would fire).
  The engine degrades the DB and publishes the state (writes are on disk,
  LSNs must advance). `resume()` creates a fresh active file and clears
  the degraded state.

Key insight: `prepare_rotation` calls `seal()` before creating the new
file. If file creation fails, the active file is sealed and unusable.
Publishing state without degrading would leave an engine that appears
healthy but fails on the next append. Degrading is the correct response;
`resume()` restores normal operation.

### Class Behavior Summary

| Class | Transition persisted | key_dir changes | LSN advances | Throws | Degraded |
|-------|---------------------|-----------------|--------------|--------|----------|
| SUCCESS | Yes — fully | Yes — full delta | Yes | No | No |
| NOSYNC | Yes — page cache only | Yes — full delta | Yes | No | No |
| A | No — not attempted | No | No | No (returns false) | No |
| B1 | No — indeterminate | No | Yes | Yes | Yes |
| B2 | No — partial write | No | Yes | Yes | Yes |
| B3 | No — indeterminate | No | Yes | Yes | Yes |
| C | No — orphaned BulkBegin | No | Yes | Yes | Yes |
| F | No — page cache only | No | Yes | Yes | Yes |
| G | No — page cache only | No | Yes | Yes | Yes |
| H | Yes — fully | Yes — full delta | Yes | Yes | Yes |

Classes A through H are syscalls that return an error. #104 added the
region they leave out, syscalls that **succeed with the wrong result**:

| Class | What succeeds wrongly | Engine behaviour | Proven by |
|-------|----------------------|------------------|-----------|
| M1 | `mmap` returns the address `munmap` just released | a span handed out before `truncate()` still reads the same bytes | observer axis, `assert_view_stable` (byte comparison, not address) |
| M2 | `open(O_CREAT)` on a stem already on disk or already hinted | aborts rather than adopting the sealed file | `createDataFileForWrite panics when the data file already exists` / `… when the stem was already hinted` in `data_file_test.cpp` |
| M3 | `rename` completes, the process does not confirm it | vacuum removes its uncommitted copy on the way out (#304); a kill leaves it, and the next open detects it and deletes it | VC6 in `[prove_vacuum_compact]`; the kill by VC5 and `recovery undoes a vacuum killed before the source was unlinked` |
| M4 | `pread` returns damaged bytes of published data | fail-stop: `resume()` refuses and truncates nothing; `open` refuses where the format can see it | `[prove_corruption]`, `[corruption]` |

Note on classes B1/B2/B3 — LSN advanced, engine degraded: Any `writev`
failure leaves the file in an indeterminate state — POSIX does not guarantee
`writev = -1` means no bytes were written. `next_lsn` is advanced past all
consumed sequence numbers unconditionally (conservative: gaps are safe, reuse
is not). Key-directory changes are NOT published. The engine degrades;
`resume()` scans the active file, truncates garbage, and restores normal
operation.

Note on classes F and G — key changes unpublished, LSN advanced, engine degraded:
Bytes are written via `writev` and reach the page cache, but `fdatasync` fails —
durability is not confirmed. Key-directory changes are NOT published: the written
key is invisible to subsequent reads, and the caller must retry. `next_lsn` is
advanced past all consumed sequence numbers to prevent LSN reuse. The engine
degrades; `resume()` scans the active file, replays any valid committed entries
(including F/G bytes if they survived in the page cache to `sync()`), and creates
a fresh active file.

Class H is similar — the transition is persisted — but the engine is
also degraded because the sealed active file cannot accept further
appends.

---

## Write Path Infrastructure

### Single write path

All mutations — `put`, `del`, `apply_batch` — route
through `apply_batch` as the single coordinator under `write_mu_`
(one writer at a time). The two-phase discipline is enforced: I/O first
(`DataFile::append`), then pure in-memory state mutations
(`TransientEngineState::apply_writes`), then publish
(`TransientEngineState::persistent()`).

`TransientEngineState` is the mutable working copy of engine state.
It provides:
- `validate_preconditions(plan)` — pure read, checks guards and W-W conflict
- `prepare_write(plan)` — assigns LSNs, inserts BulkBegin/BulkEnd markers
- `apply_writes(plan, offsets)` — pure in-memory mutations after I/O succeeds
- `persistent()` — commits back to immutable `shared_ptr<EngineState>`

If I/O fails, the transient copy is discarded — the published state
is never modified.

### Behavioral contract

[`CONTRACT.md`](../CONTRACT.md) defines the guarantees for
`apply_batch`, `vacuum_compact`, and hint files.
It is the source of truth for both the implementation and the validation
model.

### Fault injection framework

`bytecaskdb/fault_injector.h` provides:
- `FaultInjector` with name-based and count-based injection, plus a
  count-based skip set for selective cascade control
- `ScopedFaultInjector` RAII guard with four constructor forms:
  name-based, count-based, count-based with skip set, and name-based
  with post-write mode
- `PostWriteMode` (`short_write`, `throw_after`) for simulating partial
  writes — fires at `FAULT_INJECTION_POST_WRITE` checkpoints
- `io_checkpoint(name)` called at I/O boundaries
- `FAULT_INJECTION(name)` and `FAULT_INJECTION_POST_WRITE(name, fd, offset, total)`
  macros, compiled under `BYTECASK_TESTING`
- Thread-local `active_injector` pointer
- `SuspendSyscallFaults`, held by the `PageCacheModel`'s hooks so the I/O
  they do from inside the engine is not counted by the sweep below
  (*Counted fault sweep*)

Four checkpoints exist in the production code:
1. `io_data_file_append` — before `writev()` in `DataFile::append()`
2. `io_data_file_append_partial` — after `writev()` but before offset
   advance in `DataFile::append()` (post-write checkpoint)
3. `io_data_file_sync` — before `fdatasync()` in `DataFile::sync()`
4. `io_rotate_file_creation` — after sealing, before new file creation
   in `prepare_rotation()`

Six additional checkpoints exist in `bytecask.cpp` (compiled under `BYTECASK_TESTING`):

5. `io_resume_truncate` — before `file.truncate()` in `resume()` (only
   reached when the active file has orphaned bytes to discard). In
   post-write mode the same name fires after the `ftruncate` in
   `WritableFileOps::truncate` instead: a truncate that cut the file and
   then reported an error, as ext4 can.
6. `io_resume_sync` — before `file.sync()` in `resume()`
7. `io_resume_file_creation` — before creating the new active `DataFile`
   in `resume()`
8. `io_vacuum_compact_tmp_create` — before constructing the temporary
   `DataFile` in `vacuum_compact_file()`
9. `io_vacuum_compact_rename` — before `std::filesystem::rename()` in
    `vacuum_compact_file()`

Three more instrument the hint write, mirroring the data file's:

10. `io_hint_write` — before the CRC trailer `write()` in `HintFile::close()`
11. `io_hint_sync` — before the `fdatasync()` in `HintFile::close()`
12. `io_hint_rename` — before `std::filesystem::rename()` in
    `flush_hints_for()`

And one for the window a completed rename opens:

13. `io_vacuum_compact_post_rename` — after `renameDataFileExclusive()` and
    the directory sync, before `vacuum_commit()` in `vacuum_compact_file()`.
    See *Orphaned `.data` files* for what it reproduces.

And one between the staging copy's sync and the rename (#258):

- `io_vacuum_compact_shrink` — before the staging copy's `shrink_to_fit()`
  in `vacuum_compact_file()`.

And one on the read side, for the scan `resume()` runs:

14. `io_data_file_scan` — before the `pread()` in the active file's
    `read_raw()`, which every sweep of it goes through. An I/O error there says nothing about the bytes, so
    `resume()` must rethrow it rather than read it as the end of the file
    and truncate; `[degraded][resume]` holds it to that.

And one on the hint read, which recovery used to take through a memory
mapping — where a failed read is a `SIGBUS`, not an error (#237):

15. `io_hint_read` — before each `pread()` of a hint file, in the pass that
    opens and verifies it and in every scan. `FaultInjector::fail_on_nth_match`
    fails only the Nth read, so the rebuild that follows reads cleanly. A
    failure in the open pass must rebuild the hint and recover every key; one
    in a scan must fail the open with `std::system_error` and cost nothing on
    the next. The `[model]` many-frames test fails reads 1–150 in turn and
    checks each against the serial baseline, file stats included, and
    `DB::open rebuilds a hint file a read fails on` checks both outcomes
    directly. Both run at `recovery_threads = 1`: the injector is
    thread-local, and at one thread recovery runs on the thread that opens.

### Orphaned BulkBegin degrade

If a multi-entry batch fails mid-write after `BulkBegin`, the engine
degrades unconditionally. `resume()` scans the active file, truncates
back to the last valid committed offset (discarding the orphaned
`BulkBegin` and any partial entries that follow it), syncs, seals the
file, and opens a fresh active file.

### Sync failure degrade-then-rethrow

If `fdatasync` fails after all appends succeed (classes F and G), the
engine advances `next_lsn` past all consumed LSNs, degrades the DB, and
rethrows. Key-directory changes are not published — the write is invisible
to callers. Degrading forces `resume()` before further writes are accepted;
`resume()` scans the active file and replays any valid committed entries.

---

## Proof Test Generator

### Power-loss axis

Every cell in the apply_batch, vacuum_compact, group_commit and resume
generators, and the plain successful stream cells of the replication
generator, cut the power before the DB closes (#265). #245 — vacuum
dropping a durable record that only a `sync = false` write superseded — got
past every other layer here: the fault injector needs a syscall to fail, and
#245 needs none; the recovery checks reopen a directory whose page cache is
intact; the crash harness SIGKILLs, and the page cache survives that too.
Only the chaos rig cuts power, and it found the bug by chance. The one place
a `sync = false` write can actually be lost in-process is the page cache
model, so the generators now cut there. The shape is CrashMonkey and ACE's
([*Prior Art*](#prior-art)): a bounded, enumerated set of workloads, each
crashed once at a known point and checked against an oracle.

Each cell runs under `ScopedPageCacheModel`. Before the DB's destructor
would sync and hint everything, `PageCacheModel::copy_device(dir, cut)`
copies the directory the way the crash harness copies a killed one, then
overlays every page the model still holds an image of — a page written but
not synced, or one a failed `fdatasync` left clean and off the device —
with the device's bytes, looked up by the source file's inode. The live
engine is untouched: it goes on to its close, and the existing recovery
checks run on the original directory as before. A restore in place would
break both — the close would hint data the restore just zeroed, and under
`mmap` the active file is mapped `MAP_SHARED` — which is why the earlier
`restore_device` is now for closed directories only.

What the copy may recover to is decided by the crash harness's watermark
rule, applied where writes can actually be lost. A cell's setup writes with
`sync = false`, so it ends with an empty `apply_batch({.sync = true},
WritePlan{})` (`make_durable`), which flushes everything written so far. The
baseline is then durable, and the recovered copy must hold the baseline
alone, or the baseline and the whole transition; a partial transition fails
either way (`assert_power_loss_outcome`). The transition is required when the
durable watermark covers it — `durable_sequence()` at the cut, which must
itself cover any `CommitResult` that reported `durable` — and when the
recovered engine's `durable_sequence()` covers it: an engine that claims a
sequence durable must hold what it wrote. Recovered `durable_sequence()`
must reach the in-process watermark. The new NOSYNC class (a `sync = false`
transition with no fault) is what makes the "baseline alone" arm
non-vacuous, and the `rotation_threshold` shapes cut it after the rotation
that syncs it.

Cells that degrade end with `resume()`, which claims durable everything it
publishes: it rewrites and syncs the active file before it trusts a byte of
it (#240). The cut copy must therefore agree with the resumed engine
exactly, and it is checked with the same `assert_matches_recovery` the
closed directory is. Removing the rewrite fails every F and G cell in
`[prove]` and `[prove_resume]`, and the F and G group cells: `resume()`
publishes pages a failed sync left clean but off the device, the cut loses
them, and a key at or below the watermark is gone. That is a power-loss-only
bug of a different class than #245, and the axis catches it without a cell
written for it.

Hint files are not modelled, and do not need to be: a hint is a rebuildable
index, so a lost or torn one costs recovery time and a damaged one is
rebuilt from its data file (`[prove_recovery]`). What would cost keys is a
hint that indexes data the device does not hold. The engine keeps that by
ordering — rotation seals through `shrink_to_fit` (`ftruncate` +
`fdatasync`) before the hint is dispatched, vacuum syncs its copy before the
rename, and open rewrites and syncs a hint-less file before hinting it — and
the model now checks the ordering: `hint_written`, called as every hint's
trailer is written, records a violation if a page of that hint's data file
has an image that differs from its current bytes, and every cell asserts
there were none (`assert_hints_durable`). The check runs on the background
hint worker too, where no test assertion could. A page whose image equals
its current bytes is not a violation: the device holds them either way.
That case is real and CI found it on its disk-backed runners, where hint
`fdatasync`s are slow enough to expose it: a commit sync that fails right
after a rotation degrades the engine against the pre-rotation state, so
`resume()` rewrites a file the rotation has already sealed, synced and
handed to the hint worker, and the hint's trailer can land while the
rewrite has the pages dirty with the same bytes.

Directory operations are not modelled: a `rename`, `unlink` or `create`
survives the cut whether or not an `io_dir_sync_*` covered it, so a missing
directory sync on a SUCCESS path is still out of reach of the axis. The
`[dir_sync]` tests fail each existing sync and check the ordering (#199);
modelling the operations is #266.

Cost: a copy and a reopen per cell, plus one `fdatasync` for the durable
baseline. On the dev machine with `/tmp` on tmpfs, release build:

| Suite | Cells before → after | Wall before → after |
|---|---:|---:|
| `[prove]` | 1873 → 2082 | 3.1 s → 6.7 s |
| `[prove_vacuum_compact]` | 56 → 67 | 0.27 s → 0.51 s |
| `[prove_group]` | 22 → 22 | 0.03 s → 0.07 s |
| `[prove_repl]` | 178 → 178 | 0.68 s → 0.68 s |
| `[prove_resume]` | 64 → 64 | 0.11 s → 0.19 s |

The `fdatasync` per cell is what will show on CI's disk-backed runners.

### apply_batch — 2082 tests

2082 generated Catch2 tests (`[prove]` tag) cover every valid
(StateShape, PlanShape, FailureClass, Observer) combination for
`apply_batch`. The scenario matrix is 11 state shapes × 20 plan shapes ×
10 failure classes; 4 elimination rules reduce this to 1522 observer-free
cells, and the observer axis adds 560 more.

#### State shapes

| Shape | `num_keys` | `max_file_bytes` | `io_backend` | What it tests |
|-------|-----------|-----------------|-------------|---------------|
| `empty_db` | 0 | — | pread | Plan applied to a fresh database with no existing keys |
| `single_key` | 1 | — | pread | Overwrites, deletes, and guards against a single existing key |
| `populated_db` | 10 | — | pread | Multiple existing keys; exercises range guards and multi-key interactions |
| `rotation_threshold` | 1 | 1 | pread | Forces file rotation after the plan's writes — required for classes G and H |
| `deleted_key` | 1 (deleted) | — | pread | k0 created then deleted — tombstone in write history, no live keys at baseline |
| `single_key_mmap` | 1 | — | mmap | The mmap read path over a single key |
| `populated_db_mmap` | 10 | — | mmap | The mmap read path with multiple keys and cross-key interactions |
| `rotation_threshold_mmap` | 1 | 1 | mmap | Rotation with the active file mapped — sealing under classes G and H |
| `single_key_pool` | 1 | — | buffer_pool | The buffer-pool read path over a single key |
| `populated_db_pool` | 10 | — | buffer_pool | The buffer-pool read path with multiple keys |
| `rotation_threshold_pool` | 1 | 1 | buffer_pool | Rotation with a pooled file — fill and eviction under classes G and H |

The three `_mmap` shapes map the active file `MAP_SHARED` and sealed
files `MAP_PRIVATE` (see *DataFile mmap* in `bytecask_design.md`). They
are the shapes that reach `WritableMmapDataFile::truncate` through
`assert_resumable`, which is the path #87 broke: those cells ran it on
every CI run, ASan included, and none of them could fail on it, because
a cell can only assert what the contract states and the contract said
nothing about the span a reader holds across that call. `CONTRACT.md`
now does, under *View and span lifetimes*, and the observer axis below
is what lets a cell act on it.

The three `_pool` shapes serve sealed reads from the engine-owned buffer
pool. For a lent view they behave like `pread` rather than like `mmap`:
the pool copies each value out of its frames into the iterator's own
`io_buf_`, so no span ever points into a frame and no fill or eviction
can reach one.

On Emscripten builds both groups are compiled out — neither back-end is
available there, and `DB::open` rejects the option outright.

Shapes not currently covered: mid-vacuum (vacuum-in-flight during `apply_batch`),
post-resume (DB that has been degraded and resumed), and multiple sealed files with
cross-file tombstone interactions. These are deferred — the current shapes cover the
structurally distinct starting conditions for the write path, with and without mmap.

#### Observers

Every other assertion in the matrix reads the DB *after* the transition,
so the read path appears only as the oracle's instrument, never as the
subject. An observer inverts that: it takes a view **before** the
transition and checks it afterwards — once after `assert_delta`, and
again after `assert_resumable`, so the view is checked across `resume()`
as well as across the failure itself. What each view is owed is stated
in *View and span lifetimes* in [`CONTRACT.md`](../CONTRACT.md); the
observer is how a generated cell can act on it.

| Observer | Acquired before the transition | Catches |
|----------|-------------------------------|---------|
| `none` | — | (every cell; the 1313 observer-free cells) |
| `held_value` | `get` into a `Bytes` kept alive | value-path invalidation |
| `held_iter_span` | a span from `iter_from`, held across the call | #87 |
| `held_riter_span` | a span from `riter_from`, held across the call | the reverse read path |
| `held_snapshot` | `db.snapshot()` kept open | pinned-file / vacuum interaction |
| `second_instance` | a second `DB` open on the same thread | BC-243 |

`assert_view_stable(held, expected)` is the check for the two span-based
observers (`held_iter_span`, `held_riter_span`). It probes with `mincore` (via `tests/mapping_probe.h`) and
then compares bytes. The byte comparison is what discriminates:
`mmap(nullptr, …)` often returns the address `munmap` just released, so
an unmap-and-remap can look identical to never having unmapped.

Two elimination rules keep the axis affordable — a naive cross product
would be 4000 cells:

1. **An observer needs a transition that can disturb a view.** It is
   crossed only where the failure class degrades (B1, B2, B3, C, F, G,
   H — every cell that reaches `assert_resumable`, and through it
   `resume()`'s truncate) or the state shape maps the active file.
   Class A returns before any I/O, so `none` is the only observer there.
   Only `io_backend = mmap` satisfies the second arm: under `pread` and
   `buffer_pool` a lent span points into the iterator's own buffer, which
   no file event can reach, so those shapes are crossed through the
   degrading arm alone. That is why the `_mmap` shapes carry 60 / 60 / 80
   observer cells where their `pread` and `_pool` counterparts carry
   50 / 50 / 70 — the difference is the SUCCESS-class cells.
2. **Two plan shapes carry the observers per (state, failure).** What a
   transition does to a lent view is decided by the failure class and the
   state shape — which file is written, whether it is mapped, whether
   `resume()` truncates it — plus one property of the plan: whether its
   write set touches the key being observed. So each (state, failure)
   elects two representatives:

   - **disjoint** — `multi_put`, which writes fresh keys `p0`/`p1` and
     emits the `BulkBegin`/`BulkEnd` pair #87 was found in.
   - **colliding** — `sequential_overwrite` (a put on `k0`), or
     `causality_del_put` for classes needing two or more operations.
     Every observer reads `k0`, so these are the cells where the
     transition overwrites the very key being observed. That is what
     makes `held_snapshot` assert isolation rather than mere survival —
     after the write commits, `assert_delta` sees the new value and the
     held snapshot must still return the old one — and what asserts that
     a superseded entry's bytes stay put on an append-only file.

   Everything else a plan shape varies — entry count, solo versus group
   commit, guard vocabulary — is invisible to a held view, so crossing
   all 17 with every observer would multiply the matrix without asking a
   new question.

Observers that read a key (`held_value`, `held_iter_span`,
`held_riter_span`, `held_snapshot`) also require a state shape that
leaves one behind, so they skip `empty_db` and `deleted_key`.

Reverting #90 — restoring the `munmap` / `mmap` in
`WritableMmapDataFile::truncate` — makes these cells fail on the byte
comparison rather than only under ASan, and none by crashing. The
`mincore` probe passes in every one of them: `mmap` hands back the address
`munmap` just released, which is exactly the blind spot that motivated
comparing bytes. The `rotation_threshold_mmap` cells correctly keep
passing: at `max_file_bytes = 1` every write rotates, so their span points
into a sealed `MAP_PRIVATE` file that `truncate()` never touches.

This is also the answer to #104's proposed class **M1**, a `MAP_FIXED`
hook to force `mmap` to return the address `munmap` just released. Its
stated goal is to turn "often looks like it works" into a cell that either
passes or fails, and the byte comparison already does that — it does not
care what address came back, only what bytes are behind it. A hook would
add machinery for a property the axis already holds deterministically.

Reverting BC-243 — dropping the owner check from
`DB::load_state_for_read` — fails 10 `second_instance` cells on
`obs_db.get(...)` returning false: the second DB's read is served the
primary's cached generation, in which that key does not exist.

#### What the axis does not reach

Reverting BC-122 — giving `ReverseRadixTreeIterator::operator*` back the
`std::reverse_iterator` shape, where it dereferences a temporary copy and
returns a span into it — left **every cell in the matrix passing** when it
was measured. Four `radix_tree_test.cpp` cases caught it instead (the radix
tree has since been removed, and the class with it).

That is structural, not a coverage gap to close by adding cells. The
reverted class is reached only through `key_dir.rbegin()`, and no public
read path lends a caller a span that comes from it: `riter_from` goes
through `ReverseValueIterator`, which yields a `KeyDirEntry` by
reference, and the span the caller actually receives is built afterwards
by `ReverseEntryIterator` from the data file. `rkeys_from` uses
`rbegin().base()` only as a starting position and materialises an owning
`Key`. An observer sits at the DB API, so it cannot see a class the DB
API does not lend from — the radix tree's own tests are the right place
for that one, and they hold it.

#### Plan shapes

| Shape | Operations | Guards | Conflicting | What it tests |
|-------|-----------|--------|-------------|---------------|
| `single_put` | 1 put | No | No | Single-entry fast path (no BulkBegin/BulkEnd markers) |
| `single_delete` | 1 delete | No | No | Tombstone write on the single-entry path |
| `multi_put` | 2 puts | No | No | Multi-entry batch with BulkBegin/BulkEnd markers |
| `mixed_batch` | 1 put + 1 delete | No | No | Mixed operation types in one batch |
| `large_batch` | 3 puts | No | No | Larger batch — exercises per-entry fault injection at different positions |
| `single_put_with_guards` | 1 put | Yes | No | Plan with `ensure_unchanged` guards plus a write |
| `conflicting_plan` | 1 put | No | Yes | Plan that conflicts with current state — exercises class A (precondition rejection) |
| `causality_overwrite` | 2 puts (same key) | No | No | Last write wins: `put(c0, v1), put(c0, v2)` — value must be v2 |
| `causality_put_del` | 1 put + 1 delete (same key) | No | No | Put then delete: `put(c0, v1), del(c0)` — key must be absent |
| `causality_del_put` | 1 delete + 1 put (same key) | No | No | Delete then put: `del(k0), put(k0, v1)` — key must have v1 |
| `causality_put_del_put` | 1 put + 1 delete + 1 put (same key) | No | No | Three ops: `put(c0, v1), del(c0), put(c0, v2)` — value must be v2 |
| `solo_causality_overwrite` | 2 puts (same key), solo | No | No | Same as `causality_overwrite` via solo writer path |
| `solo_causality_put_del` | 1 put + 1 delete (same key), solo | No | No | Same as `causality_put_del` via solo writer path |
| `solo_causality_del_put` | 1 delete + 1 put (same key), solo | No | No | Same as `causality_del_put` via solo writer path |
| `solo_causality_put_del_put` | 1 put + 1 delete + 1 put (same key), solo | No | No | Same as `causality_put_del_put` via solo writer path |
| `sequential_overwrite` | 1 put targeting k0 | No | No | Overwrites pre-existing k0 — sequential put-then-put causality across calls |
| `solo_sequential_overwrite` | 1 put targeting k0, solo | No | No | Same as `sequential_overwrite` via solo writer path |
| `range_del` | 1 range delete over `["k", "l")` | No | No | A range tombstone as the whole transition — one append, a delta covering every key the range spans |
| `range_del_then_put` | 1 range delete + 1 put on k0 | No | No | The put lands *inside* the range it follows and wins; k0 survives with the new value, every other k-key goes |
| `put_then_range_del` | 1 put on k0 + 1 range delete | No | No | The same two operations the other way round; the range tombstone wins and k0 goes with the rest |

The three range-delete shapes are the same question asked of a write
whose delta is not bounded by its key: `del_range` appends one entry and
removes however many keys fall inside the bounds. Pairing it with a put
*inside* the range, in both orders, is what makes the cells about
sequence resolution rather than key lookup — nothing in the key
directory distinguishes the two orderings, only the sequences do, and a
failure class is precisely what perturbs the order the entries reach
disk in. They also drive the range-tombstone suppression loop in
recovery's hint replay, which is O(R) per Put and
was previously exercised on clean paths only. They carry no observers:
what a range tombstone does is settled in the key directory and at
recovery, and a lent view can see neither.

The four `causality_*` shapes verify that operation ordering within a
batch is preserved through all failure classes and recovery. Each shape
targets the same key for every operation, so the net effect depends
entirely on causal ordering. The four `solo_causality_*` shapes mirror
them but route through the solo writer (`WriteOptions::solo = true`)
instead of group commit, ensuring both write paths preserve causality.
The `sequential_overwrite` shapes test causality across separate API
calls: the setup writes k0, and the plan overwrites it. Combined with
the `deleted_key` state (tombstone in history) and `rotation_threshold`
state (cross-file writes), these cover sequential put-then-put,
del-then-put, and cross-file causality.
`assert_delta` and `assert_recoverable` verify both key presence and
expected values (the `expected_values` field in `ExpectedDelta`).

Shapes not currently covered: guards-without-writes (pure read-dependency check),
delete-only multi-entry batches, and plans exceeding the `256 KiB` solo-writer
threshold. These are deferred — the current shapes exercise every
code path branch in `apply_batch` (single vs multi entry, with and without
guards, conflict vs success, same-key causality).

#### Elimination rules

Four rules filter invalid (state, plan, failure) combinations:

1. **Class C requires multi-entry batches** — C fires on `BulkEnd`, which is only emitted for batches with 2+ operations.
2. **Class A requires a conflicting plan** — A is precondition rejection before any I/O; only `conflicting_plan` triggers it.
3. **Conflicting plan only valid for class A** — a plan that fails preconditions never reaches the I/O path.
4. **Classes G and H require `rotation_threshold` state** — rotation only occurs when the active file crosses the size threshold.

NOSYNC carries no observer: it does to a lent view exactly what SUCCESS
does, and exists for the power cut.

Each test follows the same structure:
1. Set up initial DB state from `StateShape`, under `ScopedPageCacheModel`
2. `make_durable(db)`, then capture baseline
3. Construct `WritePlan` from `PlanShape`
4. Inject fault per `FaultConfig`
5. Execute `apply_batch`
6. `assert_delta(before, db, expected)` — validates key membership,
   value correctness (causality), LSN advancement, structural consistency,
   and degraded state.
   For degraded cases, `assert_resumable(db)` is called immediately after
   to verify that `resume()` restores consistent state in-process.
7. `copy_device(dir, cut)` — the power cut, before the DB closes
8. `assert_recoverable(dir, before, expected)` — validates persistence
   invariant via fresh recovery (where applicable)
9. `assert_power_loss_outcome(cut, ...)` — the cut copy holds the durable
   baseline, alone or with the whole transition; for degraded cells,
   `assert_matches_recovery(cut, fp)` instead (see *Power-loss axis*)

### resume() — 64 tests

64 generated Catch2 tests (`[prove_resume]` tag) cover every valid
(DegradeShape, ResumeFailureClass) combination. Every cell also cuts the
power after the final `resume()` and checks the cut copy against the resumed
engine's fingerprint (*Power-loss axis*): `resume()` claims durable what it
publishes, and the F and G shapes are where that claim rests on the rewrite
from #240.

Twelve degrade shapes establish a degraded DB before resume is called:

- **degrade_H** — `io_rotate_file_creation` fires on a put at the
  rotation threshold. The write committed (both keys are in key_dir),
  rotation failed. No orphaned bytes — `valid_offset == file.size()`.
- **degrade_C** — `ScopedFaultInjector{fail_at=3}` on a 2-op batch.
  BulkEnd at checkpoint 3 fails; subsequent isolation sync and rotation
  also fail (cascade). k0 committed; orphaned BulkBegin+p0+p1 bytes
  remain in the active file. `resume()` truncates back to after k0.
- **degrade_F** — `io_data_file_sync` fires on a `put(sync=true)`.
  The entry bytes are in the page cache but commit sync (fdatasync)
  failed. key_dir not published. `resume()` scans, finds the entry as
  a valid committed record, and replays it.
- **degrade_G** — `io_data_file_sync` fires on a `put(sync=false)`
  with `max_file_bytes=1`. The pre-rotation sync fails. Same page-cache
  state as F — `resume()` replays the entry.
- **degrade_H_mmap**, **degrade_C_mmap** — H and C with
  `io_backend = mmap`. These are the shapes where `resume()` truncates
  the active file while it is mapped and lock-free readers are not
  quiesced; the mapping must survive it (see *View and span lifetimes*
  in `CONTRACT.md`, and *DataFile mmap* in `bytecask_design.md`).
- **degrade_H_pool**, **degrade_C_pool** — the same two through the
  buffer pool. All four are compiled out on Emscripten builds.
- **degrade_B2** — `io_data_file_append_partial` with `short_write`. The
  `writev` returned short, so the trailing entry is torn and its CRC
  cannot hold. Every other shape leaves a *well-formed* active file whose
  orphaned bytes parse; this one is genuinely malformed on disk, and it
  reaches that state by construction rather than by damaging bytes after
  the fact. It is the branch both #36 bugs lived on: a scan that does not
  reach EOF cleanly.
- **degrade_B3** — the same checkpoint with `throw_after`. Every byte
  reached the disk and the entry is structurally complete, but `offset_`
  never advanced, so the entry sits past the file's committed offset and
  `resume()` discards it. Torn and complete-but-unacknowledged are
  different bytes on disk arriving at the same expected delta.
- **degrade_F_range** — a `del_range` whose commit `fdatasync` fails.
  Structurally this is degrade_F; what it adds is the entry type. A range
  tombstone reached the page cache and the key directory was never told,
  so `resume()` has to replay it — and replaying a range tombstone means
  *applying* it, not stepping over it. Before the `apply_resume` fix that
  landed with these cells, all five of them failed: the resumed engine
  held keys a fresh open did not.
- **degrade_F_batch** — a committed 2-op batch sits below the failure
  point, so the active file holds `BulkBegin`(1), p0(2), p1(3),
  `BulkEnd`(4) and then k0(5), whose commit sync fails. The keys are the
  easy part; the shape exists for the file's *sequence bounds*. Markers
  consume sequences, and `resume()` used to filter them out of the entries
  it collected, so it reported `min_sequence = 2` for a file whose first
  entry is sequence 1 — while a cold open, whose hint file does carry
  markers, said 1. All five cells failed before that filter was removed.

Seven resume failure classes:

- **SUCCESS** — clean resume on first attempt.
- **R1** (`io_resume_truncate`) — truncation fails, stays degraded.
- **R1_AFTER_CUT** (`io_resume_truncate`, post-write) — the truncate cuts
  the file and then fails (#236). Stays degraded, and every key published
  before the degrade must still read back while it is: before the fix the
  file's logical end stayed past the new end of file, a read's over-read
  ran past EOF, and the degrade_C cells failed with a short read.
- **R2** (`io_resume_sync`) — sync fails, stays degraded.
- **R3** (`io_resume_file_creation`) — new active file creation fails.
- **DOUBLE** — resume succeeds, then a second resume is called (no-op).
- **CASCADE** — R2 fails, then R3 fails, then clean resume succeeds.

Two elimination rules apply:

1. **R1 requires orphaned bytes.** degrade_H, degrade_F, degrade_G,
   degrade_F_range and degrade_F_batch have none in the active file, so
   `file.size() == valid_offset` and `resume()` skips the truncation branch entirely
   (`if (file.size() != valid_offset) { ... truncate ... }`). R1 is valid
   for the degrade_C shapes (orphaned `BulkBegin`) and for degrade_B2 and
   degrade_B3, which leave a torn and a complete-but-uncommitted entry
   respectively — 7 combinations filtered. R1_AFTER_CUT needs the same
   shapes — 7 more.
2. **R2 and CASCADE require an unsealed file.** The degrade_H shapes
   seal the active file during rotation before the fault fires, so
   `resume()` never enters the truncate/sync/seal block and the sync
   fault point is unreachable — 6 more combinations filtered.

12 shapes × 7 classes = 84 minus 20 filtered = **64 tests**.

Present keys are asserted with `get`, not `contains_key`, both in-process
and in `assert_keys_recoverable`. A truncation that cut too far leaves
the key directory intact while the bytes behind it are gone, which is
exactly how the #36 bug stayed invisible to a test named for it.

Every cell also ends with `assert_matches_recovery`. The key assertions
ask whether `resume()` produced the *right* state; this one asks whether
it produced the *same* state a cold open produces from the same bytes,
which is the property `resume()` exists to preserve and the one both bugs
in this matrix's history violated. It compares `next_seq`, the full
key-value map in both directions, and per-file `live_bytes`,
`total_bytes`, `min_sequence` and `max_sequence` — keyed by the data
file's stem, because recovery assigns file ids by directory order.

Each R1/R2/R3 test uses a multi-phase pattern:
1. Establish degraded state
2. Inject resume fault → `resume()` throws, engine stays degraded
3. Clean resume → `REQUIRE_NOTHROW`, `is_degraded()` false, `assert_consistent`
4. `assert_keys_recoverable` after DB scope closes

DOUBLE tests verify the second `resume()` is a no-op on a healthy engine.
CASCADE tests inject two sequential faults (R2 then R3) before the clean
resume, proving the engine tolerates repeated failures with different
fault points.

This directly proves: *resume always eventually recovers once the underlying fault clears.*

### vacuum_compact — 94 tests

94 generated Catch2 tests (`[prove_vacuum_compact]` tag) cover ten state
shapes × eleven failure classes (SUCCESS, VC1–VC10), less the combinations
a shape cannot reach (below).

State shapes create a DB with exactly one sealed file having fragmentation > 0:

- **low_fragmentation** — sealed file with 2 entries, 1 dead (50% frag)  
- **mostly_dead** — sealed file with 6 entries, 5 dead (~83% frag)
- **low_fragmentation_mmap**, **mostly_dead_mmap** — the same two
  shapes with `io_backend = mmap`, so the file being compacted is read
  through its `MAP_PRIVATE` mapping and the staged copy is written
  through a mapped active file.
- **low_fragmentation_pool**, **mostly_dead_pool** — the same two
  through the buffer pool. All four are compiled out on Emscripten
  builds.
- **batched_file** — the sealed file's live entries sit inside a
  `BulkBegin`/`BulkEnd` pair. `max_file_bytes = 88` is the batch's exact
  size (19 per marker, 25 per entry), so file_0 seals with the whole
  batch and the delete that fragments it lands in the next file.
- **range_tombstone_file** — the sealed file holds a range tombstone
  (25 + 25 for the puts, 23 for the `RangeDel`, so `max_file_bytes = 73`).
- **unsynced_overwrite** — #245's shape: the sealed file's dead entry is
  superseded only by a `sync = false` write. Two 25-byte puts fill
  `max_file_bytes = 50` and seal file_0; `k1` is then overwritten with a
  2-byte value, a 23-byte entry that stays under the limit, so no rotation
  syncs it and only vacuum can. The other shapes make their dead entries
  with deletes, which hide this class of bug: a lost delete and a dropped
  Put leave the state the history ends in anyway.
- **unsynced_overwrite_whole_file** — both keys overwritten the same way.
  No live entry and no tombstone, so vacuum drops the file whole through
  `vacuum_remove_file`, with no staging copy: the other path through
  `vacuum_commit`, and the one #245 was found on.

The last two exist for an invariant no key-and-value assertion can see:
batch markers and range tombstones are not key data, so a compaction
that dropped one on its retry path would leave a batch that is no longer
atomic on disk, and neither the delta checks nor recovery would notice.
`capture_vacuum_baseline` therefore counts them over `changes_since`, and
`assert_vacuum_success`, `assert_vacuum_no_change` and
`assert_vacuum_recoverable` all compare that count — the success path,
the failure path, and the round trip through disk. The other six shapes
contain none, so the check is vacuous there and costs nothing.

Files with live entries always use the compact path (sealed→sealed).
`mostly_dead` uses `max_file_bytes=150` to pack all 6 keys into one
sealed file.

Failure classes: SUCCESS, VC1 (`io_vacuum_compact_tmp_create`),
VC2 (`io_data_file_append`), VC3 (`io_data_file_sync`),
VC4 (`io_vacuum_compact_rename`), VC5 (`io_vacuum_compact_unlink`),
VC6 (`io_vacuum_compact_post_rename`), VC7 (`io_data_file_sync` again,
at the `fdatasync` `vacuum_commit` issues before dropping records that only
`sync = false` writes supersede, #261 — the second checkpoint of that name
on the compaction path, after the staging copy's, and the first on the
whole-file path, which writes no copy), VC8 (`io_vacuum_compact_shrink`),
and VC9 and VC10, which fail calls below the engine (#258, below).

VC7 applies only to the two `unsynced_overwrite` shapes: with the deletes
made durable by `make_durable`, `vacuum_commit` has nothing to sync in the
others. The whole-file shape reaches only SUCCESS, VC5 and VC7: it makes
none of the calls VC1–VC4, VC6 and VC8–VC10 fault. Under VC7 the engine degrades and
vacuum throws before committing, so the old file stays
(`assert_vacuum_no_change`, `is_degraded() == true`).

Every cell then cuts the power (*Power-loss axis*). The durable baseline is
the sealed file and every delete; the overwrites are the transition. After
SUCCESS they are required in the recovered copy — vacuum made them durable
before it dropped what they superseded — and after VC7 either outcome is
allowed. Reverting #261's sync fails both SUCCESS cells with a key gone
from the copy: the old value dropped, the new one lost, a state no prefix
of the history produces.

VC4 fails at the last step before the rename: the tmp file is fully
synced and shrunk but `vacuum_commit` has not run — the old file is still
in the published state. The rename-completed case is VC6.

Every cell also checks that no `.data.tmp` outlives `vacuum()` (#235):
VC2–VC4 fail after the staging file exists, and vacuum must remove it on
the way out rather than leave a copy per retry until the next open. Every
cell but VC5 checks the same of the renamed copy (#304): no `.data` or
`.hint` file is on disk that the published state does not reference.

VC5 is the other side of the commit: `vacuum_commit` has run, so in memory
the outcome is success (`assert_vacuum_success`), but the source was never
unlinked. On disk that is a compacted file and its source holding the same
entries under the same sequences — what a kill anywhere between the rename
and the unlink leaves, a window that spans the compacted file's hint scan.
Recovery undoes the vacuum by deleting the compacted file.
`assert_vacuum_recoverable` proves the directory opens with every key and,
for every class, that the recovered files are sequence-disjoint. Before
recovery handled this pair, every VC5 test failed with "two entries share
the same sequence number but differ in physical location", which is how a
cgroup OOM kill under a write-heavy sysbench run left a database that would
not open.

VC6 is the other end of that window, and #104's class M3: the rename
completed and the commit did not. Nothing is committed, so in memory the
outcome is a failed vacuum (`assert_vacuum_no_change`), and vacuum removes
the compacted copy it placed under its final name on the way out (#304).
Before #304 the copy stayed, referenced by nothing, until the next open
deleted it; a vacuum retried under a persistent fault placed one per
attempt, and the chaos rig found an open that ran a recovery pass for each
of 58. A kill in the window still leaves the copy, and recovery deletes it
as it deletes VC5's: making recovery's undo throw fails all eight VC5
cells.

VC8 fails the staging copy's `shrink_to_fit()`, between its sync and the
rename. The outcome is VC4's: the old file stays and the staging copy goes.

VC9 and VC10 reach the fallback in `renameDataFileExclusive`, for a
filesystem without `RENAME_NOREPLACE`: `link()` places the copy, then
`unlink()` removes the staged name. No checkpoint can select that path, so
these cells use `ScopedSyscallFaults` with one rule per call (*Counted fault
sweep*): `renameat2` reports `EINVAL`, and `unlink` fails. Each cell checks
that those two calls, on the `.data.tmp`, are the first to fail, so a cell
that never reached the fallback does not pass. They are compiled on Linux
only, where the interposers are linked.

VC9 fails the staged name's `unlink()` once. The placement took effect, but
the function is about to report that it did not, so it takes the placed
name back before it throws, and vacuum's cleanup removes the staged one.
Nothing is left (`unreferenced_files` is empty), as in VC4. Without the
take-back, the placed copy stayed until the next open, one copy per retry
(mutation `rename_fallback_keeps_placed_copy`). VC10 fails every `unlink`
and `remove` in the directory, so the take-back and the cleanup fail too.
Both names stay (the cell asserts they do), and
`assert_vacuum_recoverable` proves the next open removes them and recovers
every key.

### corruption — 16 tests

16 generated Catch2 tests (`[prove_corruption]` tag) cover four positions ×
four damaged fields. Classes A through H all mean the same thing at the
POSIX level — a syscall returned an error. This axis is over the **bytes**
rather than the **calls**: a read that succeeds and hands back something
wrong.

The entry layout makes a position addressable without parsing. A data
entry is 15 bytes of header — `sequence` u64, `entry_type` u8, `key_size`
u16, `value_size` u32 — then key, then value, then a 4-byte CRC. With
2-byte keys and values every entry is 23 bytes, so entry *i* starts at
`23 * i` and each field sits at a fixed offset inside it.

| Field | Damage |
|-------|--------|
| `crc` | flip a key byte, which the entry CRC covers |
| `value_size` | a size that runs past the end of the file — what #36 sized a read buffer from |
| `entry_type` | a byte that is not a valid `EntryType` |
| `sequence` | zeroed, which the scan already treats as end of file |

crossed with position: first entry, mid-file, last entry, and inside a
batch between `BulkBegin` and `BulkEnd`.

#### The contract is fail-stop, not recovery

What separates these cells from every other shape in the framework: the
damaged entry was appended, synced **and published**. Every other degrade
shape leaves damage only in bytes a failed write appended and never
published — a torn tail nobody was served. Here readers were already served
the bytes that are now wrong, and the engine cannot know what state that
leaves it in. So it makes no promise about what a damaged file still holds.
It promises only to stop rather than make the damage worse:

- `resume()` throws `std::runtime_error`, stays degraded, and truncates
  nothing — and does the same on every retry;
- the data file is byte-for-byte what it was after the damage, including
  the unpublished entry the failed write appended after it;
- a cold open does not refuse: the damaged file is the newest, and there
  damage is indistinguishable from a torn tail (below).

The line `resume()` draws is the **published extent** — the active file's
`total_bytes` in the last published state. `resume()` exists to trim what
a failed write left behind, which is by construction above that extent. A
scan that stops below it has found damage in acknowledged data, not a torn
tail, and truncating there would destroy acknowledged entries and every
entry behind them. The check runs before the truncation, so a refusal
leaves the file as it was found. An I/O error during the scan is rethrown
as-is for the same reason: it says nothing about the bytes, and trimming
on a transient `EIO` would destroy data over a fault the next attempt may
not see.

The delta is the same shape in every cell: `resume()` refuses, stays
degraded and touches no byte; a cold open truncates at the damage.

#### What a cold open can and cannot see

| Field | `resume()` | Cold open |
|-------|-----------|-----------|
| `crc` | refuses | truncates at the damaged entry |
| `entry_type` | refuses | truncates at the damaged entry |
| `value_size` | refuses | truncates at the damaged entry |
| `sequence` | refuses | truncates at the damaged entry |

Every cell damages the newest data file, and a cold open knows no more
about it than its bytes: nothing outside the data files records how much of
it was synced. A torn tail and damage in synced data look alike there, and
open truncates at the first bad record for both, keeping what was committed
before it — the choice PostgreSQL and RocksDB make for their logs (#138).
`resume()` can refuse because the published state knows the extent. Damage
in any other hint-less file is refused at open, in every mode; the rule and
what it leaves unrefused are in `bytecask_design.md`, *Recovering a
Hint-less File*.

#### What the axis found

**The scan was one step ahead of what it had yielded.** `advance()` set
`committed_offset_` to the end of the entry it had just buffered and then
called `++cur_` to parse the next one before returning. When that threw,
the exception left the `operator++` that was about to yield the buffered
entry, so the caller never saw an intact entry below the damage and
`resume()` placed the damage one entry too early. `CommittedEntryIterator`
now defers that step to the next advance: the entry after a committed one
is parsed only when the caller moves past it, so the throw comes out of
the `operator++` that steps past an entry the caller has already counted.
Every parse and I/O error still propagates to every caller. An earlier fix
caught the error inside the iterator and ended the scan there instead.
That iterator is shared with the scan that indexes a hint-less file at
open and the one vacuum compacts a sealed file with, and both rely on the
throw: open truncated the data file to the damage, and vacuum published a
copy without the entries behind it and unlinked the original. Two cells in
`bytecask_test.cpp` (`[corruption]`) pin both down.

**`resume()` could report success over data it had cut.** With the damage
below the published extent, the old `resume()` truncated to the damage
while the key directory still pointed at the bytes it removed. A debug
build's extent check in `store_state` rejected the resumed state and left
the engine degraded; with `NDEBUG` that check is compiled out, so `resume()`
reported success and the next read of such a key `pread` past the end of
the file. Refusing before the truncation closes both: nothing is cut, so
nothing dangles. Pruning the dangling keys instead was considered and
rejected — it presents a damaged file as a consistent engine, and a key
with an older version in a sealed file would then read as absent after
`resume()` and as the old value after a cold open.

Reverting the extent check fails all 16 cells. Restoring the catching
iterator fails the two `[corruption]` cells and the cold-open half of six
more — `crc` and `entry_type` at every position but the first, where the
scan throws opening the file, before the iterator takes a step it could
catch.

### recovery — 40 tests

40 generated Catch2 tests (`[prove_recovery]` tag) cover five state shapes
× four failure classes × `recovery_threads ∈ {1, 4}`.

`hint_file.cppm` had 19 syscall sites and no fault points, and `DB::open`
had no matrix at all — recovery was proven by the `[model]` tests, every
one of which runs the clean path. The README sells the hint write as a
crash-safety mechanism (`write → fdatasync → rename`) and nothing injected
a failure into any of the three steps. Three checkpoints now mirror what
the data file already has: `io_hint_write`, `io_hint_sync`,
`io_hint_rename`.

What makes the matrix cheap to assert is that the expected delta is the
same in every cell: **a hint file is a rebuildable index, not the record**,
so a failure generating one may cost recovery time and must not cost keys.
`recovery_prepare_files` calls `flush_hints_for` without a catch, so the
failure propagates out of `DB::open`; each cell then reopens with the
fault cleared and checks every key back, by value.

#### State shapes

| Shape | Damage | What it tests |
|-------|--------|---------------|
| `crash_hintless` | newest hint removed | The file that was active when the process died: a crash leaves it hint-less, and `recovery_prepare_files` regenerates it. A clean close writes that hint, so the cell removes it |
| `multi_file_hintless` | every hint removed | Several sealed files, none with a hint; all must be rebuilt |
| `damaged_hint` | newest hint's CRC broken | `open_hint_or_rebuild` must discard it and rebuild from the data file rather than drop the keys behind it |
| `hintless_batched` | newest hint removed | `BulkBegin`/`BulkEnd` have to survive regeneration — a hint carries them so recovery can compute `durable_seq` across a batch |
| `hintless_range_del` | newest hint removed | A range tombstone has to survive it too; recovery reads it back out of the regenerated hint to suppress the range |

Serial and parallel recovery diverging is the specific risk the `[model]`
tests were built around, so every shape is recovered both ways.

#### What is out of reach, and why it is not worked around

Six call sites reach `flush_hints_for`. Four are synchronous —
`recovery_prepare_files`, `open_hint_or_rebuild`, `flush_hints` at close,
and `vacuum_compact_file` — and a fault armed on the thread calling
`DB::open` reaches them. The two post-rotation `worker_.dispatch` sites do
not: `active_injector` is thread-local and the background worker has its
own. Propagating one into the worker would need it to outlive the stack
frame that set it, and the failure it models — a missing hint — is already
the state `crash_hintless` starts from and every cell here recovers from.
So it is recorded rather than engineered around.

### group_commit — 22 tests

Every cell cuts the power before the DB closes and checks the cut copy
against the engine's fingerprint (*Power-loss axis*): a synced group is
durable, and a degraded one was resumed.

22 generated Catch2 tests (`[prove_group]` + `[concurrency]` tags) cover
six group shapes × the failure classes valid for each. Every other matrix
in this framework drives one thread; these are the cells where group
commit — a writer that finds no leader becomes one and executes every
pending write under a single lock hold — is faulted with more than one
writer in play.

#### What turned out not to need building

#118 proposed tagging each slot with its own fault, on the premise that a
follower's I/O could be failed while the leader's succeeded. The write
path does not work that way. `execute_slot` is pure in-memory — it
contains no append or sync call at all — and `execute_slots` issues **one**
`append_entries` for every slot's entries combined and **one** `fdatasync`.
There is no per-slot I/O boundary to fail, so "fail writer B's slot while
A leads" is not a state the engine can be in, and the cells that issue
proposed for it are struck rather than deferred.

What is left is group-wide, and the thread-local injector reaches it as it
stands. `WriteGroup` already carries `on_leader_start_` and
`wait_for_queue_size`, so a cell forces a batch of exactly N deterministically
rather than racing for it.

#### Where a fault has to be armed

Not one answer, and the difference is what made the first draft of these
cells flaky:

| Call | Thread that makes it |
|------|---------------------|
| the group's combined `append_entries` (B1, B2) | the leader, under `write_mu_` |
| rotation `fdatasync` and file creation (G, H) | the leader, inline — the rotation branch takes the flush role itself |
| the commit `fdatasync` (F) | **not the leader** |

On the non-rotation path `execute_slots` ends at `store_head()` and
returns; the flush is left to `flush_pending()`, run by whichever writer
holds the `FlushRole`. Arming only the leader made the F cells pass or
fail by race. They arm every writer thread instead, which is also the
truer question: when the group's `fdatasync` fails, every writer in it
must learn, whoever ran it.

#### Group shapes

| Shape | Writers | Plans | What it tests |
|-------|--------:|-------|---------------|
| `group_of_2`, `group_of_4` | 2, 4 | 1 put each | The combined append and flush for a small and a larger group |
| `group_of_2_batched`, `group_of_4_batched` | 2, 4 | 2 puts each | The same with a `BulkBegin`/`BulkEnd` pair per writer, so all-or-nothing is asked of batches rather than single entries |
| `group_of_2_rotation`, `group_of_4_rotation` | 2, 4 | 1 put each, `max_file_bytes = 1` | The rotation barrier with a group behind it — G and H |

Two elimination rules: G and H need the rotation shapes, and the rotation
shapes are not crossed with B1/B2/F — those would re-ask what the
non-rotation shapes ask against a fault point that fires at a different
call in the sequence, which says nothing new about grouping.

#### What every cell asserts

The group shares one append and one `fdatasync`, so the engine has no way
to tell one writer its write landed and another that it did not. Each cell
checks that every writer saw the same outcome, that the group's keys are
all visible or none are, and — since `store_state(published, …)` runs
before the errors are set in the H path but not in the others — which side
of that line the class falls on. Degraded cells then resume and end in
`assert_matches_recovery` like the rest of the framework.

All 22 pass, and passed 8 consecutive runs before being committed. They
found no engine bug; what they add is that the mechanism is now covered
at all, and the record of who performs which call.

### prove_replication — 211 tests

The eleven `full_stream` SUCCESS cells cut the power twice (*Power-loss
axis*): the leader once the stream is collected, so every streamed entry
must survive its loss — invariant 8 proven on the device rather than on
the return value, since `ingest` syncs and a follower cut mostly re-proves
the write path — and the follower once it has ingested, so everything it
acknowledged must (`assert_stream_survives_power_loss`,
`assert_replication_recovery` on the copy).

211 generated Catch2 tests (`[prove_repl]` + `[prove_manifest]` tags) cover
the replication pipeline: 178 ingest tests covering (StateShape × OpsShape ×
IngestFailureClass) and 33 manifest tests covering (StateShape ×
ManifestFailureClass). The scenario matrix is defined in
`tests/proof/replication/scenario_matrix.py`.

#### State shapes

11 leader workloads that produce the state to replicate:

| Shape | max_file_bytes | has_batches | has_nosync | What it tests |
|-------|---------------|-------------|------------|---------------|
| `single_key` | — | No | No | Minimal replication payload — single entry through the full pipeline |
| `multi_key` | — | No | No | Multi-entry ingest; changes_since yields multiple entries in sequence order |
| `overwrites` | — | No | No | changes_since yields both entries; ingest applies in sequence order so the overwrite wins |
| `deletes` | — | No | No | Tombstone replicates correctly — follower does not have the key after ingest |
| `range_deletes` | — | No | No | RangeDel entry walks the follower's key directory over [from, to) |
| `batches` | — | Yes | No | Batch markers preserved through changes_since; ingest uses them to write atomically |
| `multi_file` | 256 | No | No | changes_since merges entries across multiple sealed files in ascending sequence order |
| `mixed_sync_nosync` | — | No | Yes | changes_since yields only entries up to durable_sequence; unsync'd entries excluded |
| `nosync_only` | 256 | No | Yes | Entries replicable only after rotation triggers fdatasync; replication lag bounded by max_file_bytes |
| `nosync_then_sync` | — | No | Yes | The sync's fdatasync covers all prior nosync entries; durable_sequence catches up |
| `vacuumed_batches` | 256 | Yes | No | changes_since over vacuumed file yields BulkBegin/BulkEnd markers; ingest writes atomically. The follower is seeded with the history up to a point, and the leader vacuums with `retain_after` there, as a replication service would |

#### Ops shapes

How entries are delivered to the follower:

| Shape | What it tests |
|-------|---------------|
| `full_stream` | Baseline — full replication in a single ingest call |
| `incremental` | Each chunk produces a valid prefix; follower.durable_sequence() advances monotonically |
| `restart_midstream` | Recovery equivalence — reopened follower's durable_sequence() is trustworthy, next changes_since picks up without gaps |
| `duplicate_delivery` | Idempotency — entries with sequence <= durable_sequence() are silently skipped |
| `planned_promotion` | Sequence continuity on promoted node; backward sync after leadership transfer converges |

#### Ingest failure classes

| Class | Entries applied | key_dir changes | Degraded |
|-------|----------------|-----------------|----------|
| SUCCESS | All | Yes — full delta | No |
| I_B1 (`append_fails_nothing_written`) | None | No | Yes |
| I_B2 (`append_fails_partial_write`) | None | No | Yes |
| I_F (`sync_fails`) | None visible | No (not published) | Yes |
| I_C (`crash_mid_batch`) | None from the batch | No — recovery discards incomplete batch | No (recovery handles) |
| I_G (`rotation_sync_fails`) | None visible | No (not published) | Yes |
| I_H (`rotation_file_creation_fails`) | Chunk that triggered rotation | Yes — partial delta | Yes |

I_H is notable: the sync succeeded before file creation failed, so the
chunk that triggered rotation is fully durable. The follower's key_dir
reflects a partial delta — the entries from that chunk are committed.

#### Manifest failure classes

| Class | Manifest produced | Leader state |
|-------|-------------------|-------------|
| SUCCESS | Yes | Continues accepting writes |
| M_R (`rotation_fails`) | No — exception thrown | Degraded: the active file was sealed and no new one was created; `resume()` recovers |
| M_S (`sync_fails`) | No — exception thrown | Degraded: the pre-rotation `fdatasync` failed, and no later one can be trusted to write what it did not (#281); `resume()` recovers |

#### Elimination rules

Four rules filter invalid (state, ops, failure) combinations:

1. **duplicate_delivery × non-SUCCESS** — duplicates are skipped before
   I/O, so all failure classes except SUCCESS are unreachable.
2. **planned_promotion × non-SUCCESS** — a flag flip, only SUCCESS applies.
3. **I_C requires batch markers** — only valid for `batches` and
   `vacuumed_batches` state shapes.
4. **I_G / I_H require file rotation** — only valid for state shapes
   with `max_file_bytes` set (`multi_file`, `nosync_only`,
   `vacuumed_batches`).

#### Test structure

Each ingest test follows: setup leader workload → capture replication
baseline → create follower in Mode::Follower → inject fault → ingest per
ops shape → assert_replication_match or assert_replication_no_change →
assert_replication_recovery. For degraded cases, `resume()` is called
before re-ingesting remaining entries. `planned_promotion` tests run
the full leadership transfer protocol (demote leader → promote follower →
write on new leader → backward sync → verify convergence).

#### Edge cases

- **Nosync baseline**: `capture_replication_baseline` builds the expected
  key-value map by replaying `changes_since` output (not snapshot
  iteration), respecting the `durable_seq` boundary.
- **Batch boundary chunk splitting**: for small batch states, all entries
  may land in chunk 1 during incremental tests. Runtime guard handles
  empty chunk 2.
- **Hint-file batch marker gap (fixed)**: hint files now include
  BulkBegin/BulkEnd markers, so recovery correctly computes `durable_seq`
  for batch states. In `restart_midstream` tests, the second pass after
  recovery may still be too small to trigger rotation-class faults (I_H,
  I_G). The test framework uses try-catch with `bool threw` tracking to
  handle both outcomes.

### Source files

**apply_batch module** (root of `tests/proof/`):

| File | Role |
|------|------|
| [`expected_delta.py`](../tests/proof/expected_delta.py) | The reference model: `expected_delta(plan, failure, ...) → Delta` |
| [`scenario_matrix.py`](../tests/proof/scenario_matrix.py) | State shapes, plan shapes, failure classes, validity filter |
| [`fault_point_resolver.py`](../tests/proof/fault_point_resolver.py) | Maps (state, plan, failure) → `ScopedFaultInjector` configuration |
| [`generate_tests.py`](../tests/proof/generate_tests.py) | Generates `prove_apply_batch.cpp` from the matrix |

**resume module** (`tests/proof/resume/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | DegradeShape (H, C, F, G, B2, B3, F_RANGE, F_BATCH), ResumeFailureClass (SUCCESS, R1–R3, DOUBLE, CASCADE), validity filter |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: keys present/absent after all resume calls |
| `generate_tests.py` | Generates `prove_resume.cpp` |

**corruption module** (`tests/proof/corruption/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | CorruptionShape (position), CorruptField (crc, value_size, entry_type, sequence), computed byte offsets |
| `expected_delta.py` | Reference model: which keys survive by value, which are gone, what the file truncates to |
| `generate_tests.py` | Generates `prove_corruption.cpp` |

**recovery module** (`tests/proof/recovery/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | RecoveryStateShape (damage kind, batches, range deletes), RecoveryFailureClass (SUCCESS, RC1–RC3), `recovery_threads` axis |
| `fault_point_resolver.py` | Maps failure class → `io_hint_write` / `io_hint_sync` / `io_hint_rename` |
| `expected_delta.py` | Reference model: does `DB::open` throw, and (always) do all keys come back |
| `generate_tests.py` | Generates `prove_recovery.cpp` |

**group_commit module** (`tests/proof/group_commit/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | GroupShape (size, multi-op, rotation), GroupFailureClass (SUCCESS, B1, B2, F, G, H), validity filter |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint, and to which threads it is armed |
| `expected_delta.py` | Reference model: did every writer throw, are the group's keys visible, is the engine degraded |
| `generate_tests.py` | Generates `prove_group_commit.cpp` |

**vacuum_compact module** (`tests/proof/vacuum_compact/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | CompactStateShape (low_fragmentation, mostly_dead, batched_file, range_tombstone_file, unsynced_overwrite, unsynced_overwrite_whole_file), VacuumCompactFailureClass (SUCCESS, VC1–VC7), validity filter |
| `fault_point_resolver.py` | Maps (state, failure class) → fault checkpoint name and which occurrence of it |
| `expected_delta.py` | Reference model: threw/file_removed/degraded outcome |
| `generate_tests.py` | Generates `prove_vacuum_compact.cpp` |

**replication module** (`tests/proof/replication/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | StateShape, OpsShape, IngestFailureClass, ManifestFailureClass, validity filter |
| `expected_delta.py` | Reference model: `IngestDelta` and `ManifestDelta` per (state, ops, failure) |
| `fault_point_resolver.py` | Maps failure class → `ScopedFaultInjector` configuration |
| `generate_tests.py` | Generates `prove_replication.cpp` |

**Shared invariants** (`tests/proof/`):

| File | Role |
|------|------|
| [`invariants.h`](../tests/proof/invariants.h) | `capture_baseline`, `assert_consistent`, `assert_delta`, `assert_recoverable`, `assert_resumable`, `assert_keys_recoverable`, `VacuumBaseline`, `capture_vacuum_baseline`, `find_vacuum_target`, `count_structural_entries`, `assert_structural_entries_preserved`, `assert_vacuum_success`, `assert_vacuum_no_change`, `assert_vacuum_recoverable`, `OwnedEntries`, `collect_changes`, `ReplicationBaseline`, `capture_replication_baseline`, `assert_replication_match`, `assert_replication_no_change`, `assert_replication_recovery`, `assert_durable_boundary`, `make_durable`, `durable_watermark`, `apply_delta`, `assert_power_loss_outcome`, `assert_hints_durable`, `assert_stream_survives_power_loss` |

### Fault injection modes

The four `ScopedFaultInjector` modes map failure classes to the four
I/O checkpoints:

- **Name-based** — targets a single checkpoint. Used for B1, F, G, H.
  `fail_on_nth_match` picks a later occurrence of the name when one
  operation passes the same checkpoint twice: VC7 fails the second
  `io_data_file_sync` of a compaction, the active file's, not the first,
  the staging copy's.
- **Count-based** — fails from checkpoint N onward, cascading. Used for C.
- **Post-write mode** — fires at `io_data_file_append_partial` with
  `short_write` or `throw_after`. Used for B2, B3.

SQLite's harness fails the 1st, 2nd, …, n-th I/O call of an operation in
turn. The injector names its sites instead, so that each failure has a
class and each class an expected delta. The counted fault sweep below
fails calls by count as well, to reach the ones no site names; see
[*Prior Art*](#prior-art).

### Invariant helpers

`tests/proof/invariants.h` provides:

- `Baseline` / `ExpectedDelta` — data types for capturing pre-transition
  state and expressing the reference model's expected outcome.
- `capture_baseline(db)` — snapshots `next_lsn` and all key-value pairs.
- `assert_consistent(db)` — validates five structural invariants:
  live_bytes matches key_dir, no dangling file references, active file
  exists, file_stats covers all files, next_lsn ahead of all sequences.
  All five are properties of the published state; none concerns a view
  the engine has lent to a reader. That is what the observer axis adds —
  see *Observers* above, and *View and span lifetimes* in `CONTRACT.md`
  for the guarantees it asserts.
- `assert_delta(before, db, expected)` — validates key membership, LSN
  advancement, structural consistency, and degraded state against the
  reference model's expected delta.
- `assert_view_stable(held, expected)` — the view handed out before the
  transition still addresses live memory (`mincore`) and still holds the
  bytes it had. Used by the `held_value` and `held_iter_span` observers.
- `assert_resumable(db)` — calls `resume()` and verifies the engine clears
  the degraded flag and passes `assert_consistent`. Inserted immediately
  after `assert_delta` for all degraded failure classes (B1, B2, B3, C, F,
  G, H), which then end with `assert_matches_recovery` below.
- `assert_recoverable(dir, before, expected)` — opens a fresh DB from
  disk and verifies the recovered state matches the expected state
  (pre-existing keys survive, added keys present, removed keys absent,
  no extra keys, structural consistency).
- `EngineFingerprint` / `fingerprint(db)` /
  `assert_matches_recovery(dir, before)` — everything a resumed engine and
  a cold-opened one must agree on: `next_seq`, the key-value map compared
  in both directions, and per-file `FileStats`, keyed by data file stem
  because recovery assigns file ids by directory order. The other
  assertions ask whether the engine produced the right state; this asks
  whether it produced the same one a cold open produces from the same
  bytes. It is what every `[prove]` cell that degrades ends with, and the
  only check the F and G cells get — what `resume()` commits from the page
  cache cannot be predicted, but the equivalence can be asserted without
  predicting it.
- `assert_keys_recoverable(dir, keys_present, keys_absent)` — lighter
  recovery check used by resume proof tests: opens a fresh DB, reads each
  expected key back with `get` and compares its value, checks the absent
  ones, then `assert_consistent`. `contains_key` is not enough — a
  truncation that cut too far leaves the key directory intact while the
  bytes behind it are gone.
- `VacuumBaseline` / `capture_vacuum_baseline(db)` — snapshots key-values,
  `next_lsn`, per-file `FileStats`, and the structural entry counts below
  before a vacuum operation.
- `count_structural_entries(db)` / `assert_structural_entries_preserved(db,
  before)` — counts `BulkBegin`, `BulkEnd` and `RangeDel` entries over
  `changes_since` and asserts none went missing. These carry no key data,
  so every other vacuum assertion is blind to a compaction that drops
  one — and a dropped marker leaves a batch that is no longer atomic on
  disk.
- `find_vacuum_target(db)` — returns the file_id of the sealed file with
  the highest fragmentation, matching `DB::vacuum()`'s selection logic.
- `assert_vacuum_success(db, before, vacuumed_file_id)` — verifies the
  vacuumed file is removed from state, all pre-vacuum keys readable with
  correct values, `next_lsn` unchanged, `total_bytes` for the active
  file matches its actual size, `assert_consistent` passes.
- `assert_vacuum_no_change(db, before, vacuumed_file_id)` — verifies the
  vacuumed file is still in state, all keys intact, `next_lsn` unchanged,
  `file_stats` unchanged from baseline, the active file's tracked size
  matches its actual on-disk size, `assert_consistent`.
- `assert_vacuum_recoverable(dir, before)` — opens a fresh DB and verifies
  all pre-vacuum keys survive recovery with correct values.
- `unreferenced_files(db, dir)` — the `.data` and `.hint` files on disk
  that the published state does not reference; every cell but VC5 checks it
  is empty after `vacuum()`, so no failed vacuum leaves its copy (#304).
- `OwnedEntries` / `collect_changes(range)` — collects transient
  `ChangeIterator` entries into owned storage. The views returned by the
  iterator are invalidated on advance; `OwnedEntries` preserves them.
- `ReplicationBaseline` / `capture_replication_baseline(db)` — snapshots
  the leader's durable state by replaying `changes_since(snap, 0)` to
  build a key-value map that respects the `durable_seq` boundary.
- `assert_replication_match(leader_bl, follower)` — verifies sequence
  continuity, key-value equivalence, no extra keys, and structural
  consistency between leader baseline and follower state (SUCCESS case).
- `assert_replication_no_change(before, follower)` — verifies follower
  state is unchanged from its own baseline (failure case).
- `assert_replication_recovery(dir, leader_bl)` — reopens follower from
  disk and verifies replication survived recovery.
- `assert_durable_boundary(range, durable_seq)` — verifies no entry in
  a `changes_since` stream has `sequence > durable_seq`.
- `make_durable(db)` — an empty sync batch, so everything a cell's setup
  wrote with `sync = false` is durable before the baseline is captured;
  requires `durable_sequence()` to reach every sequence assigned.
- `durable_watermark(db, cr)` — `durable_sequence()` at the cut, checked to
  cover a `CommitResult` that reported durable.
- `apply_delta(baseline, expected)` — the baseline with the whole transition
  applied, as the reference model describes it.
- `assert_power_loss_outcome(cut, expectation)` — recovers the copy a
  power cut left and checks it holds the durable baseline alone or with the
  whole transition, the transition when the watermark or the recovered
  `durable_sequence()` covers it, and `assert_consistent`.
- `assert_hints_durable(model)` — no hint written in the cell indexed a page
  the device did not hold (`PageCacheModel::hint_written`).
- `assert_stream_survives_power_loss(leader_cut, streamed)` — the leader's
  cut copy holds every entry `changes_since` streamed, and its
  `durable_sequence()` reaches the stream's.

12 test cases (`[invariants]` tag) in `tests/invariants_test.cpp`
smoke-test the helpers themselves.

### Test coverage

All ten failure classes for `apply_batch` (SUCCESS, NOSYNC, A, B1, B2, B3,
C, F, G, H) are covered by the 2082 `[prove]` tests. Each class is exercised
across all valid (StateShape, PlanShape) combinations, with and without
back-end, and — for every class that can disturb a lent view — against
each of the five observers.

All three resume failure classes (R1–R3) plus DOUBLE and CASCADE across
all twelve degrade shapes (H, C, F, G, B2, B3, F_RANGE and F_BATCH, plus
H and C again through mmap and the buffer pool) are covered by the 59
`[prove_resume]` tests. R1 is correctly excluded for the degrade_H,
degrade_F, degrade_G, degrade_F_range and degrade_F_batch shapes (no
orphaned bytes to truncate — fault point unreachable), and R2/CASCADE for
degrade_H (file already sealed).

All eleven vacuum_compact classes (SUCCESS, VC1–VC10) across all ten state
shapes, less the combinations a shape cannot reach, are covered by the 94
`[prove_vacuum_compact]` tests.

All seven ingest failure classes across 11 state shapes and 5 ops shapes
are covered by the 178 `[prove_repl]` tests. All three manifest failure
classes across 11 state shapes are covered by the 33 `[prove_manifest]`
tests. Four elimination rules reduce the full matrix to 211 valid tests.

Total generated proof tests: **2277**.

Two hand-written tests remain in `bytecask_test.cpp` for mechanism
smoke testing not covered by the proof matrix:

- `mid-batch append failure rotates file and discards partial batch`
  (`[fault_inject]`) — tests `apply_batch` with a `WritePlan`
  recovery by reopening the DB and verifying orphaned batch discard.
- `reads work on a degraded DB` (`[degraded]`) — tests the full read
  API surface (`get`, `contains_key`, `snapshot`, `iter_from`,
  `keys_from`) on a degraded DB instance, then calls `resume()` to
  verify in-process recovery.
- `resume() recovers from degraded state` (`[degraded][resume]`) — injects
  `io_rotate_file_creation` to trigger class T4 (post-write rotation fail),
  verifies `DbDegraded` is thrown, calls `resume()`, and confirms writes
  succeed afterward.
- `resume() with live snapshot on degraded DB` (`[degraded][resume]`) —
  takes a snapshot while degraded, calls `resume()`, verifies the snapshot
  remains readable (pinned files not deleted) and post-resume writes succeed.

### MC/DC coverage

Branch coverage says each decision went both ways. MC/DC (modified
condition/decision coverage) says each condition in it was shown to change
the outcome on its own, with the others held fixed. It finds conditions no
test isolates, which are either a missing test or a condition that cannot
decide anything. Clang records it with `-fcoverage-mcdc`, for every decision
of two or more conditions; a single condition is branch coverage's.

**The gate.** `scripts/run_coverage.sh` builds the coverage binaries with
`-fcoverage-mcdc`, and `scripts/mcdc_report.py` fails the `coverage` CI job
unless every condition in `data_file.cppm`, `hint_file.cppm` and
`bytecask.cppm` — the write path, recovery and vacuum — is covered or
exempt. Elsewhere MC/DC is reported, not gated; the key directory modules
are #357.

**Merged per build.** The engine suite runs on all three key directories, and
each build compiles paths the others do not. `llvm-profdata` cannot merge
them: a function whose MC/DC bitmap differs between builds (`store_state`'s
debug walk differs per key directory) keeps one build's counters and drops
the rest. So each build is exported on its own and `mcdc_report.py` merges
the exports: a condition is covered if some build, or some template
instantiation, shows it independent.

**Exemptions.** A condition that cannot go the other way is marked at its
site, on the decision's first line or in the comment block directly above it:

```cpp
// mcdc-exempt(C3): close() takes the write barrier, which waits for the
// flush this waiter's sequence is in, ...
return s->next_seq > sequence || s->degraded || s->closed;
```

`mcdc-exempt:` exempts every condition of the decision; `mcdc-exempt(C3):`
only the third, counted as `llvm-cov show -show-mcdc` numbers them, so the
other two still need their tests. Every report lists the exemptions with
their reasons. A marker on a condition that is covered, or on no decision,
fails the gate, so a marker cannot outlive the miss it excused.
`MCDC_MAX_EXEMPT` in `run_coverage.sh` caps how many there are; raising it is
a line a reviewer sees, so the gate cannot be held by exempting what a test
should cover.

SQLite's alternative, `ALWAYS(x)` / `NEVER(x)` macros that a coverage build
compiles to constants, was not taken: the coverage build would stop checking
the conditions it exempts, so the build that runs the tests would differ from
the one that ships, and every module unit would need the macro header. A
comment changes no build. Its cost, a filter to maintain, is
`mcdc_report.py`.

**What Clang does not count.**

- A decision that is the condition of a `?:` whose result has class type
  records no test vectors, however often it runs: `at && kd_put_at(...) ?
  std::optional{...} : kd_put(...)` ran 293k times and reported 0%. Name
  the decision as a `bool` first. The report marks such a decision
  `[no vectors]` (#358).
- A decision that nests a boolean operator inside an operand
  (`a != b && !(c && d)`) is not instrumented at all, with the warning
  "unsupported MC/DC boolean expression". None is in the engine today.
- A process that dies by `abort()` writes no profile, so the code a death
  test reaches counts as unexecuted (`panic_on_reused_path`).

**Triage record (#353).** Every decision in the gated files that no test
isolated when MC/DC was first measured, and its answer: a test, a
simplification (the condition could not decide anything), or an exemption.

| Site | Answer |
|---|---|
| `WritableMmapDataFile` / `WritablePosixFile` ctor: `exclusive && errno == EEXIST` | Simplified: only `O_EXCL` reports `EEXIST`. |
| `ReadOnlyMmapDataFile::openForRead`: `fstat != 0 \|\| size == 0` | Tests: `fstat` failed by injection; an empty file. |
| `createDataFileForWrite`: `Mmap && capacity > 0` | Test: a zero-capacity mmap file is written with `pread`. |
| `renameDataFileExclusive`: `errno != EINVAL && errno != ENOSYS` | Tests: `renameat2` failed with `EINVAL`, `ENOSYS` (fallback) and `EIO` (throws). `ScopedSyscallFaults` takes the errno to report. |
| `sync_directory`: `rc != 0 && err != EINVAL` | Tests: `fsync` failed with `EINVAL` (done) and `EIO` (throws). |
| `DataFileIterator::buffered`: `offset < buf_start_ \|\| …` | Simplified: the sweep only moves forward (asserted). |
| `HintFile` open: `head < magic \|\| !equal(magic)` | Test: a file too short for the magic. |
| `HintFile` open: `version != 1 \|\| codec != zstd` | Test: an unknown codec. |
| `HintFile` open: `ZSTD_isError(packed) \|\| packed > cap` | Simplified: the window holds at most the cap. |
| `HintFile` open and scan: `size == UNKNOWN \|\| size == ERROR \|\| size > cap` | Simplified to `valid_frame_size(size)`: both sentinels exceed the cap (`static_assert`). |
| `HintFile` scan: `ZSTD_isError(got) \|\| got != size` | Exempt (C2): zstd checks the size a frame declares. |
| `store_state`: `degraded && old && !old->degraded` | Simplified: `state_` is never null. |
| `apply_writes`: `at && kd_put_at(...)`, `at && kd_erase_at(...)` | Covered all along; Clang recorded no vectors (see above). Named the `bool`. |
| `apply_writes`, `apply_ingest`, `apply_resume`, vacuum scan: `min == 0 \|\| seq < min` | Simplified: sequences ascend within a file, so the first is the minimum. |
| `apply_vacuum`: `cur && cur->sequence() == m.sequence` | Test: a key overwritten and one deleted while vacuum copies (`test_before_vacuum_commit_`). |
| vacuum scan: `existing && file && offset && sequence` | Simplified: a location names one record, so the sequence compare (a record read on the blind tree) goes. |
| `set_mode`: `Follower && current Leader && !degraded && durable < last` | `current Leader` removed: a follower has nothing unsynced (`ingest` syncs). Test: a degraded leader steps down without a sync. |
| `wait_published`: `published \|\| degraded \|\| closed` | Test: a conflict waiter released by a degrade (`test_in_sequence_wait_`). Exempt (C3): `close()` drains the flush first. |
| `durable_sequence`: `min == 0 \|\| reached \|\| timeout <= 0`, and its wait `reached \|\| closed` | Tests: a zero timeout; a waiter woken by `close()`. |
| `store_state`: `closed && !old->closed` | Simplified: nothing publishes after `close()`. |
| `store_state` debug walk: `fs != nullptr && past extent` (both walks) | A key whose file has no stats is now a violation too. Tests via `test_publish`. |
| `store_state` debug walk: `max_seq > 0 && next_seq <= max_seq` | Simplified: `next_seq` starts at 1. |
| `validate_state_consistency`: `fs != nullptr && past extent`, `min > 0 && min > max` | Simplified: the checks before them rule out the first operand. |
| `is_compaction_of`: same sequence, type, key and value | Test: two writes under one sequence differing in key, and in type. |
| `find_sequence_overlap`: `widest == nullptr \|\| r.max > widest->max` | Simplified: sorted and disjoint so far, the previous range reaches furthest. |
| `recovery_prepare_files`: `.tmp && (.hint \|\| .data)` | Test: staged files removed, other `.tmp` files kept. |
| `recovery_prepare_files`: `end && *end < size` | Simplified: a hint-less file always gets an end. |
| `RecoveryPhaseLog`: `e && *e == '1'` | Test: `BC_RECOVERY_PHASES` set to `1` and `0`. |
| `recovery_merge_results` (radix): `a.skipped \|\| b.skipped` | Moved to the caller: which part was `a` depended on which worker finished first. Since removed with the radix tree. |
| `recovery_build_sorted`, `recovery_load_streams`: markers and a `RangeDel` in the sorted run | Test: a crafted hint with markers, a `Delete` and a range tombstone inside the run. |
| `recovery_build_sorted`: `have_prev && key <= prev` | Removed: keys leave a heap of ascending cursors ascending. |
| `recovery_load_ranged`: `on_key && older && other file` | Simplified: each file offers one entry per key, so an older Put is from another file. |
| `ChangeIterator`: `seq > from && seq <= durable` | Test: an unsynced write is not shipped. |
| `ChangeIterator`: `needs_advance_ && entry_iter_` | Simplified: set only after a read from `entry_iter_`. |
| `ChangeIterator`: `!impl_ \|\| !has_more()` | Test: a default iterator is at its end. |

Functions in the same files that no build called were tested (the iterators'
post-increment, `ChangeIterator`'s move assignment) or deleted:
`DataFile::read_entry` and `read_entry_unverified`, which nothing but tests
called since reads went through `lend_record`, with their helpers, and four
unused `TransientEngineState` accessors and `HintFile::path`. What remains
is `panic_on_reused_path`, reached only by the death tests above.

### Counted fault sweep

The proof matrix fails checkpoints by name. A cell exists because someone
chose a checkpoint for a failure class, so a call no class names is never
failed, and a call added later is untested under failure until a cell is
written for it. The sweep closes that gap the way SQLite's I/O error tests
do: it fails the N-th I/O call of an operation, for N = 1, 2, …, until the
operation completes without reaching the fault (#317).

```
for each operation:
 for each I/O back-end (pread, buffer pool, mmap):
  for each pass (before, after, short, cascade):
    for N = 1, 2, …:
      build the starting state
      run the operation with the N-th counted call failing
      assert the generic invariants
      if the fault was never reached: next pass
```

It runs in `bytecask_tests` as `[fault_sweep]`
([`tests/fault_sweep_test.cpp`](../tests/fault_sweep_test.cpp)), on every
PR: about 5,300 failed calls, 1,850 of them on the default back-end, in
30 s on an unsanitized release build. A
failure names the call: `vacuum, fault before, N = 7:
ftruncate(…/x.data.tmp)`. `BYTECASK_SWEEP_TRACE=1` prints every call a
sweep failed.

**Counting below the engine.** The engine has no single I/O layer to count
in, and counting `FAULT_INJECTION` checkpoints would leave a call without
one unreached. So `bytecask_tests` is linked with `-Wl,--wrap=<call>` for
each I/O call it references, and the interposers in
[`tests/syscall_faults.cpp`](../tests/syscall_faults.cpp) count and fail
them. A call is counted only while a sweep is armed, and only on a file
under the DB directory, matched by its path, by what its descriptor names
(`/proc/self/fd`), or for an `*at` call by both. An interposer calls
through to the real symbol, which under a sanitizer is the sanitizer's
interceptor.

`--wrap` rewrites the references in the objects that are part of the link,
and nothing in a shared library. So `bytecask_tests` links the C++
standard library statically (`-static-libstdc++`), and `std::filesystem`'s
calls into libc are rewritten like the engine's: the hint's rename,
vacuum's removal of the file it compacted, the removal of stale `.tmp`
files at open, `file_size`, `exists`, the directory scan at open. Using one
filesystem operation pulls in libstdc++'s whole object for them, so the
binary references more than the engine calls: `openat`, `read`, `writev`,
`sendfile`, `copy_file_range`, `truncate`, `lstat`, `rename`, `unlinkat`,
`remove`, `mkdir`, `symlink`, `utimensat`, `fchmod`, `fchmodat`,
`realpath`, `opendir`, `readdir`, `fopen` and `freopen` have interposers
beside the engine's own `open`, `pread`, `pwrite`, `pwritev`, `write`,
`fdatasync`, `fsync`, `ftruncate`, `fstat`, `stat`, `renameat2`, `link`,
`unlink` and `mmap`. Some come only from the MemorySanitizer build's libc++
(`opendir`, `copy_file_range`). `remove`,
`realpath` and `fopen` call `unlink`, `lstat` and `open` inside libc,
through aliases `--wrap` cannot see, so they are wrapped themselves.

`xmake.lua` also passes `--wrap` for the other spellings of those calls
(`pread64`, `openat64`, `renameat`, `mkdirat`, glibc's old `__xstat`, the
`_FORTIFY_SOURCE` variants, …) and for calls nothing references yet
(`fallocate`, `splice`, …), without defining an interposer for
them, so a reference to one fails the link. With the standard library in
the link the guard is complete: an I/O call cannot join the binary
uncounted, from the engine or from a `std::filesystem` operation it starts
using, and a toolchain that spells a call differently is noticed. An I/O
call here opens, reads, writes, syncs, sizes, maps, lists or changes a file
or a directory entry; the rest (`close`, `lseek`, `flock`,
`posix_fadvise`, `readlink` of `/proc/self/fd`, `statvfs`, stdio past the
open, …) are listed in `xmake.lua` as left out. The first two tests in the
file check the other direction: that a put's append and sync are in fact
counted, and that `std::filesystem`'s `stat`, `openat`, `mkdir`, `rename`
and `remove` are.

**Passes.** Each pass counts only the calls it can fail, so N indexes a
different sequence in each.

| Pass | The N-th call | Counts |
|---|---|---|
| before | is not made, and reports `EIO` | every wrapped call |
| after | is made, then reports `EIO`: the bytes, the cut or the rename landed | the writes, the syncs, and the calls that change a file's size or a directory entry: `ftruncate`, `truncate`, `renameat2`, `rename`, `link`, `unlink`, `unlinkat`, `remove`, `mkdir`, … |
| short | transfers half of what was asked | `pread`, `read`, `pwrite`, `pwritev`, `write`, `writev`, `sendfile`, `copy_file_range` |
| cascade | and every counted call after it fail as in *before* | every wrapped call |

An `fdatasync` failed *before* leaves its pages clean and unwritten in the
`PageCacheModel`, as the kernel does; one failed *after* wrote them.

Outside the sweep, a test can arm several `SyscallFaultRule`s at once. Each
names a call (`"renameat2"`, `"unlink"`) or none, and counts and fails its
own sequence with its own errno; the first rule that fails a call wins.
That reaches a path an errno selects and fails a call inside it:
`renameDataFileExclusive`'s fallback, in `[data_file]` and in the
`prove_vacuum_compact` VC9 and VC10 cells. The report lists every call
failed, in order.

**Back-ends.** Each operation runs once on each `IoBackend`, since they
make different calls on the same operation. The buffer pool opens a sealed
file a second time `O_DIRECT`, proves that descriptor with one aligned
`pread` and fills through the buffered one if either fails, and fills
frames with its own `pread`, retried buffered when the direct one fails. The mmap back-end
maps the active file and each sealed one. The pool must hold
2 × `max_file_bytes`, so on it an operation on the default file size runs
with `max_file_bytes = 64 KiB` and a 1 MiB pool; none comes near either.

**Operations.** The numbers are the calls of the *before* pass on each
back-end: pread, buffer pool, mmap. The two others make fewer on a write
because a record of the active file is read from memory, the pool's frames
or the mapping, not with `pread`; they make more or fewer on an open by
their own opens, probes, fills and maps.

| Operation | Starting state | Calls |
|---|---|---:|
| `put`, overwriting | one file: six keys, a delete, a batch | 3, 2, 2 |
| `del` | the same | 4, 2, 2 |
| `del_range` | the same | 8, 2, 2 |
| `apply_batch`: puts, a delete, a range delete | the same | 13, 4, 4 |
| a put that fills the active file | the same, `max_file_bytes = 512` | 25, 26, 26 |
| `vacuum` | a sealed file with a dead entry | 29, 28, 27 |
| `create_manifest` | one file | 21, 23, 23 |
| `ingest` of a put and a batch | a follower holding the first slice | 15, 5, 5 |
| `resume()` | degraded by a failed commit sync: the entry whole, unsynced | 46, 29, 29 |
| `resume()` | degraded by a short append: the entry torn | 44, 29, 29 |
| `close()` | an unsynced batch behind it | 13, 13, 13 |
| `DB::open`, serial | three sealed files and their hints, after a clean close | 107, 131, 98 |
| `DB::open`, parallel | the same | 119, 143, 110 |
| `DB::open`, serial | the same, as a killed process leaves it: the newest file hint-less | 116, 142, 107 |
| `DB::open`, parallel | the same | 128, 154, 119 |

The `std::filesystem` calls account for the rise over the counts before
the static link (#319): the hint's `exists` check and rename on every
operation that writes a hint, vacuum's two removes, `resume()`'s size
checks, and at open the directory scans, one `readdir` per entry, which
are most of the 41 more calls an open makes.

**Invariants.** They need no expected delta per failure class, which is
what makes an operation cheap to add. After a failure at any N:

- the operation threw, or returned having done its work;
- the state is the baseline or the baseline plus the whole transition,
  and the transition if the operation returned (`assert_consistent`);
- a degraded engine resumes once the fault is lifted (`assert_resumable`),
  and `resume()` keeps or completes the transition;
- for `ingest`, whose slice is two atomic units, `resume()` or a power cut
  may also leave the first unit without the second, and delivering the
  slice again completes it (`CONTRACT.md`, `ingest`);
- for `create_manifest`, every data file a returned manifest lists
  exists, and the listed files, with whichever hints exist, open to the
  manifest's state; a hint the worker failed to write may be missing
  (#349). This is checked once the fault is disarmed, since the check's own
  `stat` calls would otherwise be counted;
- a write after the fault lands;
- a close and reopen recovers that state, serial and parallel, with the
  same file stats;
- the copy a power cut leaves, taken right after the fault and again at
  the end, obeys the watermark rule (`assert_power_loss_outcome`), and no
  hint indexed bytes the device did not hold (`assert_hints_durable`).

`close()` and `DB::open` have their own loops, since there is no open
engine on one side of them. A `close()` that returns has made the unsynced
batch durable; it is closed either way; and a reopen in the same process
recovers the batch. A failed open loses nothing: the next open, and the
device after the failed one, recover every key.

**The hint worker.** The counter is process-wide, because the flush leader
and the hint worker are not always the test's thread. The sweep waits for
the worker to go idle before it arms and again before it disarms, so an
operation's calls are the same set on every run. Under parallel recovery,
and wherever the worker runs beside the caller, their order can move, and
N with it; each N still fails one call and the invariants do not depend on
which.

**What it found.** `ReadOnlyPosixDataFile::openForRead` and
`ReadOnlyBufferPoolDataFile::openForRead` took a failed `fstat` as an empty
file. A rotation, a vacuum or a `create_manifest` then sealed a file none of
whose records could be read, and the hint written from it indexed nothing,
so the next open lost its keys. No checkpoint named the call. Both now
throw (`sealed_file_size`), and the `sealed_fstat_failure_reads_empty`
mutation reverts it.

Counting `std::filesystem`'s calls (#319) found no failure the invariants
reject. It did show that `create_manifest` returns a manifest naming a
hint whose write failed, against `CONTRACT.md`'s *File list accuracy*. The
same was already true of a failed `write` or `fdatasync` of the hint, and
no invariant checks the manifest's file list (#349). The
`rewrite_size_failure_reads_empty` mutation shows that a newly counted
site is reached: read as an empty file, a failed `file_size` in
`rewrite_durably` lets `resume()` build a hint over pages the device does
not hold, and the sweep rejects it.

**Limits.**

- *Calls glibc makes inside itself are not counted.* `--wrap` sees the
  public function, not what it calls through an internal alias, so a
  stream's reads and writes after `fopen` are not counted. The engine uses
  no stdio.
- *Linux only.* Apple's linker has no `--wrap`, so on macOS `bytecask_tests`
  is built without `tests/syscall_faults.cpp` and `tests/fault_sweep_test.cpp`.
- *The link guard is off on glibc older than 2.33.* There `<sys/stat.h>`
  turns `stat`, `fstat` and `lstat` into `__xstat`, `__fxstat` and
  `__lxstat`, which have no interposer: a newer glibc keeps them only as
  compatibility symbols, so an interposer calling through to them would not
  link where CI builds. The manylinux_2_28 wheel build, on glibc 2.28,
  configures `--fault_sweep_link_guard=n`, and there those calls go
  uncounted. Every other build keeps the guard, MemorySanitizer's included:
  its libc++ was static before the rest of the binary's standard library
  was, and it no longer needs the exception it had (#327).
- *`mmap` reads cannot be failed this way.* A failed mapped read is a
  `SIGBUS`, not a return value. The `mmap` call itself is counted, and on
  the mmap back-end failed.
- *The buffer pool's buffered retry is failed only by the cascade.* When
  the N-th call is a direct fill, the buffered `pread` that retries it is
  the (N+1)-th, which no single-fault run reaches with the N-th
  succeeding. The cascade fails both, and the read throws.
- *The oracle is weaker than the matrix's.* "Baseline or the whole
  transition" does not say which of the two a given fault must produce. The
  proof cells keep that job, and the named checkpoints stay for them.
- *One fault, one small operation, one thread.* Volume, concurrency, real
  power loss, resource limits and faults across process lives stay with the
  chaos rig.
- `close`, `flock`, `posix_fadvise`, `madvise` and `munmap` are not
  wrapped: the engine acts on none of their results but `flock`'s, which
  the lock tests cover.

### Counted allocation-failure sweep

The I/O sweep fails calls into the kernel; nothing failed an allocation.
SQLite fails the n-th allocation of each operation the way it fails the
n-th I/O call [8], and this sweep does the same (#364): for each operation
it fails allocation 1, 2, …, n until the operation completes without
reaching the fault ([`tests/alloc_sweep_test.cpp`](../tests/alloc_sweep_test.cpp)).

**Counting.** [`tests/alloc_faults.cpp`](../tests/alloc_faults.cpp)
replaces every form of the global `operator new` in `bytecask_tests` with
one that counts while armed and fails the n-th: `std::bad_alloc`, or
`nullptr` for the `nothrow` forms. The count is process-wide, since the
committer thread and the hint worker allocate on an operation's behalf. A
thread holding `SuspendSyscallFaults` is neither counted nor failed, so the
`PageCacheModel`'s bookkeeping and the test code that builds an
operation's inputs are left out, as they are of the I/O sweep. The
operations are the I/O sweep's, shared through
[`tests/sweep_operations.h`](../tests/sweep_operations.h). `malloc` is not
counted: zstd allocates with it and reports a failure as an error code,
which the hint paths already handle.

**Passes.** *one* fails only the n-th allocation, a transient spike;
*every one from N on* fails it and every allocation after it until the
operation ends, sustained pressure, which reaches the handlers that
allocate while they handle a failure.

**Each run in a child.** An allocation failure could end in
`std::terminate`, so each run is a forked child, and a termination is an
outcome to record rather than the end of the test binary. The child runs
the operation, then the checks, and reports one line on a pipe:

| Outcome | Meaning |
|---|---|
| completed | the fault was not reached |
| returned | the operation returned, its transition whole |
| threw, unchanged | it threw and published nothing |
| threw, applied | it threw after publishing its whole transition |
| degraded | the engine refused writes; `resume()`, with memory back, recovered it |
| terminated | the process died |
| hung | no result after 20 s: a waiter whose wake-up the failure lost |

After the fault, with memory back, the child checks that the state is the
baseline or the whole transition, that it passes `consistency_errors`, that
a degraded engine resumes, that a write afterwards lands, and that a clean
reopen recovers the same state serially and in parallel. A terminated
run's directory is reopened by the parent as after a SIGKILL and must hold
the baseline or the whole transition. A failed check and a hang fail the
test. A termination is counted and reported, and fails the test only with
`BYTECASK_ALLOC_SWEEP_STRICT=1`: whether the engine may end the process
for lack of memory is not settled, and the sweep is how it will be.

| Operation | Runs per pass | Outcomes, one / every one from N on |
|---|---:|---|
| `put` | 17 | 13 threw unchanged, 3 degraded, 1 completed (both) |
| `put` while a snapshot taken before it is held, and released under the fault | 17 | 13 threw unchanged, 3 degraded, 1 completed (both) |
| `del` | 14 | 10 threw unchanged, 3 degraded, 1 completed (both) |
| `del_range` | 25 | 21 threw unchanged, 3 degraded, 1 completed (both) |
| `apply_batch` | 27 | 23 threw unchanged, 3 degraded, 1 completed (both) |
| a put that fills the active file | 119 | 72 / 56 returned, 34 / 50 degraded, 12 threw unchanged |
| `vacuum` | 159 | 157 / 158 threw unchanged, 1 / 0 returned |
| `ingest` | 27 | 23 threw unchanged, 3 degraded (both) |
| `resume()`, unsynced entry | 162 | 90 / 113 degraded, 71 / 48 returned |
| `resume()`, torn entry | 156 | 86 / 111 degraded, 69 / 44 returned |

No run terminates or hangs. The whole sweep takes about 25 s. It runs on
the default I/O back-end; `BYTECASK_ALLOC_SWEEP_STRIDE`,
`BYTECASK_ALLOC_SWEEP_N` and `BYTECASK_ALLOC_SWEEP_PASS` narrow it, and
`BYTECASK_ALLOC_SWEEP_TRACE=1` prints each run's outcome and the stack of
each allocation it failed.

**What it found.** Its first run, on every operation:

- *A writer waited forever.* `flush_once` took the flush role and only
  `finish_flush` gave it back; a throw from `flush_pending` — here the
  allocation of the published copy — left it taken, and every later
  `commit_wait` blocked. `quiesce` had the same gap.
- *A write the caller was told failed came back.* An allocation after the
  append, of the new head, threw past `execute_group`'s handlers: the bytes
  were in the file, the engine stayed writable, and the next write reused
  the failed one's sequences after them. Reads showed the old value; a
  reopen replayed the failed write.
- *Freeing memory needed memory.* A transient dropped without publishing
  freed its nodes through `free_node_subtree_if`, in a `noexcept`
  destructor, with a heap-allocated work list: under sustained pressure,
  `std::terminate`. Almost every termination came from here.
- *A failed publication broke the key directory.* `VersionChain::publish`
  linked the base to its successor before the allocations that register
  it, so after a failure every later write threw "a version that already
  has a successor".
- *A half-rotated transient was published.* The rotation's handler for the
  I/O failure of creating the next file also caught an allocation that
  failed while the files were installed, and published a state with a file
  and no `file_stats` for it, which `resume()` refused.
- *Releasing a version needed memory* (#390). `VersionChain::unpin`, in the
  trees' `noexcept` destructors, queued what it freed and re-parked through
  vectors. No swept operation reached it, but releasing the newer of two
  chained versions under the fault did, and every way a version can die
  terminated with every allocation failing. Each allocation left on the
  release path now has a fallback that needs none (`bytecask_design.md`,
  *A failure after the append that no handler degrades for*). A parcel held
  whole because it could not be split is split when its holder dies, into
  nodes still reached and nodes reached by nothing; `alloc sweep: a parcel
  held whole is split when its holder dies` builds that case.

The fixes are in `bytecask_design.md`, *A failure after the append that no
handler degrades for*. Each has a deterministic test, driven by a test hook
rather than a counted allocation so that it runs in-process — where
coverage sees it; a forked child's profile is not written — and on the
sanitizer and WASM builds: `[refuse_writes]` in the same file, and
`alloc sweep: a transient tree is freed when nothing can be allocated`,
`alloc sweep: a failed publication leaves the base derivable` and
`alloc sweep: a version is released when nothing can be allocated`. Each has a
mutation in `tests/durability_mutations/` that reverts it.

**Limits.**

- *Not under ThreadSanitizer, MemorySanitizer or WASM.* Their runtimes
  define `operator new` themselves (the link fails), and WASM has no
  `fork`. The hook-driven tests run there.
- *`malloc` is not counted*, nor anything that allocates with it.
- *One back-end.* The I/O back-ends differ in their calls far more than in
  their allocations.

### ThreadSanitizer (TSan)

The full test suite (428 test cases, 1.5 M+ assertions) runs clean under
Clang ThreadSanitizer with `halt_on_error=0 history_size=4`. TSan
instruments every memory access and synchronization operation at compile
time, detecting data races that don't manifest as visible bugs on the
current hardware but will surface under different CPUs, kernel versions,
or load patterns.

Concurrency code paths exercised:

| Code path | Mechanism |
|-----------|-----------|
| State publication | `atomic<shared_ptr<EngineState>>` store/load |
| State timestamp | `atomic<int64_t>` relaxed load/store |
| Degraded flag | `atomic<bool>` acquire/release |
| Write serialization | `unique_ptr<mutex>` |
| Group commit | mutex + condition_variable |
| Background worker | mutex + condition_variable |
| Edit tag counter | `atomic<uint64_t>` relaxed fetch_add |

Run: `scripts/run_sanitizer.sh thread` (or `address` for ASan, `memory` for MSan).

### MemorySanitizer (MSan)

The full test suite runs clean under Clang MemorySanitizer with
`-fsanitize-memory-track-origins=2` (full allocation-site origin tracking).
The MSan build is compiled at `-O1` with `-fno-omit-frame-pointer
-fno-optimize-sibling-calls`, over the debug mode's `-O0`: MSan is meant to
run optimized, and at `-O0` every local goes through instrumented memory. The
two flags keep report stacks whole. Origin tracking stays at level 2, because
its cost is small next to `-O0`'s and it is what points a report at the store
that left the value uninitialized.
MSan instruments every load at compile time and reports a use of any value
that hasn't been written, catching bugs ASan and TSan don't: reads of
uninitialized stack or heap memory that happen to produce a plausible-looking
value on the current allocator/compiler/optimization level but are UB and can
flip to garbage under a different build.

**Why a custom libc++ is required**: MSan needs initialization state tracked
through every call, including into the C++ standard library. The system's
`libcxx-devel` package (used as-is by ASan and TSan above) is not built with
`-fsanitize=memory`, and linking it into an MSan binary causes constant false
positives — this is standard, documented MSan behavior, not specific to this
project. `scripts/build_msan_libcxx.sh` builds an instrumented
`libc++`/`libc++abi` from the `llvm-project` release branch matching the
installed Clang's major version (sparse, shallow checkout of just
`libcxx`/`libcxxabi`/`runtimes`/`libc` — the last only for header-only helpers
`libcxx`'s `charconv` implementation pulls in, `libc` itself is never built)
and installs it to `.msan-libcxx/`. CI caches
this build (`actions/cache`) since it takes several minutes; the key covers
both the script's contents and the installed Clang's major version, so a
Fedora LLVM bump rebuilds rather than restoring a libc++ built from the
previous release branch. The script is idempotent — CI reruns and local
reruns alike skip the build if the prefix is already populated.

**Catch2 is compiled from source for this build only**: the prebuilt `catch2`
xrepo package is compiled against the system's default libstdc++, and linking
its libstdc++-mangled symbols into a `-stdlib=libc++` binary fails at link
time (or worse, silently mismatches ABI, for symbols that happen to resolve).
`third_party/catch2_amalgamated/` vendors Catch2's official amalgamated
distribution (a single `.hpp`/`.cpp` pair); `bytecask_tests` compiles it
directly as ordinary translation units — under the same `-stdlib=libc++
-fsanitize=memory` flags as everything else — only when configured with
`--sanitizer=memory`. Every other build configuration is unaffected and keeps
using the normal `catch2` package.

**Known reduced-sensitivity area**: `crc32c` (the only other linked
dependency) is *not* rebuilt with MSan. Its public API takes only pointers
and primitive integers — no standard-library types cross the boundary — so
this doesn't cause false positives (LLVM's MSan treats a call into
uninstrumented code as producing fully-initialized output by design); it just
means bugs inside `crc32c` itself, if any, wouldn't be caught by this MSan
run.

Run: `scripts/run_sanitizer.sh memory`. Target scope matches the ASan/TSan
jobs above: `bytecask_tests` only. Trigger scope does not — origin tracking makes the
MSan test step the slowest of the three and by far the least predictable
(2m48s and 11m56s on two runs of the same commit, on an early, much smaller
suite; per-job runners vary by ~1.8x and the seeded `[model]` workloads
account for much of the rest), so it is excluded from the `pull_request`
matrix and runs nightly and on `workflow_dispatch`. The job times out after
two hours rather than GitHub's six, and the test run prints `--durations`,
so a stalled run fails early and its log names the last test that finished.
The six-hour MSan runs before that limit existed also built at `-O0`; at
`-O1` the MSan legs finish in 20–65 minutes.

Every sanitizer job builds in debug mode, where each published state walks
the whole key directory to check its invariants (`store_state`). A test that
loads N keys with N single puts is therefore O(N²) there: loaded that way,
40,000 keys cost one `count_keys` test 42–49 minutes on the radix key
directory under ASan and TSan, and pushed those jobs past their 60-minute
limit. A test that needs a large key set loads it in one `apply_batch`.

### Sanitizer matrix across key directories

The sanitizer jobs live in `.github/workflows/sanitizers.yml`, which
`ci.yml` calls. A pull request, and the push to `main` that merges it, run
ASan, TSan and UBSan on the default blind-leaf key directory. The nightly
schedule runs the full matrix: `{blind, btree}` (`BYTECASK_KEYDIR`) ×
`{address, thread, memory, undefined}`, eight jobs. The keyed tree shares
its inner nodes and `BuildSession` with the default, so bugs in shared code already surface in the PR run; the full matrix catches what is
specific to one tree. UBSan is built with `-fno-sanitize-recover=undefined`,
so any report fails the job; its leg on the blind tree also runs
`btree_tests`, which does not depend on `BYTECASK_KEYDIR`. The three MSan
jobs share one cache entry for the instrumented libc++.

### Sanitizers on the Python binding

`python-nightly.yml` builds the Python extension with ASan and with UBSan,
one job each, and runs the binding's suite (`bytecaskdb-python/tests/` and
`tests/proof/test_independence.py`) against it every night (#291). The
interpreter is not instrumented, so each job preloads the matching runtime
into `python3`. The ASan job sets `PYTHONMALLOC=malloc`, so Python's own
objects are allocated where ASan sees them, and turns leak detection off,
since the interpreter leaves allocations live at exit by design. pytest runs
with `--capture=sys`: its default capture of file descriptor 2 loses the
report when the sanitizer ends the process. An out-of-bounds read planted
in a binding function failed the ASan job at its line, and a planted signed
overflow failed the UBSan job.

What this guards is the binding's own memory: wrappers freed in whatever
order the collector picks, and iterators that outlive their snapshot or
`DB`. Removing the `keep_alive` from `Snapshot.iter_from` was not reported,
and is not a bug: the engine's iterator owns what it reads, so `keep_alive`
there is a second guard, not the only one.

### Fuzz testing (libFuzzer)

Three harnesses feed arbitrary bytes to the code that parses what recovery
and the sweeps read from disk, where damage from bad hardware or a crash
reaches the engine. Each accepts `std::runtime_error` as the rejection of
bad input; any other exception, an abort or a sanitizer report is a finding.

The two data-file harnesses read a control byte ahead of the input. Its bit 0
recomputes the CRC of each entry before parsing. Without it a mutated entry
almost never carries a valid CRC-32C, and the run explores the CRC
rejection and little behind it; with it clear, that rejection stays covered.

**`fuzz_data_entry`** — `deserialize_entry(span)` on one entry: header,
size validation, CRC.

**`fuzz_hint_entry`** — the hint `deserialize_entry(span)` in a loop, as
`Scanner::next` walks a frame: length validation across consecutive
entries. Hint entries carry no CRC of their own (the file trailer covers
them), so it has no control byte.

**`fuzz_data_file_scan`** — the input as a whole data file, swept by
`DataFileIterator` (bit 1 of the control byte picks `OnDamage::Stop` over
`Throw`) and by `CommittedEntryIterator` on top of it. This is the sweep
behind hint generation at open, `resume()`, vacuum's copy and
`changes_since`. Besides surviving, it checks that:

- each entry yielded lies inside the file, directly after the one before,
  and equals `deserialize_entry` of its own bytes;
- under `Stop` the sweep never throws;
- `CommittedEntryIterator` yields the raw entries minus a batch still open
  at the end, never part of a batch, and its `committed_offset()` is the
  end of the last entry it yielded; when the sweep throws, what it yielded
  before is a prefix of that.

The file is held in memory (a `DataFile` whose point reads abort, since the
sweep never makes them), so a run is not bound by syscalls. Bits 2–3 of the
control byte set the sweep's chunk size from 16 B to 4 KiB, through a
`DataFileIterator` constructor argument that is the 1 MiB `kChunkBytes`
everywhere else. With the engine's chunk every input fits in one read, and
the refill at a chunk boundary and the growth for an entry larger than a
chunk would go unexercised.

All three build with `-fsanitize=fuzzer,address` and run nightly
(`fuzz-nightly.yml`), 30 minutes per target; PR CI does not build them, so
a harness broken by an engine change fails the next night. Seeds
(`tests/fuzz/seed/`, written by `gen_fuzz_corpus`) are committed. The
evolving corpus (`tests/fuzz/corpus/`) is gitignored locally and, in CI,
cached from one night to the next and minimised with `-merge=1`, so each
run continues from what the last one found.

Run: `scripts/run_fuzz.sh fuzz_data_file_scan 300` (5-minute run).

### Process-crash harness (SIGKILL)

The fault injector answers "what happens if *this* syscall fails". The
crash harness (`tests/crash/crash_consistency.cpp`) answers "what happens if
the process stops anywhere", including between two syscalls the injector
treats as one step and inside the background hint worker.

The binary re-executes itself as a child. The child opens the database and
runs a random single-writer workload: `put`, `del`, `del_range`, and
`apply_batch` with `ensure_present`/`ensure_absent` guards, mixed `sync`.
Most iterations also run a thread calling `vacuum()`. The child streams
every operation to the parent over a pipe: an Intent frame before the call,
and a Commit (`sequence`, `durable`) or Abort frame after it returns. It
also reports `durable_sequence()` every few operations. The backend (`Pread`, `Mmap`,
`BufferPool`), `max_file_bytes` (16 KiB–1 MiB) and the sync ratio are
drawn per iteration, so rotation, hint generation and zero-fill-ahead are
in flight when the kill lands. The parent SIGKILLs the child after a
log-uniform delay (1 ms to 1.5 s, so many kills land inside `DB::open`).
It then opens two copies of the directory and checks:

- **Prefix.** The recovered key/value set equals the model after some prefix
  of the child's operations in commit order. A batch is one operation, so
  this also checks batch atomicity. A value that was never written, or an
  older value where a newer one is required, fails it. Values are unique
  per write (`<child seed>.<ordinal>`), so a failure names the exact write.
- **Watermark.** That prefix covers every operation at or below the durable
  watermark: the highest `durable_sequence()` the child reported and every
  `CommitResult` with `durable == true`. Recovered `durable_sequence()` is
  at least the watermark.
- **Default open.** `DB::open` with `fail_recovery_on_crc_errors = true`
  succeeds on the killed directory.
- **Serial = parallel.** Serial and 4-thread recovery agree on contents,
  `file_stats`, `keydir_keys` and `durable_sequence`, the same check the
  `[model]` tests make.
- **Live history.** While the child ran, `del` returned `nullopt` exactly
  when the model had the key absent, and a guarded batch aborted exactly
  when its guard failed.

The next child opens the killed directory itself, so every iteration
after the first also starts from a crash: recovery of crash leftovers
(`.hint.tmp`, `.data.tmp`, a vacuum's compacted copy, the zero-filled
active tail) is exercised again and again. It also asserts that the child's own open
agrees with the parent's verified reopen. The directory is wiped every 25
iterations.

The kernel page cache survives SIGKILL, so this is the process-crash
contract, not power loss. Two things follow from that. No run so far has
lost a write whose call returned, `sync` or not; the harness counts
those losses but does not fail on them, because the contract promises only
`durable` writes. And the two reverts the harness was first expected to
catch cannot be caught by it: removing the sync before degrading (B1–B3)
changes nothing when no write fails, and writing hints in place, without
the temp-then-rename, leaves a torn hint that fails its CRC and is rebuilt
from its data file. Both need power loss. The chaos rig below models it
out of process, on a FUSE filesystem; the generators' *Power-loss axis* cuts
it in process, over `PageCacheModel`, applying this harness's watermark rule
to a directory whose unsynced pages are really gone. What that model leaves
out — `fdatasync` as a barrier, the hint worker's timing, directory
operations (#266) — is the chaos rig's.

What it did catch: #166, `vacuum()` dropping a file with
`live_bytes == 0` along with its tombstones, so an older `Put` came back on
reopen. The harness failed on it about once every ten iterations. Since the
fix (#171, which counts tombstones in `FileStats` and keeps any file that
holds one), 400 iterations with vacuum running pass.

**Concurrent writers are covered by the Elle run, not here.** This harness
kills a single writer, so it knows the exact commit order and checks an exact
prefix. It never kills the process while several sync writers share one
`fdatasync` through group commit, with the next batch already appended; the
in-flight set and its order are not known to the parent there. The
isolation check's kill mode (`run_isolation_check.py --kill`, #176) covers
that window: 16 writer threads, a SIGKILL at a random point of the workload,
a reopen, ten times per history, and Elle infers the order
([`isolation_checking_design.md`](isolation_checking_design.md), *Kill*).

Run: `xmake build crash_consistency && xmake run crash_consistency --iterations 200`.
The seed and a rerun line are printed first. `--seed` replays the same
workloads and kill delays, but the operation a kill lands on still depends on
timing. `--no-vacuum` isolates the write path. On failure the directory as
the child found it, the directory the kill left, and the full operation
history are kept under `<dir>/failure/`. `crash-nightly.yml` runs 400
iterations every night in release and under ASan (with a 5 s kill ceiling,
since the ASan child is several times slower).

### Chaos soak

The proof matrix covers what can be enumerated. Two things cannot:

- **Interleaving.** Every proof cell is single-threaded. TSan reports
  only races that execute, so a suite with no overlap between the write
  path and `vacuum`, `resume`, `set_mode` or `create_manifest` gives it
  nothing to find there. The exclusion rules between those paths
  (`WriteBarrier`, the flush role, `vacuum_mu_`) are never exercised by
  a cell.
- **Volume.** A cell mints a handful of files. State that only builds up
  under sustained load — file churn at one write per file, hint worker
  backlog, key-directory versions pinned by live snapshots — has no cell
  shape.

`tests/soak_test.cpp` (`[soak]`, hidden from the default run) runs
readers, writers and a lifecycle thread against one DB for a time
budget, in epochs. Each epoch draws its configuration from the run seed:
thread counts, `io_backend`, `max_file_bytes` (including 1),
`verify_checksums`, key and value limits with
keys and values at those limits, then closes, checks and reopens.

| Thread | Does |
|---|---|
| Writers | `put`, `del`, `del_range`, `apply_batch` with and without a snapshot and guards, `sync` and `solo` varied; a snapshot read on each side of its own write |
| Readers | `get`, `contains_key`, `iter_from`, `riter_from`, `keys_from`, `rkeys_from` on `DB`, and the same through a `Snapshot` whose iterators outlive it; each `EntryView` held across a loop body that does other engine work; now and then one goes idle for over a second, so the read-cache scrape takes its cached state while others write, and its next read races the scrape |
| Lifecycle | `vacuum`, snapshot churn, `set_mode` round trips, `create_manifest`, an injected fault (commit or rotation `fdatasync`, rotation file creation) held degraded under load, then `resume()`; `stats()`, `durable_sequence()` waits, its own writes |

Each writer owns its keys, so it knows what each one holds except after
a write that threw an I/O-shaped error, whose bytes `resume()` may or
may not replay; that key is unknown until the writer's next confirmed
write to it. Values carry their key, writer, counter and a derived
payload, so any thread can check any value without coordination. The
checks: every value is well formed, belongs to the key it was read
under, and was attempted; a confirmed write reads back exactly; `del`
and guarded `apply_batch` return what the model predicts; a sync write
is durable on return, and no reader sees one while `durable_sequence()`
is below its sequence; `get` never goes back in time on one thread;
iterators yield ordered keys and held spans keep their bytes; a snapshot
answers the same way twice and outlives its `DB`; the engine is never
degraded outside an injected fault, `resume()` always clears one, and
`degraded_transitions` counts exactly those; reopen with a different
`recovery_threads` yields exactly what was there before close.

```
BYTECASK_SOAK_SECONDS=60 BYTECASK_SOAK_SEED=1234 bytecask_tests "[soak]"
```

`BYTECASK_SOAK_EPOCH` starts at the epoch a failure names,
`BYTECASK_SOAK_LIFECYCLE=vacuum,degrade,...` narrows the lifecycle
thread when bisecting, and `BYTECASK_SOAK_KEEP=1` leaves a failed
epoch's directory on disk. A seed fixes each epoch's configuration and
every thread's operation sequence, not the interleaving, so it
reproduces a failure's shape; the failures below recurred when their
seed and epoch were rerun. `soak-nightly.yml` runs it for 25
minutes per sanitizer (ASan, TSan) with the run id as the seed.

The soak trusts the kernel to return the bytes that were written. The
job checks that first, with no engine code: it logs `uname -a` and runs
`tests/platform/pread_truncate_stress.cpp`, one writer appending into a
zero-filled file with `fdatasync` while readers `pread` the newest
records and check every byte. On a kernel that returns wrong bytes, the
job fails at that step and names the kernel; the soak does not run.
ByteCaskDB does not work around a platform that returns wrong data: the
read fails, on a CRC mismatch or a record that does not parse. The
runners' `6.17.0-1022-azure` does this on ext4 (#211): a `pread` of
written bytes can return the same file's contents from a page earlier.
The soak runs on the `ubuntu-22.04` runner to stay off that kernel
(#228); the container shares the host's kernel, so the runner image is
what picks it.
Testing builds add to a failed record read where its descriptor points
and what the same bytes read now, which is what separated that kernel
from an engine bug.

#### What it found

Its first runs, on `main`, found five engine bugs the proof matrix had
executed around without being able to fail on, and a sixth the day the
blind-leaf key directory (#160) merged. Each has a regression test that
fails without its fix — under TSan, for the last. Five are fixed with
the soak (#170); the vacuum one is fixed by #171:

| Bug | Symptom | Regression test |
|---|---|---|
| A rotating batch quiesced behind another writer's failed flush and published its own state over the degrade, leaving `flush_error_` set under a healthy state | every later sync write got the stale I/O error back; a conflicting `apply_batch` waited forever for entries nothing would publish | `pipeline: rotation behind a failed flush stays degraded` |
| `vacuum` dropped every file with no live keys without a scan — tombstones included, which never count as live | a key deleted by a tombstone in that file came back at the next open, from a Put in an older file | fixed separately in #171 (#166), which the process-crash harness above (#93, #172) found at the same time; its tests cover it |
| B+ tree `place()` judged room in an oversized leaf by its capacity, then compacted it into a node sized back down to 4 KiB | heap overflow, then a corrupt key directory (4 KiB keys through `del_range`) | `BTree insert into a compacted oversized leaf` |
| B+ tree `pack()` took an emptied inner node's prefix from an entry it did not have | garbage prefix, then a crash on the next rebuild | `BTree rebuild of an inner node with no entries` |
| `degraded_transitions` counted only degrades published through the checked `store_state`; a failed flush, append or rotation sync publishes directly | the counter read 44 after 166 degrades | assertions added to the flush-failure pipeline tests |
| The active file's logical end was stored and loaded relaxed, but `fetch_record` (new with #160) reads past the record it wants up to that end — into buffer-pool frame bytes a concurrent append is writing | a data race: the blind tree confirms every lookup by reading its record, so every `get` against a busy active file raced the appender | `io_backend=BufferPool: reads of the active file race no append` |

The B+ tree bugs are single-threaded; nothing in the matrix or in
`btree_test.cpp` put keys near the 4 KiB node size through erase and
reinsert. `BTree model: keys near the leaf size` now does. Both sit in
the build session the blind-leaf tree (#160, now the default key
directory) shares for its inner nodes, where a 4 KiB key becomes a 4 KiB
separator; the leaf case of the first is specific to
`BYTECASK_KEYDIR=btree`, which CI still runs.

#### Mutations

A soak earns its nightly cost only if it catches what the matrix
cannot. This is mutation testing ([*Prior Art*](#prior-art)) with
hand-written mutants. `tests/soak_mutations/` holds one-line mutations of the
engine's synchronization, and `scripts/soak_mutation_check.sh <san>
[seconds]` applies each, runs the soak under that sanitizer, and fails
if a mutation expected to be caught survives.

| Mutation | Result |
|---|---|
| `publish_before_fdatasync` — the commit flush publishes before its `fdatasync` | caught |
| `resume_without_write_barrier` — `resume()` without its `WriteBarrier` | caught |
| `read_cache_release_relaxed` — `ReadCacheSlot::release` store weakened to relaxed | caught, under TSan (a data race); ASan cannot see it |
| `sync_flag_read_after_queue` — a queued writer reads its slot's `sync` flag while the batch's executor writes the slot | caught, under TSan (a data race); also by `WriteGroup slots may be destroyed as soon as submit returns` |
| `mmap_end_relaxed` — `mmap_end_` weakened from release/acquire to relaxed | survives, by construction |

Both caught mutations surface first as the engine refusing to derive a
key-directory version that already has a successor, which the soak
reports as an unexpected exception from a writer or from `resume()`.
The soak's durability check did not fire before that on
`publish_before_fdatasync`: moving the publication moves the published
`durable_seq` with it, so `durable_sequence()` agrees with what a reader
sees, and the damage shows only when that flush then fails.

`mmap_end_relaxed` was in #92's acceptance list and is out of reach for
the same reason BC-122 was for #103's observer axis: not a coverage gap.
`mmap_end_` orders nothing. The mapping's address and length are fixed
at construction, and the bytes behind them are written by `pwritev`,
which no sanitizer models as a memory write. What makes a lowered bound
safe is offset containment (P in `CONTRACT.md`), which holds whatever
ordering the load uses, so relaxed is a correct ordering and there is
nothing to report. `read_cache_release_relaxed` replaces it as the
weakened-release mutation: that store publishes a reader's writes to its
read-cache entry to the scrape, which runs on a writer's thread and may
delete the entry, and only concurrent execution reaches it. It survived
the first version of the soak, whose threads never went quiet long
enough for the scrape to take anything (an entry is taken after about a
second idle); the idle reader was added for it.

#### TSan and shared exceptions

The engine hands one failed flush's exception object to every writer it
failed, through `exception_ptr`. libstdc++ frees that object, and the
message buffer its copies share, under a reference count in the
uninstrumented runtime, so TSan reports a read of it on one thread and
its release on another as a race. The soak classifies exceptions by type
and reads `what()` only when reporting a failure.

### Chaos rig (power loss, I/O faults, resource limits)

Every layer above ends a run with a SIGKILL, which keeps the page cache, or
fails one syscall from inside the engine. The chaos rig
(`tests/chaos/`, [`chaos_testing_design.md`](chaos_testing_design.md))
runs the engine as a black box on a filesystem that can do anything POSIX
and Linux allow: return `EIO` from reads, writes, `fdatasync` and metadata
calls, run out of space, go read-only, write short, stall, and lose power.
Its nearest relatives are LazyFS and ALICE ([*Prior Art*](#prior-art)).

- `chaosfs.py` is an in-memory FUSE filesystem with a volatile and a durable
  image per file and per directory. `fdatasync` copies dirty pages to the
  durable image; a directory `fsync` makes its entry changes durable. A failed
  `fdatasync` follows Linux: the pages are marked clean without reaching the
  disk, the error is reported once, and reads keep returning the lost bytes
  until an eviction. Power loss keeps each dirty page whole, not at all, or
  torn at 512-byte sectors, and a prefix (or, in a quarter of crashes, a
  subset) of each directory's unsynced changes.
- `chaos_worker run` is the crash harness's single-writer workload plus
  vacuum and reader threads, on the mount. A write that throws is recorded
  as unknown; the worker then recovers (`resume()` while degraded) and sends
  the whole database as a View before writing again, so every unknown
  outcome is settled.
- `run_chaos.py` runs chains of process lives on one directory, each with
  its own fault windows, resource limits (`RLIMIT_AS`, `RLIMIT_NOFILE`) and
  end: power loss, SIGKILL, or a clean close with the hazards lifted.
- `chaos_worker check` holds each life to the invariants: every View and the
  recovered directory are a prefix of the history covering the durable
  watermark, a View with no degrade before it lost nothing, recovery with
  default options opens, serial and parallel recovery agree, a `close()`
  that returned lost nothing, and readers never saw a value that was not
  written or an error that was not I/O.

Its first runs found six engine bugs, none reachable by the matrix or the
SIGKILL harness:

| Bug | Symptom |
|---|---|
| `resume()` trusts page-cache bytes after a failed `fdatasync` (#231, fixed by #240) | after power loss, a durable hint indexes zeroed records and `DB::open` refuses the database |
| `DB::open` never syncs the newest hint-less file it indexes (#231, fixed by #240) | the same, after a SIGKILL followed by power loss, with no I/O error at all |
| vacuum drops a durable record superseded only by a non-durable write (#245, fixed by #261) | after a power cut, the key holds neither its old value nor its new one |
| vacuum leaks its `.data.tmp` staging file when compaction fails (#235, fixed by #247) | a copy of a file's live data per failed attempt, most often under `ENOSPC` |
| a failed `ftruncate` that did cut the file leaves the logical end stale (#236, fixed by #248) | while degraded, reads of published records fail with `pread failed: Success` |
| recovery memory-maps hint files (#237, fixed by #255) | a read error on a hint during `DB::open` kills the process with `SIGBUS` |

`chaos-nightly.yml` runs it for 40 minutes a night, in release and under
ASan, with every hazard; a bug found and not yet fixed gets its hazards
listed in the workflow's `KNOWN_BUGS` until it is. `--disable` leaves hazards
out, to bisect a failure or to run past a known bug; a failure keeps the directory before and after the life, the
history, the timeline and chaosfs's fault log.

#### Mutations

`tests/chaos_mutations/` holds mutations of the engine, most of them reverts
of the fixes the rig prompted, and `scripts/chaos_mutation_check.sh` proves
the rig catches them (#268): it applies each, rebuilds `chaos_worker`, and runs
the rig on two seeds, 5 minutes each unless the patch sets a `Budget:` or
focuses the run with `Disable:`.

| Mutation | Reverts | Result |
|---|---|---|
| `commit_skips_fdatasync`: the commit flush skips its `fdatasync` and still reports durable | — | caught in ~20 s, every run |
| `no_dir_sync_new_data_file`: a new data file's name is not synced | #199 | caught in 30–90 s, every run |
| `resume_trusts_page_cache`: `resume()` publishes the tail without rewriting it | #240 (#231) | caught in 1–2 min, every run |
| `open_trusts_page_cache`: `open` indexes a hint-less file without making it durable | #240 (#231) | caught in 0.5–3 min, every run |
| `close_swallows_error`: `close()` drops its final `fdatasync`'s error | #260 (#257) | caught in 0.5–3 min, every run (every life a clean close) |
| `hint_read_error_kills_process`: a hint read error kills the process | #255 (#237) | caught in 2–5 min, every run |
| `vacuum_drops_before_durable`: vacuum drops records superseded by non-durable writes | #261 (#245) | caught in 2 of 5 runs of 10 min: rare |
| `hint_written_in_place`: hints written in place, not renamed | — | caught twice in ~45 min: rare |
| `no_sync_before_degrade`: a failed append degrades without syncing | — | not caught: #240's rewrite in `resume()` covers it |
| `truncate_lowers_end_after`: the logical end drops after `ftruncate`, not before | #248 (#236) | not caught: the stale end lasts one `resume()` retry; #248's `prove_resume` cell guards it |

"Rare" (`Expected: caught (rare)`) marks a mutation the rig catches but not
within a fixed budget: the script runs and reports it, and a survival is not a
failure.

The set is kept current as the engine moves. Patches apply by three-way
merge (`scripts/mutation_patch.sh`, for both mutation sets): a change
elsewhere in a file merges, and only a conflict, when the targeted code or a
line directly next to it changed, means the patch must be regenerated.
`ci.yml` fails on a conflicting patch. A fix for a bug the rig found adds a
patch reverting it, and a change to the rig, or to engine code a mutation
patches, runs the check and reports its summary. It is not scheduled: it only
goes stale when one of those changes.

Building the set changed the rig: chaosfs now sometimes evicts a failed
`fdatasync`'s lost pages at once (`evict_failed`), and the worker counts a
read's I/O error as a violation when nothing in the run can fail a read
(`--strict-reads`). See [`chaos_testing_design.md`](chaos_testing_design.md),
*Proving the rig*.

### Isolation checking (Elle)

The layers above check one write at a time or one writer at a time. None of
them checks the isolation claims (snapshot isolation from `Snapshot`, and
serializable from a guarded `WritePlan`) against a concurrent history, which
is where G-single, G2-item and lost update show up.
`tests/elle/isolation_history.cpp` records `list-append` histories from
concurrent `WritePlan` transactions, with vacuum and degrade/`resume()`
running. `scripts/run_isolation_check.py` checks them with Elle and with a
direct cross-check against each write's commit sequence, and
`isolation-nightly.yml` runs it every night. Its first runs, on a base
without #170, hung on the rotation-barrier bug that the chaos soak had
found independently (listed in its findings table above). See
[`isolation_checking_design.md`](isolation_checking_design.md). Its extension
to the whole replication protocol (bootstrap, tailing, planned transfer,
promotion, re-targeting, re-bootstrap) runs in the same nightly and is
described in [`replication_checking_design.md`](replication_checking_design.md).
Its topology runs found that a planned transfer lost writes acknowledged
with `sync = false` (`set_mode(Follower)` now syncs), and that promoting a
follower other than the most advanced one forks the cluster.

The same writers also run under the chaos rig's hazards (#232):
`run_chaos.py --workload elle` drives `isolation_history` on chaosfs with
power loss, failed `fdatasync`, evictions and full disks, so group commit is
checked under faults, not only under SIGKILL. A read of a write the page
cache then lost is `:info`; everything durable is held to the cross-check and
Elle ([`isolation_checking_design.md`](isolation_checking_design.md),
*Chaos*).

---

## Durability sites

Every place where a mistake loses or corrupts acknowledged data, or leaves a database that will not open, with the test that fails if it breaks and a mutation that proves the test does (#280). The sites come from a sweep of every `fdatasync`, directory sync, rename, truncate and unlink in the engine, every degrade on an I/O error, and every durability fix in the git history.

Each mutation is a patch that breaks one site. Its `Guarded-by:` header names the test, as a Catch2 test spec; when the patch was written, that test was run with the patch applied and failed. Mutations the chaos rig reaches live in `tests/chaos_mutations/` (*Chaos rig*, *Mutations*), and their `Expected:` header still says what the rig sees. The rest live in `tests/durability_mutations/`. `ci.yml` checks that every patch still applies.

A site whose break nothing has to catch says why instead.

### Commit path

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `flush_pending`: the commit `fdatasync` before `durable_seq` moves | `a sync write survives a power cut as soon as it returns` | `commit_skips_fdatasync` (chaos) | — |
| `flush_pending`: the state is published only after the `fdatasync` | `pipeline: sync write is invisible until its fdatasync returns; …` | `publish_before_fdatasync` (soak) | #92 |
| `flush_pending`: a failed commit sync degrades and publishes nothing | `class F: key not visible after commit sync failure` | `commit_sync_error_ignored` | BC-155, BC-163 |
| `execute_slots`: a failed append degrades | `*__group_append_fails` (`prove_group_commit`) | `append_failure_not_degraded` | BC-163 |
| `execute_slots`: a failed append syncs what it can before it degrades | not needed: `resume()` rewrites and syncs the file before trusting it (#240) | `no_sync_before_degrade` (chaos, not caught) | — |
| `execute_slots`: a rotation behind a failed flush stays degraded | `pipeline: rotation behind a failed flush stays degraded` | `rotation_publishes_over_degrade` | #170 |
| `execute_slots`: a sync write that appends nothing makes earlier unsynced writes durable | `apply_batch: an empty sync plan makes earlier unsynced writes …` | `sync_only_write_skips_sync` | #192 |
| `execute_group`: a throw after the append refuses writes | `refuse writes: a throw after the append refuses writes until resume()` | `post_append_failure_not_refused` | #364 |
| `refuse_writes`: the flag refuses when the degraded state cannot be published | `refuse writes: the refusal holds when the degraded state cannot be published` | `refusal_needs_published_state` | #364 |
| `flush_once`: a `flush_pending` that throws gives the flush role back | `refuse writes: a flush that throws releases the flush role` | `flush_role_kept_on_throw` (hangs) | #364 |
| `quiesce`: the same for a write barrier's flush | `refuse writes: a barrier's flush that throws releases the flush role` | `barrier_flush_role_kept_on_throw` (hangs) | #364 |
| `finish_rotation`: a rotation that fails in memory publishes nothing | `refuse writes: a rotation that fails in memory publishes nothing` | `half_rotation_published` | #364 |
| Key directory: a dropped transient is freed without allocating | `alloc sweep: a transient tree is freed when nothing can be allocated` | `free_subtree_allocates` | #364 |
| Key directory: `publish` reserves its record before it links the base | `alloc sweep: a failed publication leaves the base derivable` | `publish_links_before_reserving` | #364 |
| Key directory: releasing a version never throws for want of memory | `alloc sweep: a version is released when nothing can be allocated` | `release_queue_allocates` | #390 |

### Rotation and new files

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `execute_slots`: a failed rotation `fdatasync` degrades | `class G: key not visible after rotation sync failure` | `rotation_sync_error_ignored` | BC-155 |
| `prepare_rotation`: `shrink_to_fit` cuts the preallocated tail and syncs the length | not needed: a sealed file's zero tail costs space, not data; open drops a zero tail past the last record (`recovery_check_tail`), and the rotation `fdatasync` before it has already made the data durable | — | — |
| `create_active_file`: the directory sync before the first write into a new file (open, rotation, `resume()`) | `directory sync: a failed sync at rotation degrades …`, `… in resume() stays degraded`, `… open fails when the active file's entry cannot be synced` | `no_dir_sync_new_data_file` (chaos) | #199 |
| `sealed_file_size`: a sealed file whose `fstat` fails is not opened as an empty one | `fault sweep: write across a rotation` | `sealed_fstat_failure_reads_empty` | #317 |
| `rewrite_durably`: a file whose `file_size` fails is not rewritten as an empty one | `fault sweep: resume after a failed sync` | `rewrite_size_failure_reads_empty` | #319 |
| rotation: a new file that cannot be created degrades | `*rotation_file_creation_fails` | `rotation_create_failure_not_degraded` | — |
| `createDataFileForWrite`: a reused file name panics instead of reopening a live file | `createDataFileForWrite panics when the data file already exists` | `data_file_create_not_exclusive` | #35 |
| `renameDataFileExclusive`: vacuum's copy never replaces a file holding its name | `renameDataFileExclusive panics rather than replacing a live file` | `data_file_rename_replaces` | #35 |

### Hint files

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `flush_hints_for`: written to `.hint.tmp` and renamed into place | `DB recovery: a crash while open writes a hint leaves no hint behind it, …` | `hint_written_in_place` (chaos) | — |
| `HintFile::close`: the hint's `fdatasync` before the rename | `*hint_sync_fails*` (`prove_recovery`) | `hint_skips_fdatasync` | — |
| `flush_hints_for`: the directory sync after the rename | not needed: a lost rename leaves the file hint-less, and the next open rebuilds the hint (`directory sync: a hint rebuilt at open is synced` checks the call is made) | — | #199 |
| `flush_hints_for`: batch markers are kept, so a restart's `next_seq` covers them | `a write after a restart does not reuse a batch marker's sequence` | `hint_skips_batch_markers` | 362cc590 |
| `open_hint_or_rebuild`: a hint that fails its CRC is rebuilt, not skipped | `DB recovery: a corrupt hint is rebuilt from its data file` | `corrupt_hint_not_rebuilt` | #123 |
| `open_hint_or_rebuild`: a hint a read fails on is rebuilt | `DB::open rebuilds a hint file a read fails on` | `hint_read_error_kills_process` (chaos) | #255 (#237) |
| `open_hint_or_rebuild`: an error that says nothing about the bytes leaves the hint alone | `DB::open leaves a hint alone on an error that says nothing about …` | `hint_rebuilt_on_any_error` | #270 (#262) |

### Vacuum

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `vacuum_commit`: what superseded a dropped record is synced first | `vacuum does not drop a durable record that only an unsynced write supersedes` | `vacuum_drops_before_durable` (chaos) | #261 (#245) |
| `vacuum_commit`: that sync failing degrades and keeps the source | `vacuum whose fdatasync fails degrades, keeps the source file, …` | `vacuum_sync_failure_ignored` | #261 |
| `vacuum_compact_file`: the staging file's `fdatasync` | not needed: `shrink_to_fit()` syncs it again right after | — | — |
| `vacuum_compact_file`: the directory sync after the rename, before the source is unlinked | `directory sync: a failed sync in vacuum keeps the source` | `vacuum_unlinks_before_dir_sync` | #199 |
| `vacuum_compact_file`: the staging file is removed on every failure | `prove_vacuum_compact__*` | `vacuum_leaks_staging_file` | #247 (#235) |
| `vacuum_compact_file`: the renamed copy and its hint are removed on every failure before the commit | `vacuum that fails between its rename and its commit removes its copy`, `prove_vacuum_compact__*` | `vacuum_leaks_renamed_copy` (chaos) | #304 |
| `renameDataFileExclusive`: the fallback takes its `link()` back when the staged name's `unlink()` fails | `renameDataFileExclusive takes the placement back …`, `*__fallback_unlink_fails` (`prove_vacuum_compact`) | `rename_fallback_keeps_placed_copy` | #258 |
| `vacuum`: a file of tombstones only is not dropped whole | `vacuum keeps a tombstone-only file that shadows an older put` | `vacuum_drops_tombstone_only_file` | #171 (#166) |
| `vacuum_scan_and_copy`: a damaged entry fails the compaction | `DB vacuum: a damaged sealed file is not compacted away` | `vacuum_compacts_damaged_file` | #136 |
| `vacuum_scan_and_copy`: batch markers are copied | `vacuum preserves BulkBegin/BulkEnd markers` | `vacuum_drops_batch_markers` | BC-197 |

### `resume()`

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `rewrite_durably` before the scan | `resume() makes durable what it publishes after a failed fdatasync` | `resume_trusts_page_cache` (chaos) | #240 (#231) |
| refuses to cut below the published extent | `resume() refuses when a failed fdatasync's pages were evicted, …` | `resume_truncates_published` | #136, #240 |
| an I/O error in the scan truncates nothing | `resume() rethrows an I/O error from its scan and truncates nothing` | `resume_truncates_on_io_error` | #136 |
| `WritableFileOps::truncate`: the logical end is lowered before `ftruncate` | `*__truncate_fails_after_cut` (`prove_resume`) | `truncate_lowers_end_after` (chaos, not caught by the rig) | #248 (#236) |
| the `fdatasync` after the truncate | `resume() stays degraded when the sync after its truncate fails` | `resume_skips_sync_after_truncate` | — |
| `apply_resume`: range tombstones are replayed | `prove_resume__degrade_F_range__*` | `resume_ignores_range_del` | #135 |

### Open and recovery

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `DB::DB`: each directory open creates is synced in its parent | `directory sync: open syncs each directory it creates` | `no_dir_sync_created_dir` | #199 |
| `recovery_prepare_files`: a hint-less file is rewritten and synced before it is read | `open makes a hint-less file durable before it indexes it: a process killed before its sync` | `open_trusts_page_cache` (chaos) | #240 (#231) |
| `recovery_check_tail`: a tail is cut only in the newest file | `DB recovery: a hint-less file's tail is truncated only in the newest file` | `tail_cut_in_any_file` | #206 (#138) |
| `recovery_check_tail`: a tail is cut in one file per open | `DB recovery: a hint-less file's tail is truncated only in the newest file` | `two_torn_files_both_cut` | #303 |
| `truncate_durably`: the cut's new length is synced | `DB recovery: open fails when the cut of a torn tail cannot be synced, …` | `open_truncate_not_synced` | — |
| `DataFileIterator`: an entry whose size runs past the file ends the sweep | `resume() does not trust an entry size that runs past the file` | `scan_trusts_entry_size` | #36, ed3ad5df |
| `recovery_undo_interrupted_vacuum`: only a compacted copy of the other file is removed | `recovery refuses two different writes under one sequence` | `interrupted_vacuum_any_pair` | #137 |
| recovery workers hand their errors to the open instead of terminating | `DB parallel recovery: an unrebuildable hint throws instead of …` | `recovery_worker_error_terminates` | BC-157 |
| `recovery_prepare_files`: stale `.tmp` files are removed | not needed: a `.tmp` is never a source of truth | — | — |

### `close()` and replication

| Site | Guarded by | Mutation | Origin |
|---|---|---|---|
| `close()`: the final `fdatasync` and its error | `close() throws when its fdatasync fails, closes all the same, …` | `close_swallows_error` (chaos) | #260 (#257) |
| `close()`: a degraded engine reports writes that are not durable | `close() on a degraded engine reports acknowledged writes …` | `degraded_close_silent` | #260 |
| `close()`: the active file's hint | not needed: without it the next open rewrites the hint-less file, which costs time only (`a clean close writes the active file's hint` checks it) | — | #240 |
| `set_mode(Follower)`: unsynced acknowledged writes are synced | `set_mode(Follower) makes unsynced acknowledged writes durable` | `set_mode_skips_sync` | #190 |
| `set_mode(Follower)`: that sync failing degrades | `set_mode(Follower): a failed fdatasync degrades and keeps the mode` | `set_mode_sync_failure_ignored` | #190 |
| `create_manifest`: a failed sync degrades | `create_manifest whose fdatasync fails degrades, …` | `manifest_sync_keeps_healthy` | #282 (#281) |
| `create_manifest`: a failed rotation degrades | `prove_manifest__*` | `manifest_rotation_failure_not_degraded` | 362cc590 |
| `ingest`: the final `fdatasync` before publishing | `prove_repl__*` | `ingest_skips_final_sync` | — |
| `ingest`: that sync failing degrades | `prove_repl__*` | `ingest_sync_failure_ignored` | — |
| `ingest`: a slice that ends inside a batch is refused, so no rotation seals a `BulkBegin` without its `BulkEnd` | `ingest refuses a slice that ends inside an atomic batch` | `ingest_open_batch_accepted` | #188 |

### What building the table found

- **#281.** `create_manifest` threw on a failed sync without degrading, so the next sync write claimed the lost writes durable. Fixed in #282.
- **#248's guard could not fail.** Its cells fail `io_resume_truncate` to model an `ftruncate` that cut the file and then reported an error, but the fault fired after the logical end was lowered whichever order the code used. The fault now stands in for `ftruncate`'s result.
- **The commit `fdatasync` had no test of its own.** Removing it failed other tests only because their first sync write was lost with it.
- **`vacuum preserves BulkBegin/BulkEnd markers` never compacted the batch.** Its batch file held only live entries, which vacuum does not touch. A later write now kills one of them.
- **Three syncs had no test that could see them.** The hint's `fdatasync`, `resume()`'s sync after its truncate, and open's sync of a cut tail: the page cache model covers data file pages, not hint files or file lengths. Each is guarded the way the directory syncs are, by failing its fault point, which a mutation that removes the sync removes too. The hint's fault point moved into the call (`sync_hint`) for that.
- **The `hint_written_in_place` mutation had no test of its own;** the rig caught it twice in about 45 minutes. A test now fails the hint write between its frames and its trailer.

- **A failed `fstat` sealed a file as empty.** No checkpoint named the call; the counted fault sweep reached it (*Counted fault sweep*, #317).

The radix key directory's leak on a failed recovery merge (#203), before the radix tree was removed, was not a durability site; LeakSanitizer caught it in CI.

## Output Structure

```
tests/
  proof/
    __init__.py                    ← Python package marker
    generate_tests.py              ← apply_batch generator
    expected_delta.py              ← apply_batch reference model
    fault_point_resolver.py        ← apply_batch fault configs
    scenario_matrix.py             ← apply_batch input matrix
    invariants.h                   ← shared assert helpers for all proof suites
    resume/
      __init__.py
      scenario_matrix.py
      expected_delta.py
      fault_point_resolver.py
      generate_tests.py
    vacuum_compact/
      __init__.py
      scenario_matrix.py
      expected_delta.py
      fault_point_resolver.py
      generate_tests.py
    replication/
      __init__.py
      scenario_matrix.py
      expected_delta.py
      fault_point_resolver.py
      generate_tests.py
    generated/
      prove_apply_batch.cpp     ← generated, never hand-edited
      prove_resume.cpp             ← generated, never hand-edited
      prove_vacuum_compact.cpp     ← generated, never hand-edited
      prove_group_commit.cpp       ← generated, never hand-edited
      prove_recovery.cpp           ← generated, never hand-edited
      prove_corruption.cpp         ← generated, never hand-edited
      prove_replication.cpp        ← generated, never hand-edited
```

The fault injection infrastructure (`fault_injector.h`) lives in
`bytecaskdb/` alongside the production code it instruments.

Build targets for the proof tests are added to the root `xmake.lua`,
consistent with the existing test and benchmark targets.

The generated files are committed to the repository. They are
evidence. Regenerating them and seeing no diff confirms the model
and the generator are stable. A diff after regeneration means either
the model changed or the generator changed — both are intentional
and reviewable.

---

## `rename()` Handling

`rename()` on a consistent local filesystem (ext4, xfs, btrfs) is
atomic. The engine assumes the filesystem handles rename consistently.
Under this assumption the ambiguous cases reduce to one: `EINTR`.

```
Success (returns 0)    — rename happened, guaranteed
EINTR                  — interrupted, may or may not have happened
Any other error        — rename did not happen, guaranteed
```

### Retry on EINTR

```cpp
auto atomic_rename(const path& from, const path& to) -> void {
    while (true) {
        if (::rename(from.c_str(), to.c_str()) == 0) return;

        if (errno == EINTR) {
            // Ambiguous — check if rename completed
            if (!std::filesystem::exists(from) &&
                 std::filesystem::exists(to)) {
                return;  // completed despite the error
            }
            continue;  // retry
        }

        // Any other error — rename definitively did not happen
        throw std::system_error{errno, std::generic_category(),
                                "rename failed"};
    }
}
```

### Orphaned `.data` files

If `rename()` completes but the process is killed before the source is
unlinked, the compacted copy and its source are on disk together, holding
the same entries under the same sequences. `io_vacuum_compact_post_rename`
reproduces the earliest point of that window — renamed, not yet committed —
and VC5 (`io_vacuum_compact_unlink`) the latest, committed but with the
source still on disk.

Recovery undoes the vacuum: it deletes the copy once it has checked that
every entry in it is also in the source, and refuses to open on any other
pair of files that share sequences (#137, and
[`vacuum_crash_recovery_design.md`](vacuum_crash_recovery_design.md) for
why undo and not finish). Before that, a kill anywhere in the window left a
database that would not open — `DB::open` threw under `Pread`, and under
`Mmap` the process aborted in `~DB`. Measured after #137 with the
post-rename point on both backends: `~DB` returns, the next open removes
the copy, and every key reads back.

This is #104's class **M3**. Its reference model assumed detection and
removal were already implemented; #137 is what implemented them. The two
ends of the window are cells now — VC6 for the post-rename point and VC5
for the unlink (see *vacuum_compact* above) — and they are the reference
model this section used to spell out in prose.

---

## What This Validates

```
For every (S1, P, F) in the scenario matrix:
  transition(S1, P, F) → S2
  where S2 satisfies all invariants
  and delta(S1, S2) == expected_delta(P, F)
```

**What it does not claim**: exhaustive coverage of all state values.
It claims exhaustive coverage of all structural failure classes for
all plan shapes in the matrix. That coverage targets the failure
modes that matter in practice. The argument that a bounded matrix is
enough is the small scope hypothesis ([*Prior Art*](#prior-art)): it is a
hypothesis, and the soak, the crash harness and the chaos rig exist for
what the bound leaves out.

### What this does not cover

The validation framework proves properties of state transitions under
*software-modeled* failure classes — `writev` returning errors, short
writes, and `fdatasync` failures. It does not cover failures that
originate below the POSIX syscall boundary or outside the engine's
control:

- **Lying storage devices** — drives that ACK writes and lose them
  (consumer SSDs with volatile write caches, USB drives). The engine
  trusts that a successful `fdatasync` return means bytes are durable.
- **Filesystem write reordering** — filesystems that reorder writes
  across `fdatasync` boundaries (some network/FUSE filesystems). The
  engine assumes `fdatasync` is a barrier.
- **Silent `fdatasync` failures on Linux** — the "fsyncgate" issue
  (PostgreSQL, 2018): on some kernel/filesystem combinations, `fsync`
  can return success after an earlier async writeback error was consumed
  by another fd. The engine trusts the return value (see "fdatasync
  trust assumption" in `CONTRACT.md`).
- **Bit rot at rest** — silent data corruption on disk between writes.
  CRC verification catches this on read when `verify_checksums` is
  enabled, but no periodic scrub is performed.
- **Sub-sector torn writes** — a `writev` of a multi-sector entry can
  partially land at the 512-byte sector level on power loss. CRC-per-
  entry detects this on recovery (the entry fails CRC and is truncated),
  which is the correct behavior — but the fault injector models failures
  at the `writev` boundary, not the sector boundary. The recovery test
  `a hint-less file's tail is truncated only in the newest file` builds
  the shapes a power cut leaves by hand instead.
- **Hardware-level fault injection** — kernel block-layer error injection
  (`dm-flakey`, `dm-dust`), power-cut testing rigs, or filesystem-
  specific fault tools. The fault injector operates at the application
  syscall layer only. The process-crash harness kills the process at
  arbitrary points, but the page cache survives it, so it does not stand
  in for power loss either. The chaos rig models power loss and
  filesystem errors in a FUSE layer, as a filesystem that keeps the POSIX
  contract may produce them; it does not model a device that loses what an
  `fdatasync` confirmed. The one power-loss hazard the engine controls
  beyond `fdatasync` — a directory entry that is not durable when something
  depends on it — is checked by ordering instead: each directory sync has
  its own `io_dir_sync_*` checkpoint, and the `[dir_sync]` tests fail each
  one and check that nothing it guards goes ahead (#199). A failed
  `fdatasync` that leaves pages clean but off the device (fsyncgate, #231)
  is modelled one layer up: `PageCacheModel` tracks each data file page's
  device image, an injected sync failure keeps the images, and the
  `[fsyncgate]` tests cut the power or evict the pages by writing them
  back, then check that `resume()` and `DB::open` published nothing the
  device does not hold. The proof generators cut the power under the same
  model after every cell (*Power-loss axis*, #265), so a decision made
  durable on state that is not — #245's class — is caught at the cell.
  The model covers data file pages and checks the ordering hints depend
  on; it does not model directory operations (a `rename` or `unlink`
  survives the cut whether or not a directory sync covered it, #266), and
  it takes `fdatasync` as a barrier and the hint worker's timing as given.
- **Time bounds on close and open** — the failure classes are about what a
  failure does to data. A close or open that is correct but too slow for
  the supervisor holding the stopwatch damages nothing, so no class here
  models it. The one lifecycle bound the engine keeps is the hint backlog
  (`Options::max_hint_backlog`, #146): close writes at most that many hint
  files, and an open after a kill rebuilds at most one more.
  `[hint_backlog]` in `tests/bytecask_test.cpp` checks both ends of it.

---

## Why This Approach

The failure classes reduce an infinite problem space to a finite
and tractable one. What matters is not which specific operation
failed or what the key bytes were — it is which phase boundary the
failure crossed, which determines whether the transition was fully
persisted or not. Eleven classes cover the entire behavioral space of
`apply_batch` under I/O failure.

The Python generator makes the model explicit, auditable, and
evolvable. When a new failure class is identified, it is added to the
matrix and all existing state shapes and plan shapes are automatically
covered against it.

`DataFile::append_entries` batches entries into `writev()` calls of up
to `kMaxEntriesPerWritev` entries each. In production this is
`IOV_MAX / 4` (256 entries on Linux) — larger than any realistic batch.
Under `BYTECASK_TESTING`, the limit is lowered to 2 so that multi-entry
batches (4–5 entries including markers) require 2–3 `writev()` calls.
This forces the chunking loop through the existing proof matrix without
adding new test shapes — the B1/B2/B3 failure classes now exercise
partial writes where the first chunk committed and a later chunk failed.

The correctness baseline this produces is a ratchet. Any change
to the engine that breaks a generated test is rejected. The
baseline moves forward or stays still, never backward. This makes
the following safe to attempt without losing correctness:

- `io_uring` or alternative I/O backends
- Alternative index structures
- Alternative thread models
- External contributors

Each experiment either clears the baseline or it does not.

---

## Prior Art

None of the techniques here is new. This section names what each layer
follows, and where it departs.

**Model-based testing.** In Utting, Pretschner and Legeard's taxonomy [1],
the proof generator is model-based testing with a *transition-based* model
— their term for state machines and transition systems. The model
(`expected_delta.py`) is written for testing and kept apart from the
engine; the tests are generated offline, and the model is covered
structurally, by enumerating its transitions.

**Category-partition.** The matrix's axes — state shapes, plan shapes,
failure classes — and its elimination rules are Ostrand and Balcer's
categories, choices and constraints [2], with the generator producing the
test frames. Using one fixed set of symbolic values per cell rests on what
Bernot, Gaudel and Marre call a uniformity hypothesis [3]: every member of
a class behaves as its representative does. *Property-Based Validation of
the Independence Assumption* tests that hypothesis with random values, in
the manner of QuickCheck [4].

**Bounded exhaustive testing.** Korat [5] generates every input up to a
size bound that satisfies a validity predicate, and runs the code on each.
The generators do the same over transitions: every combination within the
bound, minus those the elimination rules mark impossible. The case that a
small bound is enough is Jackson's small scope hypothesis [6]: most bugs
have small counterexamples. Plan shapes here are a handful of operations,
and `kMaxEntriesPerWritev` is lowered to 2 under test so that the bound
still reaches the chunking loop. The hypothesis says nothing about interleaving
or volume; *Chaos soak* covers those by sampling.

**Combinatorial testing.** Covering arrays [7] cut a product of parameters
down to the rows that cover every *t*-way interaction, on the evidence
that few failures need more than a handful of parameters to interact. The
matrix does not use them. After elimination the product is a few thousand
tests, small enough to run in full, and every cell has its own expected
delta to check. Covering arrays are the fallback if a new axis makes the
product too large.

**Fault injection by count.** SQLite [8] instruments its allocator and its
VFS to fail the N-th call, and reruns each operation with N = 1, 2, …
until it completes without reaching the fault. The fault injector here
names its sites instead: a failure class is a named checkpoint and mode,
and the model gives each class an expected delta, which a count cannot
carry. A counted loop, though, reaches a new I/O call without anyone
registering it, and a named one does not. So both run: the
[*Counted fault sweep*](#counted-fault-sweep) fails each I/O call of an
operation in turn, counted below the engine with `--wrap`, and holds the
result to invariants that need no expected delta; the named checkpoints
keep the per-class deltas. SQLite counts through its own VFS; the sweep
counts at the libc boundary, and links the C++ standard library
statically so that what the engine does through `std::filesystem` crosses
it in the binary (#319). Allocations are failed by count too, by the
[*Counted allocation-failure sweep*](#counted-allocation-failure-sweep),
which replaces `operator new` where SQLite has its own allocator. SQLite's
crash tests, which
run on a VFS that drops or damages unsynced writes at a simulated crash,
are the ancestor of `PageCacheModel`.

**Crash-consistency testing.** CrashMonkey and ACE [9] test file systems
by bounded black-box crash testing: ACE enumerates every workload of a
few file-system operations, and CrashMonkey records each one's block I/O,
builds the state a crash after a persistence point leaves, and checks it
against an oracle. The *Power-loss axis* has that shape one layer up: the
workloads are the generators' cells, the cut comes once per cell, and the
oracle is the durable baseline and watermark. CrashMonkey itself is not
used. It tests the file system below the engine: on a correct file system
it yields the one state that file system produces, where the engine's
bugs are in what any conforming file system may do with unsynced state.

ALICE [10] is the closer relative, since it tests applications. It traces
an application's system calls, constructs the crash states an abstract
persistence model permits, and runs the application's own checker on
each. The *Chaos rig* produces the same kind of state — what POSIX
allows, not what one file system does — on a FUSE filesystem, as LazyFS
[11] does for Jepsen's tests, but picks its crash points at random where
ALICE enumerates them. No layer here enumerates every crash point inside
an operation: the axis cuts once, at the end of a cell, and the rig
samples.

**Mutation testing.** The mutation sets (*Durability sites*, and the
*Mutations* tables of the soak and the chaos rig) are mutation testing
[12]: a test suite is judged by the deliberate faults it detects. The
mutants are written by hand, one per durability site or synchronization
rule, not generated by operators, so each is a bug worth catching: one
that survives is a missing test, or is kept as `Expected: NOT caught` with
the reason.

**Isolation checking.** Elle [13] infers a transaction dependency graph
from an observed history and reports the cycles; *Isolation checking*
uses it as published.

1. M. Utting, A. Pretschner, B. Legeard. *A taxonomy of model-based
   testing approaches.* Software Testing, Verification and Reliability
   22(5), 2012.
2. T. J. Ostrand, M. J. Balcer. *The category-partition method for
   specifying and generating functional tests.* Communications of the
   ACM 31(6), 1988.
3. G. Bernot, M.-C. Gaudel, B. Marre. *Software testing based on formal
   specifications: a theory and a tool.* Software Engineering Journal
   6(6), 1991.
4. K. Claessen, J. Hughes. *QuickCheck: a lightweight tool for random
   testing of Haskell programs.* ICFP 2000.
5. C. Boyapati, S. Khurshid, D. Marinov. *Korat: automated testing based
   on Java predicates.* ISSTA 2002.
6. D. Jackson. *Software Abstractions: Logic, Language, and Analysis.*
   MIT Press, 2006.
7. D. R. Kuhn, D. R. Wallace, A. M. Gallo. *Software fault interactions
   and implications for software testing.* IEEE Transactions on Software
   Engineering 30(6), 2004.
8. [*How SQLite Is Tested*](https://www.sqlite.org/testing.html):
   out-of-memory, I/O error and crash testing.
9. J. Mohan, A. Martinez, S. Ponnapalli, P. Raju, V. Chidambaram.
   *Finding crash-consistency bugs with bounded black-box crash testing.*
   OSDI 2018.
10. T. S. Pillai, V. Chidambaram, R. Alagappan, S. Al-Kiswany,
    A. C. Arpaci-Dusseau, R. H. Arpaci-Dusseau. *All file systems are not
    created equal: on the complexity of crafting crash-consistent
    applications.* OSDI 2014.
11. [LazyFS](https://github.com/dsrhaslab/lazyfs): a FUSE filesystem that
    loses unsynced writes on demand.
12. R. A. DeMillo, R. J. Lipton, F. G. Sayward. *Hints on test data
    selection: help for the practicing programmer.* IEEE Computer 11(4),
    1978.
13. K. Kingsbury, P. Alvaro. *Elle: inferring isolation anomalies from
    experimental observations.* PVLDB 14(3), 2020.

---

## Property-Based Validation of the Independence Assumption

### The assumption

The proof matrix makes a structural independence assumption: the
correctness of a transition depends only on operation types and their
ordering, not on the specific key bytes, value bytes, or value sizes.
Every test uses one fixed set of symbolic values (`"new0"`, `"v0"`,
etc.) across all 2,082 matrix cells. If some code path accidentally
depends on value content — a length-dependent branch, a key that
collides with an internal sentinel, or a value size that crosses a
buffer boundary — the current matrix would not catch it.

The testing literature calls this a uniformity hypothesis
([*Prior Art*](#prior-art)): one representative stands for its whole
class.

### Approach

Only a transition that lands has values to be independent of: the
SUCCESS, NOSYNC and H cells, 535 of the 2,082. Their deltas differ only
in the `degraded`/`threw` flags and in what a power cut may take, and
the solo, guarded and observer variants of a plan leave its delta
unchanged, so the 19 plan shapes that write (the conflicting plan writes
nothing) collapse to 13 structurally distinct deltas:

| # | Delta shape | seq_advance | Representative plan |
|---|-------------|-------------|---------------------|
| 1 | 1 added, 1 value | 1 | `single_put` |
| 2 | 1 added, 1 value | 4 | `causality_overwrite` (last-write-wins) |
| 3 | 1 added, 1 value | 5 | `causality_put_del_put` |
| 4 | 1 added + 1 removed, 1 value | 4 | `mixed_batch` |
| 5 | 2 added, 2 values | 4 | `multi_put` |
| 6 | 3 added, 3 values | 5 | `large_batch` |
| 7 | 0 added, 1 removed | 1 | `single_delete` |
| 8 | 0 added, 1 removed | 4 | `causality_put_del` |
| 9 | 1 changed, 1 value | 1 | `sequential_overwrite` |
| 10 | 1 changed, 1 value | 4 | `causality_del_put` |
| 11 | every key in the range removed | 1 | `range_del` |
| 12 | range removed, 1 key put back | 4 | `range_del_then_put` |
| 13 | range removed, the put with it | 4 | `put_then_range_del` |

**13 property tests** total.

### What each property test does

For each distinct delta:
1. Generate a starting state, the plan's keys and values, and how the
   DB is opened.
2. Run the plan against the real engine.
3. Assert the whole DB equals a dict the same operations were applied
   to — in the live DB, and after serial and 4-thread recovery — and
   that the sequence advanced by the plan's entry count.

The comparison is of the whole DB, by `iter_from`, `riter_from`,
`keys_from` and a `get` of every key, not of the plan's keys alone: a
key misplaced because of its bytes shows up as damage to a neighbour.

What is generated is chosen for the code that could depend on it:

- **Keys that share a prefix.** The blind-leaf key directory holds no
  key bytes, only a crit bit and a fingerprint per key, and reads a
  neighbour's record to place a key. Uniformly random keys diverge in
  their first byte and exercise none of that. The keys of one example
  share a prefix (empty, short, or within a few bytes of
  `max_key_bytes`) and differ in a suffix of up to six bytes over
  `{00, 01, 80, FF}`, so they are prefixes of one another and differ in
  a single bit. The empty key is included.
- **A populated starting state.** Empty, up to a dozen keys, or 170 to
  290 sharing the prefix — several leaves' worth — so the plan's keys
  land among neighbours and across leaf splits.
- **Range bounds drawn from the same family**, so which keys a range
  delete covers is decided at those single-bit and prefix boundaries.
- **Value sizes**: empty, 1 byte, typical (64 B), around a page
  (4095–4097 B), 8–16 KiB, and `max_value_bytes` and one byte under.
- **How the DB is opened**: `Pread`, `Mmap` and `BufferPool`, with the
  default file size or 64 KiB files, where the setup rotates and the
  plan lands on a DB of several sealed files.

### Scope

SUCCESS only. Class H (the write commits, the rotation after it fails)
and NOSYNC take other paths through the engine and are not covered:
the Python bindings do not expose `ScopedFaultInjector`, and nothing in
them cuts the power. A C++ property framework (rapidcheck) could cover
both.

The generated keys are adversarial for prefixes and crit bits, not for
fingerprints: no example is constructed to make two keys' fingerprints
collide.

### What this does NOT replace

The deterministic proof matrix remains the primary validation. It
exhausts the behavioral space (all failure classes × all plan shapes ×
all state shapes). The property tests complement it by sampling the
value space for each behavioral equivalence class — validating the
assumption that lets the deterministic matrix use symbolic values.

### Implementation

13 Hypothesis property tests in `tests/proof/test_independence.py`, 50
examples each, run on every pull request (`ci.yml`) and against each
built wheel (`build-wheels.yml`).

Run: `PYTHONPATH=bytecaskdb-python pytest tests/proof/test_independence.py`
