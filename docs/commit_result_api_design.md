# CommitResult API — Sequence-Returning Write Operations

**Status: approved.** Scope covers the engine, public C++ API, Python,
C, and synchronous Node/WASM bindings in one breaking pre-1.0 change.
Backward compatibility is not retained for any surface: `current_sequence`/
`currentSequence` are removed outright, not aliased or deprecated.

## Problem

Write operations report almost nothing about what they did:

| Operation | Current return | Information lost |
|---|---|---|
| `put` | `void` | assigned sequence |
| `del` | `bool` (existed) | assigned sequence |
| `del_range` | `void` | assigned sequence |
| `apply_batch` | `bool` (committed) | assigned sequence range |

The engine assigns a globally monotonic `u64` sequence to every entry at write
time (`TransientEngineState::prepare_write`), but the caller never sees it. The
return types are also closed: adding any future metadata (durability
confirmation, batch statistics) means changing every signature again.

The motivating scenario is **read-your-own-writes (RYOW) across replication**.
An application runs on a follower node, forwards a write to the leader over
RPC, and must not consider the operation complete until that change is visible
on the local follower. With the leader-assigned sequence in hand, the follower
can wait for exactly that sequence to arrive via `ingest`. Without it, the
application has no token to wait on.

Backward compatibility is explicitly not a goal (pre-1.0).

## Design

Two changes:

1. **`CommitResult`** — an open struct returned by every write operation,
   carrying the assigned sequence and durability status. Operations that can
   fail a precondition return `std::optional<CommitResult>`; operations that
   cannot conflict return `CommitResult` directly, so "conflict with a
   sequence" is unrepresentable.
2. **`durable_sequence(min_sequence, timeout)`** — the single sequence
   primitive, renaming and generalising `current_sequence(timeout)`: block
   until the durable sequence reaches at least `min_sequence` (or the timeout
   expires), then return it. Polling, the replication-loop wake-up, and the
   RYOW wait are all this one call with different targets.

No on-disk format change. No change to the I/O path: the sequence range is
already computed per write slot; the change is to surface it instead of
discarding it.

### CommitResult

```cpp
// Outcome of a committed write. sequence is the highest sequence assigned to
// the write (for a multi-op batch this is the BulkEnd marker's sequence).
// A reader — local or follower — whose durable_sequence() >= sequence is
// guaranteed to see every entry of this write.
export struct CommitResult {
  // Highest sequence assigned to this write. 0 means nothing was written
  // (empty plan, guard-only plan, or empty-range del_range) — there is
  // nothing to wait for.
  std::uint64_t sequence{0};

  // True if fdatasync confirmed durability of this write before return.
  // Always true for sync=true writes. May be true for sync=false writes
  // that were coalesced into a group containing a sync writer, or that
  // triggered a rotation sync.
  bool durable{false};
};
```

Why a struct and not a bare `uint64_t`: the struct is the extension point the
current API lacks. Future metadata (e.g. bytes appended, group batch id) is a
field addition, not a signature change at every call site.

Why `sequence` is the *highest* sequence of the batch: `ingest` on the
follower applies entries in ascending sequence order and
`durable_sequence()` reflects the last applied entry. Waiting until the
follower reaches the batch's last sequence (the `BulkEnd` marker for
multi-op plans) guarantees the entire batch is visible. The first sequence
has no wait-related use and is omitted.

### New DB signatures

```cpp
class DB {
public:
  // Cannot conflict — always returns a CommitResult.
  auto put(const WriteOptions &opts, BytesView key, BytesView value)
      -> CommitResult;

  // Engaged iff the key existed (tombstone appended).
  // nullopt: key was absent — nothing was written, no sequence assigned.
  [[nodiscard]] auto del(const WriteOptions &opts, BytesView key)
      -> std::optional<CommitResult>;

  // Cannot conflict. If from >= to, returns {sequence = 0} without writing.
  auto del_range(const WriteOptions &opts, BytesView from, BytesView to)
      -> CommitResult;

  // Engaged iff the plan committed. nullopt: a guard failed or the implicit
  // W-W check detected a conflict — nothing was written.
  // An empty plan returns {sequence = 0, durable = true}.
  // A guard-only plan that passes returns {sequence = 0, durable = true}.
  [[nodiscard]] auto apply_batch(WriteOptions opts, WritePlan plan)
      -> std::optional<CommitResult>;

  // The single sequence primitive — replaces current_sequence(timeout).
  // Blocks until the durable sequence (highest sequence confirmed by
  // fdatasync) is >= min_sequence or the timeout expires; returns the
  // durable sequence at return. min_sequence = 0, or a target already
  // reached, returns immediately. Identical semantics in Leader and
  // Follower mode (on a follower it reflects the last synced ingest).
  [[nodiscard]] auto durable_sequence(
      std::uint64_t min_sequence = 0,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) const
      -> std::uint64_t;
};
```

