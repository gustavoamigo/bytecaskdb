# ByteCaskDB — Correctness Validation Plan

> **Status**: Phases 1–3 complete. The single write path, transient/persistent
> state discipline, behavioral contract, fault injection framework,
> `DbPoisoned`, the `invariants.h` test helpers, and the Python test
> generator are implemented and tested. 172 generated proof tests cover
> every valid (StateShape, PlanShape, FailureClass) combination for
> `apply_batch_if`. 382 total tests pass. The CI model-diff step
> (Phase 4) is not yet configured.
>
> This document should be read alongside [`CONTRACT.md`](../CONTRACT.md) and
> the [project plan](bytecask_project_plan.md).

---

## Conceptual Model

The append-only data file is a persistence mechanism for `EngineState`
transitions. The database *is* `EngineState`. The file exists only to
make `EngineState` recoverable after a crash.

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

## Current State — What Is Built

The foundation for the formal validation model is in place. These are
the building blocks that the prove infrastructure will exercise.

### Single write path (BC-128, done)

All mutations — `put`, `del`, `apply_batch`, `apply_batch_if` — route
through `apply_batch_if` as the single coordinator. The two-phase
discipline is enforced: I/O first (`DataFile::append`), then pure
in-memory state mutations (`TransientEngineState::apply_writes`), then
publish (`TransientEngineState::persistent()`).

`TransientEngineState` is the mutable working copy of engine state.
It provides:
- `validate_preconditions(plan)` — pure read, checks guards and W-W conflict
- `prepare_write(plan)` — assigns LSNs, inserts BulkBegin/BulkEnd markers
- `apply_writes(plan, offsets)` — pure in-memory mutations after I/O succeeds
- `persistent()` — commits back to immutable `shared_ptr<EngineState>`

If I/O fails, the transient copy is discarded — the published state
is never modified.

### Behavioral contract (done)

[`CONTRACT.md`](../CONTRACT.md) defines the guarantees for
`apply_batch_if`, `vacuum_compact`, `vacuum_absorb`, and hint files.
It is the source of truth for both the implementation and the validation
model.

### Fault injection framework (BC-132, done)

`bytecaskdb/fault_injector.h` provides:
- `FaultInjector` with name-based and count-based injection, plus a
  count-based skip set for selective cascade control
- `ScopedFaultInjector` RAII guard with three constructor forms:
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

### Orphaned BulkBegin rotation (BC-131, done)

If a multi-entry batch fails mid-write after `BulkBegin`, the engine
force-rotates to isolate the orphaned marker. Without this, subsequent
writes to the same file would be silently discarded on recovery.

### Sync failure publish-before-rethrow (BC-133, done)

If `fdatasync` fails after all appends succeed, the engine publishes
the new state before rethrowing. This prevents duplicate LSNs on the
next write.

### Existing test coverage

- **25 unit tests** for `TransientEngineState::validate_preconditions` —
  pure, no DB, no disk I/O. Covers all guard types, range guards,
  W-W conflict detection, combined scenarios, snapshot-less plans.
- **10 fault injection tests** covering all failure classes:
  - BC-131: mid-batch append failure rotates file and discards partial
    batch (count-based injection)
  - BC-133: sync failure publishes state before rethrow (name-based)
  - BC-134: isolation rotation failure poisons DB (count-based cascade);
    reads work on poisoned DB; single-entry failure does not poison
  - BC-135: partial write on single-entry poisons (short_write and
    throw_after modes); multi-entry batch with partial write isolates
    correctly
  - BC-136: rotation sync failure publishes state without poison;
    rotation file creation failure poisons but preserves writes
  - BC-137: isolation sync failure poisons DB independently
    (count-based with skip set)
- **12 invariant helper tests** (`[invariants]` tag) — validate
  `assert_consistent`, `assert_delta`, `assert_recoverable`, and
  `capture_baseline` across empty, populated, deleted, overwritten,
  batched, poisoned, and recovery scenarios.

---

## What Is Not Built

### CI model-diff step

