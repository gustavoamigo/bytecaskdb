# MariaDB Plugin — Correctness Validation

> This document describes a correctness validation framework for the
> `ha_bytecaskdb` MariaDB plugin. It is the plugin-level analogue of
> the engine's [`correctness_validation.md`](../../docs/correctness_validation.md)
> and should be read alongside it. The engine document is the source
> of truth for guarantees made by `apply_batch`; this document is the
> source of truth for guarantees made by the layer that sits on top
> of it.

---

## Conceptual Model

The plugin is a **translation layer** between MariaDB's `handler` API
and ByteCaskDB's `apply_batch`. It owns no durable state of its own —
every byte that survives a crash went through the engine. What it
owns is:

```
MariaDBTxn                 — buffered DML for the current SQL transaction
  ├── ops_                 — ordered log of buffered put/del operations
  ├── lookup_              — RYOW map (key → value | tombstone)
  └── snap_                — bytecask::Snapshot, OCC anchor

Catalog (in-engine state)  — table metadata + counters in 0x01 namespace
Row counter (in-memory)    — atomic per-table row count, lazily flushed
```

Three MariaDB concepts that this document keeps distinct:

- **Statement** — a single SQL command. The plugin sees a statement
  boundary as `commit(all=false)`, which is *not* a real commit — it
  flushes nothing to the engine and leaves `MariaDBTxn` alive.
- **Transaction** — one or more statements between `BEGIN` and
  `COMMIT`/`ROLLBACK`. The plugin sees a transaction boundary as
  `commit(all=true)`, which is where `MariaDBTxn::commit()` builds
  the `WritePlan` and calls `apply_batch`. Plugin-level invariants
  are stated against this boundary.
- **Session** — a client connection. A session contains many
  transactions; `MariaDBTxn` is per-transaction, not per-session.

Every SQL statement is a sequence of `handler` calls that mutate
`MariaDBTxn`. At transaction commit, `MariaDBTxn::commit()` materialises
`ops_` into a `WritePlan` anchored on `snap_` and calls `apply_batch`.

A plugin operation is correct if and only if:

1. **If `apply_batch` succeeds** — every buffered op is durable, every
   secondary-index entry agrees with the primary record, the row counter
   reflects the new cardinality, and the catalog is consistent.

2. **If `apply_batch` returns `false` (OCC conflict)** — no buffered op
   is durable; in-memory plugin state (counters, snapshot pin) is restored
   to what it was before the conflicting transaction started; MariaDB
   sees `HA_ERR_LOCK_DEADLOCK`.

3. **If `apply_batch` throws** — same as the conflict case, plus the
   exception is mapped to a sane `HA_ERR_*` and the transaction is
   discarded. If the engine is degraded, subsequent SQL statements either
   fail cleanly until the engine is resumed, or succeed if the plugin
   surfaces `resume()`.

There is no valid intermediate state. A transaction either commits
in full or is fully absent — both at the engine level *and* in the
plugin's own state (counters, catalog, snapshot lifetime).

### Why this needs its own framework

The engine's proof matrix establishes that `apply_batch(plan)` is atomic
under every I/O failure class. That is necessary but not sufficient
for plugin correctness, because the plugin can:

- **Construct the wrong plan.** A bug in `update_row` can omit a
  secondary-index `del` for the old key, or in `write_row` skip a
  unique-secondary `put`. The engine will atomically commit a *wrong*
  plan.
- **Mutate state outside the plan.** `row_count_atomic_` is bumped in
  `write_row` / `delete_row` *before* commit. A failed commit leaves
  the counter ahead.
- **Lose snapshot identity.** OCC depends on `WritePlan` being anchored
  on the snapshot taken at transaction start. If a later code path
  allocates a fresh snapshot, the OCC guard checks the wrong baseline.
- **Issue catalog writes outside the txn fence.** DDL paths
  (`create()`, `delete_table()`) call `apply_batch` directly, not
  through `MariaDBTxn`. A crash between catalog commit and MariaDB's
  DDL completion leaves the catalog out of sync with MariaDB's
  internal frm/dictionary state.

Each of these is invisible to the engine's validation. The plugin
framework targets them directly. The four gap modes map onto the
plugin invariants below as follows:

