# The functional suite

A black-box test of the SQL layer, and its conformance report. It starts
`sql_layer_server`, talks to it as a client does, over the PostgreSQL
wire protocol, and checks what each statement does. Every case goes
through every layer: the wire protocol and the session, the parser,
binder, planner and code generator, the plan API and the engine, and the
files on disk when a case restarts the server.

Several suites run this way, our own cases and suites from elsewhere, and
one report says how much of each the layer passes:

| Suite | Coverage | Tests | Checks |
|---|---|---|---|
| SQL:2016 Core | x% | n/743 sqltest | statement accepted |
| PostgreSQL features | x% | n/N cases | results |
| ByteCaskDB features | x% | n/N cases | results |
| TPROC-C | x% | 5 transactions | consistency conditions 1–9 |

The suites also set the scope. They are the SQL the layer is meant to
run. A case the layer can't run yet stays in, marked not implemented, so
the report shows how far the layer is from it.

How it differs from the other tests (`testing_design.md`):
- **The proof machine** generates random statements and compares the
  layer with PostgreSQL as it runs. The functional suite is fixed cases,
  and runs without PostgreSQL.
- **`tests/server/test_wire.py`** tests the protocol. The functional
  suite tests what the statements do.
- **The C++ tests and the golden files** test one layer at a time, and
  fix each plan's shape.

## How a run works

```
export BYTECASK_SQL_SERVER=build/linux/x86_64/release/sql_layer_server
# the gate: what must pass
python3 -m pytest bytecaskdb-sql-layer/tests/functional
# conformance: everything, for the report
python3 -m pytest bytecaskdb-sql-layer/tests/functional --conformance --results=results.json
python3 bytecaskdb-sql-layer/tests/functional/report.py results.json > report.md
```

`--suite=postgresql` runs one suite, `-k F041` the cases of one feature,
`-k "several and NULLs"` one case (`-k` takes `and`, not spaces).

- **One case model.** A case is a list of steps. Each step is SQL and
  one expectation: it succeeds, it returns these rows (in order, or
  sorted), or it fails with a given SQLSTATE. A case has an id, the
  suite it belongs to, the features it covers, and whether it is
  implemented yet.
- **A loader per suite** reads that suite's files in their own format
  and yields cases, when the run starts. Nothing is converted by hand or
  committed in our format. The loaders change the format, never the SQL:
  a statement runs as its suite wrote it, so the number measures that
  suite and not our edit of it.
- **pytest runs the cases.** Each case is a test. The client is
  psycopg, and the suite imports nothing from the layer, not even the
  `bytecask_sql` module, so it sees what a client sees.
- **The server** is started with `tests/server/pgserver.py`'s `Server`,
  on a fresh directory and a free port, and stopped at the end; its log
  is kept when a case fails. `BYTECASK_SQL_SERVER` names the binary, and
  the suite skips when it is unset.
- **The results** go to `results.json`, one entry per case with its
  suite, id, features and outcome, and to the usual JUnit file.
- **`report.py`** reads `results.json` and the feature manifest, and
  prints the table above and a table per SQL feature (*The feature
  manifest*).

## Not implemented, and the two ways to run

- **Our cases** carry `unimplemented: true`.
- **Another suite's cases** can't be edited, so each suite has a list
  of the ids the layer doesn't pass yet: `not_implemented/<suite>.txt`.
  A list starts from a first run and is kept by hand after.

The flags and the lists are for the build, and only for it:
- **The gate** (the default, and what CI and the build run): a
  not-implemented case is skipped. Every other case must pass, so
  removing a flag or a line makes that case a test the layer must pass
  from then on. A marked case is never run by the gate, so a gap that
  closes is seen in the conformance report, not as a failed build.
- **Conformance** (`--conformance`): the flags and the lists are
  ignored. Every case runs as a test, and its real outcome is recorded.
  A failure doesn't fail the run: this run measures, and the report is
  made from it. It also lists the cases still marked not implemented
  that pass, whose flags can come off.
- **No issue per case.** The flags and the lists are the record of
  what's missing.
- **A limitation is a gap, not a difference.** What the layer can't do
  is marked not implemented. A case's expected value differs from its
  suite's only where a recorded decision says the layer differs on
  purpose, and a comment beside it names the decision.

## The suites

### SQL:2016 Core: sqltest