A CI step that regenerates tests and verifies no diff. Not yet
configured.

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
  F   — an I/O failure class (one of A..F, or SUCCESS)

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

Every I/O failure in `apply_batch_if` falls into exactly one structural
class. The class determines the expected delta — not which specific
operation failed, not what the key bytes were.

```python
class FailureClass(Enum):
    SUCCESS = "success"               # no failure — transition fully persisted
    A  = "before_any_io"              # conflict check fails — no I/O attempted
    B1 = "append_fails_nothing_written"   # writev returns -1, no bytes on disk
    B2 = "append_fails_partial_write"     # writev returns short, file tainted
    B3 = "append_fails_after_full_write"  # writev ok, post-write fault, file tainted
    C  = "on_bulk_end_append"         # BulkEnd write fails — orphan isolation
    D  = "isolation_sync_fails"       # sync during orphan isolation fails — poison
    E  = "isolation_rotation_fails"   # rotation during orphan isolation fails — poison
    F  = "commit_sync_fails"          # sync in commit phase — persisted, throws
    G  = "rotation_sync_fails"        # post-write rotation sync fails — persisted, throws
    H  = "rotation_file_creation_fails"  # post-write rotation fails after seal — poison
```

### Write outcome subclasses (B1, B2, B3)

The original class B ("append fails mid-batch") assumed `writev` either
fully succeeds or fails before writing anything. In reality, `writev`
can produce three outcomes:

- **B1 — nothing written**: `writev` returns -1. No bytes on disk.
  `offset_` is unchanged, kernel fd position is unchanged. Safe.
- **B2 — partial write**: `writev` returns 0 < N < total. N bytes are
  on disk, kernel fd position advanced by N. `offset_` is not advanced
  (the throw in `append()` skips `offset_ +=`). The file is `tainted`.
- **B3 — full write + failure**: `writev` returns total (all bytes on
  disk), but the caller throws before `offset_` advances. Only possible
  via fault injection (`PostWriteMode::throw_after`). The file is
  `tainted`.

For multi-entry batches, B2 and B3 are safe: the isolation rotation
moves to a new file, abandoning the tainted one. For single-entry
writes, there is no isolation — `offset_` diverges from the kernel's
file position (the file was opened with `O_APPEND`). The next `writev`
would write at the kernel's true EOF (past the stale bytes), but
`offset_` would still point to the old position. The returned offset
for subsequent entries would be wrong — silent corruption.

The fix: `DataFile::append()` sets `tainted_ = true` when `writev`
returns a short write (`written > 0`). In the `apply_batch_if` catch
block, if `!multi && file.is_tainted()`, the engine poisons the DB.
Recovery (process restart) clears the state.

### Post-write rotation failure subclasses (G, H)

After all appends succeed and in-memory state is applied, the engine
checks whether the active file has reached the rotation threshold. If
so, it syncs the file and rotates to a new one. Two distinct failures
can occur at this point:

- **G — rotation sync fails**: The pre-rotation `fdatasync` fails.
  The file is not sealed. The transition is fully persisted (appends
  succeeded), so the state is published and the exception is rethrown.
  The DB is not poisoned — the next write will retry rotation or
  continue appending to the same file.
- **H — rotation file creation fails**: The sync succeeded but
  `rotate_active_file` fails after sealing the active file. The sealed
  file cannot accept further appends (`assert(!sealed_)` would fire).
  The engine poisons the DB and publishes the state (writes are on disk,
  LSNs must advance). Recovery (process restart) clears the sealed file
  problem by creating a fresh active file.

Key insight: `rotate_active_file` calls `seal()` before creating the new
file. If file creation fails, the active file is sealed and unusable.
Publishing state without poisoning would leave an engine that appears
healthy but fails on the next append. Poisoning is the correct response.

### Class Behavior Summary