| Gap mode | Invariant that closes it |
|---|---|
| Construct the wrong plan | P-INV-1 (index synchronisation) |
| Mutate state outside the plan | P-INV-2 (counter atomicity) |
| Lose snapshot identity | P-INV-3 (snapshot identity) |
| Catalog writes outside the txn fence | P-INV-7 (catalog atomicity, plugin side) |

The remaining invariants (P-INV-4 RYOW, P-INV-5 savepoints, P-INV-6
OCC error mapping) are not gap-mode coverage — they are plugin-internal
correctness properties that the engine has no opinion on. The four gap
modes are necessary but not sufficient.

---

## Operating Principle — the model is the spec, not the code

**This is the most important rule in this document. Read it before
touching the framework.**

The reference model in `expected_delta.py` is the **specification** of
what the plugin must do. The plugin code is the **implementation**.
When a generated proof test fails, the direction of the fix is fixed:

```
Test fails  →  the implementation is wrong  →  fix the plugin
Test fails  →  the model is wrong           →  fix the model only if
                                                the invariant itself is
                                                wrong, with explicit
                                                review and a rationale
                                                in the commit message
```

The direction is **never**:

```
Test fails  →  the test is inconvenient  →  relax the assertion
Test fails  →  the matrix entry is hard  →  add an elimination rule
Test fails  →  the invariant is annoying →  weaken the invariant
```

A proof test exists because a defined scenario must satisfy a defined
invariant. If the implementation cannot satisfy it, the bug is in the
implementation. If the scenario or the invariant is genuinely wrong,
that is a *specification change* and must be reviewed as such — never
as a side effect of making CI green.

### Why this rule exists

The proof matrix is a **ratchet**. Its value comes entirely from the
fact that it only moves in one direction: forward, or not at all.
If failing tests can be silenced by edits to the model, the matrix,
or the elimination rules, the ratchet is broken and the framework is
worth nothing — it becomes a self-justifying artifact that proves
"whatever the code currently does."

This is not a hypothetical concern. AI coding assistants
(Claude Code, Copilot, Cursor, and similar) will reliably attempt to
"fix" a failing proof test by editing the test, the expected delta, or
the elimination filter rather than the production code. That behaviour
must be rejected on sight. **A proof test failure is a signal about
the plugin, not about the framework.**

### The decision procedure when a proof test fails

1. **Read the failing assertion.** Identify which invariant (P-INV-N)
   is violated, in which scenario (DML × index × txn × failure).
2. **Default assumption: the plugin is wrong.** Investigate the
   production code path under that scenario. Find the bug. Fix it in
   the plugin.
3. **If — and only if — the invariant itself is incorrect** (the
   scenario should not be expected to satisfy it, for a reason that
   holds independently of the current code), open a separate change
   that:
   - Updates this document to explain why the invariant is wrong.
   - Updates the model and regenerates the tests.
   - Is reviewed as a specification change, not a test fix.
4. **If — and only if — the scenario is genuinely impossible** in the
   plugin's API surface (not "hard to support" — *impossible*), add
   an elimination rule to `scenario_matrix.py` with a comment
   explaining the impossibility.

Edits to `expected_delta.py`, the elimination rules, or the invariant
list that arrive in the same commit as a "fix" for a failing proof
test should be rejected in code review. They almost always indicate
the ratchet was bypassed.

### What this rule does not forbid

- **Improving the model.** Adding a new invariant, refining a
  scenario, tightening an `expected_delta` entry — all welcome,
  reviewed as specification changes.
- **Adding new elimination rules for genuine impossibilities.** A
  shape that the plugin's API cannot represent is correctly
  eliminated.
- **Reorganising the generator** without changing the matrix output.
  Regeneration produces identical `prove_plugin.cpp`.

The forbidden move is exactly one: **changing the spec to match a
broken implementation.**

---

## Goal

For every supported (DML shape, index topology, transaction shape) and
every plugin-relevant failure class, prove:

```
transition(plugin_state_before, sql_input, failure)  →  plugin_state_after
where:
  plugin_state_after satisfies all plugin invariants (P-INV-1 … P-INV-7)
  delta(before, after) == expected_delta(sql_input, failure)
```

The framework does **not** re-validate engine atomicity — that is the
engine's job. It validates that the plugin produces a correct
`WritePlan`, restores its own state correctly on every failure path,
and maps engine outcomes to MariaDB error codes faithfully.

---

## Invariants

