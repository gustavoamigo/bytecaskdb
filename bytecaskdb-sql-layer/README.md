# ByteCaskDB SQL layer

A PostgreSQL-dialect SQL layer over the ByteCaskDB engine. SQL is parsed, bound, planned, and compiled to native code in process, and the compiled plan runs against the engine through a small plan API. It is built for TPROC-C first, so its dialect is a growing subset of PostgreSQL 17, not all of it.

MIT, like the engine. It uses the engine's public API only (`include/bytecask.hpp`).

## How it works

```
SQL ─▶ libpg_query ─▶ AST ─▶ binder ─▶ logical plan ─▶ rewrites ─▶ search ─▶ physical plan ─▶ codegen ─▶ C ─▶ TinyCC ─▶ plan API ─▶ engine
```

- **Plans are C.** The compiler writes each statement as a C program against `algebra/bc_plan.h` and loads it with TinyCC. Only plans the layer generates run.
- **PostgreSQL decides semantics.** Results, errors (SQLSTATE) and NULL placement follow PostgreSQL 17; a model-based test compares the two statement by statement.
- **Never waits.** Nothing blocks on a lock. Snapshot isolation by default; a write-write conflict is retried (autocommit) or fails with 40001 (transaction).
- **Refuses rather than guesses.** A statement outside the dialect is refused with a message and its position (42601); one the planner can't plan, or a guard it can't give, is refused with 0A000.

## Conformance

How much of each suite the layer passes, from the functional suite's conformance run (`tests/functional/report.py`; every PR's CI run uploads the full report, with a table per SQL feature):

| Suite | Coverage | Tests | Checks |
|---|---|---|---|
| SQL:2016 Core (sqltest) | 69% | 514/743 tests | statement accepted |
| PostgreSQL features | 84% | 223/264 cases | results |
| ByteCaskDB features | 100% | 13/13 cases | results |
| TPROC-C | 100% | 1/1 case: five transactions, then consistency conditions 1–9 | consistency conditions 1–9 |

The suite is young: its cases are the first of the plan in `docs/functional_testing_design.md`, so a high percentage means few cases, not wide coverage. Of the sqltest tests that fail, the largest groups need `GRANT` and `REVOKE`; the rest stop at many smaller gaps.

## Current state

Built and tested:

