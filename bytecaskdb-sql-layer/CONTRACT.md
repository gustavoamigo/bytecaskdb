# ByteCaskDB SQL layer — Correctness Contract

What the SQL layer guarantees, and what it does not yet. *Definitions* and
*Conditions* come first, then one table per area: each row is one
guarantee, and its second cell names what fails if the row is broken: the
Catch2 tests of the layer, by name, or none.

The test for every sentence here: if it could change while every guarantee
stays the same, it is mechanism and belongs in [`docs/`](docs/), not here.
The contract names only what `algebra/bc_plan.h` and the embedded API
(`api/database.cppm`) declare, plus SQL itself; CI enforces it. A change to
a guarantee is named in the pull request that makes it: added, widened,
narrowed or removed. The engine's own guarantees, which this layer builds
on, are in the engine's [`CONTRACT.md`](../CONTRACT.md).

The layer is in its second stage (#487). Where a guarantee is a stage-1
refusal, it says so: a later stage widens it, never contradicts it.

---

## Definitions

**Plan**: a function in the C subset of `docs/plan_api_design.md` that calls
the plan API and nothing else. The compiler generates one per statement;
tests hand-write some.

**Execution**: everything from `bc_ctx_begin` to `bc_ctx_commit` or
`bc_ctx_reset` on one context: one snapshot, one write plan. `bc_execute`
runs one execution, and repeats it after a conflict.

**Run**: one `bc_ctx_run` of a plan within an execution. In stage 1 every
statement is one execution of one run.

**Statement**: one SQL statement given to `Database::execute`.

**Wrong plan**: a plan that calls the API in a way its contract does not
allow: a stale handle, an undeclared object, a type that is not the
column's, a second write of one row.

---

## Conditions

| Condition | What is required | If it does not hold | Proved by |
|---|---|---|---|
| **Plans stay in the subset** | A plan calls only the plan API, and loops only on its iterations. The compiler's plans do; the checker that will refuse other text (#493) is not built. | A plan outside the subset can do anything C can: the plan-safety rows below assume this condition. A plan that names a function outside the API does not link. | "tcc: a plan reaching outside the plan API does not link" |
| **One context, one thread** | A context is used by one thread at a time. Contexts run concurrently. | Undefined. | none |

---

## Plan safety

| Guarantee | Proved by |
|---|---|
| A plan in the subset has no undefined behaviour of its own: the arithmetic C leaves undefined is defined by the API (`bc_add` and the other helpers report overflow, division by zero and out-of-range conversion as `BC_E_ARITH`), and a stale or foreign handle or view is `BC_E_HANDLE`, never a dangling read. | "plan API: the arithmetic helpers define what C leaves undefined", "plan API: bc_sub reports overflow instead of wrapping", "plan API: a handle from an earlier execution is stale", "plan API: a cursor's views die when it steps" |
| No plan runs unbounded: every read and cursor step is charged to the execution's `bc_budget`, and its deadline is checked as the work is charged. Exhausting either is `BC_E_BUDGET`. | "plan API: the budget ends a plan, and scans stay inside their table", "plan API: a charge of several units does not step over the deadline check", "plan writes: the reads each read and write is charged" |
| A plan never waits: a run takes no lock another run holds, and the commit happens after the plan returns. | none |
| Errors are sticky: after the first, every call returns a neutral value, the run ends, and nothing it planned is committed. | "plan API: an error is sticky and nothing is committed" |
| A wrong plan gets an error code, or a wrong answer from data it was entitled to read; never a crash, a corrupted database or a write outside the tables it declared. | "plan API: a plan names only the objects its reference declares", "plan API: a second commit and a too-wide result are plan errors, not UB or corruption", "plan writes: a second write of one row in one plan is refused" |
| No exception reaches a plan. An allocation that fails in any call is `BC_E_MEMORY`, and the process continues; a commit whose allocation fails is never reported `BC_COMMITTED`. | "plan API: an allocation failure in any call is BC_E_MEMORY, never an exception into the plan", "plan API: a commit whose allocations fail is never BC_COMMITTED with an error set" |
| Stored data that does not parse is `BC_E_CORRUPT`, never read as NULL or as another value. | "plan API: a stored row that does not parse is BC_E_CORRUPT, never NULL", "plan API: bc_row_key and bc_row_from report a damaged row as BC_E_CORRUPT" |

---

## Errors

| Guarantee | Proved by |
|---|---|
| `BC_E_CONSTRAINT` is final: `bc_execute` does not retry it, and nothing of the execution is committed. Only `BC_CONFLICT` is retried. | "plan API: a raised constraint error is final: no retry, nothing committed", "plan writes: a duplicate is BC_E_CONSTRAINT and never retried" |
| A duplicate primary or unique key is `BC_E_CONSTRAINT` with `BC_C_UNIQUE` and the table's or the index's id; SQL reports it as SQLSTATE 23505. Nulls are distinct: a unique index part that is NULL collides with nothing. | "plan writes: a duplicate is BC_E_CONSTRAINT and never retried", "embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE" |
| A NULL for a `NOT NULL` column, given or left unset with no default, is `BC_E_CONSTRAINT` with `BC_C_NOT_NULL` and the table's id. The column keeps its default. | "plan writes: a NULL for a NOT NULL column is BC_C_NOT_NULL, never retried, and keeps the default" |
| Every `BC_E_LAYOUT` carries a `bc_layout_detail`. `BC_L_INCOMPATIBLE` is the only one a recompile of the statement can cure. `BC_L_PLAN` is a malformed plan or `bc_schema_ref`; `BC_L_ROW` a row used with a table it is not of. | "plan API: bc_ctx_run checks what a plan depends on, not a catalog version", "plan API: result rows follow the schema ref's types; limits and types are checked", "plan API: a plan names only the objects its reference declares", "plan writes: a missing row and a wrong row are errors, never skipped" |
| `BC_L_RETIRED`, a row of a layout generation `REBUILD` retired, cannot occur in stage 1, which has no `REBUILD` (#503). | none |

---

## Statements

| Guarantee | Proved by |
|---|---|
| One statement is one atomic batch: all its writes commit, or none do. | "plan API: an error is sticky and nothing is committed", "plan API: a raised constraint error is final: no retry, nothing committed" |
| A row a statement reads and then writes needs no guard: a concurrent write of that row makes the commit a conflict, retried on a fresh snapshot. No update is lost, and none is applied to a row deleted concurrently. | "plan writes: concurrent read-then-write of one row loses no update", "plan writes: a delete racing updates of one row has one outcome" |
| A second write of one row within an execution is refused with `BC_E_HANDLE`, except an insert of a key the execution already inserted, which is `BC_C_UNIQUE`. A range deleted by `bc_del_range` counts as written. | "plan writes: a second write of one row in one plan is refused", "plan writes: a row written by one run is refused to a later run of the same execution" |
| `bc_insert`, `bc_update`, `bc_delete` and `bc_del_range` keep every index of the table exact: one entry per row and index, unique, `DESC` and nullable parts included, for every index the table has when the statement runs. | "plan writes: insert, update and delete keep every index exact, unique, DESC and nullable included", "plan writes: an index created after the plan was compiled is maintained by it", "embedded API: a cached write plan maintains an index created after it" |
| A row an index entry points to is never silently missing: `bc_scan_row` reports it as `BC_E_CORRUPT`. | "plan writes: a missing row and a wrong row are errors, never skipped" |

---

## DDL

| Guarantee | Proved by |
|---|---|
| Each DDL statement is atomic: the catalog and the data it changes (an index's entries, a column's default, a dropped table's rows) change together or not at all, on disk and in memory, and survive a reopen. | "catalog: a DDL the engine refuses leaves nothing on disk or in memory", "catalog: refused DDL writes nothing", "catalog: every DDL survives a reopen" |
| Concurrent DDL serialise: each publishes its own catalog version, and a write to a table racing a DDL's backfill of it is a conflict. | "catalog: concurrent DDL each returns the version it published", "catalog: a backfill raced by a write, and two catalogs on one database, conflict" |
| Table, index and column ids are never reused, even after the object is dropped. | "catalog: two catalogs on one database interleave DDL without reusing an id", "catalog: counters of the wrong width or past the id ceiling are refused at load" |
| A catalog the build cannot decode is refused at open; it is never read as something else. | "catalog: a damaged or foreign-format record is refused" |

---

## Schema compatibility (stage 1)

| Guarantee | Proved by |
|---|---|
| A compiled statement stays valid across `ADD COLUMN` and `CREATE INDEX`: it runs without a recompile, its writes give the new column its default, and maintain the new index. | "embedded API: compatibility decides which cached plans recompile", "plan API: bc_ctx_run checks what a plan depends on, not a catalog version" |
| In stage 1, a statement that names a dropped table, index or column, or a column whose type changed, is refused with `BC_E_LAYOUT` and `BC_L_INCOMPATIBLE` before it runs; `Database::execute` recompiles it from its text and runs it again, or reports why it no longer binds. | "embedded API: a plan the catalog refuses is recompiled and run again", "plan API: bc_ctx_run checks what a plan depends on, not a catalog version" |
| Column ids are permanent: a column keeps its id for its life, and a new column never takes an id a column had. | "catalog: every DDL survives a reopen", "catalog: any name is accepted; plan constants come from ids and never collide" |
| A row written before a column was added reads the column's default when it was added with one, and NULL otherwise. | "embedded API: compatibility decides which cached plans recompile", "row format: a row shorter than the layout reads as null, in both directions" |
| In stage 1, a `NOT NULL` column added without a default is refused. | "binder: DDL types, defaults and the drop and alter forms", "catalog: refused DDL writes nothing" |

---

## Transaction isolation

| Guarantee | Proved by |
|---|---|
| Autocommit only: `Database::execute` runs each statement as its own transaction. | none |
| Statements the compiler generates run under snapshot isolation: each reads one snapshot, and a write-write conflict with a concurrent commit is caught and retried. | "plan writes: concurrent read-then-write of one row loses no update", "plan writes: a concurrent insert of the same key conflicts with no absent guard" |
| Serializable conflict detection only with hand-written guards (`bc_guard_unchanged`, `bc_guard_range`) over what a plan reads and does not write; generated statements emit none beyond the unique checks'. | none |
| A unique index is enforced by a check of its prefix on the snapshot and a range guard over it, so a concurrent insert of the same key is a conflict, and its retry reports the duplicate. | "tcc: a unique insert raced by another commit conflicts, and its retry reports the duplicate" |
| `BC_CONFLICT` is retried automatically, for autocommit statements only, a bounded number of times. | "tcc: a unique insert raced by another commit conflicts, and its retry reports the duplicate" |
| Isolation against concurrent histories of many statements is not checked by any tool yet. | none |

---

## Schema evolution

The rule catalog for evolving a schema while older statement text keeps
running arrives with stage 2.5, after the owner's contract discussion
(#487). Until then the stage-1 rules above are all there is.

---

## Exclusions

What the layer does not guarantee yet.

| Not guaranteed | Tracked in |
|---|---|
| A unique secondary key shared by two inserts of one statement is not caught. The compiler emits one write per plan, so no generated statement reaches it. | #507 |
| Multi-statement transactions, and a statement reading what an earlier statement of its transaction wrote. | #491 |
| Plans not produced by the compiler are not checked against the subset. | #493 |