These are the properties the validation suite proves, exhaustively
across the scenario matrix. Every plugin-level test asserts a subset
of them; the matrix ensures every (shape × failure) combination is
covered.

### P-INV-1 — Index synchronisation

After any sequence of `write_row` / `update_row` / `delete_row` calls
that successfully commits, the secondary-index namespace
(`0x03[table_id][index_id]…`) and the primary namespace
(`0x02[table_id]…`) describe the same set of logical rows.

- Every PK record has exactly one entry in every secondary index it
  participates in.
- No secondary-index entry points to a missing PK.
- After `update_row` that changes an indexed column, the old secondary
  entry is gone and the new one exists.

### P-INV-2 — Counter atomicity

`row_count_atomic_` reflects the engine's view of the table after the
transaction settles, regardless of commit outcome:

- On commit success, the counter equals the pre-commit value plus the
  net cardinality delta of the buffered ops.
- On OCC conflict or engine throw at `apply_batch`, the counter equals
  the pre-commit value (the bump is rolled back).
- On any path that does not reach `apply_batch` — including
  plugin-internal faults that fire after the counter bump but before
  commit submission — the counter equals the pre-commit value.
- The counter is durable across plugin restart (re-derived from a
  catalog scan or persisted explicitly — both must agree).

### P-INV-3 — Snapshot identity

Every `WritePlan` constructed by `MariaDBTxn::commit()` is anchored
on the same `bytecask::Snapshot` that was acquired by
`begin_if_needed()` for that transaction.

- No code path replaces `snap_` mid-transaction.
- A statement-level `commit(all=false)` does not release or rebuild
  the snapshot.
- After commit (success or failure), `snap_` is released exactly once.

### P-INV-4 — RYOW consistency

Within a single transaction, reads observe the buffer:

- A read after `buffer_put(k, v)` returns `v`.
- A read after `buffer_del(k)` returns "not found".
- A read of an unwritten key returns the snapshot value.
- The `MergeIterator` yields the same key set as the snapshot iterator
  with the buffer applied.

### P-INV-5 — Savepoint reversibility

After `savepoint_rollback(sp)`:

- `ops_` contains exactly the prefix that existed when `sp` was set.
- `lookup_` is rebuilt from that prefix and is observationally
  indistinguishable from a fresh transaction that performed only
  those ops.
- Subsequent commit produces the same engine state as if the
  rolled-back ops had never been buffered.

### P-INV-6 — OCC error mapping

When `apply_batch` returns `false`:

- The plugin returns `HA_ERR_LOCK_DEADLOCK` (errno 1213).
- `ops_`, `lookup_`, `snap_` are all reset.
- `row_count_atomic_` is restored to its pre-transaction value.
- A retry of the same SQL statement against the post-conflict state
  is well-formed (no leaked state from the failed attempt).

When `apply_batch` throws `DbDegraded`:

- The plugin returns a stable `HA_ERR_*` (e.g. `HA_ERR_CRASHED`).
- Transaction state is discarded as in the conflict case.
- Subsequent statements either fail cleanly or succeed after `resume()`.

When `apply_batch` returns successfully but the engine is degraded
(engine class H — writes durable, but the active file cannot accept
further appends):

- The plugin returns success to MariaDB. The writes from this
  transaction are committed and visible.
- `is_degraded()` is *not* checked in the commit path. The degraded
  state is surfaced to MariaDB on the next statement that attempts a
  write, where `apply_batch` will throw `DbDegraded`.
- Rationale: the writes succeeded; reporting them as failed would
  contradict the engine's contract for class H. Eager checking would
  also race with a concurrent `resume()`. Surfacing on the next
  attempt is the cheapest correct option.

### P-INV-7 — Catalog atomicity (plugin side)

P-INV-7 covers the **catalog half** of DDL atomicity: the bytes the
plugin writes to its own `0x01` namespace. It does *not* cover
reconciliation between that catalog and MariaDB's frm/dictionary
state — see "What this does NOT cover" for the frm half.

After any DDL path returns to MariaDB:

- If the plugin reports success, the catalog (`0x01` namespace) is
  fully consistent with the table the plugin claims to have
  created/dropped: `TableMeta` is present (or absent) in full, and
  any allocated `table_id` / `index_id` either appears in the catalog
  or is rolled back.
- If the plugin reports failure, the catalog has no partial entries
  for the table being created/dropped.
