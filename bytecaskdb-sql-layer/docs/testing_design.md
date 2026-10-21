# SQL layer — proof testing

The SQL layer is checked against PostgreSQL 17 by one model-based test: a
Hypothesis state machine that runs every statement on both and requires them
to agree (#515). This document covers the approach, how to run it, and the
differences from PostgreSQL found so far. Increment 1 of #515 built it; later
increments add rules to the same machine.

## Approach

`tests/proof/test_sql_machine.py` holds one `RuleBasedStateMachine`. Each
rule is an action: it runs one statement on the layer, through the
`bytecask_sql` Python module, and on PostgreSQL, through psycopg. After
every rule, the invariants are checked.

- **PostgreSQL holds the expected data.** The machine holds only the
  expected catalog: the tables it created, their columns and primary keys.
- **Outcomes agree.** Both sides accept a statement or both refuse it. A
  constraint violation must be the same one: a duplicate key is SQLSTATE
  23505 on both, a NULL in a NOT NULL column 23502, a value outside its
  declared type 22003, 22001 for a string too long, 22008 for a date or
  time, 22021 for text that is not UTF-8. The layer has one kind for these,
  `range`; the oracle tells them apart by how the message starts, in one
  table (`RANGE_SQLSTATE` in `oracle.py`), and a test produces every
  message, so a reworded one fails there. Any other refusal counts
  as a refusal on both, whatever its message. The layer's refusals are
  `SqlError`: parse and bind errors, a DDL the catalog refuses, parameters
  of the wrong number or type; the planner's "no plan" refusal is one
  too, and follows the next rule instead. Any other exception from the
  layer, an
  engine refusal at run time included, fails the test.
- **Statements the planner serves.** The machine generates only statements
  the planner serves, and a "no plan" refusal
  (`docs/frontend_design.md`, *What the planner refuses*) fails the test.
  The planner's refusals are tested by their messages in
  `sql_layer_tests`.
- **Counts agree.** An INSERT, UPDATE or DELETE changes as many rows on both.
- **Rows agree.** A lookup returns the same rows. After every step, each
  table holds the same rows on both. The layer's rows are read from the
  engine by `table_rows`, since SQL has no full scan. PostgreSQL's come from
  `SELECT … ORDER BY` the primary key. `table_rows` is test only: it is
  compiled into `sql_layer_testing`, the build of the library with
  `BYTECASK_SQL_TESTING` that the tests and `bytecask_sql` link, and is not
  in `sql_layer`.
- **Values compare as values.** `bytecask_sql` converts each value by its
  column's SQL type, and the oracle (`tests/proof/oracle.py`) converts
  PostgreSQL's the same way. Values compare by type too: `True` is not `1`,
  `1` is not `1.0`, and `-0.0` is not `0.0`. Text columns are `COLLATE "C"`
  in PostgreSQL, so its order is byte order, the order of the layer's keys.
  A REAL comes from PostgreSQL as the shortest text naming its float; the
  oracle packs it back to that float, which is what the layer stores. A
  lookup by a SMALLINT, INTEGER or REAL key casts `$n` to the column's type
  on PostgreSQL's side: the layer types `$n` by its column, and psycopg
  tells PostgreSQL int8 or float8.

Rules in increment 1:
- **CREATE TABLE.** Up to four columns over BIGINT, INTEGER, SMALLINT, DOUBLE
  PRECISION, REAL, BOOLEAN, TEXT, VARCHAR(4) and BYTEA. Primary keys of one or two
  columns, NOT NULL and nullable columns, and DEFAULT literals. Refused
  variants: an existing name, a duplicate column, a primary key naming no
  column, and two primary keys (on two columns, or on a column and the
  table).
- **DROP TABLE**, with and without `IF EXISTS`.
- **INSERT**, weighted towards duplicate keys, NULLs, left-out NOT NULL
  columns (a primary key column among them) and values outside their
  declared types, with and without a column list.
- **Point lookup** by the full primary key, an existing key or not, NULL
  and values outside the key's type included: both refuse a value outside
  the type's range (22003, also when another key part is NULL), and find
  nothing for one only longer than a VARCHAR(n).
- **Reopen** of the layer's database.

Rules for transactions (#491):
- **BEGIN**, then the inserts and lookups above inside the transaction,
  which read its own writes. PostgreSQL runs a real `REPEATABLE READ`
  transaction on its connection, which also takes its snapshot at the
  first statement.
- **COMMIT** and **ROLLBACK** of any transaction. After a failed
  statement both sides answer every statement with 25P02, except one that
  does not parse, which both report as the syntax error, and COMMIT rolls
  back on both: the layer's `commit()` and PostgreSQL's command tag both
  say ROLLBACK. After each statement the two must agree on whether the
  transaction is aborted.
- The tables are compared while no transaction is open; CREATE, DROP and
  the reopen run only then. One connection on each side: nothing commits
  during a transaction, so no commit conflicts.

Fixed cases are plain pytest functions at the end of the file. They call
the machine's steps in a set order. Every failure Hypothesis finds is
committed as one.

## Running

```sh
xmake f --sql_layer=y && xmake build bytecask_sql
pip install -r bytecaskdb-sql-layer/tests/proof/requirements.txt
BYTECASK_SQL_ORACLE="host=localhost user=postgres password=postgres dbname=postgres" \
PYTHONPATH=bytecaskdb-sql-layer/python \
  python3 -m pytest bytecaskdb-sql-layer/tests/proof
```

- **Without `BYTECASK_SQL_ORACLE`** the tests skip.
- **Each test** gets a database directory and a PostgreSQL schema of its own.
- **`HYPOTHESIS_PROFILE=ci`** is derandomized, with a fixed seed and a
  bounded budget. `sql-layer.yml` runs it on every pull request, against a
  PostgreSQL 17 service container.
- **The default profile, `dev`,** explores.

## Sanitizers

`sql-layer.yml` runs `sql_layer_tests` under ASan, TSan and UBSan through
the engine's `sanitizers.yml`. Under ASan the allocation-failure sweeps,
the `[alloc]` tests, run in an invocation of their own (TSan and MSan
replace `operator new`, so the sweeps skip there). Under a failed allocation the
engine's key directory leaks a node rather than free it while still
reachable (#390), and only that invocation tells LeakSanitizer so, with
`tests/lsan_alloc_sweeps.supp`. Every other test runs with no suppression.
Locally, with an ASan build:

```sh
build/linux/x86_64/debug/sql_layer_tests "~[alloc]"
LSAN_OPTIONS=suppressions=$PWD/bytecaskdb-sql-layer/tests/lsan_alloc_sweeps.supp \
  build/linux/x86_64/debug/sql_layer_tests "[alloc]"
```

## The MVP gate harness

Status: **design, approved (owner, 2026-10-21).** The MVP gate (#487, *MVP*;
`docs/frontend_design.md`, *The MVP gate* and Decision 7) runs TPROC-C's
transaction mix through SQL at 1, 16 and 48 threads and reports the
layer's own numbers. This section is what the harness does; the
statements it sends and the `h_id` scheme are `frontend_design.md`'s
(*Where the statements come from*, *The statements*), and are not
repeated here.

**One binary, two modes.** `bench/tpcc.cpp`, xmake target `sql_layer_tpcc`
(not built by default, as `sql_layer_bench`). `sql_layer_tpcc load`,
`sql_layer_tpcc run` and `sql_layer_tpcc elle`. The loader, the
transactions and the checks are one module, `bytecask.sql.tpcc`, which
the smoke test in `sql_layer_tests` uses too. Everything goes through the
embedded API (`Database`, `Transaction`, `Statement`), in the harness's
own process. It links the testing build of the layer, as `bytecask_sql`
does, for the two reads the API keeps there: `Database::table_rows` for
the consistency check and the engine's `stats()`. Every statement is prepared once, before the threads start,
and shared by them.

**The loader.**
- `load --dir D --warehouses W`: the schema fixture (`tests/tpcc_schema.cppm`),
  then TPC-C's initial population (clause 4.3.3): 100,000 items; per
  warehouse 100,000 stock rows and 10 districts; per district 3,000
  customers with one history row each, 3,000 orders with their lines, and
  the last 900 orders in `new_order`. Random strings, `c_last` from the
  syllables, `NURand` with its constants, 10% `BC` credit, as the clause
  says.
- One prepared `INSERT` per table, many rows per `Transaction`: as many as
  fit well inside its 64 MiB bound.
- The loader inserts `harness_run`'s row `(1, 0)` and uses run 0 for its
  `h_id`s.
- The cardinalities are parameters with the specification's values as
  defaults. Only the smoke test shrinks them.

**The driver.**
- `run --dir D --threads 1,16,48 --duration S --warmup S --seed N`. Each
  thread count is one phase on the same database, run in order. Each phase
  bumps `harness_run` for its own run, so a phase's thread indices never
  reuse another phase's `h_id`s.
- Closed loop, no keying or think time, as HammerDB's default. Thread `t`
  has home warehouse `t mod W + 1`, and a per-thread generator seeded from
  `--seed` and `t`.
- The mix is the specification's deck (New-Order 45, Payment 43, and
  Order-Status, Delivery and Stock-Level 4 each), drawn per transaction.
  It is restricted to the transactions built so far and renormalised.
  Payment and Order-Status come first, with #542; each later front-end
  slice adds its transaction to the deck and to the inputs below.
- Inputs follow the profiles (clauses 2.4 to 2.8): customers by last name
  60% of the time and by `NURand` id otherwise; a remote warehouse for 15%
  of Payments when `W > 1`; `h_amount` from 1.00 to 5,000.00; a `BC`
  customer's new `c_data`. The client logic is the harness's, as in
  HammerDB's client-side driver.
- One `Transaction` per TPROC-C transaction, committed with `commit()`.

**Errors and retries.**
- `SerializationFailure` (40001) reruns the transaction with the same
  inputs in a new `Transaction`, up to 10 attempts. Retries are counted
  per transaction type. A transaction still failing after its tenth
  attempt is counted as abandoned and the thread moves on.
- New-Order's 1% of orders naming an unused item roll back, as the
  specification requires, and count as completed.
- Any other error stops the run with a non-zero exit: 23505,
  `TransactionResolutionUnknown`, a constraint, an I/O error. In TPROC-C
  none is expected, so each one is a bug to look at, not a number.

**What it reports.** Per phase:
- per transaction type, completed, retries and abandoned, and latency
  p50, p95, p99 and max, from the first attempt's start to the commit that
  completed it;
- TPM, completed transactions of every type per minute, and NOPM, completed
  New-Orders per minute (`n/a` until New-Order is in the mix);
- the engine's `fsyncs` and `group_writer_batches` over the phase, read
  from `stats()`, to set beside `commit_probe`.

Latencies go into a fixed log-bucket histogram per thread, from 1 µs to
10 s, merged at the end, so a long run holds no per-transaction list. The
report is printed as a table. With `--json FILE`, one JSON object per
phase is appended to FILE as well. These runs are the only performance
numbers the harness produces; there are no micro-benchmarks.

**Consistency after a run.** After the last phase, the harness checks
TPC-C's consistency conditions (clause 3.3.2) for the transactions in the
mix, reading every table through `Database::table_rows`:
- with Payment: condition 1 (`w_ytd` is the sum of its districts'
  `d_ytd`), and conditions 8 and 9 (`w_ytd` and `d_ytd` are each the sum of
  their `history` rows' `h_amount`);
- New-Order and Delivery add conditions 2 to 4 and 5 to 7 when they land.

A failed condition exits non-zero.

**The smoke test.** `tpcc: a short mixed run keeps TPC-C's consistency
conditions` is a Catch2 test in `sql_layer_tests`. It loads two warehouses
at a tiny scale, then runs four threads for a few hundred transactions. It
requires that every type in the mix completed at least once, that no
error other than 40001 occurred, and that the consistency conditions hold.
It is the only part CI runs: full runs and Elle are manual.

**Elle through SQL.** `elle` records a list-append history through SQL, in
the engine harness's history format (`docs/isolation_checking_design.md`),
and `scripts/sql_isolation_check.py` checks it with the engine's tooling:
`run_elle` and `cross_check` from `scripts/run_isolation_check.py`.
- One table, `elle (k BIGINT PRIMARY KEY, v VARCHAR(2048))`. Each key holds
  its list as decimals separated by commas. Every key's row exists before
  the clients start, empty, so every read returns a row.
- Keys rotate as in the engine harness: 8 active keys, retired after 64
  appends.
- A transaction is 1 to 4 micro-operations in one `Transaction`. A read is
  `SELECT v FROM elle WHERE k = $1`; an append reads the same way, then
  `UPDATE elle SET v = $2 WHERE k = $1`.
- `guarded`: the reads of keys the transaction does not append use
  `FOR UPDATE`. It must be valid under `strict-serializable`.
- `unguarded`: plain reads. It must be valid under `snapshot-isolation`,
  and under `strict-serializable` report only write skew (`G2-item`), at
  least once over the run.
- `blind`: every statement in autocommit, with no `Transaction`. It must
  be invalid, which shows the check can see a lost update.
- A completed `commit()` is `:ok`. A `SerializationFailure` is `:fail`.
  `TransactionResolutionUnknown` is `:info`. An error in a statement
  aborts the transaction, which is `:fail`.
- **The one difference from the engine's check:** `commit()` returns no
  commit sequence, so the cross-check's rule that a key's final list
  follows commit-sequence order cannot apply. Every other rule does:
  reads are prefixes, nothing failed is visible, every `:ok` append is
  there, and real-time visibility.

Commands:

```sh
xmake f -m releasedbg --sql_layer=y && xmake build sql_layer_tpcc
sql_layer_tpcc load --dir /tmp/tpcc --warehouses 48
sql_layer_tpcc run --dir /tmp/tpcc --threads 1,16,48 --duration 300 --warmup 60
python3 bytecaskdb-sql-layer/scripts/sql_isolation_check.py \
  --binary build/linux/x86_64/releasedbg/sql_layer_tpcc --elle-jar elle-cli.jar
```

## Known differences

Found so far. The machine expects each one, so it does not fail on them.

- **A table needs a primary key.** The layer refuses `CREATE TABLE` without
  one; PostgreSQL accepts it. The machine sends that variant to the layer
  only and expects the refusal.
- **A VARCHAR(n) value whose excess is only spaces is refused.**
  PostgreSQL truncates it to n characters and stores it; the layer refuses
  it as too long (22001). The machine does not generate one.
- **A DEFAULT outside its column's type is refused by the DDL.** The layer
  refuses `CREATE TABLE` (and `ADD COLUMN`) with, say, `SMALLINT DEFAULT
  100000`, `REAL DEFAULT 1e39` or `VARCHAR(2) DEFAULT 'abc'`. PostgreSQL
  accepts the DDL and refuses each INSERT that takes the default (22003,
  22001; checked on 16.15). A DEFAULT that is not UTF-8 never reaches
  either: PostgreSQL refuses the statement's text (22021). The machine's
  defaults stay inside their types.
- **A parameter compared with columns of two SQL types is refused.**
  `WHERE a = $1 AND b = $1`, with `a DOUBLE PRECISION` and `b REAL`,
  `INTEGER` and `BIGINT`, or `DECIMAL`s of two scales, is refused at bind:
  a compared parameter has one SQL type, checked before the plan runs. A
  `DECIMAL` of scale 0 counts as a `BIGINT`, so that pair is not refused.
  PostgreSQL runs it. The oracle sends it two `float8` parameters, one per
  `$1` (`pg_params`), and with `$1 = 0.1` PostgreSQL compares
  `real::float8 = 0.1`, false, where a `REAL` key part keyed as the float
  of 0.1 would match; a range check per comparison would not close that.
  PostgreSQL given one `$1`, typed from its first use, could compare
  differently again. Refusing is safe either way. The machine gives each
  compared column its own parameter
  (`test_a_parameter_compared_with_two_types_is_refused`; the DECIMAL
  scales, which the oracle does not model, in the embedded API's test).
- **DML takes no literals:** values are parameters (`$1`). The machine's
  statements use parameters on both sides.
