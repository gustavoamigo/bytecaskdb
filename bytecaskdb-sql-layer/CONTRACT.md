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
| A duplicate primary or unique key is `BC_E_CONSTRAINT` with `BC_C_UNIQUE` and the table's or the index's id; SQL reports it as SQLSTATE 23505. Nulls are distinct: a unique index part that is NULL collides with nothing. Two rows one statement gives one unique value are a duplicate too; so is a unique value a statement moves from one of its rows to another, whatever order it writes them in. | "plan writes: a duplicate is BC_E_CONSTRAINT and never retried", "embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE", "proof: test_duplicate_primary_key_is_23505", "plan writes: two rows one plan gives one unique value are a duplicate", "proof: test_one_unique_value_given_to_two_rows_is_23505", "proof: test_sql_machine" |
| A value outside its column's declared type is `BC_E_CONSTRAINT` with `BC_C_RANGE` and the table's id, for every value a statement supplies, given or computed, and is checked when it is written; a column's DEFAULT is checked the same way at `CREATE TABLE` and `ADD COLUMN`, and a REAL DEFAULT is held as its literal rounded once to single precision, as PostgreSQL rounds a float4 literal, and a value copied from a stored row was checked when it was stored. A parameter a `WHERE` compares with a column, or a `SET` adds to or subtracts from one, is also checked before the plan runs, against that column's type in the catalog the plan runs on, without its modifier (a DECIMAL operand of `SET`: 18 digits at the column's scale), as PostgreSQL types a `$n`: a lookup or bound key outside its column's type is refused, never wrapped, rounded to a neighbour or taken to find nothing. A parameter an `UPDATE` assigns to a column is checked before the plan runs against the column's whole type, so it is refused even when no row matches. A `$n` used with columns of two SQL types, compared with, assigned to or added to them in any order (a DECIMAL's scale included; a DECIMAL of scale 0 counts as a BIGINT), is refused as a SQL error, whatever its value, when the statement is compiled: at prepare, or at a recompile after a DDL. Refused: a SMALLINT or INTEGER out of range, a REAL that overflows or underflows float, a TEXT or VARCHAR that is not UTF-8 or holds a NUL, a VARCHAR(n) of more than n code points, a DECIMAL(p, s) of more than p digits, a DATE, TIMESTAMP or TIME outside PostgreSQL's range for it (a TIME from 00:00 to 24:00). SQL reports it as SQLSTATE 22003, 22001 for a string too long, 22021 for text that is not UTF-8, or 22008 for a DATE, TIMESTAMP or TIME. | "embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY", "binder: DDL types, defaults and the drop and alter forms", "proof: test_values_outside_their_declared_type_are_22003_and_22001", "proof: test_a_key_outside_its_type_is_refused", "embedded API: parameters are checked against the plan that will run", "binder: anything outside the scope is rejected with a message and position", "proof: test_a_parameter_compared_with_two_types_is_refused", "embedded API: a parameter compared with DECIMALs of two scales is refused", "embedded API: a parameter a recompile finds compared with two types is refused", "proof: test_every_range_message_maps_to_its_sqlstate", "proof: test_text_that_is_not_utf8_is_refused_on_write", "proof: test_column_plus_parameter_at_the_edges", "proof: test_a_parameter_used_with_two_types_is_refused", "proof: test_sql_machine" |
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

## Queries