- A crash mid-DDL leaves recoverable catalog state: the next plugin
  start either sees the table fully present or fully absent in the
  catalog. No half-created `TableMeta`. No orphan `table_id` /
  `index_id` allocations that would collide with future DDL.

The frm half — whether MariaDB's view of the schema after restart
agrees with the plugin's catalog — is out of scope for this framework.
That requires server-kill testing (see MTR notes below).

---

## Failure Classes

Every failure that the plugin must handle falls into exactly one
class. The class determines the expected delta — not which specific
op failed, not which key bytes were involved.

```python
class PluginFailureClass(Enum):
    SUCCESS                       = "success"
    OCC_CONFLICT                  = "apply_batch_returns_false"
    ENGINE_DEGRADED               = "apply_batch_throws_DbDegraded"
    ENGINE_IO_FAIL                = "apply_batch_throws_system_error"
    ENGINE_PARTIAL_COMMIT         = "engine_class_H_succeeds_partially"
    PLUGIN_ROWCOUNT_BEFORE_COMMIT = "fault_after_counter_bump"
    PLUGIN_INDEX_HALF_BUFFERED    = "fault_between_PK_and_secondary_buffer"
    PLUGIN_DDL_MIDPOINT           = "fault_between_catalog_apply_and_DDL_ack"
```

The first five are induced by reusing the engine's existing
`ScopedFaultInjector` to manipulate `apply_batch`'s outcome — no new
infrastructure required. The last three are induced by adding three
new `FAULT_INJECTION` checkpoints inside the plugin code (compiled
under `BYTECASK_TESTING`):

| Checkpoint | Location | Models |
|---|---|---|
| `plugin_after_row_count_update` | `ha_bytecaskdb::write_row` / `delete_row` after `track_row_count_delta` | P-INV-2 |
| `plugin_after_pk_buffer` | inside `write_row` / `update_row` between PK and first secondary `buffer_put` | P-INV-1 |
| `plugin_ddl_after_catalog_apply` | `ha_bytecaskdb::create` / `delete_table` after catalog `apply_batch` returns | P-INV-7 |

These three checkpoints are the only genuine *internal* phase
boundaries in the plugin. Everything else is logical state that the
engine fault classes already cover.

### Class behaviour summary

| Class | Plan submitted | Committed | Counter | snap_ released | MariaDB error |
|---|---|---|---|---|---|
| SUCCESS | Yes | Yes | Bumped & kept | Yes | 0 |
| OCC_CONFLICT | Yes | No (returns false) | Restored | Yes | 1213 |
| ENGINE_DEGRADED | Yes | No (throws) | Restored | Yes | HA_ERR_CRASHED |
| ENGINE_IO_FAIL | Yes | No (throws) | Restored | Yes | HA_ERR_CRASHED |
| ENGINE_PARTIAL_COMMIT | Yes | Yes (engine class H) | Bumped & kept | Yes | 0 + degraded |
| PLUGIN_ROWCOUNT_BEFORE_COMMIT | No | No | Restored | Yes | HA_ERR_GENERIC |
| PLUGIN_INDEX_HALF_BUFFERED | No | No | Restored | Yes | HA_ERR_GENERIC |
| PLUGIN_DDL_MIDPOINT | Catalog only | Catalog only | n/a | n/a | HA_ERR_GENERIC; recovery cleans up |

---

## Scenario Matrix

The matrix axes are the structurally distinct shapes the plugin must
handle. Combinations are filtered by elimination rules.

### DML shapes

| Shape | What it tests |
|---|---|
| `single_insert` | Minimal write path — one PK + N secondary buffer ops |
| `single_update_no_index_change` | `update_row` where indexed columns unchanged — secondaries should not be touched |
| `single_update_index_change` | `update_row` where an indexed column changes — old secondary deleted, new secondary inserted |
| `single_update_pk_change` | PK changes — old PK deleted, new PK inserted, all secondaries rewritten |
| `single_delete` | Tombstone for PK + all secondaries |
| `multi_row_insert` | Several `write_row` calls in one statement — batch causality |
| `mixed_batch` | INSERT + UPDATE + DELETE in one statement |
| `replace_existing` | `REPLACE` that resolves to delete-then-insert |
| `unique_dup_in_batch` | Two inserts in the same statement that collide on a unique secondary |