- **Statements:** `CREATE TABLE`, with or without a primary key (a table without one keeps duplicate rows) and with `UNIQUE` constraints, `CHECK` constraints (tested on every `INSERT` and `UPDATE`) and `REFERENCES` (checked when created, not enforced), `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX`, `ALTER TABLE … ADD COLUMN`, `CREATE VIEW` and `DROP VIEW`; `INSERT` of one row; `SELECT`; `UPDATE` and `DELETE` of any number of rows; `BEGIN`/`COMMIT`/`ROLLBACK` with `READ ONLY` and `SET TRANSACTION`; `DEFAULT` in `INSERT` and `UPDATE`.
- **Queries:** one table, or up to eight joined by a comma list or `ON` (any comparison, or a predicate over both tables), `USING` or `NATURAL` (the first join), inner (index nested loop or hash) or `LEFT` outer (nested loop), in a chain, `RIGHT` of two tables, a table joined with itself under aliases, or none (`SELECT` of constants); `WHERE` of comparisons, `AND`/`OR`/`NOT`, `IS [NOT] NULL`, `<>`, `[NOT] BETWEEN`, `[NOT] IN` of a list, `[NOT] LIKE`; uncorrelated subqueries: `(SELECT …)` as a value, `[NOT] EXISTS`, `[NOT] IN (SELECT …)`, `ANY`/`SOME`/`ALL`; table and column aliases (`AS`), including a column list on a table alias (`t AS x (a, b)`); views, read like tables (alone, joined, in a subquery) and read-only; `UNION`, `INTERSECT` and `EXCEPT`, with `ALL` or `DISTINCT`, of plain `SELECT`s, under `LIMIT`, in a subquery, and as a view's query (created, not read yet); `SELECT *` and `t.*`; `+`, `-`, `*`, `/`, comparisons, `IS [NOT] NULL`, `AND`, `OR` and `NOT` of integer and `DECIMAL` columns as select-list values (`/` by zero is 22012); `CASE` (searched and simple), `COALESCE` and `NULLIF` of constants, columns, parameters and subqueries (a branch is evaluated only when taken; a `WHEN` may be any boolean expression above); `CURRENT_DATE`, `LOCALTIME` and `LOCALTIMESTAMP` as select-list values (UTC; one reading per statement, per transaction inside one); casts of dates, times and timestamps to text and to each other; `||`, `UPPER`, `LOWER`, `SUBSTRING`, `TRIM`, `POSITION`, `CHAR_LENGTH`, `OCTET_LENGTH` and `OVERLAY` of constants; `ORDER BY` of columns or select-list positions with `NULLS FIRST`/`LAST`, also of a set operation's rows; `LIMIT`; `SELECT DISTINCT`; `GROUP BY` and `HAVING`; `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, each with `ALL` or `DISTINCT`, of integer, `DECIMAL` and `DOUBLE PRECISION` columns (`MIN` and `MAX` also of dates and times); `FOR UPDATE` on one table. Full scans and sorts are served.
- **Literals:** integers, numerics of up to 18 digits, strings, `TRUE`/`FALSE`, `NULL` and casts of strings (`DATE '…'`, `CAST('5' AS INTEGER)`), typed as PostgreSQL types them, in `VALUES`, `SET`, `WHERE`, `HAVING` and the select list; `+`, `-`, `*`, `/` and comparisons of them folded (integer `/` truncates; a numeric quotient has PostgreSQL's scale; `/` by zero is 22012).
- **Types:** `SMALLINT`, `INTEGER`, `BIGINT`, `REAL`, `DOUBLE PRECISION`, `BOOLEAN`, `TEXT`, `VARCHAR(n)`, `CHAR(n)` (stored without its trailing spaces, returned padded; a database holding one cannot be opened by an earlier build), `BYTEA`, `DECIMAL(p, s)` with p ≤ 18, `DATE`, `TIME`, `TIMESTAMP`, `TIMESTAMP WITH TIME ZONE` / `TIMESTAMPTZ` (always UTC: an offset in a literal is taken off, `CURRENT_TIMESTAMP` and `now()` return it; a database holding one cannot be opened by an earlier build).
- **Transactions:** statements read their own writes; DDL is its own statement, not transactional.
- **Interfaces:** the embedded C++ API (`api/database.cppm`), and `sql_layer_server`, a PostgreSQL wire-protocol server that psql, psycopg and HammerDB connect to. Trust authentication only: it listens on loopback unless told otherwise.
- **HammerDB gate:** TPROC-C over the wire, driven by HammerDB (`hammerdb/run-gate.sh`; its defaults are 48 warehouses at 1, 16 and 48 virtual users). Nightly CI runs its smoke: 2 warehouses, 2 users, one minute, then the consistency conditions.

Not built yet (each is refused, with a message):

- updatable views, `WITH CHECK OPTION`, `CREATE OR REPLACE VIEW`, reading a view defined with a set operation, and, over a grouped view, `SUM`, `COUNT`, `AVG`, an aggregate of its own aggregate, `DISTINCT` or a join (`MIN`/`MAX` of one with no aggregate work);
- a set operation of a query that groups, aggregates or is `DISTINCT`, and `ORDER BY` inside a parenthesised set operation within another;
- more than eight tables, `FULL JOIN`, `RIGHT JOIN` of more than two tables, `USING` or `NATURAL` after the first join, a join in parentheses, `FOR UPDATE` of a join, correlated or nested subqueries and subqueries in `FROM`, `DISTINCT ON`;
- `ILIKE`, `IS DISTINCT FROM`, a predicate in `HAVING`;
- `CHAR(n)` as a key, `UNIQUE` or index column, compared with a parameter, cast to, under `LIKE`, `MIN` or `MAX`; a length in `CHARACTERS` or `OCTETS` is not PostgreSQL syntax;
- `OFFSET`, `RETURNING`, `ON CONFLICT`, multi-row `VALUES`, the `::` cast;
- `%`, a numeric quotient of columns, `REAL` and `DOUBLE PRECISION` arithmetic, unary minus of a column, an expression in `ORDER BY`, `GROUP BY`, `SET`, a `WHERE` or an aggregated select list, `CASE` over `REAL` or `DOUBLE PRECISION` beside other numbers, `GREATEST` and `LEAST`, `||` and the string functions of columns and parameters, the pattern `SUBSTRING`, `EXTRACT`, `CAST` of anything but a string literal or a date, time or timestamp constant, `CURRENT_TIME` and `CURRENT_TIMESTAMP` (WITH TIME ZONE types), the time outside the select list, date arithmetic; a numeric literal past 18 digits, and a constant compared with a column whose type does not hold it exactly (`smallint_col = 40000`);
- a table of no columns, `ALTER TABLE … ADD CONSTRAINT` and `ADD COLUMN … CHECK`, a default that is not a constant (`DEFAULT CURRENT_DATE`), enforcing a foreign key, `ON DELETE CASCADE` and the other actions, `UNIQUE NULLS NOT DISTINCT` or `INCLUDE`, `CREATE TABLE IF NOT EXISTS` (refused with 42P07), `TRUNCATE`, an `UPDATE` of a primary-key column;
- `SELECT` without `FROM` but of constants, a constant beside an aggregate, `EXPLAIN`, `SAVEPOINT`, sequences, serializable isolation (`SERIALIZABLE` is refused; the other levels are accepted and run at snapshot isolation), transactional DDL;
- a comparison of two columns, comparison of a `DOUBLE PRECISION` column, `SUM`, `AVG`, `MIN` and `MAX` of a `REAL` column, an aggregate of an expression, `HAVING` on an aggregate of a `DOUBLE PRECISION` column, a timestamp with a seventh fractional digit (PostgreSQL rounds it).

The functional suite (`tests/functional/`) marks what a case needs but the layer lacks, so its report shows the distance to PostgreSQL; the list above is the same gap seen from the dialect.

## Run it

```sh
xmake f --sql_layer=y
xmake build sql_layer_server
build/linux/x86_64/release/sql_layer_server --dir /tmp/db --listen 127.0.0.1:5432
psql "host=127.0.0.1 user=me dbname=db"      # the database is named after the directory
```

Flags: `--database`, `--max-connections`, `--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION`, `--sync-interval-ms`, `--io-backend pread|mmap|buffer_pool`, `--buffer-pool-size`, `--buffer-pool-direct-io`. SIGINT or SIGTERM stops it cleanly.

The layer is part of the build only with `--sql_layer=y`, so building the engine fetches neither libpg_query (PostgreSQL licence) nor TinyCC (LGPL-2.1).

## Tests

| What | How |
|---|---|
| C++ unit tests: parser, binder, planner, codegen, golden plans, plan API, transactions, TPROC-C smoke | `xmake build sql_layer_tests && xmake run sql_layer_tests` |
| Proof machine: random statements against PostgreSQL 17 | `BYTECASK_SQL_ORACLE=... pytest tests/proof` (needs `bytecask_sql`) |
| Wire protocol | `BYTECASK_SQL_SERVER=... pytest tests/server` |
| Functional suite: fixed cases over the wire, and a conformance report | `BYTECASK_SQL_SERVER=... pytest tests/functional`, then `--conformance --results=results.json` and `tests/functional/report.py` |
| TPROC-C in process: five transactions, consistency conditions 1–9, Elle histories | `sql_layer_tprocc` |
| HammerDB gate | `hammerdb/run-gate.sh`; `--smoke` nightly in CI |

CI is `.github/workflows/sql-layer.yml`, with the sanitizers (ASan, TSan, UBSan) on the C++ tests.

## Where things are

| Path | What |
|---|---|
| `codec/`, `catalog/` | key and row formats; tables, columns and indexes, stored in the engine under namespace `0x01` |
| `sql/` | AST, parser (libpg_query), binder |
| `plan/` | logical plan, rewrites, System R search, code generator, TinyCC loader |
| `algebra/` | `bc_plan.h`, the plan API, and its runtime |
| `api/` | the embedded API |
| `server/`, `hammerdb/` | the wire-protocol server; the HammerDB driver and gate |
| `harness/` | `sql_layer_tprocc`, the in-process TPROC-C harness |
| `python/` | the `bytecask_sql` module the tests use |
| `tests/` | C++ tests, golden plans, proof, server and functional suites |
| `docs/` | design documents, one per area: `frontend_design.md`, `planner_design.md`, `plan_api_design.md`, `catalog_design.md`, `types_design.md`, `transaction_design.md`, `server_design.md`, `testing_design.md` |
| `CONTRACT.md` | what the layer guarantees, and the test proving each row |
