# ByteCaskDB SQL layer — Correctness Contract

What the SQL layer guarantees, and what it does not yet. *Definitions* and
*Conditions* come first, then one table per area: each row is one
guarantee, and its second cell names what fails if the row is broken: the
Catch2 tests of the layer, by name; the proof tests against PostgreSQL
(`tests/proof/`, as "proof: " and the pytest name); or none.

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
execution of a statement is one run; a statement retried after a conflict or
recompiled is several executions.

**Statement**: one SQL statement given to `Database::execute`, or to a
`Transaction`'s `execute`.

**Transaction**: the statements run through one `Transaction` from
`Database::begin()`, from the first to `commit()` or `rollback()`.

**Wrong plan**: a plan that calls the API in a way its contract does not
allow: a stale handle, an undeclared object, a type that is not the
column's, a second write of one row.

---

## Conditions

| Condition | What is required | If it does not hold | Proved by |
|---|---|---|---|
| **Plans stay in the subset** | A plan calls only the plan API, and loops only on its iterations. Only plans the layer's compiler generates are supported, and they do. | A plan outside the subset can do anything C can: the plan-safety rows below assume this condition. A plan that names a function outside the API does not link. | "tcc: a plan reaching outside the plan API does not link" |
| **One context, one thread** | A context is used by one thread at a time. Contexts run concurrently. | Undefined. | none |

---

## Plan safety

| Guarantee | Proved by |
|---|---|
| A plan in the subset has no undefined behaviour of its own: the arithmetic C leaves undefined is defined by the API (`bc_add` and the other helpers report overflow, division by zero and out-of-range conversion as `BC_E_ARITH`), and a stale or foreign handle or view is `BC_E_HANDLE`, never a dangling read. | "plan API: the arithmetic helpers define what C leaves undefined", "plan API: bc_sub reports overflow instead of wrapping", "plan API: a handle from an earlier execution is stale", "plan API: a cursor's views die when it steps" |
| No plan runs unbounded: every read and cursor step is charged to the execution's `bc_budget`, and its deadline is checked as the work is charged. Exhausting either is `BC_E_BUDGET`. In a transaction, a read the transaction's own writes answer is charged as a record read, and a read never goes past its bounds. | "plan API: the budget ends a plan, and scans stay inside their table", "plan API: a charge of several units does not step over the deadline check", "plan writes: the reads each read and write is charged", "transactions: a read stays in its bounds and passes a deleted range in one seek, every read charged", "transactions: a reverse scan charges the record at its bound it steps past" |
| A run never waits: it takes no lock another run holds, and the commit happens after the plan returns. Only `bc_ctx_begin` may wait, and only for the instant between a DDL of the same catalog reaching the database and the new catalog taking effect; never while the DDL writes or syncs. | "catalog: a reader pinned during a DDL's commit is not blocked and sees the old version" |
| Errors are sticky: after the first, every call returns a neutral value, the run ends, and nothing it planned is committed. | "plan API: an error is sticky and nothing is committed" |
| A wrong plan gets an error code, or a wrong answer from data it was entitled to read; never a crash, a corrupted database or a write outside the tables it declared. | "plan API: a plan names only the objects its reference declares", "plan API: a second commit and a too-wide result are plan errors, not UB or corruption", "plan writes: a second write of one row in one plan is refused" |
| No exception reaches a plan. An allocation that fails in any call is `BC_E_MEMORY`, and the process continues; a commit whose allocation fails is never reported `BC_COMMITTED`. | "plan API: an allocation failure in any call is BC_E_MEMORY, never an exception into the plan", "plan API: a commit whose allocations fail is never BC_COMMITTED with an error set" |
| Stored data that does not parse is `BC_E_CORRUPT`, never read as NULL or as another value: a row whose header does not parse, and a column the row marks present but does not hold, whether a plan reads it, copies the row or emits it, or SQL returns it. A column whose value runs past the row is `BC_E_CORRUPT` too, `bc_col_null` included. A column the row predates, not marked present, reads as NULL. | "plan API: a stored row that does not parse is BC_E_CORRUPT, never NULL", "plan API: bc_row_key and bc_row_from report a damaged row as BC_E_CORRUPT", "plan API: a present column the row does not hold is BC_E_CORRUPT, never 0, empty or NULL", "embedded API: a stored column that does not decode is BC_E_CORRUPT, never 0, empty or NULL", "row format: a row shorter than the layout reads as null, in both directions" |