| Class | Transition persisted | key_dir changes | LSN advances | Throws | Poisoned |
|-------|---------------------|-----------------|--------------|--------|----------|
| SUCCESS | Yes — fully | Yes — full delta | Yes | No | No |
| A | No — not attempted | No | No | No (returns false) | No |
| B1 | No — nothing written | No | No | Yes | No |
| B2 (multi) | No — partial write | No | No | Yes | No (isolation rotates) |
| B2 (single) | No — partial write | No | No | Yes | Yes (tainted) |
| B3 (multi) | No — full write | No | No | Yes | No (isolation rotates) |
| B3 (single) | No — full write | No | No | Yes | Yes (tainted) |
| C | No — partial write | No | No | Yes | Yes |
| D | No — partial write | No | No | Yes | Yes |
| E | No — partial write | No | No | Yes | Yes |
| F | Yes — fully | Yes — full delta | Yes | Yes | No |
| G | Yes — fully | Yes — full delta | Yes | Yes | No |
| H | Yes — fully | Yes — full delta | Yes | Yes | Yes |

Note: Classes F and G are the cases where the transition is fully persisted
but the caller receives an exception. The state is published before the
exception is rethrown. Class H is similar — the transition is persisted —
but the engine is also poisoned because the sealed active file cannot
accept further appends.

### Current test coverage by failure class

| Class | Covered | How |
|-------|---------|-----|
| SUCCESS | Yes | All existing engine tests exercise this path |
| A | Yes | 25 `validate_preconditions` unit tests (BC-129) |
| B1 | Partial | 1 fault injection test (BC-131) — count-based, not name-based |
| B2 (single) | Yes | `[partial_write]` test — `PostWriteMode::short_write` (BC-135) |
| B2 (multi) | Yes | `[partial_write]` test — multi-entry batch with isolation (BC-135) |
| B3 (single) | Yes | `[partial_write]` test — `PostWriteMode::throw_after` (BC-135) |
| C | No | Requires count-based injection targeting `BulkEnd` specifically |
| D | Yes | BC-137 `[poisoned]` test — count-based with skip set isolates sync failure (BC-134 cascade also covers D+E combined) |
| E | Yes | BC-134 `[poisoned]` test — count-based cascade through `io_rotate_file_creation` |
| F | Yes | 1 fault injection test (BC-133) |
| G | Yes | BC-136 `[rotation_failure]` — sync failure publishes state, no poison |
| H | Yes | BC-136 `[rotation_failure]` — file creation failure poisons, preserves writes |

---

## The Reference Model — `expected_delta`

A pure function written independently of the C++ implementation. It
defines what the transition should produce for every combination of plan
shape and failure class. This is the model everything is validated
against.

