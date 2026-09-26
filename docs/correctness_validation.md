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

---

## Failure Classes

Every I/O failure in `apply_batch` falls into exactly one structural
class. The class determines the expected delta — not which specific
operation failed, not what the key bytes were.

```python
class FailureClass(Enum):
    SUCCESS = "success"               # no failure — transition fully persisted
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
- **B2 — partial write**: `writev` returns 0 < N < total. N bytes are on
  disk, kernel fd position advanced by N. `offset_` is not advanced (the
  throw in `append()` skips `offset_ +=`). The file is `tainted`. This
  class also covers sub-entry torn writes at the sector level: a power
  loss mid-flush can land some 512-byte sectors on disk and not others,
  even though `writev` never returned to the caller. On recovery, the
  entry fails CRC verification and is truncated — the same mechanism that
  handles B2 at the `writev` boundary.
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
  `rotate_active_file` fails after sealing the active file. The sealed
  file cannot accept further appends (`assert(!sealed_)` would fire).
  The engine degrades the DB and publishes the state (writes are on disk,
  LSNs must advance). `resume()` creates a fresh active file and clears
  the degraded state.

Key insight: `rotate_active_file` calls `seal()` before creating the new
file. If file creation fails, the active file is sealed and unusable.
Publishing state without degrading would leave an engine that appears
healthy but fails on the next append. Degrading is the correct response;
`resume()` restores normal operation.

### Class Behavior Summary

| Class | Transition persisted | key_dir changes | LSN advances | Throws | Degraded |
|-------|---------------------|-----------------|--------------|--------|----------|
| SUCCESS | Yes — fully | Yes — full delta | Yes | No | No |
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
| M3 | `rename` completes, the process does not confirm it | the next open detects the uncommitted copy and deletes it | VC6 in `[prove_vacuum_compact]` |
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

Four checkpoints exist in the production code:
1. `io_data_file_append` — before `writev()` in `DataFile::append()`
2. `io_data_file_append_partial` — after `writev()` but before offset
   advance in `DataFile::append()` (post-write checkpoint)
3. `io_data_file_sync` — before `fdatasync()` in `DataFile::sync()`
4. `io_rotate_file_creation` — after sealing, before new file creation
   in `rotate_active_file()`

Six additional checkpoints exist in `bytecask.cpp` (compiled under `BYTECASK_TESTING`):

5. `io_resume_truncate` — before `file.truncate()` in `resume()` (only
   reached when the active file has orphaned bytes to discard)
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
    before `vacuum_commit()` in `vacuum_compact_file()`. See *Orphaned
    `.data` files* for what it reproduces and why no cell asserts it yet.

And one on the read side, for the scan `resume()` runs:

14. `io_data_file_scan` — before the `pread()` in the active file's
    `read_raw()`, which every sweep of it goes through. An I/O error there says nothing about the bytes, so
    `resume()` must rethrow it rather than read it as the end of the file
    and truncate; `[degraded][resume]` holds it to that.

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

### apply_batch — 1873 tests

1873 generated Catch2 tests (`[prove]` tag) cover every valid
(StateShape, PlanShape, FailureClass, Observer) combination for
`apply_batch`. The scenario matrix is 11 state shapes × 20 plan shapes ×
9 failure classes; 4 elimination rules reduce this to 1313 observer-free
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
was measured. Four `radix_tree_test.cpp` cases catch it instead.

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
`recovery_build_from_hints`, which is O(R) per Put during hint replay and
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

Each test follows the same structure:
1. Set up initial DB state from `StateShape`
2. Capture baseline
3. Construct `WritePlan` from `PlanShape`
4. Inject fault per `FaultConfig`
5. Execute `apply_batch`
6. `assert_delta(before, db, expected)` — validates key membership,
   value correctness (causality), LSN advancement, structural consistency,
   and degraded state.
   For degraded cases, `assert_resumable(db)` is called immediately after
   to verify that `resume()` restores consistent state in-process.
7. `assert_recoverable(dir, before, expected)` — validates persistence
   invariant via fresh recovery (where applicable)

### resume() — 59 tests

59 generated Catch2 tests (`[prove_resume]` tag) cover every valid
(DegradeShape, ResumeFailureClass) combination.

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

Six resume failure classes:

- **SUCCESS** — clean resume on first attempt.
- **R1** (`io_resume_truncate`) — truncation fails, stays degraded.
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
   respectively — 7 combinations filtered.
2. **R2 and CASCADE require an unsealed file.** The degrade_H shapes
   seal the active file during rotation before the fault fires, so
   `resume()` never enters the truncate/sync/seal block and the sync
   fault point is unreachable — 6 more combinations filtered.

12 shapes × 6 classes = 72 minus 13 filtered = **59 tests**.

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

### vacuum_compact — 56 tests

56 generated Catch2 tests (`[prove_vacuum_compact]` tag) cover eight state
shapes × seven failure classes (SUCCESS, VC1–VC6).

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
VC6 (`io_vacuum_compact_post_rename`).

VC4 is the most critical: the tmp file is fully synced and renamed
(a new `.data` file exists on disk) but `vacuum_commit` has not run —
the old file is still in the published state. `assert_vacuum_recoverable`
confirms that recovery does not replay the orphaned new file as a
secondary source and sees only the data the old file guaranteed.

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
completed and the process did not get to confirm it. Nothing is committed,
so in memory the outcome is a failed vacuum (`assert_vacuum_no_change`), and
on disk the compacted copy sits under its final name, referenced by nothing
in the published state. The cell records that orphan's path before closing
and checks the next open deleted it, on top of what
`assert_vacuum_recoverable` proves for every class. Making recovery's undo
throw instead fails all eight VC6 cells, and all eight VC5 cells with them.

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
- a cold open under the default `fail_recovery_on_crc_errors` refuses as
  well, wherever the format lets it see the damage (below), and leaves the
  file untouched.

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

The delta is the same shape in every cell: refuse, stay degraded, touch no
byte.

#### What a cold open can and cannot see

| Field | `resume()` | Cold open |
|-------|-----------|-----------|
| `crc` | refuses | refuses — the entry fails verification |
| `entry_type` | refuses | refuses — the entry does not parse |
| `value_size` | refuses | cannot tell it from the end of the file |
| `sequence` | refuses | cannot tell it from the end of the file |

An entry that claims to run past the end of the file and a zeroed sequence
both read as the end of written data, which is also what the zero-filled,
unwritten tail of a crashed active file looks like. A hint-less file
records no committed length, so a cold open has nothing to tell the two
apart with and trims the file to the last entry it could parse. `resume()`
can tell because the published state knows the extent. The `value_size`
and `sequence` cells therefore assert the `resume()` refusal only; closing
the gap at open needs the committed length on disk, which is a format
change.

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
| `crash_hintless` | none needed | The file that was active at shutdown. A clean close already leaves it hint-less — `flush_hints` skips `active_file_id` — so this *is* the shape a crash produces, and the one `recovery_prepare_files` regenerates from |
| `multi_file_hintless` | every hint removed | Several sealed files, none with a hint; all must be rebuilt |
| `damaged_hint` | newest hint's CRC broken | `open_hint_or_rebuild` must discard it and rebuild from the data file rather than drop the keys behind it |
| `hintless_batched` | none needed | `BulkBegin`/`BulkEnd` have to survive regeneration — a hint carries them so recovery can compute `durable_seq` across a batch |
| `hintless_range_del` | none needed | A range tombstone has to survive it too; recovery reads it back out of the regenerated hint to suppress the range |

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
| `vacuumed_batches` | 256 | Yes | No | changes_since over vacuumed file yields BulkBegin/BulkEnd markers; ingest writes atomically |

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
| M_R (`rotation_fails`) | No — exception thrown | Unchanged |
| M_H (`hint_generation_fails`) | No — exception or timeout | Active file was rotated (sealed) |

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
| `scenario_matrix.py` | CompactStateShape (low_fragmentation, mostly_dead, batched_file, range_tombstone_file), VacuumCompactFailureClass |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: threw/file_removed outcome |
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
| [`invariants.h`](../tests/proof/invariants.h) | `capture_baseline`, `assert_consistent`, `assert_delta`, `assert_recoverable`, `assert_resumable`, `assert_keys_recoverable`, `VacuumBaseline`, `capture_vacuum_baseline`, `find_vacuum_target`, `count_structural_entries`, `assert_structural_entries_preserved`, `assert_vacuum_success`, `assert_vacuum_no_change`, `assert_vacuum_recoverable`, `OwnedEntries`, `collect_changes`, `ReplicationBaseline`, `capture_replication_baseline`, `assert_replication_match`, `assert_replication_no_change`, `assert_replication_recovery`, `assert_durable_boundary` |

### Fault injection modes

The four `ScopedFaultInjector` modes map failure classes to the four
I/O checkpoints:

- **Name-based** — targets a single checkpoint. Used for B1, F, G, H.
- **Count-based** — fails from checkpoint N onward, cascading. Used for C.
- **Post-write mode** — fires at `io_data_file_append_partial` with
  `short_write` or `throw_after`. Used for B2, B3.

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
- `unreferenced_data_files(db, dir)` — the `.data` files on disk that the
  published state does not reference; VC6 uses it to name the orphan a
  post-rename kill leaves, and to check the next open deleted it.
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

12 test cases (`[invariants]` tag) in `tests/invariants_test.cpp`
smoke-test the helpers themselves.

### Test coverage

All nine failure classes for `apply_batch` (SUCCESS, A, B1, B2, B3, C,
F, G, H) are covered by the 1873 `[prove]` tests. Each class is exercised
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

All seven vacuum_compact classes (SUCCESS, VC1–VC6) across all eight state
shapes are covered by the 56 `[prove_vacuum_compact]` tests.

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
| Radix tree refcount | `atomic<uint32_t>` intrusive refcount |
| Edit tag counter | `atomic<uint64_t>` relaxed fetch_add |

Run: `scripts/run_sanitizer.sh thread` (or `address` for ASan, `memory` for MSan).

### MemorySanitizer (MSan)

The full test suite runs clean under Clang MemorySanitizer with
`-fsanitize-memory-track-origins=2` (full allocation-site origin tracking).
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
jobs above: `bytecask_tests` only, not `radix_tree_memory_tests` or
`unordered_view_tests`. Trigger scope does not — origin tracking makes the
MSan test step the slowest of the three and by far the least predictable
(2m48s and 11m56s on two runs of the same commit, against a steady ~1m50s
for ASan and TSan; per-job runners vary by ~1.8x and the seeded `[model]`
workloads account for much of the rest), so it is excluded from the
`pull_request` matrix and runs on push to `main` and on
`workflow_dispatch`.

### Fuzz testing (libFuzzer)

Two buffer-level fuzz harnesses exercise the parser code that handles
adversarial input on recovery — the one code path where untrusted bytes
from bad hardware or corruption reach the engine.

**`fuzz_data_entry`** — feeds arbitrary bytes to
`data_entry::deserialize_entry(span)`. Exercises header parsing, size
validation, and CRC checking. 3.7 M executions/minute on seed corpus.

**`fuzz_hint_entry`** — feeds arbitrary bytes to
`hint_entry::deserialize_entry(span, key_buf)` in a sequential loop
simulating `Scanner::next`. Exercises prefix compression key
reconstruction and length validation across multiple entries. 1.9 M
executions/minute on seed corpus.

Both harnesses run with `-fsanitize=fuzzer,address` (libFuzzer + ASan).
Seed corpus files (`tests/fuzz/seed/`) are committed; evolving corpus
(`tests/fuzz/corpus/`) is gitignored.

Run: `scripts/run_fuzz.sh fuzz_data_entry 300` (5-minute run).

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
from its data file. Both need the power-loss follow-up (`dm-flakey`,
`dm-log-writes`, or a FUSE layer that drops unsynced writes).

What it did catch: #166, `vacuum()` dropping a file with
`live_bytes == 0` along with its tombstones, so an older `Put` came back on
reopen. The harness failed on it about once every ten iterations. Since the
fix (#171, which counts tombstones in `FileStats` and keeps any file that
holds one), 400 iterations with vacuum running pass.

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
`verify_checksums`, `staleness_tolerance`, key and value limits with
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
cannot. `tests/soak_mutations/` holds one-line mutations of the
engine's synchronization, and `scripts/soak_mutation_check.sh <san>
[seconds]` applies each, runs the soak under that sanitizer, and fails
if a mutation expected to be caught survives.

| Mutation | Result |
|---|---|
| `publish_before_fdatasync` — the commit flush publishes before its `fdatasync` | caught |
| `resume_without_write_barrier` — `resume()` without its `WriteBarrier` | caught |
| `read_cache_release_relaxed` — `ReadCacheSlot::release` store weakened to relaxed | caught, under TSan (a data race); ASan cannot see it |
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

---

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
modes that matter in practice.

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
  at the `writev` boundary, not the sector boundary.
- **Hardware-level fault injection** — kernel block-layer error injection
  (`dm-flakey`, `dm-dust`), power-cut testing rigs, or filesystem-
  specific fault tools. The fault injector operates at the application
  syscall layer only. The process-crash harness kills the process at
  arbitrary points, but the page cache survives it, so it does not stand
  in for power loss either.
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
- Persistent radix tree (relax keys-in-memory requirement)
- Alternative index structures
- Alternative thread models
- External contributors

Each experiment either clears the baseline or it does not.

---

## Property-Based Validation of the Independence Assumption

### The assumption

The proof matrix makes a structural independence assumption: the
correctness of a transition depends only on operation types and their
ordering, not on the specific key bytes, value bytes, or value sizes.
Every test uses one fixed set of symbolic values (`"new0"`, `"v0"`,
etc.) across all 800 matrix cells. If some code path accidentally
depends on value content — a length-dependent branch, a key that
collides with an internal sentinel, or a value size that crosses a
buffer boundary — the current matrix would not catch it.

### Approach

The delta model produces 18 structurally distinct deltas across 800
matrix cells. These collapse further:

- 2 "empty deltas" (nothing written) — failure cases. No value
  independence to validate.
- 4 "remove-only deltas" — no new values written. Value independence
  is trivially true, but **key-byte independence** is testable.
- 12 "value-writing deltas" — these come in 6 SUCCESS/H pairs
  (identical except `degraded`/`threw` flags).

The real cardinality for property-based testing is:

| # | Delta shape | seq_advance | Representative plan |
|---|-------------|-------------|---------------------|
| 1 | 1 added, 1 value | 1 | `single_put` |
| 2 | 1 added, 1 value | 4 | `causality_overwrite` (last-write-wins) |
| 3 | 1 added, 1 value | 5 | `causality_put_del_put` |
| 4 | 1 added + 1 removed, 1 value | 4 | `mixed_batch` |
| 5 | 2 added, 2 values | 4 | `multi_put` |
| 6 | 3 added, 3 values | 5 | `large_batch` |

Plus 2 remove-only deltas for key-byte independence:

| 7 | 0 added, 1 removed | 1 | `single_delete` |
| 8 | 0 added, 1 removed | 4 | `causality_put_del` |

**8 property tests** total.

### What each property test does

For each distinct delta:
1. Generate random key bytes and value bytes (varying lengths,
   binary content, edge sizes near buffer boundaries).
2. Run the corresponding plan against the real engine.
3. Assert the structural delta matches `expected_delta`'s prediction
   — same key membership, same seq_advance, same causal ordering.

This directly tests: "for a given plan shape and failure class, the
engine produces the same structural delta regardless of what concrete
bytes the keys and values contain."

### Scope

- Cover SUCCESS for all 8 deltas. Optionally cover class H for
  deltas 1–6 (the engine takes a different code path: write commits
  but rotation fails). That would add 6 more for 14 total.
- Use Hypothesis (Python) driving the C++ engine through the Python
  bindings, or a C++ property framework (rapidcheck) if bindings
  don't cover the needed surface.
- Value size ranges should include: empty, 1 byte, typical (64 B),
  near page boundary (4095/4096/4097 B), and near max_value_bytes.
- Key size ranges should include: 1 byte, typical, and near
  max_key_bytes.

### What this does NOT replace

The deterministic proof matrix remains the primary validation. It
exhausts the behavioral space (all failure classes × all plan shapes ×
all state shapes). The property tests complement it by exhausting the
value space for each behavioral equivalence class — validating the
assumption that lets the deterministic matrix use symbolic values.

### Implementation

8 Hypothesis property tests in
`tests/proof/test_independence.py` cover SUCCESS for all
8 delta shapes. Each test generates random key bytes (1 B to 4096 B)
and value bytes (0 B to 16 KiB) including null bytes and
page-boundary-adjacent sizes, executes the corresponding plan, and
asserts:

1. Key membership matches the delta prediction.
2. Values read back identically.
3. Sequence advance matches `n + (2 if n > 1 else 0)`.
4. Engine is not degraded.
5. Recovery (close + reopen) preserves all keys and values.

Class H coverage (fault injection during rotation) is deferred — the
Python bindings do not expose `ScopedFaultInjector`. A future
rapidcheck-based C++ implementation could cover the H path.

Run: `PYTHONPATH=bytecaskdb-python pytest tests/proof/test_independence.py`
