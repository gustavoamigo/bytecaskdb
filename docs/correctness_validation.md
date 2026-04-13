# ByteCaskDB — Correctness Validation Plan

> **Status**: Foundation complete. The single write path, transient/persistent
> state discipline, behavioral contract, and basic fault injection are
> implemented and tested. The formal validation model (Python test generator,
> `DbPoisoned`, exhaustive prove tests) is designed but not yet built.
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

### Fault injection framework (BC-132, partial)

`bytecaskdb/fault_injector.h` provides:
- `FaultInjector` with name-based and count-based injection
- `ScopedFaultInjector` RAII guard for test safety
- `io_checkpoint(name)` called at I/O boundaries
- `FAULT_INJECTION(name)` macro, compiled under `BYTECASK_TESTING`
- Thread-local `active_injector` pointer

Two checkpoints exist in the production code:
1. `io_data_file_append` — in `DataFile::append()`
2. `io_data_file_sync` — in `DataFile::sync()`

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
- **2 fault injection tests** — BC-131 (mid-batch append failure
  rotates file and discards partial batch, count-based injection) and
  BC-133 (sync failure publishes state before rethrow, name-based
  injection).

---

## What Is Not Built

### `DbPoisoned` / `is_poisoned`

The contract defines poisoning (see [CONTRACT.md §Rotation Safety](../CONTRACT.md)):
if the isolation rotation fails after an orphaned `BulkBegin`, the
engine must poison itself. A poisoned DB refuses all write operations
(`put`, `del`, `apply_batch`, `apply_batch_if`, `vacuum`) with a
`DbPoisoned` exception. Read operations (`get`, `contains_key`,
`snapshot`, iterators) remain available — the in-memory state was
rolled back correctly and agrees with what recovery would produce.

Currently the engine catches rotation/sync failures in the isolation
path silently. There is no `poisoned_` member, no `is_poisoned()`
method, no `DbPoisoned` exception class.

**Required before**: failure classes D and E can be validated.

### Fine-grained fault checkpoints

The plan requires checkpoints at each failure class boundary. The
current two checkpoints (`io_data_file_append`, `io_data_file_sync`)
cannot distinguish between a failure on `BulkBegin` append vs.
`BulkEnd` append without relying on count-based injection.

Named checkpoints needed:
- `bulk_begin_append` — first marker in a batch
- `op_N_append` — each data entry in a batch
- `bulk_end_append` — closing marker
- `isolation_sync` — sync during orphan isolation
- `isolation_rotation` — file rotation during orphan isolation
- `commit_sync` — sync in the commit phase

**Required before**: the Python generator can map failure classes to
deterministic injection points.

### `assert_consistent` / `assert_delta` helpers

No `invariants.h` file exists. The generated prove tests depend on
these helpers to validate the actual delta against the expected delta
from the reference model.

### Python test generator

The core deliverable of the formal validation model. None of the
planned files exist:
- `generate_tests.py` — the generator
- `expected_delta.py` — the reference model
- `fault_point_resolver.py` — maps classes to checkpoint numbers
- `scenario_matrix.py` — the input matrix
- `generated/*.cpp` — committed evidence

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
| C | No — partial write | No | No | Yes | No |
| D | No — partial write | No | No | Yes | Yes |
| E | No — partial write | No | No | Yes | Yes |
| F | Yes — fully | Yes — full delta | Yes | Yes | No |

Note: Class F is the only class where the transition is fully persisted
but the caller receives an exception. The state is published before the
exception is rethrown. The caller knows the write succeeded durably.

### Current test coverage by failure class

