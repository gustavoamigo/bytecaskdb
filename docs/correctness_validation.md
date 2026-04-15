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
    D  = "isolation_sync_fails"       # sync during orphan isolation fails — degrade
    E  = "isolation_rotation_fails"   # rotation during orphan isolation fails — degrade
    F  = "commit_sync_fails"          # sync in commit phase — persisted, throws
    G  = "rotation_sync_fails"        # post-write rotation sync fails — persisted, throws
    H  = "rotation_file_creation_fails"  # post-write rotation fails after seal — degrade
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
- **B3 — full write + error return**: `writev` writes all bytes to
  disk successfully, but an error is returned to the caller (simulated
  by `PostWriteMode::throw_after`, which throws after `writev` returns
  but before `offset_` advances). The entry is structurally complete on
  disk with a valid CRC. The file is `tainted`.

For multi-entry batches, B2 and B3 are safe: the isolation rotation
moves to a new file, abandoning the tainted one. Recovery discards the
orphaned batch because the BulkBegin/BulkEnd framing is incomplete.

For single-entry writes, there is no isolation — `offset_` diverges
from the kernel's file position (the file was opened with `O_APPEND`).
The next `writev` would write at the kernel's true EOF (past the stale
bytes), but `offset_` would still point to the old position. The
returned offset for subsequent entries would be wrong — silent
corruption.

The fix: `DataFile::append()` sets `tainted_ = true` when `writev`
returns a short write (`written > 0`). In the `apply_batch_if` catch
block, if `!multi && file.is_tainted()`, the engine calls
`DataFile::try_recover_failed_append`: it preads the entry at the
pre-write offset and CRC-verifies it. Valid CRC → publish normally
(B3 — write actually succeeded). Invalid CRC → degrade the DB (B2).

B2 and B3 differ in recovery behavior for single-entry writes:

- **B2 (single)** — partial bytes on disk. The entry has an invalid CRC
  or is truncated. Recovery skips it. The transition is not recovered.
- **B3 (single)** — all bytes on disk, valid CRC. As of BC-156, the engine
  attempts a read-back before degrading: `pread` the entry at the known
  offset and CRC-verify it. Valid CRC → the write is durable; the engine
  publishes state normally and returns success to the caller (no throw, no
  degrade). Invalid CRC or short read → degrade as for B2. Before BC-156, B3
  always degraded — the recovery note below applied then but not now.

  *Recovery note (pre-BC-156 / B2 reference):* For B2, the partial bytes on
  disk have an invalid CRC; recovery skips the entry and the transition is
  not recovered.

### Post-write rotation failure subclasses (G, H)

After all appends succeed and in-memory state is applied, the engine
checks whether the active file has reached the rotation threshold. If
so, it syncs the file and rotates to a new one. Two distinct failures
can occur at this point:

- **G — rotation sync fails**: The pre-rotation `fdatasync` fails.
  The file is not sealed. The transition is fully persisted (appends
  succeeded), so the state is published and the exception is rethrown.
  The DB is not degraded — the next write will retry rotation or
  continue appending to the same file.
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
| B1 | No — nothing written | No | No | Yes | No |
| B2 (multi) | No — partial write | No | No | Yes | No (isolation rotates) |
| B2 (single) | No — partial write | No | No | Yes | Yes (tainted) |
| B3 (multi) | No — orphaned batch | No | No | Yes | No (isolation rotates) |
| B3 (single) | Yes — full write, valid CRC | Yes — read-back verifies CRC, publishes if valid (BC-156) | No (in-process) | Yes | No — B3 no longer degrades; B2 still degrades |
| C | No — partial write | No | No | Yes | Yes |
| D | No — partial write | No | No | Yes | Yes |
| E | No — partial write | No | No | Yes | Yes |
| F | Yes — fully | No — lsn only | Yes | Yes | No |
| G | Yes — fully | No — lsn only | Yes | Yes | No |
| H | Yes — fully | Yes — full delta | Yes | Yes | Yes |

Note on classes F and G — key changes unpublished, LSN advanced: The data
is written via `writev` and reaches the file, but `fdatasync` fails — the
transition is not confirmed durable. Key-directory changes are NOT published:
the written key is invisible to subsequent reads, and the caller must retry.
`next_lsn` is advanced past all consumed sequence numbers to prevent LSN
reuse for bytes now in the page cache. The engine is not degraded — subsequent
writes proceed normally, and the next write may retry the rotation if G failed.

Class H is similar — the transition is persisted — but the engine is
also degraded because the sealed active file cannot accept further
appends.

---

## Write Path Infrastructure

### Single write path

All mutations — `put`, `del`, `apply_batch`, `apply_batch_if` — route
through `apply_batch_if` as the single coordinator under `write_mu_`
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
`apply_batch_if`, `vacuum_compact`, `vacuum_absorb`, and hint files.
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
8. `io_vacuum_absorb_sync` — before `active.sync()` in `vacuum_absorb_file()`
9. `io_vacuum_compact_tmp_create` — before constructing the temporary
   `DataFile` in `vacuum_compact_file()`
