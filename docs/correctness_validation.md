# ByteCaskDB — Correctness Validation

> This document describes the correctness validation framework for
> ByteCaskDB's write path. It should be read alongside
> [`CONTRACT.md`](../CONTRACT.md) and the
> [project plan](bytecask_project_plan.md).

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

### apply_batch — 492 tests

492 generated Catch2 tests (`[prove_apply_batch]` tag) cover every valid
(StateShape, PlanShape, FailureClass) combination for `apply_batch`.
The scenario matrix is 5 state shapes × 17 plan shapes × 9 failure
classes; 4 elimination rules reduce this to 492 valid tests.

#### State shapes

| Shape | `num_keys` | `max_file_bytes` | What it tests |
|-------|-----------|-----------------|---------------|
| `empty_db` | 0 | — | Plan applied to a fresh database with no existing keys |
| `single_key` | 1 | — | Overwrites, deletes, and guards against a single existing key |
| `populated_db` | 10 | — | Multiple existing keys; exercises range guards and multi-key interactions |
| `rotation_threshold` | 1 | 1 | Forces file rotation after the plan's writes — required for classes G and H |
| `deleted_key` | 1 (deleted) | — | k0 created then deleted — tombstone in write history, no live keys at baseline |

Shapes not currently covered: mid-vacuum (vacuum-in-flight during `apply_batch`),
post-resume (DB that has been degraded and resumed), and multiple sealed files with
cross-file tombstone interactions. These are deferred — the current shapes cover the
five structurally distinct starting conditions for the write path.

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
delete-only multi-entry batches, plans exceeding the `256 KiB` solo-writer
threshold, and causality shapes with `del_range` (covered by manual tests in
`bytecask_test.cpp`). These are deferred — the current shapes exercise every
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

### resume() — 21 tests

21 generated Catch2 tests (`[prove_resume]` tag) cover every valid
(DegradeShape, ResumeFailureClass) combination.

Four degrade shapes establish a degraded DB before resume is called:

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

Six resume failure classes:

- **SUCCESS** — clean resume on first attempt.
- **R1** (`io_resume_truncate`) — truncation fails, stays degraded.
- **R2** (`io_resume_sync`) — sync fails, stays degraded.
- **R3** (`io_resume_file_creation`) — new active file creation fails.
- **DOUBLE** — resume succeeds, then a second resume is called (no-op).
- **CASCADE** — R2 fails, then R3 fails, then clean resume succeeds.

R1 is filtered for degrade_H, degrade_F, and degrade_G: these shapes
have no orphaned or partial bytes in the active file, so
`file.size() == valid_offset` and `resume()` skips the truncation
branch entirely (see `bytecask.cpp` resume path: `if (file.size() !=
valid_offset) { ... truncate ... }`). R1 is only valid for degrade_C.

4 shapes × 6 classes = 24 minus 3 filtered = **21 tests**.

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

### vacuum_compact — 10 tests

10 generated Catch2 tests (`[prove_vacuum_compact]` tag) cover two state
shapes × five failure classes (SUCCESS, VC1–VC4).

State shapes create a DB with exactly one sealed file having fragmentation > 0:

- **low_fragmentation** — sealed file with 2 entries, 1 dead (50% frag)  
- **mostly_dead** — sealed file with 6 entries, 5 dead (~83% frag)

Files with live entries always use the compact path (sealed→sealed).
`mostly_dead` uses `max_file_bytes=150` to pack all 6 keys into one
sealed file.

Failure classes: SUCCESS, VC1 (`io_vacuum_compact_tmp_create`),
VC2 (`io_data_file_append`), VC3 (`io_data_file_sync`),
VC4 (`io_vacuum_compact_rename`).