```python
@dataclass
class Delta:
    keys_added:   list        # keys that must appear in key_dir after transition
    keys_removed: list        # keys that must be absent from key_dir after transition
    lsn_advance:  int         # how much next_lsn must advance in published state
    stats_delta:  StatsDelta  # expected file_stats changes
    poisoned:     bool        # whether DB must be poisoned after transition
    threw:        bool        # whether caller must receive an exception


def expected_delta(P: PlanShape, F: FailureClass) -> Delta:
    """
    The reference model. Written independently of the C++ implementation.
    Defines what transition(S1, P, F) must produce.

    The binary rule:
      transition_fully_persisted(F) → full delta, recoverable
      not transition_fully_persisted(F) → empty delta, not recoverable
    """

    if F == FailureClass.SUCCESS:
        # Transition fully persisted and applied
        return Delta(
            keys_added   = [op for op in P.ops if op.type == Put],
            keys_removed = [op for op in P.ops if op.type == Delete],
            lsn_advance  = len(P.ops) + (2 if len(P.ops) > 1 else 0),
            stats_delta  = full_stats_delta(P),
            poisoned     = False,
            threw        = False,
        )

    elif F in {FailureClass.B1}:
        # Transition not fully persisted — must not be applied.
        # B1: writev returned -1, no bytes on disk. Safe.
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,       # published next_lsn unchanged
            stats_delta  = NoDelta,
            poisoned     = False,
            threw        = True,
        )

    elif F == FailureClass.C:
        # BulkEnd failed — partial batch on disk, isolation rotates.
        # Engine detects incomplete batch and poisons.
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,
            stats_delta  = NoDelta,
            poisoned     = True,
            threw        = True,
        )

    elif F in {FailureClass.B2, FailureClass.B3}:
        # Partial or full write on disk, offset_ not advanced.
        # Multi-entry: isolation rotation handles it — safe, not poisoned.
        # Single-entry: offset_ diverges from kernel fd — must poison.
        multi = len(P.ops) > 1
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,
            stats_delta  = NoDelta,
            poisoned     = not multi,  # single-entry taint → poison
            threw        = True,
        )

    elif F in {FailureClass.D, FailureClass.E}:
        # Transition not persisted and engine cannot recover safely
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,
            stats_delta  = NoDelta,
            poisoned     = True,    # engine must poison itself
            threw        = True,
        )

    elif F == FailureClass.F:
        # Transition fully persisted — state published before rethrow
        return Delta(
            keys_added   = [op for op in P.ops if op.type == Put],
            keys_removed = [op for op in P.ops if op.type == Delete],
            lsn_advance  = len(P.ops) + (2 if len(P.ops) > 1 else 0),
            stats_delta  = full_stats_delta(P),
            poisoned     = False,
            threw        = True,    # caller receives exception but write succeeded
        )

    elif F == FailureClass.G:
        # Post-write rotation sync fails — transition fully persisted,
        # file not sealed, state published, DB not poisoned.
        return Delta(
            keys_added   = [op for op in P.ops if op.type == Put],
            keys_removed = [op for op in P.ops if op.type == Delete],
            lsn_advance  = len(P.ops) + (2 if len(P.ops) > 1 else 0),
            stats_delta  = full_stats_delta(P),
            poisoned     = False,
            threw        = True,
        )

    elif F == FailureClass.H:
        # Post-write rotation file creation fails after seal —
        # transition fully persisted, state published, DB poisoned.
        return Delta(
            keys_added   = [op for op in P.ops if op.type == Put],
            keys_removed = [op for op in P.ops if op.type == Delete],
            lsn_advance  = len(P.ops) + (2 if len(P.ops) > 1 else 0),
            stats_delta  = full_stats_delta(P),
            poisoned     = True,
            threw        = True,
        )

    elif F == FailureClass.A:
        # No I/O attempted — transition not started
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,
            stats_delta  = NoDelta,
            poisoned     = False,
            threw        = False,   # returns false, no exception
        )
```

---

## The Scenario Matrix

The inputs to the generator. Structural descriptions only — no
concrete values.

```python
state_shapes = [
    StateShape(keys=0,  label="empty_db"),
    StateShape(keys=1,  label="single_key"),
    StateShape(keys=10, label="populated_db"),
    StateShape(keys=1,  label="rotation_threshold", at_rotation=True),
]

plan_shapes = [
    PlanShape(ops=[Put],               label="single_put"),
    PlanShape(ops=[Delete],            label="single_delete"),
    PlanShape(ops=[Put, Put],          label="multi_put"),
    PlanShape(ops=[Put, Delete],       label="mixed_batch"),
    PlanShape(ops=[Put, Put, Put],     label="large_batch"),
    PlanShape(ops=[Put], guards=True,  label="single_put_with_guards"),
    PlanShape(ops=[Put], conflict=True,label="conflicting_plan"),
]

failure_classes = list(FailureClass)  # all eleven classes
```

Total generated tests:

```
len(state_shapes) × len(plan_shapes) × len(failure_classes)
= 4 × 7 × 11
= 308 tests
```

Each test is deterministic, independent, and covers exactly one cell
in the matrix.

---

## The Generator

Takes the matrix and produces Catch2 test cases. One test per
`(S1, P, F)` combination.