[sqltest](https://github.com/elliotchance/sqltest) (MIT) has tests for
the mandatory features of the SQL:2016 standard, by feature id: 743
tests over 134 features, in `standards/2016/{E,F,S,T}/*.tests.yml`.
Core SQL was defined in SQL:1999; the tests are the 2016 edition's, so
the row says 2016.

- **Source:** the generated `*.tests.yml` files, unchanged, in
  `tests/functional/suites/sqltest/`, pinned to one commit, with its MIT
  notice.
- **Loader:** each test is a case with sqltest's id (`f041_03_01_04`)
  and its feature (`F041-03`), and each of its statements a step that
  must succeed.
- **Checks:** that the statements are accepted. sqltest has no expected
  results, and keeping its rule keeps the number comparable with other
  projects'. Its tables have no primary key. A first run passed 16 of
  the 743, 374 stopping at the layer's refusal of such a table; with
  tables without a primary key, literals, `SELECT` without `FROM` and
  comparisons of constants and UNIQUE constraints built, 229 pass, and the rest stop at forms the dialect leaves out. The list is
  `tests/functional/not_implemented/sqltest.txt`.

### PostgreSQL features and ByteCaskDB features: our cases

Cases we write, in YAML, in `cases/`.
- **PostgreSQL features:** what PostgreSQL has beyond the standard, such
  as `LIMIT` and `OFFSET`, `RETURNING`, `ON CONFLICT`, `$n` parameters,
  `NULLS FIRST` and `LAST`, and `::` casts; and the standard features
  sqltest only parses, where the answer matters (NULL in aggregates and
  joins, overflow, a type's edges).
- **ByteCaskDB features:** the layer's own behaviour: what survives a
  restart, the sync modes and I/O backends, the statement memory
  budget, `FOR UPDATE`'s guards.

Their format is in *Our cases*, below.

### TPROC-C

The in-process harness (`testing_design.md`, *The TPROC-C harness*),
run as cases of the suite: the five transactions on a small load, then
`sql_layer_tprocc check`, the nine consistency conditions. The HammerDB
gate stays where it is (`server_design.md`, *Running the gate*).

## Our cases

A YAML file is a list of cases, one file per area (`joins.yaml`,
`types.yaml`). Every case here is written for this layer.

```yaml
# Unique constraints and NULL.

- name: a UNIQUE column takes several NULLs
  features: [pg:unique-nulls-distinct]
  setup:
    - CREATE TABLE u (id INTEGER PRIMARY KEY, k INTEGER UNIQUE)
  steps:
    - sql: INSERT INTO u VALUES (1, NULL)
    - sql: INSERT INTO u VALUES (2, NULL)
    - sql: INSERT INTO u VALUES (3, 7)
    - sql: INSERT INTO u VALUES (4, 7)
      error: "23505"
    - sql: SELECT id, k FROM u
      expected: [[1, null], [2, null], [3, 7]]
  teardown:
    - DROP TABLE u
```

- **`name`** is the case's id, with its file: `unique/a UNIQUE column
  takes several NULLs`.
- **`features`** lists the features the case covers: SQL feature ids
  (`F041-03`), or our own for what the standard doesn't number
  (`pg:returning`, `layer:restart`). Every id must be in the manifest,
  or the run fails.
- **`setup`** runs before the case, and must succeed. **`teardown`**
  always runs after it, and its errors are ignored.
- **`query` and `expected`** are the short form of a case with one
  query. **`steps`** is the long form: each step has `sql`, and may have
  `expected`, `rowcount` or `error`. A step with none of them must
  succeed.
- **`expected`** is rows, each a list of values, compared as
  PostgreSQL's text form for its type (`7`, `1.50`, `t`, `2026-10-25`).
  YAML's `null` is SQL's NULL.
- **`rowcount`** is the rows an `INSERT`, `UPDATE` or `DELETE` changed.
- **`error`** is a SQLSTATE, as a string. The message is not compared.
- **`params`** is a list of values sent through the extended protocol,
  for a statement written with `$1`, `$2`.
- **`ordered: true`**, on a case or a step, compares rows in order.
  Without it both lists are sorted first, since SQL gives no order
  without `ORDER BY`. A case sets it when the order is what it checks,
  and only when its `ORDER BY` gives every row one place. A case doesn't
  use `LIMIT` without `ORDER BY`.
- **`restart`**, a step on its own, stops the server cleanly and starts
  it again on the same directory.
- **`server_flags`**, on a case, are flags for the server, such as
  `["--sync", "AT_INTERVAL"]`.
- **`unimplemented: true`**: see *Not implemented*.
- **Comments** (`#`) say anything a reader should know. The runner
  ignores them, and the recorder keeps them.

A file's cases share one server, and each drops what it made in its
`teardown`. A case with `restart` or `server_flags` gets a server of
its own.

### Recording expected results

Nobody writes expected values by hand. `record.py` runs a file's cases
on a PostgreSQL 17 and writes what it returned into `expected`,
`rowcount` and `error`; the file is reviewed and committed.

```
python3 bytecaskdb-sql-layer/tests/functional/record.py \
  --pg "host=localhost user=postgres password=... dbname=postgres" cases/joins.yaml
```

- **The test never touches PostgreSQL.** The YAML is the expected
  result, so the layer can differ from PostgreSQL on purpose and the
  file shows it.
- **It fills only what is missing.** Re-recording a case is a run on
  that case (`--case NAME`), so a value written by hand, where the layer
  differs on purpose, is not overwritten.
- **It keeps the file as written,** with `ruamel.yaml`: comments, key
  order and layout. The runner only reads, with PyYAML.
- `restart` steps are skipped on PostgreSQL and `server_flags` ignored.
  Each case gets a schema of its own there.
- **Not-implemented cases are recorded too,** so the answer is known
  before the feature exists.

When a case is written, sqltest's file for its feature, and PostgreSQL's
regression tests (`src/test/regress`), are checklists of the forms and
edges worth covering. Their SQL is not copied into our cases.

## The feature manifest

`tests/functional/features.yaml` is every feature the suites may name:
- each SQL subfeature, with its id and name, from sqltest's
  `features.yml` (the SQL:2016 taxonomy, MIT), whether it is mandatory,
  and whether PostgreSQL supports it (PostgreSQL's documentation,
  *SQL Conformance*);
- our own ids for the rest (`pg:…`, `layer:…`), each with a line saying
  what it is;
- a comment on any feature we leave out of scope, saying why.

The per-feature table in the report gives, for each feature: the layer
passes all its cases, some, or none; PostgreSQL supports it; or no case
covers it yet.

## CI

In `sql-layer.yml`:
- **Every PR:** builds the server and runs the gate over every suite,
  with a small TPROC-C load. A failing case fails the job. Then a
  conformance run makes the report, which goes to the job's summary
  page; `results.json` and `report.md` are uploaded. The conformance
  run never fails the job.

No job needs PostgreSQL: the expected values are in the files.

## Out of this suite

- **Concurrency:** a class of tests of its own, when it is needed. A
  concurrent case can't be recorded from PostgreSQL: PostgreSQL makes a
  second writer wait, and the layer never waits, so one of them fails
  at commit with 40001.
- **Plan shapes:** the golden files fix them, and the proof machine
  checks that a statement's alternative plans agree.
- **SQLLogicTest:** left out for now. Its files could be loaded the same
  way later, but it is about 7.4 million queries, hours over the wire.
- **TPC-H and TPC-DS:** left out for now (2026-10-25). Their tools need
  TPC's registration, so a CI job can't fetch them, and a generator and
  queries of our own would measure something other than the benchmarks.

## Plan

One PR each. Every step leaves the run and the report working.

1. **The machinery, with our cases.** The case model, the YAML loader,
   the runner and server fixtures, `record.py`, `report.py`, the feature
   manifest, the not-implemented checks and the CI job. A first set of
   PostgreSQL-features and ByteCaskDB-features cases, recorded from
   PostgreSQL. The TPROC-C row.
2. **SQL:2016 Core.** sqltest in the repository, its loader, and its
   not-implemented list from a first run.
3. **More of our cases,** area by area, through the manifest's features
   that no case covers yet.

Also: `frontend_design.md` lists "a table needs a primary key" under
*Known differences from PostgreSQL*. It is a gap, not a decision, and
moves to what the layer doesn't do yet.

## Decisions

The owner's, 2026-10-25:
1. **One black-box framework, in Python, over the wire.**
2. **Several suites, one report:** SQL:2016 Core (sqltest),
   PostgreSQL features, ByteCaskDB features, and TPROC-C. The SQL Core
   row is labelled by the edition its tests come from.
3. **Other suites are loaded in their own format and turned into our
   cases at run time**, and only the format changes: a suite's SQL
   runs as it wrote it (Decision 13).
4. **PostgreSQL writes our cases' expected results, and is not part of
   the test.**
5. **The suites are the scope.** What the layer can't run yet is marked
   not implemented. The build skips marked cases, and must pass the
   rest; a conformance run ignores the marks and runs everything, for
   the report, which lists the marked cases that pass, so their marks can come off.
6. **No issue per not-implemented case.**
7. **Cases name the SQL features they cover, as metadata.** File names
   stay by area.
8. **Order is compared only when a case says `ordered: true`.**
9. **One server per file**, and a server of its own for a case with
   `restart` or `server_flags`.
10. **Concurrency and plan shapes are other classes of tests.**
    SQLLogicTest, TPC-H and TPC-DS are left out for now.
11. **A table without a primary key is a gap.** The layer's refusal is
    not a recorded decision. Since closed: such a table gets a hidden row
    id (`catalog_design.md`, *Tables without a primary key*).
12. **The suite's runner is the owner's own work and carries an MIT
    header.**
13. **A loader changes the format, never the SQL** (2026-10-25). A sqltest
    table without a primary key is not implemented until the layer
    supports one; adding a key to it would make the row measure our edit
    of the suite, and hide the gap Decision 11 names.