---

## Errors

| Guarantee | Proved by |
|---|---|
| `BC_E_CONSTRAINT` is final: `bc_execute` does not retry it, and nothing of the execution is committed. Only `BC_CONFLICT` is retried. | "plan API: a raised constraint error is final: no retry, nothing committed", "plan writes: a duplicate is BC_E_CONSTRAINT and never retried" |
| A statement that conflicted on every retry is an `ExecError` whose `conflict()` is true, told apart from a failure with the same code (`BC_CONFLICT` and `BC_E_IO` are both 1). | "embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY", "proof: test_a_conflict_on_every_retry_sets_conflict" |
| A duplicate primary or unique key is `BC_E_CONSTRAINT` with `BC_C_UNIQUE` and the table's or the index's id; SQL reports it as SQLSTATE 23505. Nulls are distinct: a unique index part that is NULL collides with nothing. | "plan writes: a duplicate is BC_E_CONSTRAINT and never retried", "embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE", "proof: test_duplicate_primary_key_is_23505", "proof: test_sql_machine" |
| A value outside its column's declared type is `BC_E_CONSTRAINT` with `BC_C_RANGE` and the table's id, for every value a statement supplies, given or computed, and is checked when it is written; a column's DEFAULT is checked the same way at `CREATE TABLE` and `ADD COLUMN`, and a REAL DEFAULT is held as its literal rounded once to single precision, as PostgreSQL rounds a float4 literal, and a value copied from a stored row was checked when it was stored. A parameter a `WHERE` compares with a column is also checked before the plan runs, against that column's type in the catalog the plan runs on, without its modifier, as PostgreSQL types a `$n`: a lookup or bound key outside its column's type is refused, never wrapped, rounded to a neighbour or taken to find nothing. A `$n` compared with columns of two SQL types (a DECIMAL's scale included; a DECIMAL of scale 0 counts as a BIGINT) is refused as a SQL error, whatever its value, when the statement is compiled: at prepare, or at a recompile after a DDL. Refused: a SMALLINT or INTEGER out of range, a REAL that overflows or underflows float, a TEXT or VARCHAR that is not UTF-8 or holds a NUL, a VARCHAR(n) of more than n code points, a DECIMAL(p, s) of more than p digits, a DATE, TIMESTAMP or TIME outside PostgreSQL's range for it (a TIME from 00:00 to 24:00). SQL reports it as SQLSTATE 22003, 22001 for a string too long, 22021 for text that is not UTF-8, or 22008 for a DATE, TIMESTAMP or TIME. | "embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY", "binder: DDL types, defaults and the drop and alter forms", "proof: test_values_outside_their_declared_type_are_22003_and_22001", "proof: test_a_key_outside_its_type_is_refused", "embedded API: parameters are checked against the plan that will run", "binder: anything outside the scope is rejected with a message and position", "proof: test_a_parameter_compared_with_two_types_is_refused", "embedded API: a parameter compared with DECIMALs of two scales is refused", "embedded API: a parameter a recompile finds compared with two types is refused", "proof: test_every_range_message_maps_to_its_sqlstate", "proof: test_text_that_is_not_utf8_is_refused_on_write", "proof: test_sql_machine" |
| A NULL for a `NOT NULL` column, given or left unset with no default, a primary key column among them, is `BC_E_CONSTRAINT` with `BC_C_NOT_NULL` and the table's id; SQL reports it as SQLSTATE 23502. It is checked when the row is complete: a NULL a later value replaces is not refused, and a row that also holds a value outside its type is `BC_C_RANGE`, as PostgreSQL reports it. | "plan writes: a NOT NULL column null at bc_row_end is BC_C_NOT_NULL, never retried; one left out takes its default", "embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY", "proof: test_null_for_not_null_is_23502_and_defaults_fill_the_rest", "proof: test_insert_without_a_primary_key_column", "proof: test_sql_machine" |
| Every `BC_E_LAYOUT` carries a `bc_layout_detail`. `BC_L_INCOMPATIBLE` is the only one a recompile of the statement can cure. `BC_L_PLAN` is a malformed plan or `bc_schema_ref`; `BC_L_ROW` a row used with a table it is not of, or a column its layout lacks. | "plan API: bc_ctx_run checks what a plan depends on, not a catalog version", "plan API: result rows follow the schema ref's types; limits and types are checked", "plan API: a plan names only the objects its reference declares", "plan writes: a missing row and a wrong row are errors, never skipped" |
| `BC_L_RETIRED`, a row of a layout generation `REBUILD` retired, cannot occur in stage 1, which has no `REBUILD` (#503). | none |

---

## Statements

| Guarantee | Proved by |
|---|---|
| One autocommit statement is one atomic batch: all its writes commit, or none do. A statement in a transaction commits with its transaction (*Transactions*). | "plan API: an error is sticky and nothing is committed", "plan API: a raised constraint error is final: no retry, nothing committed" |
| A row a statement reads and then writes needs no guard: a concurrent write of that row makes the commit a conflict, retried on a fresh snapshot, or in a transaction a serialization failure. No update is lost, and none is applied to a row deleted concurrently. | "plan writes: concurrent read-then-write of one row loses no update", "plan writes: a delete racing updates of one row has one outcome", "embedded API: transactions on different threads run concurrently and lose no update" |
| A second write of one row within an execution, or within one statement of a transaction, is refused with `BC_E_HANDLE`, except an insert of a key the execution already inserted, which is `BC_C_UNIQUE`. A range deleted by `bc_del_range` counts as written. | "plan writes: a second write of one row in one plan is refused", "plan writes: a row written by one run is refused to a later run of the same execution", "transactions: the second-write rule is per statement" |
| `bc_insert`, `bc_update`, `bc_delete` and `bc_del_range` keep every index of the table exact: one entry per row and index, unique, `DESC` and nullable parts included, for every index the table has when the statement runs. | "plan writes: insert, update and delete keep every index exact, unique, DESC and nullable included", "plan writes: an index created after the plan was compiled is maintained by it", "embedded API: a cached write plan maintains an index created after it" |
| A row an index entry points to is never silently missing: `bc_scan_row` reports it as `BC_E_CORRUPT`. | "plan writes: a missing row and a wrong row are errors, never skipped" |

---

## Embedded API

| Guarantee | Proved by |
|---|---|
| Errors before a plan runs have their own types: parameters of the wrong number or type are `ParamError`, and a DDL statement the catalog refuses (a name taken or missing, a column or key part that does not exist or repeats) is `DdlRefused`. Neither writes anything. Both are `std::invalid_argument`. | "embedded API: the seven statements, then a fresh open", "embedded API: a plan the catalog refuses is recompiled and run again", "embedded API: compatibility decides which cached plans recompile" |
| `CREATE UNIQUE INDEX` over rows that hold a duplicate is an `ExecError` with `BC_E_CONSTRAINT`, `BC_C_UNIQUE` and the table's id, as a duplicate written later is; SQL reports it as SQLSTATE 23505. Nothing is written. | "embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE", "catalog: refused DDL writes nothing" |
| After `close()`, `prepare`, `execute`, `begin`, `catalog` and `table_rows` throw `DbClosed`; a second `close()` returns at once. `close()` rolls back every open transaction: its `execute` and `commit` throw `DbClosed`. | "embedded API: table_rows reads a table in key order; errors carry their detail; calls after close() throw", "embedded API: destroying an open transaction rolls it back; close() ends the open ones" |

---

## Transactions

| Guarantee | Proved by |
|---|---|
| A transaction from `Database::begin()` commits all its statements' writes atomically, or none: `commit()` returns, or throws having written nothing, but for `TransactionResolutionUnknown` (below). `rollback()`, and destroying an open `Transaction`, write nothing. | "embedded API: a transaction reads its own writes and commits them as one batch", "embedded API: destroying an open transaction rolls it back; close() ends the open ones", "transactions: a conflict at commit writes nothing", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits", "proof: test_a_transaction_reads_its_writes_and_commits_them", "proof: test_sql_machine" |
| A statement in a transaction reads the snapshot taken at the start of the transaction's first statement, not at `begin()`, plus the writes of its earlier statements, never its own: point reads, existence, counts, scans in both directions over tables and indexes, and the unique checks. | "transactions: statements read their transaction's earlier writes through every read path, and every index stays exact", "transactions: a statement does not see its own writes; the next one does", "transactions: the snapshot is taken at the first statement, not when the transaction is made", "proof: test_sql_machine" |
| A later statement may update or delete a row an earlier statement of its transaction wrote; every index stays exact. | "transactions: statements read their transaction's earlier writes through every read path, and every index stays exact", "transactions: the second-write rule is per statement" |
| A statement that fails inside a transaction writes nothing, and aborts the transaction: every later `execute()` throws `TransactionAborted`, SQLSTATE 25P02, until the transaction ends. An aborted transaction holds no snapshot. A statement that does not parse is still reported as the syntax error it is. | "transactions: a failed statement writes nothing and aborts the transaction", "transactions: an aborted transaction lets go of its snapshot; a freed one detaches its context", "embedded API: a failed statement aborts the transaction: 25P02 until it ends, nothing written", "embedded API: in an aborted transaction a statement that does not parse is a syntax error, not 25P02", "proof: test_every_statement_after_a_failed_one_is_25P02", "proof: test_sql_machine" |
| `commit()` reports what it did: `CommitOutcome::Committed`, or for an aborted transaction `CommitOutcome::RolledBack`, having written nothing, as PostgreSQL's COMMIT rolls back an aborted transaction. | "embedded API: a failed statement aborts the transaction: 25P02 until it ends, nothing written", "proof: test_every_statement_after_a_failed_one_is_25P02", "proof: test_sql_machine" |
| A transaction whose commit finds a key it updated or deleted changed by a concurrent commit since its snapshot throws `SerializationFailure`, SQLSTATE 40001, with nothing written. So does a statement whose plan does not fit the catalog the transaction's first statement saw, and the transaction ends; on the first statement itself, which has read nothing, a DDL racing it re-pins the transaction to the live catalog instead. The layer does not retry either. An allocation failure while the error is reported never changes it. | "embedded API: a conflict at commit is a serialization failure, and nothing is written", "transactions: a conflict at commit writes nothing", "embedded API: a statement that no longer fits the catalog a transaction pinned is 40001", "embedded API: a DDL racing a transaction's first statement re-pins it, not 40001", "transactions: a transaction no statement was added to can be pinned again", "embedded API: an allocation failure in commit() never changes its outcome", "embedded API: an allocation failure in an autocommit statement never changes its outcome" |
| A read that fails with an I/O, CRC or memory error fails the statement, and so aborts the transaction; it is never taken as absent. | "transactions: a read that fails is the statement's error, never absent", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits" |
| One context at a time runs a transaction's statements: from `bc_ctx_begin_txn` until it is reset, a context is attached to the transaction, and `bc_ctx_begin_txn` on another context is `BC_E_HANDLE`, changing nothing. `bc_txn_commit` touches no context and reports through the transaction (`bc_txn_error`): an ended transaction keeps the outcome its ending commit recorded (an unknown outcome stays one), an aborted one is `BC_E_HANDLE`, and so is one with a context still attached, which the refusal leaves open and unchanged. A statement left without `bc_txn_add` that failed or holds writes aborts its transaction; one that only read, or whose plan was refused as incompatible before it ran, does not. An abort and `bc_txn_free` detach the attached context, which then reads nothing of the transaction and keeps its error and its finished statement's results. | "transactions: one context at a time; a commit with a statement open is refused, changing nothing", "transactions: a statement left without bc_txn_add aborts the transaction if it failed or wrote", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits", "transactions: a statement's results stay readable after the free that detaches it", "transactions: every allocation of a statement's begin failed in turn leaves nothing dangling", "transactions: an aborted transaction lets go of its snapshot; a freed one detaches its context" |
| A statement that would take what the transaction holds past its bound (`TransactionOptions::max_bytes`, 64 MiB by default; counted conservatively, never less than the memory held) is `BC_E_LIMIT`, SQLSTATE 54000, refused before its writes are taken in, and aborts the transaction. | "transactions: a statement past the buffer bound is BC_E_LIMIT and aborts the transaction", "embedded API: a transaction past its buffer bound is BC_E_LIMIT and aborted" |
| A commit that fails and leaves the engine degraded, its batch perhaps already in the data file, throws `TransactionResolutionUnknown`, SQLSTATE 08007: its writes may be there after a reopen, whole or not at all. Every other failed commit wrote nothing. An autocommit statement and a DDL statement report the same. The layer may report 08007 for a commit that wrote nothing, when another writer's failure degraded the engine meanwhile; never the reverse. | "embedded API: an allocation failure in commit() never changes its outcome", "embedded API: an allocation failure in an autocommit statement never changes its outcome", "embedded API: a DDL whose commit may have reached the engine is 08007", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits" |
| Transactions on different threads, prepares and autocommit statements run at once: none waits for another's commit. `close()` waits for the calls in flight and ends every open transaction; a `Transaction`, moved from or outliving its `Database`, is then ended, and its calls throw rather than touch the closed database. A `Statement` runs only on the `Database` that prepared it: elsewhere it throws `std::logic_error`. | "embedded API: transactions commit concurrently while other threads prepare and run autocommit statements", "embedded API: transactions on different threads run concurrently and lose no update", "embedded API: a moved-from transaction has ended", "embedded API: a transaction may outlive its database, and close() racing it ends it", "embedded API: a statement runs only on the database that prepared it" |

---

## DDL

| Guarantee | Proved by |
|---|---|
| Each DDL statement is atomic: the catalog and the data it changes (an index's entries, a column's default, a dropped table's rows) change together or not at all, on disk and in memory, and survive a reopen. | "catalog: a DDL the engine refuses leaves nothing on disk or in memory", "catalog: refused DDL writes nothing", "catalog: every DDL survives a reopen", "proof: test_sql_machine" (the in-memory catalog after a refused DDL, and every DDL across a reopen) |
| A DDL never waits for another: one that finds another DDL of its catalog in progress fails at once, writing nothing, with `SerializationFailure` (SQLSTATE 40001) at the embedded API, and may be retried. DDL that do commit each publish their own catalog version, and a write to a table racing a DDL's backfill of it is a conflict. | "catalog: a DDL that finds another in progress fails at once and writes nothing", "embedded API: a DDL that finds another in progress is 40001 at once", "catalog: concurrent DDL each returns the version it published", "catalog: a backfill raced by a write, and two catalogs on one database, conflict" |
| Table, index and column ids are never reused, even after the object is dropped. | "catalog: two catalogs on one database interleave DDL without reusing an id", "catalog: counters of the wrong width or past the id ceiling are refused at load" |
| A catalog the build cannot decode is refused at open; it is never read as something else. | "catalog: a damaged or foreign-format record is refused" |
| A table has one primary key: `CREATE TABLE` declaring two, on two columns or on a column and the table, is refused, as PostgreSQL refuses it (42P16), and writes nothing. | "binder: DDL types, defaults and the drop and alter forms", "embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY", "proof: test_two_primary_keys_are_refused", "proof: test_sql_machine" |

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
| `Database::execute` runs each statement as its own transaction; `Database::begin()` runs several as one. | "embedded API: a transaction reads its own writes and commits them as one batch" |
| Statements the compiler generates run under snapshot isolation: each autocommit statement, and each transaction, reads one snapshot, and a write-write conflict with a concurrent commit is caught: retried for an autocommit statement, a serialization failure for a transaction. | "plan writes: concurrent read-then-write of one row loses no update", "plan writes: a concurrent insert of the same key conflicts with no absent guard" |
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
| In a transaction: a commit that conflicts on a key the transaction inserted is reported as a serialization failure, not yet as the duplicate (23505); DDL inside a transaction is refused, not yet with SQLSTATE 0A000; locking reads (SELECT … FOR UPDATE) are not supported. | #491 |
| Plans not produced by the layer's compiler are not supported. | none |