10. `io_vacuum_compact_rename` — before `std::filesystem::rename()` in
    `vacuum_compact_file()`

### Orphaned BulkBegin rotation

If a multi-entry batch fails mid-write after `BulkBegin`, the engine
force-rotates to isolate the orphaned marker. Without this, subsequent
writes to the same file would be silently discarded on recovery.

### Sync failure publish-before-rethrow

If `fdatasync` fails after all appends succeed, the engine publishes
the new state before rethrowing. This prevents duplicate LSNs on the
next write.

---

## Proof Test Generator

### apply_batch_if — 172 tests

172 generated Catch2 tests (`[prove_apply_batch_if]` tag) cover every valid
(StateShape, PlanShape, FailureClass) combination for `apply_batch_if`.
The scenario matrix is 4 state shapes × 7 plan shapes × 11 failure
classes = 308 total; 4 elimination rules reduce this to 172 valid tests.

Each test follows the same structure:
1. Set up initial DB state from `StateShape`
2. Capture baseline
3. Construct `WritePlan` from `PlanShape`
4. Inject fault per `FaultConfig`
5. Execute `apply_batch_if`
6. `assert_delta(before, db, expected)` — validates key membership,
   LSN advancement, structural consistency, and degraded state.
   For degraded cases, `assert_resumable(db)` is called immediately after
   to verify that `resume()` restores consistent state in-process.
7. `assert_recoverable(dir, before, expected)` — validates persistence
   invariant via fresh recovery (where applicable)

### resume() — 7 tests

7 generated Catch2 tests (`[prove_resume]` tag) cover every valid
(DegradeShape, ResumeFailureClass) combination.

Two degrade shapes establish a degraded DB before resume is called:

- **degrade_H** — `io_rotate_file_creation` fires on a put at the
  rotation threshold. The write committed (both keys are in key_dir),
  rotation failed. No orphaned bytes — `valid_offset == file.size()`.
- **degrade_C** — `ScopedFaultInjector{fail_at=3}` on a 2-op batch.
  BulkEnd at checkpoint 3 fails; subsequent isolation sync and rotation
  also fail (cascade). k0 committed; orphaned BulkBegin+p0+p1 bytes
  remain in the active file. `resume()` truncates back to after k0.

Three resume failure classes (R1/R2/R3) plus SUCCESS yield 7 valid
combinations (R1 is filtered for degrade_H because the truncation guard
`file.size() != valid_offset` is false — the fault point is unreachable).

Each R1/R2/R3 test uses a two-phase pattern:
1. Establish degraded state
2. Inject resume fault → `resume()` throws, engine stays degraded
3. Clean resume → `REQUIRE_NOTHROW`, `is_degraded()` false, `assert_consistent`
4. `assert_keys_recoverable` after DB scope closes

This directly proves: *resume always eventually recovers once the underlying fault clears.*

### vacuum_absorb — 6 tests

6 generated Catch2 tests (`[prove_vacuum_absorb]` tag) cover two state
shapes × three failure classes (SUCCESS, VA1, VA2).

State shapes create a DB with exactly one sealed file having fragmentation > 0:

- **low_fragmentation** — sealed file with 2 entries, 1 dead (50% frag)
- **mostly_dead** — sealed file with 6 entries, 5 dead (~83% frag)

Both use `absorb_threshold=UINT64_MAX` to force the absorb path.
Failure classes: SUCCESS, VA1 (`io_data_file_append`), VA2 (`io_vacuum_absorb_sync`).

Each test uses `capture_vacuum_baseline` / `find_vacuum_target` before
the vacuum call, then `assert_vacuum_success` or `assert_vacuum_no_change`
after. The `assert_vacuum_no_change` helper explicitly checks
`file_stats[active_id].total_bytes == actual_file_size` — this detects
the stale-stats invariant violation that would result if `vacuum_absorb_file`
failed to truncate the active file back after a sync failure. Each test
ends with `assert_vacuum_recoverable` after the DB scope closes.

### vacuum_compact — 10 tests

10 generated Catch2 tests (`[prove_vacuum_compact]` tag) cover two state
shapes × five failure classes (SUCCESS, VC1–VC4).

Same state shapes as absorb but `absorb_threshold=0` forces the compact
path. `mostly_dead` uses `max_file_bytes=150` to pack all 6 keys into one
sealed file so `live_bytes > 0` — preventing the edge case where
`absorb_threshold=0` is still satisfied and absorb is taken.

Failure classes: SUCCESS, VC1 (`io_vacuum_compact_tmp_create`),
VC2 (`io_data_file_append`), VC3 (`io_data_file_sync`),
VC4 (`io_vacuum_compact_rename`).

VC4 is the most critical: the tmp file is fully synced and renamed
(a new `.data` file exists on disk) but `vacuum_commit` has not run —
the old file is still in the published state. `assert_vacuum_recoverable`
confirms that recovery does not replay the orphaned new file as a
secondary source and sees only the data the old file guaranteed.

### Source files

**apply_batch_if module** (root of `tests/proof/`):