| Class | Covered | How |
|-------|---------|-----|
| SUCCESS | Yes | All existing engine tests exercise this path |
| A | Yes | 25 `validate_preconditions` unit tests (BC-129) |
| B1 | Partial | 1 fault injection test (BC-131) — count-based, not name-based |
| B2 (single) | Yes | `[partial_write]` test — `PostWriteMode::short_write` (BC-135) |
| B2 (multi) | Yes | `[partial_write]` test — multi-entry batch with isolation (BC-135) |
| B3 (single) | Yes | `[partial_write]` test — `PostWriteMode::throw_after` (BC-135) |
| C | No | Requires `bulk_end_append` checkpoint |
| D | Yes | BC-134 `[poisoned]` test — count-based chains through isolation sync |
| E | Yes | BC-134 `[poisoned]` test — `io_rotate_file_creation` checkpoint |
| F | Yes | 1 fault injection test (BC-133) |

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

    elif F in {FailureClass.B1, FailureClass.C}:
        # Transition not fully persisted — must not be applied.
        # B1: writev returned -1, no bytes on disk. Safe.
        # C: BulkEnd failed, isolation rotates. Safe.
        return Delta(
            keys_added   = [],
            keys_removed = [],
            lsn_advance  = 0,       # published next_lsn unchanged
            stats_delta  = NoDelta,
            poisoned     = False,
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

failure_classes = list(FailureClass)  # all nine classes
```

Total generated tests:

```
len(state_shapes) × len(plan_shapes) × len(failure_classes)
= 4 × 7 × 9
= 252 tests
```

Each test is deterministic, independent, and covers exactly one cell
in the matrix.

---

## The Generator

Takes the matrix and produces Google Test cases. One test per
`(S1, P, F)` combination.

```python
def generate_test(s1: StateShape, p: PlanShape, f: FailureClass) -> str:
    name     = f"prove__{s1.label}__{p.label}__{f.value}"
    expected = expected_delta(p, f)

    return f"""
TEST(ProveApplyBatchIf, {name}) {{
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
    active_injector = nullptr;

    // Validate against expected delta
    EXPECT_EQ(threw, {str(expected.threw).lower()});
    {"EXPECT_FALSE(result);" if f == FailureClass.A else ""}
    {"EXPECT_TRUE(db.is_poisoned());"
        if expected.poisoned
        else "EXPECT_FALSE(db.is_poisoned());"}
    assert_consistent(db);
    assert_delta(before, db, {generate_expected_delta(expected)});
    {"assert_recoverable(test_dir);" if not expected.poisoned else ""}
}}
"""
```

---

## The Fault Point Resolver

Maps a failure class to the concrete checkpoint number for a given
plan shape. This is what connects the abstract class to the
deterministic injector.

```python
def fault_point_for_class(p: PlanShape, f: FailureClass) -> FaultConfig:
    """
    Maps a failure class to the concrete injection configuration.

    B1 uses the existing pre-write checkpoint (throw_before mode).
    B2 uses the post-write checkpoint (PostWriteMode.short_write).
    B3 uses the post-write checkpoint (PostWriteMode.throw_after).
    All others use count-based pre-write injection.
    """
    multi = len(p.ops) > 1
    checkpoints = []

    if multi:
        checkpoints.append(("bulk_begin_append", FailureClass.B1))

    for i, op in enumerate(p.ops):
        checkpoints.append((f"op_{i}_append", FailureClass.B1))

    if multi:
        checkpoints.append(("bulk_end_append",   FailureClass.C))
        checkpoints.append(("isolation_sync",     FailureClass.D))
        checkpoints.append(("isolation_rotation", FailureClass.E))

    checkpoints.append(("commit_sync", FailureClass.F))

    if f == FailureClass.B2:
        # Post-write short_write on the first data append
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode=PostWriteMode.short_write,
            short_write_bytes=5,
        )

    if f == FailureClass.B3:
        # Post-write throw_after on the first data append
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode=PostWriteMode.throw_after,
        )

    # Pre-write count-based injection for all other classes
    for i, (label, cls) in enumerate(checkpoints):
        if cls == f:
            return FaultConfig(fail_at=i)

    return FaultConfig()  # SUCCESS — no fault injected
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
    auto after = db.state_.load();

    // Key membership — transition applied or not
    for (auto& key : expected.keys_added) {
        ASSERT_TRUE(after->key_dir.contains(key));
    }
    for (auto& key : expected.keys_removed) {
        ASSERT_FALSE(after->key_dir.contains(key));
    }

    // LSN advancement — transition identifier consumed or not
    ASSERT_EQ(
        after->next_lsn,
        before.next_lsn + expected.lsn_advance
    );

    // Stats consistency with key_dir
    assert_stats_match_keydir(*after);

    // Poison state
    ASSERT_EQ(db.is_poisoned(), expected.poisoned);
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

