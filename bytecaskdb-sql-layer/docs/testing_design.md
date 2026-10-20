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
  of the wrong number or type. Any other exception from the layer, an engine
  refusal at run time included, fails the test.
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
  lookup by a REAL key casts `$n::real` on PostgreSQL's side, as the layer
  types `$n` by its column.

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
  and values outside the key's type included: a lookup is not a write, so
  both find nothing rather than refuse it.
- **Reopen** of the layer's database.

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
- **A lookup by text that is not UTF-8 finds nothing.** PostgreSQL refuses
  the parameter (22021); the layer checks text only when it is written. The
  machine generates only valid text.
- **DML takes no literals:** values are parameters (`$1`). The machine's
  statements use parameters on both sides.
