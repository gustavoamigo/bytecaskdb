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
| SQL:2016 Core (sqltest) | 2% | 16/743 tests | statement accepted |
| PostgreSQL features | 53% | 16/30 cases | results |
| ByteCaskDB features | 100% | 12/12 cases | results |
| TPC-H-derived | not run yet | | results |
| TPC-DS-derived | not run yet | | results |
| TPROC-C | 100% | 1/1 case: five transactions, then consistency conditions 1–9 | consistency conditions 1–9 |

The suite is young: its cases are the first of the plan in `docs/functional_testing_design.md`, so a high percentage means few cases, not wide coverage. Most of sqltest's tests fail at their first statement: their tables have no primary key, which the layer refuses (a gap, not a decision: `docs/functional_testing_design.md`, Decision 11). The TPC rows come with their own step of that plan.

## Current state

Built and tested:

- **Statements:** `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX`, `ALTER TABLE … ADD COLUMN`; `INSERT` of one row; `SELECT`; `UPDATE` and `DELETE` of any number of rows; `BEGIN`/`COMMIT`/`ROLLBACK`.
- **Queries:** one table, or two joined (index nested loop or hash); `WHERE` as a conjunction of comparisons; `ORDER BY` with `NULLS FIRST`/`LAST`; `LIMIT`; `GROUP BY` and `HAVING`; `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`; `FOR UPDATE` on one table. Full scans and sorts are served.
- **Types:** `SMALLINT`, `INTEGER`, `BIGINT`, `REAL`, `DOUBLE PRECISION`, `BOOLEAN`, `TEXT`, `VARCHAR(n)`, `BYTEA`, `DECIMAL(p, s)` with p ≤ 18, `DATE`, `TIME`, `TIMESTAMP`.
- **Transactions:** statements read their own writes; DDL is its own statement, not transactional.
- **Interfaces:** the embedded C++ API (`api/database.cppm`), and `sql_layer_server`, a PostgreSQL wire-protocol server that psql, psycopg and HammerDB connect to. Trust authentication only: it listens on loopback unless told otherwise.
- **HammerDB gate:** TPROC-C over the wire, driven by HammerDB (`hammerdb/run-gate.sh`; its defaults are 48 warehouses at 1, 16 and 48 virtual users). Nightly CI runs its smoke: 2 warehouses, 2 users, one minute, then the consistency conditions.

Not built yet (each is refused, with a message):

- three or more tables, table aliases, outer joins, subqueries, set operations, `DISTINCT`;
- `OR`, `NOT`, `IN`, `BETWEEN`, `IS NULL`;
- `OFFSET`, `RETURNING`, `ON CONFLICT`, multi-row `VALUES`, the `::` cast;
- `*`, `/`, string functions and operators; literals other than integers (strings and decimals travel as parameters);
- a table without a primary key, and a column-level `UNIQUE` (use `CREATE UNIQUE INDEX`);
- `SELECT` without a `FROM`, `EXPLAIN`, `SAVEPOINT`, sequences, serializable and read-committed isolation, transactional DDL.

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