| Guarantee | Proved by |
|---|---|
| A statement outside the supported SQL subset is refused with a SQL error naming the construct, before anything runs or is written. | "parser: forms outside the scope are rejected with a position", "binder: anything outside the scope is rejected with a message and position", "binder: COUNT, ORDER BY and column + parameter" |
| A `SELECT` returns exactly the rows its `WHERE` matches, whichever key the planner reads them by: the primary key or an index, by an equality prefix or a range, in either direction, every comparison the key does not take checked on each row. A comparison with NULL matches no row. | "proof: test_filters_on_what_the_key_does_not_take", "proof: test_sql_machine", "proof: test_the_five_transactions_agree_with_postgresql" |
| A `SELECT` with an empty select list returns one row of no columns for each row its `WHERE` matches, and none for no match, as PostgreSQL does, whether the rows are read from the table, from a covering index, from a join's pairs or under `FOR UPDATE`; `LIMIT` applies to those rows. A write statement returns no rows. | "plan API: a read-only ref that declares no result types allows rows of no columns", "plan API: a write plan returns no rows, and bc_emit_begin needs a running plan", "proof: test_an_empty_select_list_returns_a_row_of_no_columns_per_match" |
| `ORDER BY` returns rows in that order, NULLs placed as PostgreSQL places them (last for ASC, first for `DESC`), ties in any order; `LIMIT n` returns the first n, and `LIMIT 0` none. In the MVP this covers the orders the planner serves (below). | "proof: test_order_by_places_nulls_as_postgresql", "proof: test_sql_machine", "proof: test_the_five_transactions_agree_with_postgresql" |
| An aggregate query, of one table or of a join's pairs, returns one row (none under `LIMIT 0`). `COUNT` is the rows matched, or a column's non-NULL values among them, `COUNT(DISTINCT col)` its distinct non-NULL values (`-0.0` and `0.0` one), 0 over no rows. `SUM` and `MIN` are of a column's non-NULL values, NULL over none: `SUM` of a SMALLINT or INTEGER is a bigint, of a BIGINT or DECIMAL a numeric; `MIN` is of the column's type. `SUM` takes an integer or DECIMAL column, `MIN` those and a DATE, TIMESTAMP or TIME one; any other is refused. | "proof: test_count_from_the_keys_and_by_row", "proof: test_sum_and_min_over_rows_nulls_and_none", "binder: COUNT, ORDER BY and column + parameter", "codegen: MIN from the first entry past the NULLs, or a loop; SUM by a loop", "proof: test_count_distinct", "plan API: an aggregate's groups, keyed by a column, and the range check of a computed integer", "proof: test_a_join_on_a_column", "proof: test_the_five_transactions_agree_with_postgresql", "proof: test_sql_machine" |
| An `UPDATE` or `DELETE` writes each row its `WHERE` matches in the snapshot it began on, once, and reports how many; its own writes never feed back into it. A value outside its type in any row fails the statement, which writes nothing. | "codegen: UPDATE and DELETE write each row their key reaches, and count them", "proof: test_update_and_delete_every_row_a_key_reaches", "proof: test_the_five_transactions_agree_with_postgresql", "proof: test_sql_machine" |
| In a `WHERE`, `$n + k` or `$n - k`, k an integer literal, compared with an INTEGER column is an INTEGER: a sum outside INTEGER is `BC_C_RANGE`, SQLSTATE 22003, raised before any row is read, whatever the statement would match; a NULL `$n` matches nothing. It bounds a range on a key as a parameter does; compared by `=`, it is checked on each row and never names a key, so a `WHERE` with nothing else on the key is a full scan and refused. Compared with another column type, or with k past INTEGER, it is refused. | "plan API: an aggregate's groups, keyed by a column, and the range check of a computed integer", "binder: a join of two tables, qualified columns and integer literals", "codegen: what the planner refuses, with its message", "proof: test_a_bound_of_a_parameter_plus_an_integer", "proof: test_sql_machine" |
| `SET col = col + $n` and `col - $n`, and the same with an integer literal for `$n`, on SMALLINT, INTEGER and DECIMAL: a result outside the column's type is `BC_C_RANGE`, SQLSTATE 22003, and the statement writes nothing; a NULL operand gives NULL. | "proof: test_column_plus_parameter_at_the_edges", "proof: test_column_plus_an_integer_literal", "proof: test_sql_machine" |
| A `SELECT` of two tables returns the pairs of rows its `WHERE` matches: each table named by its whole primary key, or one reached from its parameters and the other by its whole primary key, its parts from parameters and the first one's columns by `=`. A NULL key part, a NULL column joined on, a value joined on too long for any key, or a missing row pairs with nothing. A column name both tables have is refused unless qualified by its table; two columns are compared only by `=`, one of each table, of one SQL type. | "codegen: a join is of two point lookups, in FROM order", "proof: test_a_join_on_a_column", "binder: a join of two tables, qualified columns and integer literals", "proof: test_a_join_of_two_point_lookups", "proof: test_the_five_transactions_agree_with_postgresql", "proof: test_sql_machine" |
| In the MVP, a `SELECT` inside the supported subset that no key answers in its order is refused with a SQL error whose message starts `no plan:`, before anything runs or is written: one no key's leading part is compared on (a full scan), an `ORDER BY` no key gives, one whose NULLs the chosen key can't place as PostgreSQL does, or a comparison on a DOUBLE or REAL column the chosen key does not take. A join, without `ORDER BY`, is of two tables each named by its whole primary key, or of one reached from its parameters and the other read by its whole primary key from parameters and the first one's columns, and is refused the same way otherwise. An `UPDATE` or `DELETE` takes its rows as a `SELECT` without `ORDER BY` does, and is refused the same way. | "codegen: what the planner refuses, with its message", "codegen: a join is of two point lookups, in FROM order", "codegen: UPDATE and DELETE write each row their key reaches, and count them", "proof: test_order_by_places_nulls_as_postgresql", "proof: test_sql_machine" |