### Phase 1 — `DbPoisoned` (prerequisite for classes D, E) ✅

Implemented in BC-134. The engine calls `deem_as_poisoned(reason)` when
isolation rotation fails after an orphaned `BulkBegin`. All subsequent
writes throw `DbPoisoned` with a diagnostic reason; reads remain
available. `is_poisoned()` and `poison_reason()` public accessors
added. A `FAULT_INJECTION(io_rotate_file_creation)` checkpoint in
`rotate_active_file` enables deterministic testing. Three `[poisoned]`
tests cover: poison on isolation failure, reads on poisoned DB,
single-entry failure does not poison.

**Unblocks**: failure classes D and E.

### Phase 2 — Fine-grained fault checkpoints

Place named `FAULT_INJECTION` checkpoints at each failure class
boundary in `apply_batch_if`:
- `bulk_begin_append` — before first BulkBegin write
- Per-entry checkpoints within the loop
- `bulk_end_append` — before BulkEnd write
- `isolation_sync` — in the `catch(...)` isolation path, before sync
- `isolation_rotation` — in the `catch(...)` isolation path, before rotate
- `commit_sync` — before the commit-phase sync

**Depends on**: Phase 1 (poisoning must exist before isolation
checkpoints are meaningful).

**Unblocks**: deterministic name-based injection for all seven classes.

### Phase 3 — `invariants.h` helpers

Implement `assert_consistent(db)` and `assert_delta(before, db, expected)`:
- `assert_consistent`: stats match key_dir, file registry consistent,
  no dangling file references.
- `assert_delta`: validates key membership, LSN advancement, stats
  delta, and poison state against the reference model's expected delta.
- `assert_recoverable(dir)`: opens a fresh DB from disk and verifies
  the recovered state matches the in-memory state.

**Depends on**: Phase 1 (`is_poisoned()` check inside `assert_delta`).

**Unblocks**: all generated tests.

### Phase 4 — Python test generator

The core deliverable. Produces 196 deterministic C++ test cases from
the scenario matrix × failure classes × plan shapes.

| File | Purpose |
|------|---------|
| `expected_delta.py` | The reference model, independently readable |
| `fault_point_resolver.py` | Maps failure classes to checkpoint numbers |
| `scenario_matrix.py` | The input matrix, enumerable and auditable |
| `generate_tests.py` | The generator |
| `generated/*.cpp` | Committed evidence, never hand-edited |

**Depends on**: Phases 1–3.

### Phase 5 — CI model-diff step

Add a CI step that regenerates the prove tests and verifies no diff.
A diff means either the model or the generator changed — both must be
intentional and reviewable.

**Depends on**: Phase 4.

---

## Output Structure

```
test/
  proof/
    generate_tests.py          ← the generator — owns the model
    expected_delta.py          ← the reference function — owns the contract
    fault_point_resolver.py    ← maps classes to checkpoint numbers
    scenario_matrix.py         ← the input matrix
    invariants.h               ← assert_consistent(), assert_delta()
    fault_injector.h           ← deterministic I/O failure counter
    generated/
      prove_apply_batch_if.cpp    ← generated, never hand-edited
      prove_vacuum_compact.cpp    ← generated, never hand-edited
      prove_vacuum_absorb.cpp     ← generated, never hand-edited
```

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
persisted or not. Seven classes cover the entire behavioral space of
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