```python
def generate_test(s1: StateShape, p: PlanShape, f: FailureClass) -> str:
    name     = f"prove__{s1.label}__{p.label}__{f.value}"
    expected = expected_delta(p, f)

    return f"""
TEST_CASE("{name}", "[prove]") {{
    // Setup — build initial state from structural description
    {generate_setup(s1)}

    // Capture baseline before transition
    auto before = capture_baseline(db);

    // Inject fault at the class boundary
    {generate_fault_injection(p, f)}

    // Execute transition
    bool threw  = false;
    bool result = false;
    try {{
        result = db.apply_batch_if(snap, {{}}, {generate_plan(p)});
    }} catch (const std::system_error&) {{
        threw = true;
    }}

    // Validate against expected delta
    CHECK(threw == {str(expected.threw).lower()});
    {"CHECK_FALSE(result);" if f == FailureClass.A else ""}
    {"CHECK(db.is_poisoned());"
        if expected.poisoned
        else "CHECK_FALSE(db.is_poisoned());"}
    assert_consistent(db);
    assert_delta(before, db, {generate_expected_delta(expected)});
    {"assert_recoverable(test_dir);" if not expected.poisoned else ""}
}}
"""
```

---

## The Fault Point Resolver

Maps a failure class to the concrete injection configuration using the
four existing checkpoints in `DataFile` and `rotate_active_file`:

- `io_data_file_append` — fires before every `writev()` (BulkBegin,
  data entries, BulkEnd all hit this checkpoint)
- `io_data_file_append_partial` — fires after `writev()` succeeds but
  before `offset_` advances (post-write checkpoint)
- `io_data_file_sync` — fires before every `fdatasync()` (isolation
  sync, rotation sync, and commit sync all hit this checkpoint)
- `io_rotate_file_creation` — fires after sealing, before new file
  creation

The three `ScopedFaultInjector` modes map failure classes to these
checkpoints:

- **Name-based** targets a single checkpoint by name — all other
  checkpoints pass. Used for B1 (single-entry), F, G, H.
- **Count-based** fails from checkpoint N onward, cascading through
  all subsequent operations. Used for C (BulkEnd), D+E (cascade).
- **Count-based with skip set** cascades but lets named checkpoints
  pass — isolates a specific failure within a cascade. Used for D
  (skip `io_rotate_file_creation` so rotation succeeds but sync fails).
- **Post-write mode** fires at `io_data_file_append_partial` with
  `short_write` or `throw_after`. Used for B2, B3.

```python
def fault_point_for_class(p: PlanShape, f: FailureClass) -> FaultConfig:
    """
    Maps a failure class to the concrete injection configuration
    using the four existing DataFile-level checkpoints.

    The checkpoint firing sequence for a multi-entry batch with N ops:
      io_data_file_append (BulkBegin)           — ckpt 1
      io_data_file_append (op 1)                — ckpt 2
      ...
      io_data_file_append (op N)                — ckpt N+1
      io_data_file_append (BulkEnd)             — ckpt N+2
      [if BulkEnd fails → isolation path:]
        io_data_file_sync (isolation sync)      — ckpt N+3
        io_rotate_file_creation (isolation rot) — ckpt N+4
      [if appends succeed → commit path:]
        io_data_file_sync (commit sync)         — ckpt N+3
        [if rotation needed:]
          io_data_file_sync (rotation sync)     — ckpt N+4
          io_rotate_file_creation (rotation)    — ckpt N+5
    """
    multi = len(p.ops) > 1
    n_appends = len(p.ops) + (2 if multi else 0)  # +2 for BulkBegin/End

    if f == FailureClass.SUCCESS:
        return FaultConfig()  # no fault injected

    if f == FailureClass.A:
        return FaultConfig()  # conflict triggers via plan setup, not injection

    if f == FailureClass.B1:
        # Name-based: fail at the first io_data_file_append
        return FaultConfig(name="io_data_file_append")

    if f == FailureClass.B2:
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode=PostWriteMode.short_write,
            short_write_bytes=5,
        )

    if f == FailureClass.B3:
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode=PostWriteMode.throw_after,
        )

    if f == FailureClass.C:
        # Count-based: let BulkBegin + all ops pass, fail on BulkEnd.
        # fail_at = N+1 means checkpoint N+2 (BulkEnd) is the first to throw.
        # This cascades through isolation sync and rotation.
        return FaultConfig(fail_at=n_appends - 1)

    if f == FailureClass.D:
        # Count-based with skip: fail mid-batch (triggers isolation),
        # sync also fails (count exceeded), but rotation passes (skipped).
        return FaultConfig(
            fail_at=len(p.ops),  # fail on last data op
            skip=["io_rotate_file_creation"],
        )

    if f == FailureClass.E:
        # Count-based: fail mid-batch, cascade through sync and rotation.
        return FaultConfig(fail_at=len(p.ops))

    if f == FailureClass.F:
        return FaultConfig(name="io_data_file_sync")

    if f == FailureClass.G:
        # Name-based: io_data_file_sync fires for all sync calls.
        # With max_file_bytes=1, the first sync after appends is rotation sync.
        # The commit-phase sync fires first in the normal path (step 7),
        # but rotation sync (step 6) fires before it when rotation is needed.
        return FaultConfig(name="io_data_file_sync")

    if f == FailureClass.H:
        return FaultConfig(name="io_rotate_file_creation")

    return FaultConfig()
```

