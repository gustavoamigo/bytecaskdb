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
  23505 on both, a NULL in a NOT NULL column 23502. Any other refusal counts
  as a refusal on both, whatever its message.
- **Rows agree.** A lookup returns the same rows. After every step, each
  table holds the same rows on both. The layer's rows are read from the
  engine by `table_rows`, since SQL has no full scan. PostgreSQL's come from
  `SELECT … ORDER BY` the primary key.
- **Values compare as values.** `bytecask_sql` converts each value by its
  column's SQL type, and the oracle (`tests/proof/oracle.py`) converts
  PostgreSQL's the same way. Text columns are `COLLATE "C"` in PostgreSQL,
  so its order is byte order, the order of the layer's keys.

Rules in increment 1:
- **CREATE TABLE.** Up to four columns over BIGINT, INTEGER, SMALLINT, DOUBLE
  PRECISION, BOOLEAN, TEXT, VARCHAR(4) and BYTEA. Primary keys of one or two
  columns, NOT NULL and nullable columns, and DEFAULT literals. Refused
  variants: an existing name, a duplicate column, and a primary key naming
  no column.
- **DROP TABLE**, with and without `IF EXISTS`.
- **INSERT**, weighted towards duplicate keys and NULLs, with and without a
  column list.
- **Point lookup** by the full primary key, an existing key or not, NULL
  included.
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
- **INSERT may not leave out a NOT NULL column that has no default.** That
  includes a primary key column. The layer refuses the statement when it
  binds; PostgreSQL inserts NULL and reports 23502. The machine expects
  exactly that pair. The contract says a NULL "left unset with no default" is
  `BC_C_NOT_NULL`, so this is reported for the owner to decide.
- **Values stay inside their declared types.** The layer does not refuse an
  INTEGER or SMALLINT out of range, or a VARCHAR(n) longer than n, where
  PostgreSQL reports 22003 and 22001. This is reported; the machine's values
  stay in range until the types increment of #515.
- **Two column-level `PRIMARY KEY`s make a composite key in the layer.**
  PostgreSQL refuses them with 42P16. This is reported; the machine declares
  a primary key once, at table level.
- **DML takes no literals:** values are parameters (`$1`). The machine's
  statements use parameters on both sides.