VC4 is the most critical: the tmp file is fully synced and renamed
(a new `.data` file exists on disk) but `vacuum_commit` has not run —
the old file is still in the published state. `assert_vacuum_recoverable`
confirms that recovery does not replay the orphaned new file as a
secondary source and sees only the data the old file guaranteed.

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
| `incremental` | Each chunk produces a valid prefix; follower.current_sequence() advances monotonically |
| `restart_midstream` | Recovery equivalence — reopened follower's current_sequence() is trustworthy, next changes_since picks up without gaps |
| `duplicate_delivery` | Idempotency — entries with sequence <= current_sequence() are silently skipped |
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
| `scenario_matrix.py` | DegradeShape (H, C, F, G), ResumeFailureClass (SUCCESS, R1–R3, DOUBLE, CASCADE), validity filter |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: keys present/absent after all resume calls |
| `generate_tests.py` | Generates `prove_resume.cpp` |

**vacuum_compact module** (`tests/proof/vacuum_compact/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | CompactStateShape (low_fragmentation, mostly_dead), VacuumCompactFailureClass |
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
| [`invariants.h`](../tests/proof/invariants.h) | `capture_baseline`, `assert_consistent`, `assert_delta`, `assert_recoverable`, `assert_resumable`, `assert_keys_recoverable`, `VacuumBaseline`, `capture_vacuum_baseline`, `find_vacuum_target`, `assert_vacuum_success`, `assert_vacuum_no_change`, `assert_vacuum_recoverable`, `OwnedEntries`, `collect_changes`, `ReplicationBaseline`, `capture_replication_baseline`, `assert_replication_match`, `assert_replication_no_change`, `assert_replication_recovery`, `assert_durable_boundary` |

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
- `assert_delta(before, db, expected)` — validates key membership, LSN
  advancement, structural consistency, and degraded state against the
  reference model's expected delta.
- `assert_resumable(db)` — calls `resume()` and verifies the engine clears
  the degraded flag and passes `assert_consistent`. Inserted immediately
  after `assert_delta` for all degraded failure classes (B1, B2, B3, C, F, G, H).
- `assert_recoverable(dir, before, expected)` — opens a fresh DB from
  disk and verifies the recovered state matches the expected state
  (pre-existing keys survive, added keys present, removed keys absent,
  no extra keys, structural consistency).
- `assert_keys_recoverable(dir, keys_present, keys_absent)` — lighter
  recovery check used by resume proof tests: opens a fresh DB and
  checks specific keys present/absent plus `assert_consistent`.
- `VacuumBaseline` / `capture_vacuum_baseline(db)` — snapshots key-values,
  `next_lsn`, and per-file `FileStats` before a vacuum operation.
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

All eleven failure classes for `apply_batch` are covered by the 172
`[prove_apply_batch]` tests. Each class is exercised across all valid
(StateShape, PlanShape) combinations.

All three resume failure classes (R1–R3) plus DOUBLE and CASCADE across
all four degrade shapes (H, C, F, G) are covered by the 21
`[prove_resume]` tests. R1 is correctly excluded for degrade_H, degrade_F,
and degrade_G (no orphaned bytes to truncate — fault point unreachable).

All five vacuum_compact failure classes across both state shapes are
covered by the 10 `[prove_vacuum_compact]` tests.

All seven ingest failure classes across 11 state shapes and 5 ops shapes
are covered by the 178 `[prove_repl]` tests. All three manifest failure
classes across 11 state shapes are covered by the 33 `[prove_manifest]`
tests. Four elimination rules reduce the full matrix to 211 valid tests.

Total generated proof tests: **734**.

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

Run: `scripts/run_sanitizer.sh thread` (or `address` for ASan).

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

If `rename()` completes but the process crashes before confirming,
a `.data` file may exist on disk unreferenced by the published
`EngineState`. The original file remains in the published state and
is fully readable. No data is lost.

Recovery detects `.data` files not referenced by any hint file and
not registered in the engine state, and removes them as orphans.

This adds a failure sub-class to the vacuum compact model:

```
Class G — rename completes but process does not confirm
          old file remains in published state (correct)
          new .data file exists as orphan on disk
          next recovery detects and removes orphan
          DB remains operational
          no data is lost
```

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
  syscall layer only.

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