---

## The `assert_delta` Helper

Called in every generated test. Verifies the actual delta matches
the expected delta from the model. To be implemented in `invariants.h`.

```cpp
void assert_delta(
    const Baseline&      before,
    const DB&            db,
    const ExpectedDelta& expected
) {
    auto after = db.engine_state();

    // Key membership — transition applied or not
    for (auto& key : expected.keys_added) {
        INFO("expected key added: " << key);
        CHECK(db.contains_key(to_bytes(key)));
    }
    for (auto& key : expected.keys_removed) {
        INFO("expected key removed: " << key);
        CHECK_FALSE(db.contains_key(to_bytes(key)));
    }

    // LSN advancement — transition identifier consumed or not
    CHECK(after->next_lsn == before.next_lsn + expected.lsn_advance);

    // Stats consistency with key_dir
    assert_stats_match_keydir(*after);

    // Poison state
    CHECK(db.is_poisoned() == expected.poisoned);
}
```

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

## Implementation Roadmap

Work items ordered by dependency. Each builds on the previous.

### Phase 1 — `DbPoisoned` and fault injection foundation ✅

Implemented across BC-131 through BC-137. The complete write-path
failure handling and fault injection infrastructure is in place:

- **BC-131**: Orphaned `BulkBegin` rotation — mid-batch append failure
  triggers isolation rotation to a fresh file.
- **BC-132**: `ScopedFaultInjector` with three injection modes
  (name-based, count-based, post-write) and four checkpoints:
  `io_data_file_append`, `io_data_file_append_partial`,
  `io_data_file_sync`, `io_rotate_file_creation`.
- **BC-133**: Sync failure publishes state before rethrowing.
- **BC-134**: `DbPoisoned` — isolation rotation failure poisons the
  DB. `deem_as_poisoned(reason)`, `is_poisoned()`, `poison_reason()`
  API. Reads remain available on poisoned DB.
- **BC-135**: Partial write detection — `PostWriteMode` (`short_write`,
  `throw_after`) and `FAULT_INJECTION_POST_WRITE` macro. Single-entry
  taint poisons; multi-entry taint handled by isolation.
- **BC-136**: Post-write rotation failure — sync failure defers
  rotation (not poisoned); file creation failure after seal poisons.
- **BC-137**: Isolation sync failure poisons the DB. Count-based
  skip set added to `ScopedFaultInjector` for selective cascade
  control. Class D tested independently.

All eleven failure classes (A through H) have test coverage.
198 tests pass.

**No additional coordinator-level checkpoints are needed.** The four
DataFile-level checkpoints, combined with the three `ScopedFaultInjector`
modes (name-based, count-based, count-based with skip set) and the
post-write mode, can target every failure class deterministically.
See "The Fault Point Resolver" section above for the mapping.

**Unblocks**: Phases 2 and 3.

### Phase 2 — `invariants.h` helpers ✅

Implemented in BC-138. `tests/proof/invariants.h` provides:

- `Baseline` / `ExpectedDelta` — data types for capturing pre-transition
  state and expressing the reference model's expected outcome.