### Index topologies

| Shape | What it tests |
|---|---|
| `pk_only` | No secondary indexes — minimal plan, isolates P-INV-2 / P-INV-3 |
| `one_nonunique` | One non-unique secondary — exercises ordinary index sync |
| `one_unique` | One unique secondary — exercises P-INV-6 / P-INV-7 (unique-conflict path) |
| `multi_secondary` | Two non-unique + one unique — exercises full index-set rewrite on update |
| `pk_less` | No declared PK — synthetic rowid path |

### Transaction shapes

| Shape | What it tests |
|---|---|
| `autocommit` | One DML, one commit — most common path |
| `multi_statement` | Several statements before commit — `commit(all=false)` accumulation |
| `with_savepoint` | Savepoint set after K ops, rolled back, more ops, commit — P-INV-5 |
| `concurrent_w_w` | Two transactions writing the same PK — induces OCC_CONFLICT deterministically |
| `concurrent_unique` | Two transactions inserting different PKs with the same unique-secondary value |

### Elimination rules

1. **`unique_dup_in_batch` requires a `one_unique` or `multi_secondary` topology.** Other topologies cannot produce the conflict.
2. **`single_update_pk_change` requires `multi_secondary` or `pk_only`.** Tests both the index-rewrite path and the bare PK-rename path.
3. **`PLUGIN_INDEX_HALF_BUFFERED` requires at least one secondary index.** No checkpoint to fire on `pk_only`.
4. **`PLUGIN_DDL_MIDPOINT` is matrix-orthogonal — it runs against a separate DDL matrix (CREATE / DROP / ALTER) and ignores DML shapes.**
5. **`concurrent_*` transaction shapes only compose with `OCC_CONFLICT`.** Other failure classes are tested in single-transaction shapes.

A back-of-envelope count: 9 DML × 5 index × 3 single-statement
transaction shapes × 5 single-transaction failure classes ≈ 600 cases
before elimination, reducing to ~250–350 valid cases. Plus a small
concurrent matrix (~30 cases) and a DDL matrix (~20 cases). Order-of-
magnitude target: **a few hundred generated proof tests.** A real count
will only be available once a prototype generator runs against the
full elimination ruleset; further prunes (collapsing combinations whose
expected delta is identical, even if code paths differ) are likely.

---

## Approach

### Test seam

The plugin's unit-test stubs (`tests/unit/`) provide partial
simulation of `TABLE`, `Field`, and `KEY` — enough to drive
`MariaDBTxn` and the encoding layer directly. The `ha_bytecaskdb`
surface (multi-index `TABLE_SHARE`, `KEY` arrays with mixed
unique/non-unique flags, `KEY_PART_INFO` covering all field types
in the matrix) is **not** fully covered today. Building this framework
requires extending the stubs first; the gap is small but real and
should not be assumed away.