---

## Embedded API

| Guarantee | Proved by |
|---|---|
| Errors before a plan runs have their own types: parameters of the wrong number or type are `ParamError`, and a DDL statement the catalog refuses (a name taken or missing, a column or key part that does not exist or repeats) is `DdlRefused`. Neither writes anything. Both are `std::invalid_argument`. | "embedded API: the seven statements, then a fresh open", "embedded API: a plan the catalog refuses is recompiled and run again", "embedded API: compatibility decides which cached plans recompile" |
| `CREATE UNIQUE INDEX` over rows that hold a duplicate is an `ExecError` with `BC_E_CONSTRAINT`, `BC_C_UNIQUE` and the table's id, as a duplicate written later is; SQL reports it as SQLSTATE 23505. Nothing is written. | "embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE", "catalog: refused DDL writes nothing" |
| After `close()`, `prepare`, `execute`, `begin`, `catalog` and `table_rows` throw `DbClosed`; a second `close()` returns at once. `close()` rolls back every open transaction: its `execute` and `commit` throw `DbClosed`. | "embedded API: table_rows reads a table in key order; errors carry their detail; calls after close() throw", "embedded API: destroying an open transaction rolls it back; close() ends the open ones" |
| `sqlstate` gives the SQLSTATE PostgreSQL reports for each error the API throws (XX000 for any other exception), the same one the PostgreSQL server sends, except that every binder error is 42601 where PostgreSQL's code depends on the error, 42P01 for an unknown table or 42703 for an unknown column (#548). A statement describes itself before it runs: `Statement::params` (each parameter's declared type, and whether every use assigns it), `columns` and `command`, of the plan its next run uses once `Database::refit` has fitted it to the live catalog; `refit` runs nothing. | "embedded API: a statement describes its parameters, result and command before it runs", "embedded API: sqlstate reports PostgreSQL's SQLSTATE for each error", "embedded API: refit describes a cached statement against the live catalog", "proof: test_every_range_message_maps_to_its_sqlstate" |

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
| A commit that conflicts because a key the transaction inserted, a primary key or a unique index value, was taken by a concurrent commit since its snapshot throws `ExecError` with `BC_E_CONSTRAINT` and `BC_C_UNIQUE`, SQLSTATE 23505, naming the table or the index, with nothing written; the transaction ends. An autocommit statement raced the same way reports the same. A key the snapshot held, deleted by an earlier statement and inserted again, is not such a key, nor is one a later statement moved or deleted away, nor a unique value a concurrent update gave the same row: their conflict is 40001. Nor is the key reported when a row that the inserting statement, or one before it, locked with `FOR UPDATE`, updated or deleted was changed by a concurrent commit since the snapshot: the first conflict in statement order is reported, as PostgreSQL reports it, and that one is 40001. A row changed and then changed back to the same stored bytes counts as unchanged. | "embedded API: a key a transaction inserted that a concurrent commit took is 23505 at commit unless an earlier statement's row changed", "tprocc: a short mixed run keeps the specification's consistency conditions", "tcc: an insert whose last retry conflicts on a key a racing commit took reports the duplicate", "tcc: a unique insert raced by another commit conflicts, and its retry reports the duplicate" |
| `SELECT … FOR UPDATE` returns the rows the same `SELECT` returns. If a concurrent commit since the snapshot changed or deleted one of them, the commit of its transaction throws `SerializationFailure`, SQLSTATE 40001, with nothing written, whether or not the transaction wrote anything. In autocommit it reads as the `SELECT` does and asks the engine for nothing. On a follower or a degraded engine, a transaction that took `FOR UPDATE` rows fails at commit with `ExecError` and `BC_E_DEGRADED`, as a write would, with nothing written. It never waits, and never holds back another writer. `FOR SHARE`, `FOR NO KEY UPDATE`, `NOWAIT`, `SKIP LOCKED` and `FOR UPDATE OF` are refused. | "embedded API: SELECT … FOR UPDATE makes the commit 40001 if a row it returned changed", "codegen: FOR UPDATE guards the key of every row it returns, read from the row", "golden: every plan the compiler generates equals its golden file", "parser: SELECT … FOR UPDATE, and no other locking clause", "proof: test_sql_machine" |
| A DDL statement given to a `Transaction` throws `FeatureNotSupported`, SQLSTATE 0A000, before anything runs, and aborts the transaction. A DDL run outside commits at once, whatever transactions are open: one that wrote a table the DDL changed throws `SerializationFailure` at commit, with nothing written; one that only read it commits, unless it took `FOR UPDATE` rows the DDL rewrote: 40001. | "embedded API: DDL given to a transaction is 0A000 and aborts it", "embedded API: a DDL on a table an open transaction wrote makes its commit 40001; one it read does not" |
| A read that fails with an I/O, CRC or memory error fails the statement, and so aborts the transaction; it is never taken as absent. | "transactions: a read that fails is the statement's error, never absent", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits" |
| A statement that would take what the transaction holds past its bound (`TransactionOptions::max_bytes`, 64 MiB by default; counted conservatively, never less than the memory held) is `BC_E_LIMIT`, SQLSTATE 54000, refused before its writes are taken in, and aborts the transaction. | "transactions: a statement past the buffer bound is BC_E_LIMIT and aborts the transaction", "embedded API: a transaction past its buffer bound is BC_E_LIMIT and aborted" |
| A commit that fails and leaves the engine degraded, its batch perhaps already in the data file, throws `TransactionResolutionUnknown`, SQLSTATE 08007: its writes may be there after a reopen, whole or not at all. Every other failed commit wrote nothing. An autocommit statement and a DDL statement report the same. The layer may report 08007 for a commit that wrote nothing, when another writer's failure degraded the engine meanwhile; never the reverse. | "embedded API: an allocation failure in commit() never changes its outcome", "embedded API: an allocation failure in an autocommit statement never changes its outcome", "embedded API: a DDL whose commit may have reached the engine is 08007", "transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done commits" |
| Transactions on different threads, prepares and autocommit statements run at once: none waits for another's commit. `close()` waits for the calls in flight and ends every open transaction; a `Transaction`, moved from or outliving its `Database`, is then ended, and its calls throw rather than touch the closed database. A `Statement` runs only on the `Database` that prepared it: elsewhere it throws `std::logic_error`. | "embedded API: transactions commit concurrently while other threads prepare and run autocommit statements", "embedded API: transactions on different threads run concurrently and lose no update", "embedded API: a moved-from transaction has ended", "embedded API: a transaction may outlive its database, and close() racing it ends it", "embedded API: a statement runs only on the database that prepared it" |

