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

Rules for slice 1 of the front end (#492, `docs/frontend_design.md`):
- **NUMERIC(6, 2)** joins the column types. The layer takes a DECIMAL
  parameter as its integer scaled by 10^2 (`oracle.to_layer`); values are
  drawn at that scale, so both sides hold them exactly.
- **CREATE INDEX** of one or two columns, each `ASC` or `DESC`, unique or
  not; a unique one over duplicates is 23505 on both.
- **SELECT by a key**: an equality prefix of the primary key or of an
  index (its key: its parts, then every primary-key part), then maybe a
  range on the next part, values mostly from an existing row, NULLs and
  values outside the type included, the conjuncts shuffled; maybe an
  `ORDER BY` of the parts after the prefix, in the key's direction or
  against it, an equality-compared column now and then, one item flipped
  now and then; maybe a `LIMIT`. A model of the planner's rules
  (`served`) says whether the layer plans it: if not, the layer must
  refuse it with `no plan:`, and PostgreSQL is not asked. If it does, the
  rows are compared in `ORDER BY` order, as a multiset within each run of
  equal sort keys, and a run the `LIMIT` cuts must be drawn from
  PostgreSQL's (`oracle.same_rows`, against PostgreSQL's rows without the
  `LIMIT`).
- **COUNT** of `*` or of a column over the same `WHERE`, with `LIMIT 0`
  or 1 now and then.
- **SET col = col ± $n** on SMALLINT, INTEGER and NUMERIC(6, 2) columns by
  the primary key, operands at and past the type's edges and NULL; on
  PostgreSQL's side `$n` is cast to the column's type, as PostgreSQL
  types it. BIGINT is left out until its overflow is 22003 (#540).

`test_tpcc.py` runs TPROC-C's Payment and Order-Status through SQL against
PostgreSQL: a small fixture (one warehouse, two districts, thirty customers
each, some with a NULL `c_first`), then sixty transactions from a fixed
seed, one thread, each statement in a `Transaction` and in a PostgreSQL
REPEATABLE READ transaction, with the client's logic between them. Every
statement's result and every table after the run must agree. The schema is
read from `tests/tpcc_schema.cppm`, its one definition.

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
