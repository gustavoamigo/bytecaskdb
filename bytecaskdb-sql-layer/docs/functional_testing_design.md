# The functional suite

A black-box test of the SQL layer. It starts `sql_layer_server`, talks
to it as a client does, over the PostgreSQL wire protocol, and checks
what each statement returns. Every case goes through every layer: the
wire protocol and the session, the parser, binder, planner and code
generator, the plan API and the engine, and the files on disk when a
case restarts the server.

The cases also set the scope. The suite is the SQL the layer is meant
to run. A case the layer can't run yet stays in the suite, marked
`unimplemented`, so the run shows how far the layer is from it.

How it differs from the other tests (`testing_design.md`):
- **The proof machine** generates random statements within its grammar
  and compares the layer with PostgreSQL as it runs. The functional
  suite is named cases written by hand, and it runs without PostgreSQL.
- **`tests/server/test_wire.py`** tests the protocol: startup, the
  extended protocol, errors on the wire, shutdown. The functional suite
  tests what the statements do.
- **The C++ tests and the golden files** test one layer at a time, and
  fix each plan's shape.

## Layout

The machinery is the MariaDB plugin's
(`bytecaskdb-mariadb-plugin/tests/functional/`), copied and adapted. The
case queries are not: the plugin's were taken from MariaDB's MTR suite,
and every case here is written for this layer.

```
bytecaskdb-sql-layer/tests/functional/
  conftest.py          the server fixture
  runner.py            runs one case
  test_yaml_cases.py   collects every case in cases/ as a test
  record.py            fills expected results from PostgreSQL
  requirements.txt
  cases/<area>.yaml
```

- **The client** is psycopg. The suite imports nothing from the layer,
  not even the `bytecask_sql` module, so it sees what a client sees.
- **The server** is started with `tests/server/pgserver.py`'s `Server`,
  on a fresh directory and a free port. `BYTECASK_SQL_SERVER` names the
  binary, and the suite skips when it is unset, as the other wire tests
  do.
- **Python tests** beside the YAML, written by hand, are allowed when
  YAML can't express a check. They use the same fixtures and still talk
  only over the wire. None exist yet.

## A case

A YAML file is a list of cases. It keeps the plugin's shape:

```yaml
# Unique constraints and NULL.

- name: a UNIQUE column takes several NULLs
  setup:
    - CREATE TABLE u (id INTEGER PRIMARY KEY, k INTEGER UNIQUE)
  steps:
    - sql: INSERT INTO u VALUES (1, NULL), (2, NULL)
      rowcount: 2
    - sql: INSERT INTO u VALUES (3, 7)
    - sql: INSERT INTO u VALUES (4, 7)
      error: "23505"
    - sql: SELECT id, k FROM u
      expected: [[1, null], [2, null], [3, 7]]
  teardown:
    - DROP TABLE u
```

- **`name`** is the test's id, with the file's name: `unique/a UNIQUE
  column takes several NULLs`.
- **`setup`** runs before the case, and every statement in it must
  succeed. **`teardown`** always runs after it, and its errors are
  ignored.
- **`query` and `expected`** are the short form of a case with one
  query. **`steps`** is the long form: each step has `sql`, and may have
  `expected`, `rowcount` or `error`. A step with none of them must
  succeed, and what it returns is not checked.
- **`expected`** is the rows, each a list of values. A value is compared
  as PostgreSQL's text form for its type: `7`, `1.50`, `t`, `2026-10-25`.
  YAML's `null` is SQL's NULL. The plugin wrote NULL as the string
  `"NULL"`, which can't be told from a text value `'NULL'`.
- **`rowcount`** is the number of rows an `INSERT`, `UPDATE` or `DELETE`
  changed, from the command tag.
- **`error`** is a SQLSTATE, as a string: `"23505"`. The step must fail
  with it. The message is not compared. Inside `BEGIN`, an error fails
  the transaction as it does in PostgreSQL, and the steps that follow
  have to account for that.
