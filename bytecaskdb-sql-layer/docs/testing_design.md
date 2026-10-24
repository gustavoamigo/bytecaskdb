# Testing the SQL layer

How the layer is checked:
- **The proof machine.** One model-based test, a Hypothesis state machine,
  runs every statement on the layer and on PostgreSQL 17 and requires them
  to agree. What it doesn't cover yet is tracked in #515.
- **The TPROC-C statements and the harness.** The statements the layer runs,
  and the in-process harness that runs TPROC-C's mix through the embedded
  API: the consistency conditions, Elle through SQL and the CI smoke test.
- **The HammerDB gate**, which drives the layer over the wire, is in
  `server_design.md`, *Running the gate*.

The differences from PostgreSQL the machine expects are in
`frontend_design.md`, `transaction_design.md` and `server_design.md`, each
under *Known differences from PostgreSQL*.

## The proof machine

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
  time, 22021 for text that is not UTF-8. The layer reports each with its
  SQLSTATE (`sqlstate()`, the exception's `sqlstate` in Python), and
  `test_every_range_message_maps_to_its_sqlstate` produces every range
  message, so one that maps wrongly fails there.
  A binder or catalog refusal
  carries its own SQLSTATE (`SqlError.sqlstate`, `DdlRefused`), and those
  PostgreSQL gives the same statement (`BINDER_STATES` in `oracle.py`: 42P01,
  42703, 42804, 42P18, 42883, 42702, 42701, 42803, 0A000, 22003, and the
  catalog's 42P07, 42704, 42809) must agree with PostgreSQL's. The one state
  both sides use for different things is 0A000: on a `CREATE TABLE` it is the
  layer's own rule that a table needs a primary key, which PostgreSQL does not
  have, and on a message starting "no plan:" the planner's refusal; `layer_only`
  in `oracle.py` classes both as a refusal, not compared. Any other statement
  PostgreSQL runs that the layer refuses is 42601
  and a refusal on both (`test_forms_the_layer_does_not_support_are_refused_without_a_sqlstate`
  runs each on PostgreSQL to show it accepts it). A statement
  with a repeated `$n` goes to PostgreSQL as written and untyped
  (`run_pg_numbered`), since psycopg would send it as two parameters; so does
  one whose `$1` no use types (42P18), which psycopg cannot express. Any other refusal counts
  as a refusal on both, whatever its message. The layer's refusals are
  `SqlError`: parse and bind errors, a DDL the catalog refuses, parameters
  of the wrong number or type; the planner's "no plan" refusal is one
  too, and follows the next rule instead. Any other exception from the
  layer, an
  engine refusal at run time included, fails the test.
- **Statements the planner serves.** The machine generates only statements
  the planner serves, and a "no plan" refusal
  (`planner_design.md`, *What the search refuses*) fails the test.
  The planner's refusals are tested by their messages in
  `sql_layer_tests`.
- **Counts agree.** An INSERT, UPDATE or DELETE changes as many rows on both.
- **Rows agree.** A lookup returns the same rows. After every step, each
  table holds the same rows on both. The layer's rows are read from the
  engine by `table_rows`, not by a plan, so the check does not depend on
  the planner it checks (a full scan in SQL is one of its plans).
  PostgreSQL's come from
  `SELECT … ORDER BY` the primary key. `table_rows` is test only: it is
  compiled into `sql_layer_testing`, the build of the library with
  `BYTECASK_SQL_TESTING` that the tests and `bytecask_sql` link, and is not
  in `sql_layer`.
- **Values compare as values.** `bytecask_sql` converts each value by its
  column's SQL type, and the oracle (`tests/proof/oracle.py`) converts
  PostgreSQL's the same way. Values compare by type too: `True` is not `1`,
  `1` is not `1.0`, and `-0.0` is not `0.0`. A DOUBLE or REAL takes NaN,
  `Infinity` and `-Infinity` too. An `ORDER BY`'s keys compare as
  PostgreSQL orders them: every NaN equal to every other, and `-0.0` equal
  to `0.0`, so their ties form one run (`oracle.same_rows`). Text columns are `COLLATE "C"`
  in PostgreSQL, so its order is byte order, the order of the layer's keys.
  A REAL comes from PostgreSQL as the shortest text naming its float; the
  oracle packs it back to that float, which is what the layer stores. A
  lookup by a SMALLINT, INTEGER or REAL key casts `$n` to the column's type
  on PostgreSQL's side: the layer types `$n` by its column, and psycopg
  tells PostgreSQL int8 or float8.

The rules, by what they test.

**Tables and rows:**
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

**Transactions** (`transaction_design.md`):
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

**Indexes and reads by a key:**
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
  now and then, one item's `NULLS FIRST` or `LAST` now and then; or of one
  to three of any columns, each `ASC` or `DESC`, `NULLS FIRST`, `LAST` or
  neither, which a sort mostly gives; maybe a `LIMIT`. Now and then no key
  conjunct at all, only filters or none: a full scan. A model of the
  planner (`served`) says whether some plan serves it: if not, the layer
  must refuse it with `no plan:` and SQLSTATE 0A000, and PostgreSQL is not
  asked. Since planner slice 4 it refuses only a filter on a DOUBLE or REAL
  column no key takes. If it does,
  the rows are compared in `ORDER BY` order, as a multiset within each run
  of equal sort keys, and a run the `LIMIT` cuts must be drawn from
  PostgreSQL's (`oracle.same_rows`, against PostgreSQL's rows without the
  `LIMIT`).
- **Alternatives agree** (`planner_design.md`, *Tests*): after each
  `SELECT`, aggregate and join compared with PostgreSQL, in autocommit,
  every plan the search costed for it runs on the same data
  (`Database::alternatives_for_testing`), and each one's rows are checked
  as the layer's are.
- **COUNT** of `*` or of a column over the same `WHERE`, with `LIMIT 0`
  or 1 now and then.
- **SET col = col ± $n** on BIGINT, SMALLINT, INTEGER and NUMERIC(6, 2) columns by
  the primary key, operands at and past the type's edges and NULL; on
  PostgreSQL's side `$n` is cast to the column's type, as PostgreSQL
  types it. BIGINT's edges are int64 itself. The machine does not draw a
  target other than the column read; `test_column_plus_parameter_across_bigint_and_scale_0_decimal`
  does, for a BIGINT beside a NUMERIC(18, 0).

**Integer literals and joins by key:**
- **SET col = col ± n**, n an integer literal, on the same columns: small
  ones, ones at and past SMALLINT's and INTEGER's edges, and ones past
  INTEGER, which PostgreSQL types bigint.
- **A join of two tables**, each by its whole primary key, existing keys
  mostly, as `FROM a, b WHERE …` or `a JOIN b ON … WHERE …`, columns
  qualified by their table.

**Aggregates, UPDATE and DELETE of many rows:**
- **SUM and MIN** over the same `WHERE` as `COUNT`, with `LIMIT 0` or 1
  now and then: `SUM` of SMALLINT, INTEGER and NUMERIC(6, 2) columns, `MIN`
  of those and BIGINT ones. `SUM` of BIGINT is left out: its running total
  leaves `int64_t` (`frontend_design.md`, *Known differences*).
- **UPDATE and DELETE by a key**: the same `WHERE` as `SELECT`, every row
  it matches; the `UPDATE` sets a non-key column to a parameter, or adds
  one to or subtracts one from a SMALLINT, INTEGER or NUMERIC(6, 2) column
  no unique index holds. `served` decides whether the layer plans it, as
  for a `SELECT` without `ORDER BY`; the counts must agree.

**Filters, computed bounds and joins on a column:**
- **Filters**: zero to two conjuncts on any column, by any operator, beside
  the key's, in `SELECT`, `UPDATE` and `DELETE`. `served` refuses one on a
  DOUBLE or REAL column.
- **$n ± k**: now and then an INTEGER conjunct's operand is `$n + k`, k
  from -30 to 30, mostly the same value; PostgreSQL's side writes
  `$n::integer + k`. Compared by `=`, it is a filter.
- **COUNT(DISTINCT col)** beside `COUNT`.
- **A join on a column**: two tables, each of the second's key parts equal
  to a column of the first of its type or to a parameter, the first's
  conjuncts from a key path, maybe a filter on the second; targets with a
  `LIMIT`, or `COUNT(DISTINCT)` of either table's column, and now and then
  an `ORDER BY` of either table's columns. `join_served` says whether
  either table can be the outer one; where only a nested loop reading a
  table once per row of the other would serve it, the cost model decides,
  and the layer must run it or refuse it for the hash join (slice 5).
- **A table read once**: one named by its key in parameters, the other
  scanned by parameters, linked by equalities of columns neither key takes
  (`join_read_once`) or of the scanned table's primary-key columns, which
  its index holds as its key's suffix (`join_read_once_on_keys`).

`test_tprocc.py` runs TPROC-C's five transactions through SQL against
PostgreSQL: a small fixture (one warehouse, two
districts, thirty customers each, some with a NULL `c_first`, fifty items
and their stock, the orders without a carrier undelivered in `new_order`),
then a hundred transactions from a fixed seed, one thread, each statement
in a `Transaction` and in a PostgreSQL REPEATABLE READ transaction, with
the client's logic between them. Every tenth is a Delivery, of each
district's oldest undelivered order, and every tenth another a
Stock-Level, with a threshold wider than the specification's so its counts
are not all 0; the rest are in roughly TPROC-C's mix of the other three.
One New-Order in twenty names an item that does not exist and rolls back.
Every statement's result and every table after the run must agree.
The schema is read from `tests/tprocc_schema.cppm`, its one definition.

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

## The TPROC-C statements

The statements the harness and the HammerDB driver send, and where they come
from.

### Where they come from

HammerDB's PostgreSQL TPROC-C runs each transaction as one PL/pgSQL
procedure. Those procedures are set-oriented: `UNNEST` over arrays,
`array_agg`, DML inside CTEs, `UPDATE … FROM`, `RETURNING … INTO`. The
layer has no procedures (#495) and none of those forms.

HammerDB's MySQL driver also has a client-side mode
(`mysql_no_stored_procs`, `src/mysql/mysqloltp.tcl`). The driver sends
each statement itself and keeps the procedure's logic (the random choices,
`s_quantity`'s wrap, `ol_amount`, the new `c_data`) in the client. These are
the per-row statements of the benchmark specification's transaction profiles,
the ones HammerDB's MySQL procedures run against InnoDB.

**The layer runs a statement set of its own, written from those
statements** (owner, 2026-10-21): in PostgreSQL's dialect, with `$n` parameters where HammerDB
splices values into the text, and no stored procedures. The HammerDB driver
script sends them over the wire (`server_design.md`), and the layer's harness through the
embedded API, each with the client logic in itself; the proof suite checks
them against PostgreSQL 17. HammerDB's PostgreSQL driver is not used: it
calls PL/pgSQL procedures (`server_design.md`, *The HammerDB
driver*). Two statements are rewritten to mean in
PostgreSQL what they mean in HammerDB, and one table gains a key (owner,
2026-10-21):
- Delivery's `LIMIT 1` without `ORDER BY` relies on InnoDB returning the
  primary key's order; it becomes `ORDER BY no_o_id LIMIT 1`, the
  specification's "lowest `no_o_id`".
- Order-Status's derived table, `SELECT … FROM (SELECT … ORDER BY o_id
  DESC) AS sb LIMIT 1`, is flattened to `… ORDER BY o_id DESC LIMIT 1`.
- `history` has no key in HammerDB. The schema fixture
  (`tests/tprocc_schema.cppm`) gives it `h_id`, supplied by the harness until
  sequences exist. Each `h_id` is unique across threads and runs:
  `(run << 48) | (thread << 40) | n`, with `n` counted per thread from 0.
  - `run` comes from a one-row table of the harness's own,
    `harness_run (id INTEGER PRIMARY KEY, last_run BIGINT NOT NULL)`. At
    start, outside the measured transactions, the harness runs `UPDATE
    harness_run SET last_run = last_run + 1 WHERE id = 1` and reads the row
    back in the same transaction.
  - Distinct runs give disjoint `h_id`s, so harnesses may run at once. A
    bump that loses to another harness's (40001) is retried in a new
    transaction and gets a new run, up to 10 attempts, then the harness
    stops with an error. Only that conflict retries. A bump that updates
    no row (the load hasn't run) is an error.
  - The load inserts the row `(1, 0)` and uses run 0.
  - This gives 2^15 runs, the load's included, 256 threads and 2^40 rows
    per thread per run. The harness refuses to start past the last run or
    with more threads, and a thread refuses a row past its 2^40th.

### The statements

Each statement as the harness and the driver send it. A TPROC-C transaction
runs its statements in one `Transaction` (`transaction_design.md`).

**Payment** (43% of the mix):

| # | Statement |
|---|---|
| P1 | `UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2` |
| P2 | `SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = $1` |
| P3 | `UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3` |
| P4 | `SELECT d_street_1, …, d_name FROM district WHERE d_w_id = $1 AND d_id = $2` |
| P5 | `SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3` |
| P6 | `SELECT c_first, c_middle, c_id, c_street_1, …, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3 ORDER BY c_first` |
| P7 | `SELECT c_first, c_middle, c_last, …, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3` |
| P8 | `SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3` |
| P9 | `UPDATE customer SET c_balance = $1, c_data = $2 WHERE c_w_id = $3 AND c_d_id = $4 AND c_id = $5` |
| P10 | `UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4` |
| P11 | `INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES ($1, …, $9)` |

**Order-Status** (4%):

| # | Statement |
|---|---|
| O1 | `SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3` |
| O2 | `SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3 ORDER BY c_first` |
| O3 | `SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_id = $1 AND c_d_id = $2 AND c_w_id = $3` |
| O4 | `SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1` |
| O5 | `SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3` |

**New-Order** (45%; N4 to N7 once per order line, 5 to 15 times):

| # | Statement |
|---|---|
| N1 | `SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = $1 AND c_w_id = $1 AND c_d_id = $2 AND c_id = $3` |
| N2 | `SELECT d_next_o_id, d_tax FROM district WHERE d_id = $1 AND d_w_id = $2 FOR UPDATE` |
| N3 | `UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = $1 AND d_w_id = $2` |
| N4 | `SELECT i_price, i_name, i_data FROM item WHERE i_id = $1` |
| N5 | `SELECT s_quantity, s_data, s_dist_01, …, s_dist_10 FROM stock WHERE s_i_id = $1 AND s_w_id = $2` |
| N6 | `UPDATE stock SET s_quantity = $1 WHERE s_i_id = $2 AND s_w_id = $3` |
| N7 | `INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ($1, …, $9)` |
| N8 | `INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES ($1, …, $7)` |
| N9 | `INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1, $2, $3)` |

**Delivery** (4%; once per district, ten times):

| # | Statement |
|---|---|
| D1 | `SELECT no_o_id FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 ORDER BY no_o_id LIMIT 1` |
| D2 | `DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3` |
| D3 | `SELECT o_c_id FROM orders WHERE o_id = $1 AND o_d_id = $2 AND o_w_id = $3` |
| D4 | `UPDATE orders SET o_carrier_id = $1 WHERE o_id = $2 AND o_d_id = $3 AND o_w_id = $4` |
| D5 | `UPDATE order_line SET ol_delivery_d = $1 WHERE ol_o_id = $2 AND ol_d_id = $3 AND ol_w_id = $4` |
| D6 | `SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3` |
| D7 | `UPDATE customer SET c_balance = c_balance + $1 WHERE c_id = $2 AND c_d_id = $3 AND c_w_id = $4` |

HammerDB's PostgreSQL procedure finds D1's order with `min(no_o_id)`; D1's
`ORDER BY no_o_id LIMIT 1` plans as the same scan.

**Stock-Level** (4%):

| # | Statement |
|---|---|
| S1 | `SELECT d_next_o_id FROM district WHERE d_w_id = $1 AND d_id = $2` |
| S2 | `SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4` |

N4 finds no row for New-Order's 1% of orders that name an unused item, and
the client rolls back.

**The load.** HammerDB loads with multi-row `INSERT … VALUES` built as
text. The harness's loader inserts one row per prepared statement, `INSERT
INTO t (…) VALUES ($1, …)`, many rows to a transaction (*The TPROC-C
harness*). Multi-row `VALUES` is #534.


## The TPROC-C harness

The harness runs TPROC-C's transaction mix through the embedded API. It
checks the benchmark specification's consistency conditions, runs Elle
through SQL and the CI smoke test, and gives the layer's numbers with no
network. The HammerDB gate, over the wire, is `server_design.md`'s. The
statements the harness sends and the `h_id` scheme are *The TPROC-C
statements* above.

**One binary, five commands.** `harness/main.cpp`, xmake target
`sql_layer_tprocc` (not built by default, as `sql_layer_bench`):
`sql_layer_tprocc load`, `sql_layer_tprocc run` and `sql_layer_tprocc elle`,
and for the HammerDB gate (`server_design.md`, *Running the gate*)
`check` (the consistency conditions alone, on what a run left), `next-run`
(a `harness_run` for the driver's `h_id`s) and `load --sql-out` (the
population as a psql script, for PostgreSQL). The
loader, the transactions and the checks are one module,
`bytecask.sql.tprocc` (`harness/tprocc.cppm`), which the smoke test in
`sql_layer_tests` uses too. Run it from a `releasedbg` build: a debug
engine walks every key-directory entry on each commit it publishes, so at
the specification's scale a commit takes hundreds of milliseconds. Everything goes through the
embedded API (`Database`, `Transaction`, `Statement`), in the harness's
own process. It links the testing build of the layer, as `bytecask_sql`
does, for the engine's `stats()`, which the embedded API keeps there. Every statement is prepared once, before the threads start,
and shared by them.

**The loader.**
- `load --dir D --warehouses W`: the schema fixture (`tests/tprocc_schema.cppm`),
  then the specification's initial population (clause 4.3.3): 100,000 items; per
  warehouse 100,000 stock rows and 10 districts; per district 3,000
  customers with one history row each, 3,000 orders with their lines, and
  the last 900 orders in `new_order`. Random strings, `c_last` from the
  syllables, `NURand` with its constants, 10% `BC` credit, as the clause
  says.
- One prepared `INSERT` per table, many rows per `Transaction`: as many as
  fit well inside its 64 MiB bound.
- The loader creates `harness_run`, inserts its row `(1, 0)` and uses run
  0 for its `h_id`s. A run bumps it with `last_run = last_run + $1`, `$1`
  being 1.
- `w_ytd` and `d_ytd` start at what the history rows sum to: 300,000.00
  and 30,000.00 at the specification's scale, as clause 4.3.3.1 says, and
  proportionally less at a smaller one, so the consistency conditions hold
  at any scale.
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
  Order-Status, Delivery and Stock-Level 4 each), drawn per transaction,
  with all five transactions in it.
- Inputs follow the profiles (clauses 2.4 to 2.8): customers by last name
  60% of the time and by `NURand` id otherwise; a remote warehouse for 15%
  of Payments when `W > 1`; `h_amount` from 1.00 to 5,000.00; a `BC`
  customer's new `c_data`; New-Order's 5 to 15 lines, each item by
  `NURand`, 1% of lines from a remote warehouse when `W > 1`, and 1% of
  orders ending on an unused item; Delivery's carrier from 1 to 10;
  Stock-Level's threshold from 10 to 20, on a district drawn once per
  thread, as the driver draws it once per virtual user. The
  client logic is the harness's, as in HammerDB's client-side driver.
- One difference from the specification, as in HammerDB: Payment does not update
  `c_ytd_payment` or `c_payment_cnt`, since it runs HammerDB's statements
  (P9, P10) by decision; nor does New-Order update `s_ytd`, `s_order_cnt`
  or `s_remote_cnt` (N6), or Delivery `c_delivery_cnt` (D7).
- `ol_amount` is HammerDB's: quantity × price × (1 + `w_tax` + `d_tax`) ×
  (1 − `c_discount`), which its New-Order computes client-side and in its
  stored procedures alike. The specification's is quantity × price, with
  the taxes and the discount applied to the order's total (clause
  2.4.2.2).
- Stock-Level's district is drawn at random once per thread, as HammerDB
  draws it once per virtual user, so two threads can share one. The
  specification gives each terminal its own (clause 2.8.1.2).
- Delivery runs as one transaction over the warehouse's ten districts, as
  HammerDB runs it, not deferred (clause 2.7.2).
- One `Transaction` per TPROC-C transaction, committed with `commit()`.

**Errors and retries.**
- `SerializationFailure` (40001) reruns the transaction with the same
  inputs in a new `Transaction`, up to 10 attempts. Retries are counted
  per transaction type. A transaction still failing after its tenth
  attempt is counted as abandoned and the thread moves on.
- New-Order's 1% of orders naming an unused item roll back, as the
  specification requires. They count as completed, and so in TPM, as
  HammerDB counts a rollback, but not in NOPM.
- Any other error stops the run with a non-zero exit: 23505,
  `TransactionResolutionUnknown`, a constraint, an I/O error. In TPROC-C
  none is expected, so each one is a bug to look at, not a number.

**What it reports.** Per phase:
- per transaction type, completed, retries and abandoned, and latency
  p50, p95, p99 and max, from the first attempt's start to the commit that
  completed it;
- TPM, completed transactions of every type per minute, rollbacks
  included, and NOPM, committed New-Orders per minute: HammerDB's
  definitions, its NOPM being the change in `d_next_o_id`, so the harness's
  numbers mean what the gate's do;
- the engine's `fsyncs` and `group_writer_batches` over the measured
  window, read from `stats()` at its start and end, to set beside
  `commit_probe`.

A transaction counts, with its retries, when it starts and ends inside
the phase's measured window, after the warm-up; the rates divide by the
window's length. Latencies go into a fixed log-bucket histogram per
thread, from 1 µs to 10 s, merged at the end, so a long run holds no
per-transaction list. Each phase's report is printed as a table as the
phase ends, so a later phase's failure loses none of the earlier ones.
With `--json FILE`, one JSON object per phase is appended to FILE as well.
An error in any thread ends its phase at once. These runs are the only performance
numbers the harness produces; there are no micro-benchmarks.

**Consistency after a run.** After the last phase, the harness checks
the specification's consistency conditions (clause 3.3.2) for the transactions in the
mix, through SQL: `warehouse` and `district` by a key range, and `history`
in pages of 10,000 rows in `h_id` order, so no run holds it all:
- with Payment: condition 1 (`w_ytd` is the sum of its districts'
  `d_ytd`), and conditions 8 and 9 (`w_ytd` and `d_ytd` are each the sum of
  their `history` rows' `h_amount`);
- with New-Order and Delivery: conditions 2 to 7, per district, its
  orders in pages of 1,000 `o_id`s with each page's `order_line` and
  `new_order` rows read by the same `o_id` range: `d_next_o_id - 1` is
  `max(o_id)` and `max(no_o_id)` (2); `new_order`'s ids are contiguous (3);
  `o_ol_cnt` sums to the district's line count (4); an order has a carrier
  exactly when it has no `new_order` row (5); its `o_ol_cnt` is its line
  count (6), and its lines are delivered exactly when it has a carrier (7).

A failed condition exits non-zero.

**The smoke test.** `tprocc: a short mixed run keeps the specification's
consistency conditions` is a Catch2 test in `sql_layer_tests`. It loads two warehouses
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
  show a lost append, which shows the check can see a lost update: found
  by the cross-check (an `:ok` append missing from the final read, or a
  read the final value does not extend), or else by Elle. Elle runs on it
  only when the cross-check found none: the history is large and invalid,
  and Elle's search and plots of it take tens of minutes.
- A completed `commit()` is `:ok`, a `SerializationFailure` `:fail`, and
  `TransactionResolutionUnknown` `:info`. In `blind`, a statement that
  conflicted on every retry is `:info`: the statements before it committed.
  Any other error stops the run.
- **The one difference from the engine's check:** `commit()` returns no
  commit sequence, so the cross-check's rule that a key's final list
  follows commit-sequence order cannot apply. `cross_check` takes
  `require_sequence=False` for it (owner decision, 2026-10-21): the rule
  skips an operation with no `sequence`. Every other caller keeps the
  default, under which an `:ok` append without one is a violation. Every
  other rule does apply: reads are prefixes, nothing failed is visible,
  every `:ok` append is there, and real-time visibility.

Commands:

```sh
xmake f -m releasedbg --sql_layer=y && xmake build sql_layer_tprocc
sql_layer_tprocc load --dir /tmp/tprocc --warehouses 48
sql_layer_tprocc run --dir /tmp/tprocc --threads 1,16,48 --duration 300 --warmup 60
python3 bytecaskdb-sql-layer/scripts/sql_isolation_check.py \
  --binary build/linux/x86_64/releasedbg/sql_layer_tprocc --elle-jar elle-cli.jar --rounds 4
```