| File | Role |
|------|------|
| [`expected_delta.py`](../tests/proof/expected_delta.py) | The reference model: `expected_delta(plan, failure, ...) → Delta` |
| [`scenario_matrix.py`](../tests/proof/scenario_matrix.py) | State shapes, plan shapes, failure classes, validity filter |
| [`fault_point_resolver.py`](../tests/proof/fault_point_resolver.py) | Maps (state, plan, failure) → `ScopedFaultInjector` configuration |
| [`generate_tests.py`](../tests/proof/generate_tests.py) | Generates `prove_apply_batch_if.cpp` from the matrix |

**resume module** (`tests/proof/resume/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | DegradeShape (H, C), ResumeFailureClass (SUCCESS, R1–R3), validity filter |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: keys present/absent after all resume calls |
| `generate_tests.py` | Generates `prove_resume.cpp` |

**vacuum_absorb module** (`tests/proof/vacuum_absorb/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | AbsorbStateShape (low_fragmentation, mostly_dead), VacuumAbsorbFailureClass |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: threw/file_removed outcome |
| `generate_tests.py` | Generates `prove_vacuum_absorb.cpp` |

**vacuum_compact module** (`tests/proof/vacuum_compact/`):

| File | Role |
|------|------|
| `scenario_matrix.py` | CompactStateShape (low_fragmentation, mostly_dead), VacuumCompactFailureClass |
| `fault_point_resolver.py` | Maps failure class → fault checkpoint name |
| `expected_delta.py` | Reference model: threw/file_removed outcome |
| `generate_tests.py` | Generates `prove_vacuum_compact.cpp` |

**Shared invariants** (`tests/proof/`):

| File | Role |
|------|------|
| [`invariants.h`](../tests/proof/invariants.h) | `capture_baseline`, `assert_consistent`, `assert_delta`, `assert_recoverable`, `assert_resumable`, `assert_keys_recoverable`, `VacuumBaseline`, `capture_vacuum_baseline`, `find_vacuum_target`, `assert_vacuum_success`, `assert_vacuum_no_change`, `assert_vacuum_recoverable` |

### Fault injection modes

The four `ScopedFaultInjector` modes map failure classes to the four
I/O checkpoints:

- **Name-based** — targets a single checkpoint. Used for B1, F, G, H.
- **Count-based** — fails from checkpoint N onward, cascading. Used for C, D, E.
- **Count-based with skip set** — cascades but lets named checkpoints
  pass. Used for D (skip `io_rotate_file_creation`).
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
  after `assert_delta` for all degraded failure classes (C, D, E, H).
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
  matches its actual on-disk size (detecting a failed absorb that wrote
  bytes without rolling them back), `assert_consistent`.
- `assert_vacuum_recoverable(dir, before)` — opens a fresh DB and verifies
  all pre-vacuum keys survive recovery with correct values.

12 test cases (`[invariants]` tag) in `tests/invariants_test.cpp`
smoke-test the helpers themselves.

### Test coverage

All eleven failure classes for `apply_batch_if` are covered by the 172
`[prove_apply_batch_if]` tests. Each class is exercised across all valid
(StateShape, PlanShape) combinations.

All three resume failure classes (R1–R3) plus both degrade shapes are
covered by the 7 `[prove_resume]` tests. R1 is correctly excluded for
degrade_H (fault point unreachable).

All three vacuum_absorb failure classes across both state shapes are
covered by the 6 `[prove_vacuum_absorb]` tests.

All five vacuum_compact failure classes across both state shapes are
covered by the 10 `[prove_vacuum_compact]` tests.

Total generated proof tests: **195**.

Two hand-written tests remain in `bytecask_test.cpp` for mechanism
smoke testing not covered by the proof matrix:

- `mid-batch append failure rotates file and discards partial batch`
  (`[fault_inject]`) — tests `apply_batch` (not `apply_batch_if`)
  recovery by reopening the DB and verifying orphaned batch discard.
- `reads work on a degraded DB` (`[degraded]`) — tests the full read
  API surface (`get`, `contains_key`, `snapshot`, `iter_from`,
  `keys_from`) on a degraded DB instance, then calls `resume()` to
  verify in-process recovery.
- `resume() recovers from degraded state` (`[degraded][resume]`) — injects
  `io_rotate_file_creation` to trigger class T4 (post-write rotation fail),
  verifies `DbDegraded` is thrown, calls `resume()`, and confirms writes
  succeed afterward.

---

## Output Structure

```
tests/
  proof/
    __init__.py                    ← Python package marker
    generate_tests.py              ← apply_batch_if generator
    expected_delta.py              ← apply_batch_if reference model
    fault_point_resolver.py        ← apply_batch_if fault configs
    scenario_matrix.py             ← apply_batch_if input matrix
    invariants.h                   ← shared assert helpers for all proof suites
    resume/
      __init__.py
      scenario_matrix.py
      expected_delta.py
      fault_point_resolver.py
      generate_tests.py
    vacuum_absorb/
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
    generated/
      prove_apply_batch_if.cpp     ← generated, never hand-edited
      prove_resume.cpp             ← generated, never hand-edited
      prove_vacuum_absorb.cpp      ← generated, never hand-edited
      prove_vacuum_compact.cpp     ← generated, never hand-edited
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