---

## The PostgreSQL wire protocol

The server (sql_layer_server, docs/server_design.md) serves one database
to libpq clients over protocol 3.0. "wire:" names a test in
`tests/server/test_wire.py`.

| Guarantee | Proved by |
|---|---|
| Startup takes protocol 3.0, and a newer 3.x is negotiated down to it. Authentication is trust only: anyone who reaches the port is in. A request for SSL or GSSAPI encryption is answered no, and the client goes on in plaintext. The client encodings taken are UTF8, its alias UNICODE, and SQL_ASCII, with no conversion, as PostgreSQL does none for SQL_ASCII. Started with no database name, the server names its database after the directory's last component. Refused as FATAL, and the connection closed: another database name (3D000), an unknown startup parameter (42704), another client encoding (22023), no user (28000), another protocol (0A000), a connection past the limit (53300). After a FATAL nothing more the connection sent is run. | "wire: test_ssl_and_gssenc_requests_are_answered_n", "wire: test_a_newer_minor_version_is_negotiated_down_to_3_0", "wire: test_startup_refusals_are_fatal", "wire: test_utf8_its_alias_and_sql_ascii_are_the_client_encodings_taken", "wire: test_the_default_database_name_is_the_directory_s_last_component", "wire: test_the_connection_past_the_limit_is_refused_53300", "wire: test_a_bad_message_length_is_fatal_and_nothing_after_it_runs" |
| A statement over the wire does what it does through the embedded API: the same rows, counts and SQLSTATEs, and the same block status. Values travel in text format, written as PostgreSQL 17 writes them, with its type OIDs, and parameters are read as the type the layer inferred for them, in PostgreSQL's input forms for the types and forms docs/server_design.md lists. | "proof: test_sql_machine" (its wire target), "wire: test_text_forms_match_postgresql", "wire: test_describe_statement_gives_parameter_types_and_columns", "wire: test_a_parameter_its_type_does_not_take_is_22p02" |
| `BEGIN`, `COMMIT` and `ROLLBACK` are SQL statements, with PostgreSQL's tags and warnings (25001, 25P01). Any error inside `BEGIN` fails the block: every statement but `COMMIT` and `ROLLBACK` is 25P02 until one of them ends it, and `COMMIT` then answers `ROLLBACK`, having written nothing. A connection that ends, by Terminate or dropped, rolls back its open transaction. Outside `BEGIN`, the statements of one extended-query block up to its Sync commit together or not at all, a Query sent before the Sync included; one alone runs as an autocommit statement, and so does one whose Flush and Sync have both arrived when it runs. One followed by a Flush with nothing after it yet runs in the block's transaction, committed at its Sync, and its reply is sent at once; a DDL sent that way is refused (0A000). A DDL statement runs only alone, and is refused (0A000) beside others, with nothing committed. A portal ends with its transaction, and an Execute of it after that is 34000. | "wire: test_an_error_inside_begin_fails_the_block_until_its_end", "wire: test_warnings_for_begin_inside_a_block_and_commit_outside_one", "wire: test_terminate_and_a_dropped_connection_roll_back_an_open_transaction", "wire: test_executes_before_one_sync_are_one_implicit_transaction", "wire: test_ddl_runs_alone_and_is_refused_beside_another_statement", "wire: test_an_error_discards_messages_up_to_sync", "wire: test_a_query_inside_an_extended_block_joins_its_transaction_and_commits_it", "wire: test_execute_flush_sync_is_one_statement", "wire: test_portals_end_with_their_transaction", "proof: test_sql_machine" |
| `SET` of client_encoding to an encoding startup takes is taken, and answers `SET`; to another encoding it is 22023. Refused, with nothing run: binary format (0A000), Execute with a row limit (0A000), several statements in one Query (0A000), any other `SET`, `SHOW` and SQL `PREPARE` (0A000), a parameter in a simple Query (42P02), and a compared or added DECIMAL parameter finer than its column's scale, or NaN (22P02). | "wire: test_simple_query_refusals_and_the_empty_query", "wire: test_set_client_encoding_is_the_one_set_taken", "wire: test_binary_format_and_a_row_limit_are_refused", "wire: test_a_compared_decimal_finer_than_its_scale_and_nan_are_refused" |
| SIGTERM stops the server: each connection finishes the statement it is running, is sent FATAL 57P01, and its open transaction rolls back; the database is then closed. A client still reading gets the reply to its last statement before the 57P01, however long that reply takes; one that takes no reply bytes for ten seconds once the stop has begun is closed without the 57P01, and the stop still ends. | "wire: test_shutdown_sends_57p01_to_an_idle_connection", "wire: test_a_stop_drops_a_client_that_stopped_reading", "wire: test_a_stop_lets_a_client_still_reading_take_its_whole_reply", "proof: test_sql_machine" (its wire target, across each reopen) |

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
| A plan that deletes a row and then inserts a row holding the deleted row's unique secondary key value is refused with `BC_C_UNIQUE`, where the result would be unique. No generated statement both deletes and inserts. | #507 |
| Plans not produced by the layer's compiler are not supported. | none |
| Overflow in `SET col = col + $n` or `col - $n` on a BIGINT, or a DECIMAL of scale 0 (a BIGINT here), is `BC_E_ARITH`, not yet `BC_C_RANGE` (SQLSTATE 22003). | #540 |
| A `SUM` whose running total leaves 64 bits, a DECIMAL's in its scaled integer, fails the statement with `BC_E_ARITH`; PostgreSQL's numeric sum holds it. | none |
| A join reads each inner row, and builds its key and a `COUNT(DISTINCT)` group key, in memory the statement holds until it ends, a few MiB: a join over enough outer rows to fill it fails with `BC_E_MEMORY`. | #551 |
| A `COUNT(DISTINCT)` whose distinct values take more than 16 MiB, each counted as its encoded bytes plus a fixed 112, fails the statement with `BC_E_MEMORY`, of one table or of a join's pairs; PostgreSQL spills to disk. | none |
| A DECIMAL literal operand of `SET col = col + n` or `col - n` past 18 digits once scaled is refused at bind ("numeric field overflow"), whatever the column holds; PostgreSQL fails only when the sum does not fit the column. | none |
| The planner's refusals carry no SQLSTATE yet (PostgreSQL would run each statement; the design gives 0A000). | #541 |