- `capture_baseline(db)` — snapshots `next_lsn` and all key-value pairs.
- `assert_consistent(db)` — validates five structural invariants:
  live_bytes matches key_dir, no dangling file references, active file
  exists, file_stats covers all files, next_lsn ahead of all sequences.
- `assert_delta(before, db, expected)` — validates key membership, LSN
  advancement, structural consistency, and poison state against the
  reference model's expected delta.
- `assert_recoverable(dir, before, expected)` — opens a fresh DB from
  disk and verifies the recovered state matches the expected state
  (pre-existing keys survive, added keys present, removed keys absent,
  no extra keys, structural consistency).

12 test cases (`[invariants]` tag) in `tests/invariants_test.cpp`
validate the helpers. 210 total tests pass.

**Depends on**: Phase 1.

**Unblocks**: all generated tests.

### Phase 3 — Python test generator ✅

Implemented in BC-139. All four Python modules and the generated C++
test file are in place:

| File | Purpose |
|------|---------|
| `tests/proof/scenario_matrix.py` | 4 state shapes × 7 plan shapes × 11 failure classes; validity filter yields 172 combinations |
| `tests/proof/expected_delta.py` | Pure reference model: `expected_delta(plan, failure, ...)` → `Delta` |
| `tests/proof/fault_point_resolver.py` | Maps (state, plan, failure) → `FaultConfig` for `ScopedFaultInjector` |
| `tests/proof/generate_tests.py` | Generator: produces `prove_apply_batch_if.cpp` (172 Catch2 `[prove]` tests) |
| `tests/proof/generated/prove_apply_batch_if.cpp` | Committed evidence — never hand-edited |

**Scope**: `apply_batch_if` only (no vacuum). Invalid combinations
are silently skipped (4 elimination rules reduce 308 → 172 tests).

Every test follows the same structure:
1. Set up initial DB state from `StateShape`
2. Capture baseline
3. Construct `WritePlan` from `PlanShape`
4. Inject fault per `FaultConfig`
5. Execute `apply_batch_if`
6. `assert_delta(before, db, expected)` — validates key membership,
   LSN advancement, structural consistency, and poison state
7. `assert_recoverable(dir, before, expected)` — validates persistence
   invariant via fresh recovery (where applicable)

Regeneration (`python3 tests/proof/generate_tests.py`) is deterministic
and produces no diff. 382 total tests pass (210 existing + 172 prove).

**Depends on**: Phases 1–2.

**Findings during implementation**: Class C (`on_bulk_end_append`) was
originally modeled as non-poisoning. The proof tests revealed that the
engine poisons on BulkEnd failure (partial batch on disk triggers
poison). The reference model and behavior summary table were corrected.

**Unblocks**: CI model-diff step (Phase 4).

### Phase 4 — CI model-diff step

Add a CI step that regenerates the prove tests and verifies no diff.
A diff means either the model or the generator changed — both must be
intentional and reviewable.

**Depends on**: Phase 3.

---

## Output Structure

```
tests/
  proof/
    generate_tests.py          ← the generator — owns the model
    expected_delta.py          ← the reference function — owns the contract
    fault_point_resolver.py    ← maps classes to injection configurations
    scenario_matrix.py         ← the input matrix
    invariants.h               ← assert_consistent(), assert_delta() ✅
    generated/
      prove_apply_batch_if.cpp    ← generated, never hand-edited
      prove_vacuum_compact.cpp    ← generated, never hand-edited
      prove_vacuum_absorb.cpp     ← generated, never hand-edited
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

---

## Why This Approach

The failure classes reduce an infinite problem space to a finite
and tractable one. What matters is not which specific operation
failed or what the key bytes were — it is which phase boundary the
failure crossed, which determines whether the transition was fully
persisted or not. Eleven classes cover the entire behavioral space of
`apply_batch_if` under I/O failure.

The Python generator makes the model explicit, auditable, and
evolvable. When a new failure class is identified — for example,
when the WriteGroup is reintegrated — it is added to the matrix
and all existing state shapes and plan shapes are automatically
covered against it.

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