`put` and `del_range` are not `[[nodiscard]]`: fire-and-forget writes are
legitimate, and the result is informational. `del` and `apply_batch` keep
`[[nodiscard]]` because ignoring a conflict is a correctness bug.

### One sequence primitive: `durable_sequence`

Every use of the sequence API is the same operation — "block until the
durable sequence reaches at least N, then return it" — differing only in the
target:

| Use case | `min_sequence` |
|---|---|
| Poll / lag monitoring | 0 — always reached, returns immediately |
| Replication loop wake-up | `follower.durable_sequence() + 1` — "the leader has something the follower lacks" |
| RYOW wait | `r.sequence` from the leader's `CommitResult` |

Rather than shipping an advance-based long-poll (`current_sequence(timeout)`)
and a separate target-based wait side by side, the engine exposes the general
form once. The advance-based wait is the special case `target = current + 1`.

The target-based wake-up is also *more correct* for the replication loop. The
advance-based form blocks until the leader's durable sequence moves past its
value at call entry — a proxy for the condition the loop actually wants ("the
leader has entries the follower lacks").
`docs/replication_primitives_design.md` documents the resulting wart: on an
idle leader the first loop iteration blocks for the full timeout even when
the leader is already ahead of the follower. The Phase 2 loop becomes:

```
loop:
    leader.durable_sequence(follower.durable_sequence() + 1, timeout=30s)
    snap = leader.snapshot()
    it = leader.changes_since(snap, follower.durable_sequence())
    for entry in it:
        follower.ingest(entry)
```

A leader that is already ahead wakes the loop immediately; blocking happens
only when there is genuinely nothing to replicate.

**Why the rename.** "Current" is ambiguous: the engine tracks both the
highest *allocated* sequence (includes visible-but-unsynced entries) and the
highest *durable* one, and the replication contract depends on the
distinction — the replication doc needs a section clarifying that
`current_sequence` returns the durable value. `durable_sequence` names
exactly what is returned, matching the internal `durable_seq` field and the
replication doc's own terminology ("the replication boundary is the durable
sequence, not the visible sequence"). The noun form keeps the common poll
reading naturally as a getter — `db.durable_sequence()` — while blocking is
visible at exactly the call sites that block, where the `timeout` argument
appears.

The rename also makes the signature change fully loud: no old
`current_sequence(...)` call in any binding can silently bind to the new
semantics, because the name ceases to exist.

### Semantics

**Durability vs visibility.** `sequence` is assigned before I/O; `durable`
reports whether `fdatasync` covered it before the call returned. On the
success path, state publication happens after sync, so a returned
`CommitResult` from a sync write is both visible and durable. For
`sync=false`, the write is visible locally but `durable=false` unless the
group leader synced anyway (coalesced with a sync writer, or rotation).

**RYOW requires sync writes on the leader.** `changes_since` only yields
entries up to the leader's *durable* sequence. A `sync=false` write's
sequence does not replicate until a later sync advances `durable_seq` on the
leader. An application waiting on a follower for a `durable=false` result may
therefore block until the leader's next sync. The RYOW pattern should use
`sync=true` (the default); the `durable` field makes the hazard visible.

**Partial-batch waits are safe.** Because `sequence` is the `BulkEnd`
marker's sequence and `ingest` applies entries in ascending order, a
follower whose `durable_sequence()` has reached it has applied the whole
batch — there is no window where the wait succeeds on a partially applied
batch.

**`CommitResult{0}` needs no special-casing.** A nothing-written result
(`sequence == 0`) passed as `min_sequence` is trivially reached — the wait
returns immediately, matching "there is nothing to wait for".

**`del` of an absent key writes nothing.** This is current behavior
(`del` routes through `ensure_present` + `apply_batch`; the failed
precondition prevents the append) — the new return type states it honestly:
`nullopt` means no tombstone exists for this call, so there is nothing to
replicate or wait for.

### The RYOW pattern, end to end

```cpp
// On the follower node: application forwards the write to the leader.
CommitResult r = rpc_to_leader_put(key, value);   // leader: db.put({}, k, v)

// Wait until the local follower has ingested that sequence.
if (follower_db.durable_sequence(r.sequence, 2s) < r.sequence) {
  // Replication lag exceeded budget — degrade to reading via the leader,
  // or keep waiting; application policy.
}

// Guaranteed: local reads now see the write (and everything before it).
Bytes out;
(void)follower_db.get({}, key, out);
```

## Implementation

### Sequence capture (`EngineSlot`)

`execute_slot` already knows the slot's sequence range — `prepare_write`
returns the entries with sequences assigned. `EngineSlot::result` changes
from `bool` to the final result type:

```cpp
export struct EngineSlot : Slot {
  WritePlan plan;
  WriteOptions opts;
  std::optional<CommitResult> result;  // was: bool result
};
```

- `execute_slot`: on validation failure, `result = nullopt`. On success,
  `result = CommitResult{.sequence = entries.back().sequence}` (or
  `{.sequence = 0}` for empty/guard-only plans). `durable` is left false.
- `execute_slots` phase 1 collects entries from every slot in the batch, but
  a batch made up entirely of empty/guard-only plans produces no entries at
  all (`all_entries.empty()`) — `execute_slots` returns immediately after
  phase 1 in that case, before phase 3 (sync/rotate/publish) ever runs. Since
  every committed slot on that path already carries `sequence == 0`, there is
  nothing to wait for and no I/O to confirm, so this early-return path
  explicitly sets `durable = true` on each committed slot before returning —
  otherwise those results would incorrectly default to `{0, false}` for
  batches that skip phase 3 entirely. This is the zero-entry durability fix:
  every successful zero-entry result is `{sequence = 0, durable = true}`,
  regardless of which return path in `execute_slots` produced it.
- `execute_slots` phase 3: after the final `apply_sync`/`store_state`, set
  `slot->result->durable = (t.durable_seq() >= slot->result->sequence)` for
  each committed slot. This naturally covers group-coalesced syncs and
  rotation syncs. Error paths are unchanged — slots get `err` and the caller
  throws, same as today.
- `apply_batch` returns `slot.result`. `put`/`del_range` unwrap it
  (`*result` — cannot be nullopt by construction); `del` returns it as-is.

No change to `prepare_write`, `apply_writes`, offsets, group commit, or any
I/O ordering.

### `durable_sequence`

Reuses the existing `durable_mu_` / `durable_cv_` long-poll infrastructure —
`store_state` already notifies when `durable_seq` advances. The wait
predicate becomes `load_state()->durable_seq >= min_sequence`; on return the
function reads and returns the watermark. The old advance-based predicate
("moved past the value at entry") is removed. Works unmodified in Follower
mode because `ingest` always syncs and advances `durable_seq` through the
same `store_state` path.

### Affected surfaces

| Surface | Change |
|---|---|
| `bytecaskdb/bytecask.cppm` | `CommitResult`, new write signatures, `EngineSlot`, `current_sequence` → `durable_sequence(min_sequence, timeout)` |
| `include/bytecask.hpp` + `bytecaskdb/bytecask_hpp.cpp` | Mirror `CommitResult` (plain struct, no PIMPL needed) and all signature changes, including the rename, through the PIMPL layer |
| `bytecaskdb-python/` | Full binding — see below |
| `bytecaskdb/bytecask_c.cpp` / `bytecask_c.h` | Follow-up (sketch below) |
| `bytecaskdb-node/` | Follow-up (sketch below) |
| `bytecaskdb-mariadb-plugin/` | Consumes `bytecask.hpp` (BC-220); call sites updated mechanically — `bool committed` → `optional` checks. No behavioral change. |
| Tests | BC-204 replication proof suite: mechanical rename of `current_sequence` calls; new `durable_sequence` coverage below |
| Docs | `README.md` API reference + features; `bytecask_design.md`; `replication_primitives_design.md` (rename throughout; primitive #1 and the Phase 2 loop become target-based; Durable Sequence section); `CONTRACT.md` |

## Python API

`CommitResult` is bound in the nanobind module as a small read-only class:

```python
class CommitResult:
    sequence: int   # highest sequence assigned; 0 = nothing written
    durable: bool   # fdatasync confirmed before return
```

### `ext.py` wrapper changes

```python
class DB:
    def put(self, key, value, *, sync=True, solo=False) -> CommitResult: ...
    def delete(self, key, *, sync=True, solo=False) -> CommitResult | None:
        """None if the key was absent (nothing written)."""
    def delete_range(self, from_key, to_key, *, sync=True, solo=False) -> CommitResult: ...

    def durable_sequence(self, min_sequence: int = 0, timeout: float = 0.0) -> int:
        """Block until the durable sequence >= min_sequence or timeout
        (seconds) expires; returns the durable sequence. Replaces
        current_sequence(timeout_ms). GIL released while waiting."""
```

The rename makes the break loud in Python too: old
`current_sequence(timeout_ms=...)` calls fail with `AttributeError` instead
of silently binding a timeout as a target.

Dunder methods keep their Python-native contracts: `db[k] = v` and
`del db[k]` still return `None` (Python fixes those signatures anyway).
`delete` changes from `bool` to `CommitResult | None` — truthiness still
means "the key existed", so `if db.delete(k):` keeps working.

Context managers commit on `__exit__`, which cannot return a value, so the
result is exposed as an attribute after the block:

```python
with db.batch() as b:
    b[b"k1"] = b"v1"
    b.delete_range(b"log:", b"log:~")
follower.durable_sequence(b.result.sequence, timeout=2.0)

with db.transaction() as txn:      # raises ConflictError on conflict (unchanged)
    txn[b"stock"] = new_stock
seq = txn.result.sequence          # set only when the commit succeeded
```

`_Batch._commit` / `_Transaction._commit` store the returned result on
`self.result` (`None` before commit; `_Transaction.result` is never a
conflict value because conflict raises).

`_bytecaskdb.pyi` gains the `CommitResult` class and the updated signatures.

## C API

`include/bytecask_c.h` gains two new POD structs and one new function, and
changes the signatures of `bytecask_put`, `bytecask_del`, `bytecask_del_range`,
and `bytecask_apply_batch`. No old symbol is kept as a compatibility shim.

```c
typedef struct {
  uint64_t sequence;   /* 0 = nothing written */
  int durable;          /* 1 = fdatasync confirmed before return */
} bytecask_commit_result_t;

/* Nullable: a NULL pointer selects sync=true, solo=false (current
   defaults). Extensible — new fields can be appended without breaking
   existing callers that zero-initialize or use a NULL pointer. */
typedef struct {
  int sync;   /* nonzero = fdatasync after the write (default: nonzero) */
  int solo;   /* nonzero = bypass the write group (default: 0) */
} bytecask_write_options_t;

int bytecask_put(bytecask_db_t *db, const char *key, size_t key_len,
                 const char *val, size_t val_len,
                 const bytecask_write_options_t *opts /* nullable */,
                 bytecask_commit_result_t *out /* nullable */);

int bytecask_del(bytecask_db_t *db, const char *key, size_t key_len,
                 const bytecask_write_options_t *opts /* nullable */,
                 bytecask_commit_result_t *out /* nullable */);

int bytecask_del_range(bytecask_db_t *db, const char *from, size_t from_len,
                       const char *to, size_t to_len,
                       const bytecask_write_options_t *opts /* nullable */,
                       bytecask_commit_result_t *out /* nullable */);

int bytecask_apply_batch(bytecask_db_t *db, bytecask_write_plan_t *plan,
                         const bytecask_write_options_t *opts /* nullable */,
                         bytecask_commit_result_t *out /* nullable */);

uint64_t bytecask_durable_sequence(bytecask_db_t *db,
                                   uint64_t min_sequence,
                                   uint64_t timeout_ms);
```

Status-code conventions are preserved exactly as today — the struct out-param
is additive, not a replacement for the status return:

- `bytecask_put` / `bytecask_del_range`: `0` success, `-1` error. Cannot
  conflict, so `out` (if non-null) is always filled on `0`.
- `bytecask_del` / `bytecask_apply_batch`: `1` committed, `0` absent/conflict
  (no write occurred), `-1` error. `out` (if non-null) is filled only on `1`.

If `out` is non-null, it is cleared to `{0, 0}` on entry to every one of
these functions, before the operation runs, and then filled only for the
"committed" status codes above. This means a caller that ignores the return
code and only inspects `out->sequence != 0 || out->durable` cannot mistake a
stale value from a previous call for this call's result — every call
resets it first, matching the nullable-optional discipline of the C++ API.
`bytecask_durable_sequence` on a null/errored `db` returns `0` and sets the
thread-local error, same as other read accessors today.

`bytecask_current_sequence` is removed, not aliased — no C caller can
silently link against the old advance-based semantics.

A dedicated C API behavioral test (`tests/bytecask_c_test.cpp` or similar)
exercises status codes, nullable options/out-params, and durability flags
from C++ calling through the C ABI. A separate plain-C compilation smoke
test (a `.c` file, not `.cpp`) validates that `bytecask_c.h` is valid C11 and
that the new struct layouts and function signatures compile without a C++
compiler in the loop. Both are wired into `xmake.lua` as new targets.

## Node / WASM API

`bytecaskdb-node` stays synchronous (Embind, single-threaded WASM). Every
sequence-bearing value converts end-to-end to exact `bigint` — `number`
loses precision above 2^53 and sequences are `uint64_t` — including
`DataEntry.sequence`, manifest through-sequence, `changesSince`/`ingest`
sequence arguments, and wait targets. This may require the Embind build to
add `-sWASM_BIGINT`; the build script (`bytecaskdb-node/wasm/build.sh`) is
updated if so, with a centralized checked conversion helper (bounds-checked
`u64` ↔ `BigInt`) used at every binding boundary instead of ad hoc casts.

```ts
interface CommitResult {
  sequence: bigint;
  durable: boolean;
}

class Database {
  put(key: Uint8Array, value: Uint8Array, opts?: WriteOptions): CommitResult;
  delete(key: Uint8Array, opts?: WriteOptions): CommitResult | null;
  deleteRange(from: Uint8Array, to: Uint8Array, opts?: WriteOptions): CommitResult;
  applyBatch(plan: WritePlan, opts?: WriteOptions): CommitResult | null;

  // Synchronous — a positive timeoutMs blocks the calling thread, which in
  // Node.js is the event loop. Work scheduled on that same event loop
  // (timers, other I/O callbacks) cannot run and cannot be what advances
  // the sequence while this call blocks; only another thread/process
  // (e.g. the native addon's own I/O) can. This is a sharp edge specific
  // to single-threaded synchronous WASM bindings and is documented in the
  // API reference and README, not hidden.
  durableSequence(minSequence?: bigint, timeoutMs?: number): bigint;
}
```

`currentSequence` is removed outright — no alias, no deprecation warning.
Defaults: `minSequence = 0n`, `timeoutMs = 0` (non-blocking poll), matching
the C++/Python/C defaults. `wasm/bytecask_embind.cpp` implements the wait as
a blocking call into the same `durable_mu_`/`durable_cv_` primitive used
natively — WASM being single-threaded does not change the wait's semantics,
only the fact that no other JS work can run concurrently with it, which is
already implied by "blocks the event loop".

TypeScript (`src/types.ts`, `src/wasm-backend.ts`), the Vitest suite, and
`bytecaskdb-node/wasm/API.md` are updated to match; existing tests that call
`currentSequence` are renamed to `durableSequence` with target-based
arguments instead of advance-based ones.

## Testing

- **Unit**: sequence monotonicity across `put`/`del`/`del_range`/batches;
  batch result equals the `BulkEnd` sequence (verifiable via
  `changes_since`); `del` absent → `nullopt`; empty and guard-only plans →
  `sequence == 0`; conflict → `nullopt` and no sequence consumed beyond the
  failed slot.
- **Durability flag**: `sync=true` → `durable`; solo `sync=false` →
  `!durable`; concurrent group with one sync writer → coalesced `sync=false`
  writers report `durable=true` (deterministic via the existing
  `test_write_group()` seam).
- **durable_sequence**: `min_sequence=0` and already-reached targets return
  immediately (in particular: a leader ahead of the target does not block —
  the idle-first-iteration wart is gone); `timeout=0` is a non-blocking
  poll; timeout expiry returns a watermark `< min_sequence`; a concurrent
  waiter is woken by a sync write reaching its target. The
  `current_sequence` → `durable_sequence` rename in existing tests is
  compile-driven.
- **RYOW loopback**: leader + follower DBs in-process; write on leader,
  pump `changes_since` → `ingest`, assert
  `follower.durable_sequence(r.sequence, t) >= r.sequence` and the follower
  reads the value; assert a timeout result (watermark `< r.sequence`) when
  the pump is paused.
- **Model-based tests**: no update required — no on-disk format or recovery
  change. The mechanical `(void)db.del(...)` → result-type changes in
  existing tests are compile-driven.

## Considered and rejected

- **`std::expected<CommitResult, Conflict>`** — treats conflict as an error;
  the project convention is that conflicts are expected outcomes. `optional`
  says exactly "committed or not" with no error machinery.
- **Returning first *and* last sequence** — no consumer needs the first
  sequence; per-op sequences inside a batch are not individually awaitable
  anyway (the batch is atomic). Minimal surface wins.
- **`ingest` returning a result** — the caller supplies the sequences and
  already knows the max; `void` stays.
- **A durable-watermark field on `CommitResult` instead of `bool durable`** —
  the global watermark is available from `durable_sequence()`; duplicating
  it per result invites confusion with the write's own sequence.
- **Two wait primitives: `current_sequence(timeout)` + a separate
  `wait_for_sequence(seq, timeout)`** — an earlier revision kept the shipped
  advance-based long-poll unchanged and added a target-based wait beside it.
  Rejected: two overlapping waits over the same condition variable, where
  the advance-based form is strictly a special case of the target-based one
  (`target = current + 1`). The single general primitive is a smaller
  surface and fixes the idle-leader wake-up wart in the replication loop.
- **Removing the long-poll and making `current_sequence` a pure getter**
  (the first revision of this design) — pushed the deadline/retry loop into
  every consumer of the replication wake-up. Rejected for the same reason
  the two-primitive split was: the wait belongs in the engine, once.
- **Keeping the name `current_sequence`** — ambiguous between the highest
  allocated and highest durable sequence; the replication contract hinges on
  the difference, and the docs had to keep clarifying which one it returns.
- **Verb names (`wait_for_sequence`, `await_sequence`)** — the most common
  call is the no-argument poll, which reads absurdly as a verb ("wait for
  nothing"). The noun names the value; the `timeout` argument marks the
  blocking call sites.

## Open questions

1. Should there be a `visible_sequence()` counterpart exposing the highest
   allocated (not-yet-durable) sequence? Deferred — no current consumer;
   `durable_seq` is the replication boundary and the correct RYOW predicate.
   The naming leaves room for it.

## Implementation sequencing

All surfaces above are accepted scope for BC-231, implemented in phases so
each stage is independently buildable and testable:

1. Engine (`bytecaskdb/bytecask.cppm`) and public C++ PIMPL
   (`include/bytecask.hpp`, `bytecaskdb/bytecask_hpp.cpp`), with focused core
   tests. Call sites in `bytecask_c.cpp` and the MariaDB plugin that consume
   the changed C++ signatures are adapted mechanically in this phase only as
   far as needed to keep them compiling (optional-engagement checks,
   discarding `[[nodiscard]]` results) — their own result-surfacing follow-up
   API changes land in the next phase.
2. Language/ABI surfaces: C API, Python, Node/WASM, and MariaDB plugin call
   sites, each with their own focused tests.
3. Documentation and final integration: `CONTRACT.md`, design docs, README,
   and the full verification matrix.