- **`params`**, on a step, is a list of values sent separately, through
  the extended protocol, for a statement written with `$1`, `$2` and so
  on.
- **Comments** (`#`) carry anything a reader should know: why a case is
  unimplemented, or why its expected value is ours and not PostgreSQL's.
  The runner ignores them, and the recorder keeps them.

### Three additions

- **`restart`**, a step on its own (`- restart`), stops the server
  cleanly and starts it again on the same directory. The steps after it
  see what recovery brought back: the catalog, the rows, the indexes.
- **`server_flags`**, on a case, is a list of flags for the server:
  `["--sync", "AT_INTERVAL"]`, `["--io-backend", "buffer_pool", ...]`.
  The expected results are the same with any flags. A case uses them to
  check that they are.
- **`unimplemented: true`**, on a case, says the layer doesn't run it
  yet. See *Scope*, below.

## Comparing rows

- **Without `ordered`,** both lists of rows are sorted before they are
  compared, so only which rows came back counts, duplicates included.
  SQL gives no order without `ORDER BY`, and the layer's order (often
  the key's) need not be PostgreSQL's, nor stay the same when a plan
  changes.
- **With `ordered: true`,** on a case or a step, the lists are compared
  as they are. A case sets it when the order is what it checks, and
  only when its `ORDER BY` gives every row one place. Rows that tie on
  the sort key can come back either way, on both systems.
- The runner does not look for `ORDER BY` in the SQL. It can appear
  where it orders nothing the client sees: in a subquery, or in an
  aggregate's `ORDER BY`.
- A case does not use `LIMIT` without `ORDER BY`, since which rows it
  returns is undefined. Counting them (`SELECT count(*) FROM (...)`) is
  fine.

## Servers

- **One server per file.** Each case drops what it made, in its
  `teardown`. A case that leaves something behind breaks the cases after
  it, which shows where the leak is.
- **A server of its own** for a case with `restart` or `server_flags`,
  started on a fresh directory and stopped when the case ends.

## Expected results come from PostgreSQL

Nobody writes expected rows by hand. `record.py` runs a file's cases on
a PostgreSQL 17 and writes what PostgreSQL returned into `expected`,
`rowcount` and `error`. The file is then reviewed and committed.

- **The test never touches PostgreSQL.** The YAML is the expected
  result. We may choose to differ from PostgreSQL, and a reader sees the
  answer in the file, not in another database.
- **It fills only what is missing.** A step with an expected result is
  left alone. Re-recording a case is a deliberate run on that case
  (`record.py --case NAME`). So a value edited by hand, where the layer
  differs from PostgreSQL on purpose, is not overwritten by the next
  recording. Such a value carries a comment saying why.
- **It keeps the file as written.** It reads and writes with
  `ruamel.yaml`, which keeps comments, key order and layout. The runner
  only reads, with PyYAML.
- **What PostgreSQL can't answer:** a `restart` step is skipped on
  PostgreSQL, since rows after a restart are the rows before it.
  `server_flags` are not PostgreSQL's and are ignored.
- **Each case gets a schema of its own** on PostgreSQL, dropped after
  it.
- **`unimplemented` cases are recorded too.** The answer the layer
  should give is known before the feature exists.

The connection is a libpq conninfo: `record.py --pg "host=localhost
user=postgres password=... dbname=postgres" cases/unique.yaml`.

## Scope

The cases in `cases/` are the dialect we want the layer to run. Adding a
case says "the layer should do this". Something we decide never to
support is not added.

- **`unimplemented: true`** marks a case the layer doesn't run yet. The
  run expects it to fail: a wrong answer, or a refusal such as 0A000.
  It is reported as unimplemented, not as a failure.
- **A case marked `unimplemented` that passes fails the run.** Whoever
  builds the feature removes the flag in the same change. So the flags
  stay true.
- **No issue per case.** The YAML is the list of what's missing. A case
  may name the work it waits on in a comment.