For the bulk of the matrix, no live `mariadbd` is required. Only the
DDL crash subset (P-INV-7's frm half, explicitly out of scope here)
needs MTR-style server kill.

### Generator pipeline

Mirroring the engine's pipeline:

```
bytecaskdb-mariadb-plugin/tests/proof/
├── __init__.py
├── scenario_matrix.py          ← DMLShape, IndexShape, TxnShape, PluginFailureClass
├── expected_delta.py           ← reference model: pre/post (PK set, secondary set, counter)
├── fault_point_resolver.py     ← maps (shape, failure) → ScopedFaultInjector config
├── generate_tests.py           ← emits prove_plugin.cpp
├── invariants.h                ← assert_index_synchronised, assert_counter_consistent,
│                                  assert_snapshot_identity, assert_ryow, assert_savepoint_revert,
│                                  assert_occ_error_mapping, assert_catalog_atomic
└── generated/
    └── prove_plugin.cpp        ← committed, never hand-edited
```

The model is the same shape as the engine's `expected_delta`: a pure
function from (shape, failure) to a `Delta` containing expected PK
keys, secondary keys, counter delta, error code, and degraded flag.
A test runs the scenario, collects an actual delta, and compares.

### Reusing the engine's fault injector

The first five failure classes (SUCCESS, OCC_CONFLICT, ENGINE_*) are
induced by configuring `ScopedFaultInjector` against engine
checkpoints already documented in the engine's framework:

- `OCC_CONFLICT` — set up a concurrent writer that touches a
  conflicting key before commit; no fault injector needed.
- `ENGINE_DEGRADED` / `ENGINE_IO_FAIL` — name-based injection on
  `io_data_file_append` / `io_data_file_sync`.
- `ENGINE_PARTIAL_COMMIT` — name-based injection on
  `io_rotate_file_creation` (engine class H).

The plugin proof tests do not need to reason about *which* engine
class fires — they assert the plugin's response to each
*outcome* the engine can produce.

### New plugin-level checkpoints

Three `FAULT_INJECTION` macros are added under `BYTECASK_TESTING`:

```cpp
// ha_bytecaskdb.cc, write_row():
txn->track_row_count_delta(table_id_, +1);
FAULT_INJECTION("plugin_after_row_count_update");
txn->buffer_put(pk_view, row_view);
FAULT_INJECTION("plugin_after_pk_buffer");  // before secondary loop
for (uint i = 0; i < table->s->keys; ++i) { ... }

// ha_bytecaskdb.cc, create():
db_->apply_batch({.sync=true}, std::move(catalog_plan));
FAULT_INJECTION("plugin_ddl_after_catalog_apply");
// ... rest of create() ...
```

Each checkpoint exercises a distinct plugin invariant. They are the
only internal phase boundaries that exist between an engine call and
externally-observable plugin state.

### Invariant helpers

Mirroring `tests/proof/invariants.h` in the engine:

| Helper | Asserts |
|---|---|
| `assert_index_synchronised(db, table_id)` | P-INV-1: walks `0x02[table_id]…` and `0x03[table_id]…`, verifies every secondary points to a live PK and every PK has the right secondaries. |
| `assert_counter_consistent(db, table_id, expected)` | P-INV-2: `row_count_atomic_ == expected` AND a fresh scan agrees. |
| `assert_snapshot_identity(txn, expected_snap_addr)` | P-INV-3: the snapshot pointer hasn't been replaced. |
| `assert_ryow(txn, k, expected)` | P-INV-4: txn read returns the merged-buffer answer. |
| `assert_savepoint_revert(txn_before, txn_after)` | P-INV-5: post-rollback `ops_` and `lookup_` match the prefix. |
| `assert_occ_error_mapping(rc, txn)` | P-INV-6: `rc == HA_ERR_LOCK_DEADLOCK` and txn is reset. |
| `assert_catalog_atomic(db, expected_tables)` | P-INV-7: catalog scan yields exactly the expected `TableMeta` set, no orphan IDs. |

`assert_consistent(db)` from the engine is reused as-is to prove the
underlying engine state is always structurally sound.

### What the proof matrix does NOT cover

The same honesty principle as the engine's framework applies — name
the gaps explicitly:

- **MariaDB callback failures.** `field->store()` returning an
  unexpected status, `key_copy` producing garbage. The MariaDB API
  contract is assumed to hold; validating it is out of scope.
- **Encoding correctness for new field types.** A new MariaDB type
  (JSON, INET6, extended temporal) needs an entry in
  `make_mem_comparable` and a regression in
  `mariadb_encoding_test.cpp` — not a proof matrix entry. The MTR
  `type_*_indexes` suite is the right venue for end-to-end
  validation.
- **DDL crash recovery semantics beyond catalog atomicity.** Whether
  MariaDB's frm/dictionary state agrees with our catalog after a
  server kill mid-DDL is an MTR concern. The plugin proof matrix
  covers the *catalog* side of P-INV-7; MTR covers the
  *frm-vs-catalog* side.
- **Replication, binlog, FK cascade execution.** Out of scope for
  this framework; covered by functional tests under
  `tests/functional/cases/`.
- **Performance regressions.** The proof matrix is a correctness
  ratchet, not a performance one. Sysbench (`tests/run-sysbench.sh`)
  remains the perf gate.

---

## Why this approach

The engine's correctness proof leaves a known gap: it assumes the
caller submits a *correct* `WritePlan`. The plugin is the caller. A
plugin bug that submits the wrong plan — a missing secondary `del`, a
counter bump that outlives a failed commit, a snapshot mid-swap — is
indistinguishable from correct behaviour to the engine's proof matrix.

Closing that gap requires asserting plugin-level invariants on
plugin-level state. Those invariants are stable, finite, and small
(seven of them). The shapes that can violate them are also stable,
finite, and small (a handful of DML × index × txn combinations). And
the failure classes the plugin must handle reduce to a short
taxonomy of engine outcomes plus three internal phase boundaries.

The Python-generator + Catch2 pipeline is the right tool because:

- The matrix is large enough to make hand-written tests miss
  combinations, but small enough to enumerate exhaustively.
- The reference model is pure: given a shape and a failure, the
  expected delta is mechanical to compute. That makes the model
  itself reviewable.
- The generator output is committed evidence. Regenerating with no
  diff means model + generator are stable. A diff means a deliberate
  change to one or the other.
- A new field type, DML shape, or failure class is a one-line matrix
  edit, after which every existing combination is automatically
  re-covered against it.

The plugin's "fault model" is mostly a **response taxonomy** to
engine outcomes, not an independent fault surface. That keeps the
framework small and lets it lean heavily on the engine's existing
infrastructure rather than duplicating it.

---

## Status

**Phase 1 complete.** The framework is implemented and running. The
first slice of the scenario matrix (3 DML × 3 index × 1 txn × 3
failure = 24 generated tests) passes, plus 4 hand-written proof tests
covering P-INV-1 through P-INV-4.

### What is live

- **Test stubs extended.** `tests/unit/mariadb_stubs.h` covers
  `TABLE`, `TABLE_SHARE`, `KEY`, `KEY_PART_INFO`, `Field_long`,
  `key_copy`, `key_restore`, and all types needed by the encoding
  layer and handler.
- **`libbytecask_testing.a`** — static archive with `BYTECASK_TESTING`
  for fault injection support (xmake target `bytecask_testing`).
- **`PluginTestHarness`** (`tests/proof/harness.h`, `harness.cpp`) —
  creates a live `bytecask::DB`, builds `TABLE`/`KEY`/`Field`
  structures from a `TableSpec`, registers catalog metadata, and
  provides `insert_row`, `update_row`, `delete_row`, `commit`,
  `rollback` entry points without a live MariaDB server.
- **Generator pipeline** — `scenario_matrix.py`, `expected_delta.py`,
  `fault_point_resolver.py`, `generate_tests.py` produce
  `tests/proof/generated/prove_plugin.cpp` (24 tests).
- **Invariant helpers** — `tests/proof/invariants.h` provides
  `assert_counter_matches_pk_count` (P-INV-1),
  `assert_counter_delta` (P-INV-2),
  `assert_sec_index_count_matches_pk` (P-INV-3),
  `assert_error_code` (P-INV-6).
- **Fault injection checkpoints** — `plugin_after_row_count_update`,
  `plugin_after_pk_buffer` added to `ha_bytecaskdb.cc`.
- **CMake targets** — `mariadb_proof_tests` (proof suite),
  `mariadb_txn_tests` (transaction unit tests), `mariadb_plugin_tests`
  (encoding/catalog unit tests).

### Bugs found and fixed by the framework

- **P-INV-2 violation (row counter not reverted on rollback).**
  `write_row` and `delete_row` called `catalog_row_count_add` directly,
  bypassing transaction tracking. On rollback or commit failure, the
  counter was never decremented. Fixed by routing through
  `MariaDBTxn::track_row_count_delta()`, which records the delta and
  reverts it in `rollback()` and on `commit()` failure paths.

### Current coverage

| DML shapes | `single_insert`, `single_delete`, `single_update_index_change` |
|---|---|
| Index topologies | `pk_only`, `one_nonunique`, `one_unique` |
| Transaction shapes | `autocommit` |
| Failure classes | `SUCCESS`, `OCC_CONFLICT`, `ENGINE_DEGRADED` |

Total: **28 proof test cases** (24 generated + 4 manual), **60 CTest
cases** including existing unit tests.

### Next steps

- Expand DML shapes: `single_update_no_index_change`,
  `single_update_pk_change`, `multi_row_insert`, `mixed_batch`.
- Expand transaction shapes: `multi_statement`, `with_savepoint`.
- Add remaining failure classes: `ENGINE_IO_FAIL`,
  `ENGINE_PARTIAL_COMMIT`, `PLUGIN_INDEX_HALF_BUFFERED`.
- Add `assert_index_synchronised` (full bidirectional PK↔secondary
  check) to invariant helpers.
- Target: ~250–350 generated proof tests covering the full matrix.