- **The run reports completeness:** per file, how many cases pass and
  how many are unimplemented. A failing case that is not marked is a
  bug.

### Areas

One file per area. The first set:

- **DDL:** `CREATE TABLE` for each type, `PRIMARY KEY`, `UNIQUE`,
  `NOT NULL`, `DEFAULT`, `CREATE INDEX`, `ALTER TABLE`, `DROP`.
- **Inserts, updates and deletes:** `INSERT` forms (one row, many
  rows, `INSERT … SELECT`, `RETURNING`), `UPDATE`, key columns included,
  `DELETE`, row counts.
- **`SELECT`:** `WHERE`, `ORDER BY` with `NULLS FIRST` and `LAST`,
  `LIMIT` and `OFFSET`, `DISTINCT`, `GROUP BY` and `HAVING`, aggregates.
- **Joins:** inner, cross, outer, more than two tables, aliases,
  self-joins.
- **Subqueries:** scalar, `IN`, `EXISTS`, derived tables, CTEs.
- **Set operations:** `UNION`, `INTERSECT`, `EXCEPT`.
- **Expressions:** arithmetic and its overflow, comparisons, `AND`,
  `OR`, `NOT`, `IS NULL`, `CASE`, `COALESCE`, `NULLIF`, casts.
- **Functions:** string, math, date and time.
- **Types:** literal forms, each type's range, text output, `DECIMAL`
  scales.
- **Parameters:** how a `$n` is typed, and its errors.
- **Transaction statements:** `BEGIN`, `COMMIT`, `ROLLBACK`,
  `SAVEPOINT`, `RELEASE`, `ROLLBACK TO`, isolation levels.
- **Unique constraints and NULL.**
- **Indexes:** the rows a query returns through an index, ascending and
  descending, after updates and deletes. PostgreSQL has no `FORCE
  INDEX`, so a case can't pick the path: it checks the rows.
- **Restart:** what the catalog, rows and indexes hold after a restart.

### What the plugin's suite contributes

The plugin's cases (39 files, 220 cases) and Python tests (27) are read
for what they check, not for their SQL. Each behaviour is listed under
its area with one of:
- **recreate:** in scope, and a case is written for it;
- **not applicable:** MariaDB's, such as `AUTO_INCREMENT`, `REPLACE`,
  system variables, `CHECK TABLE`, collation weights and debug-only
  indexes;
- **covered:** by another of the layer's tests, which is named.

## Out of this suite

- **Concurrency:** a second class of tests, designed when it is needed.
  The plugin's are Python tests with threads (`test_concurrency.py`). A
  concurrent case can't be recorded from PostgreSQL: PostgreSQL makes a
  second writer wait for the first, and the layer never waits, so one of
  them fails at commit with 40001.
- **Plan shapes:** the golden files fix them, and the proof machine
  checks that a statement's alternative plans agree.

## Running

```
BYTECASK_SQL_SERVER=build/linux/x86_64/release/sql_layer_server \
  python3 -m pytest bytecaskdb-sql-layer/tests/functional
```

`-k unique` runs one file. `-k "several NULLs"` runs one case.
`sql-layer.yml` runs the suite in the job that builds the server, with
JUnit output like the other suites. The job's PostgreSQL is not used.

## Decisions

The owner's, 2026-10-25:
1. **One black-box suite, in Python, over the wire, with YAML cases.**
   The plugin's machinery is reused; its MTR-derived queries are not.
2. **PostgreSQL writes the expected results, and is not part of the
   test.** The YAML is the expected result, so the layer can diverge
   from PostgreSQL on purpose, and the file shows it.
3. **The suite is the scope.** A case the layer can't run yet is marked
   `unimplemented: true`, and one that passes with the flag fails the
   run.
4. **No issue per unimplemented case.** The flags are the list.
5. **Order is compared only when a case says `ordered: true`.**
6. **One server per file**, and a server of its own for a case with
   `restart` or `server_flags`.
7. **Concurrency and plan shapes are other classes of tests.**
