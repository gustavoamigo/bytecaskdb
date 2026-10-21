# SQL front end for the MVP: dialect subset and statement IR

Status: **design (#492), approved (owner, 2026-10-21): the *Decisions*;
the rest reviewed with the coordinator.** Slices 0 and 1 are built
(*Slices*); the rest is design. It decides
the SQL the MVP accepts, the intermediate form the code generator prints
plans from, and the planner's choices, all scoped by TPROC-C (#487, *MVP*).
The rest of #492 (rewrites, hash joins, `EXPLAIN`, the plan cache's design)
comes after the MVP and is listed under *Later*.

## Standing rules

- **Do what PostgreSQL does.** Every accepted construct has PostgreSQL 17's
  semantics, and the proof machine compares it with PostgreSQL.
- **Never wait.** Nothing here blocks. `FOR UPDATE` is a guard, checked at
  commit (`docs/transaction_design.md`).
- **Only the compiler's plans are supported.** The IR and the code
  generator are the only source of plan text.
- **Refuse rather than guess.** A statement outside the subset is refused at
  parse or bind, with a message and a position. One inside it that no index
  serves is refused by the planner (*What the planner refuses*). Either
  way, nothing has run.

## Where the statements come from

HammerDB's PostgreSQL TPROC-C runs each transaction as one PL/pgSQL
procedure. Those procedures are set-oriented: `UNNEST` over arrays,
`array_agg`, DML inside CTEs, `UPDATE … FROM`, `RETURNING … INTO`. The MVP
has no procedures (#495) and none of those forms.

HammerDB's MySQL driver also has a client-side mode
(`mysql_no_stored_procs`, `src/mysql/mysqloltp.tcl`). The driver sends
each statement itself and keeps the procedure's logic (the random choices,
`s_quantity`'s wrap, `ol_amount`, the new `c_data`) in the client. These are
the per-row statements of the TPC-C specification's transaction profiles,
the ones HammerDB's MySQL procedures run against InnoDB.

**The MVP runs a statement set of its own, written from those
statements:** in PostgreSQL's dialect, with `$n` parameters where HammerDB
splices values into the text, and no stored procedures. The layer's harness
sends them through the embedded API, with the client logic in the harness,
and the proof suite checks them against PostgreSQL 17. HammerDB's
PostgreSQL driver is not used. Two statements are rewritten to mean in
PostgreSQL what they mean in HammerDB, and one table gains a key
(*Decisions*, 1 and 3):
- Delivery's `LIMIT 1` without `ORDER BY` relies on InnoDB returning the
  primary key's order; it becomes `ORDER BY no_o_id LIMIT 1`, the
  specification's "lowest `no_o_id`".
- Order-Status's derived table, `SELECT … FROM (SELECT … ORDER BY o_id
  DESC) AS sb LIMIT 1`, is flattened to `… ORDER BY o_id DESC LIMIT 1`.
- `history` has no key in HammerDB. The schema fixture
  (`tests/tpcc_schema.cppm`) gives it `h_id`, supplied by the harness until
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

## The statements

Columns: the statement as the harness sends it; whether the layer runs it
today; what it lacks. Each runs inside a `Transaction`
(`docs/transaction_design.md`), which none can do today.

**Payment** (43% of the mix):

| # | Statement | Today | Missing |
|---|---|---|---|
| P1 | `UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2` | no | `col + $n` (today only `col - $n`) |
| P2 | `SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = $1` | yes | |
| P3 | `UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3` | no | `col + $n` |
| P4 | `SELECT d_street_1, …, d_name FROM district WHERE d_w_id = $1 AND d_id = $2` | yes | |
| P5 | `SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3` | no | `COUNT`; an equality prefix of `customer_i2` |
| P6 | `SELECT c_first, c_middle, c_id, c_street_1, …, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3 ORDER BY c_first` | no | an equality prefix; `ORDER BY` from the index |
| P7 | `SELECT c_first, c_middle, c_last, …, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3` | yes | |
| P8 | `SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3` | yes | |
| P9 | `UPDATE customer SET c_balance = $1, c_data = $2 WHERE c_w_id = $3 AND c_d_id = $4 AND c_id = $5` | yes | |
| P10 | `UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4` | yes | |
| P11 | `INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES ($1, …, $9)` | yes | |

**Order-Status** (4%):

| # | Statement | Today | Missing |
|---|---|---|---|
| O1 | `SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3` | no | as P5 |
| O2 | `SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3 ORDER BY c_first` | no | as P6 |
| O3 | `SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_id = $1 AND c_d_id = $2 AND c_w_id = $3` | yes | |
| O4 | `SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1` | no | a prefix of `orders_i2`, scanned in reverse; `LIMIT` |
| O5 | `SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3` | no | an equality prefix of the primary key |

**New-Order** (45%; N4 to N7 once per order line, 5 to 15 times):

| # | Statement | Today | Missing |
|---|---|---|---|
| N1 | `SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = $1 AND c_w_id = $1 AND c_d_id = $2 AND c_id = $3` | no | a join of two point lookups |
| N2 | `SELECT d_next_o_id, d_tax FROM district WHERE d_id = $1 AND d_w_id = $2 FOR UPDATE` | no | `FOR UPDATE` |
| N3 | `UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = $1 AND d_w_id = $2` | no | an integer literal; `col + constant` |
| N4 | `SELECT i_price, i_name, i_data FROM item WHERE i_id = $1` | yes | (no row: the harness rolls back, 1% of orders) |
| N5 | `SELECT s_quantity, s_data, s_dist_01, …, s_dist_10 FROM stock WHERE s_i_id = $1 AND s_w_id = $2` | yes | |
| N6 | `UPDATE stock SET s_quantity = $1 WHERE s_i_id = $2 AND s_w_id = $3` | yes | |
| N7 | `INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ($1, …, $9)` | yes | |
| N8 | `INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES ($1, …, $7)` | yes | |
| N9 | `INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1, $2, $3)` | yes | |

**Delivery** (4%; once per district, ten times):

| # | Statement | Today | Missing |
|---|---|---|---|
| D1 | `SELECT no_o_id FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 ORDER BY no_o_id LIMIT 1` | no | a primary-key prefix, covering; `ORDER BY` / `LIMIT` |
| D2 | `DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3` | yes | |
| D3 | `SELECT o_c_id FROM orders WHERE o_id = $1 AND o_d_id = $2 AND o_w_id = $3` | yes | |
| D4 | `UPDATE orders SET o_carrier_id = $1 WHERE o_id = $2 AND o_d_id = $3 AND o_w_id = $4` | yes | |
| D5 | `UPDATE order_line SET ol_delivery_d = $1 WHERE ol_o_id = $2 AND ol_d_id = $3 AND ol_w_id = $4` | no | an UPDATE of every row under a key prefix |
| D6 | `SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3` | no | `SUM` |
| D7 | `UPDATE customer SET c_balance = c_balance + $1 WHERE c_id = $2 AND c_d_id = $3 AND c_w_id = $4` | no | `col + $n` |

HammerDB's PostgreSQL procedure finds D1's order with `min(no_o_id)`. `MIN`
is in the subset too (#487 lists it), planned as D1's scan.

**Stock-Level** (4%):

| # | Statement | Today | Missing |
|---|---|---|---|
| S1 | `SELECT d_next_o_id FROM district WHERE d_w_id = $1 AND d_id = $2` | yes | |
| S2 | `SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4` | no | a join on a column of the outer row; a range after an equality prefix; a computed bound; a residual filter; `COUNT(DISTINCT)` |

**The load.** HammerDB loads with multi-row `INSERT … VALUES` built as
text. The MVP's loader inserts one row per prepared statement, `INSERT INTO
t (…) VALUES ($1, …)`, which runs today, many rows to a transaction. A
multi-row `VALUES` is later.

## The dialect subset

The parser stays libpg_query 17, transformed into the layer's AST
(#492, *Decided*). The binder resolves names against the pinned catalog and
types every expression, as PostgreSQL does. What the MVP adds:

**Statements.**
- `SELECT` targets: columns, `COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT
  col)`, `SUM(col)`, `MIN(col)`. No `GROUP BY`; an aggregate makes the query
  ungrouped and returns exactly one row. A query mixing aggregates and bare
  columns is refused (42803), as in PostgreSQL.
- `FROM`: one table, or two joined by a comma or `[INNER] JOIN … ON`.
  Columns may be qualified by the table's name; table aliases are later.
- `UPDATE` and `DELETE` whose `WHERE` names any number of rows, not only
  one primary key.
- `ORDER BY`, `LIMIT`, and `FOR UPDATE`, below.

**Literals and expressions.**
- Integer literals, typed as PostgreSQL types them: `integer` if they fit,
  else `bigint`, else refused. Other literals stay refused: values travel as
  parameters (`docs/testing_design.md`).
- `+` and `-` on integers and on `DECIMAL`, with PostgreSQL's result type:
  `smallint + smallint` is `smallint`, `integer + bigint` is `bigint`, and
  so on. `DECIMAL ± DECIMAL` keeps the larger scale; the MVP needs only
  equal scales, and refuses the rest. The result is checked against its
  type at the operation, as PostgreSQL's operators check it:
  `integer out of range`, `smallint out of range`, `numeric field
  overflow`, SQLSTATE 22003. An expression involving NULL is NULL.
- `*`, `/`, string concatenation and functions are not in the MVP:
  TPROC-C's arithmetic on values (`ol_amount`, the new `c_data`) is the
  client's, in HammerDB's driver as in the harness.
- Where expressions may appear: `SET col = expr`, a comparison's operand
  (`ol_o_id >= $3 - 20`), and `VALUES`.

**`WHERE`.** A conjunction (`AND`) of comparisons (`=`, `<`, `<=`, `>`,
`>=`) between expressions, in any order, on any columns. No `OR`, `NOT`,
`IN`, `BETWEEN`, `IS NULL` or subqueries in the MVP. A comparison with NULL
is not true, so it rejects the row, as in SQL's three-valued logic.
Comparing two columns of different SQL types is refused (see *Known
differences*).

**`ORDER BY` and `LIMIT`.** `ORDER BY` a list of columns, each `ASC` or
`DESC`; ties come back in any order, as PostgreSQL allows. `LIMIT` takes an
integer literal or a parameter; `LIMIT NULL` is no limit, and a negative
limit is refused (2201W), as in PostgreSQL. `OFFSET` is not in the MVP.

**Aggregates**, with PostgreSQL's results:
- `COUNT(*)` counts rows, `COUNT(col)` non-NULL values, `COUNT(DISTINCT
  col)` distinct non-NULL values. All three are `bigint`, and 0 over no
  rows.
- `SUM` over `SMALLINT` or `INTEGER` is `bigint`, over `BIGINT` or
  `DECIMAL` is `numeric`, kept at the input's scale. NULL over no rows.
- `MIN` is of the column's type, and NULL over no rows. In the MVP it takes
  a numeric or temporal column; `MIN` of a `TEXT`, `VARCHAR` or `BYTEA`
  column is outside the subset (#534), since keeping the least value seen
  needs a buffer the plan API doesn't have, and `bc_pin` would copy every
  new minimum into the arena.

**`FOR UPDATE`**, on a single-table `SELECT`, inside a transaction:
- every row returned is guarded with `bc_guard_unchanged`, and the commit
  fails with 40001 if another transaction committed a change to one first;
- refused with an aggregate (0A000, "FOR UPDATE is not allowed with
  aggregate functions"), as PostgreSQL refuses it;
- on a join, later.

**Parameters** are typed by use, as PostgreSQL types `$n`:
- compared with a column, or combined with one in arithmetic: the column's
  type, the rule #523 set;
- assigned to a column: the column's type;
- combined only with an integer literal (`$3 - 20` where `$3` has no other
  use): `integer`;
- otherwise: "could not determine data type of parameter $n" (42P18).

**Refused**, with a message and the node's position: everything not above.
That includes `OR`, subqueries, derived tables, `GROUP BY`, `HAVING`,
`DISTINCT` on the select list, outer joins, three or more tables, table
aliases, `OFFSET`, `RETURNING`, set operations, functions, `MIN` of a text
or bytes column, and literals
other than integers. A statement inside the subset can still be refused by
the planner (*What the planner refuses*).

## The statement IR

Today's code generator prints C from string templates, one per statement
form, and its seven forms already strain them (#492, *Decided*). The
planner builds a small tree instead, and the code generator prints each
node with one fixed pattern. A plan's text is a pure function of the IR,
so golden plans pin the planner's choices and the printing together.

```cpp
namespace bytecask::sql::ir {

// A value in scope: a parameter, a constant, or a column or key part of a
// row an outer loop holds. Typed: its SqlType decides its decoder, its
// NULL handling and its arithmetic.
struct Expr {
  enum class Kind { Param, Const, Column, KeyPart, Add, Sub } kind;
  SqlType type;
  std::uint32_t param{0}, loop{0}, col{0};  // Param; Column and KeyPart: which loop, which column
  std::int64_t constant{0};                 // Const
  std::unique_ptr<Expr> lhs, rhs;           // Add, Sub
};

struct Compare { ast::Compare::Op op; Expr lhs, rhs; };

// How one table is reached. Every id and type the code generator prints is
// here: it never reads the catalog.
struct PointGet {                    // the whole primary key, by equality
  std::uint32_t table;
  std::vector<Expr> key;
};

struct Bound { Expr value; bool inclusive; };
struct Range { std::optional<Bound> lo, hi; };   // on the part after the prefix
struct PresentOnly {};               // the part after the prefix, its NULL part skipped:
                                     // [successor(prefix + NULL), successor(prefix))

struct Scan {
  std::uint32_t table;
  std::uint32_t object;              // the table (its primary key) or one of its indexes
  std::vector<Expr> eq;              // the equality prefix
  std::variant<std::monostate, Range, PresentOnly> rest;
  bool reverse{false};
  bool covering{false};              // every column used comes from the key
};

// The two-range form (*The planner's choices*): the present values, then
// the NULL part (BC_SCAN_PREFIX on prefix + NULL), or the NULL part first.
struct NullSplitScan {
  Scan present;                      // its rest is PresentOnly
  bool nulls_first;                  // DESC
  std::vector<Compare> residual;
};

struct Loop {
  std::variant<PointGet, Scan> path;
  std::vector<Compare> residual;     // the conjuncts the key does not express, in WHERE order
};

struct Aggregate { enum class Fn { CountStar, Count, CountDistinct, Sum, Min } fn; Expr arg; };
struct CountFromKeys {};             // bc_count_range over the scan's range
struct MinFromFirstKey {};           // the scan's first entry; skips NULLs by its PresentOnly
struct Rows {
  std::vector<Expr> targets;
  std::optional<Expr> limit;
  bool for_update{false};
};

struct Select {
  // One table, ordered or not; or a join, which takes no ORDER BY in the MVP.
  std::variant<Loop, NullSplitScan, std::pair<Loop, Loop>> from;   // a join: outer, inner
  std::variant<Rows, Aggregate, CountFromKeys, MinFromFirstKey> out;
};

struct Insert { std::uint32_t table; std::vector<std::optional<Expr>> values; };  // per column; nullopt: default
struct Update { Loop rows; std::vector<std::pair<std::uint32_t, Expr>> set; };  // no order, so never split
struct Delete { Loop rows; };

using Statement = std::variant<Select, Insert, Update, Delete>;

}  // namespace bytecask::sql::ir
```

The types exclude the combinations that can't be planned: a point lookup
has no range, no order and no NULL split; a join and an `UPDATE` or
`DELETE` never hold a `NullSplitScan`; a range and the NULL split are
alternatives of one `rest`. What the types can't express, `ir::check`
enforces on every IR before it is printed, and a failure is a compiler
bug, raised as an internal error, never a plan:
- a `NullSplitScan`'s scan has `rest` = `PresentOnly`;
- `MinFromFirstKey` reads a forward `Scan` in a `Loop`, with no residual,
  whose `rest` is `PresentOnly` or nothing;
- `CountFromKeys` reads a `Scan` in a `Loop` with no residual;
- `limit` and `for_update` are only on `Rows`.

**How each construct maps to the plan API.** No new loop or structure is
needed; every pattern below uses calls `bc_plan.h` declares.

| Construct | IR | Plan text |
|---|---|---|
| A whole primary key by equality | `PointGet` | `bc_key_params` (consecutive parameters) or `bc_key_begin` / `bc_key_*` / `bc_key_end`; `bc_get` |
| An equality prefix | `Scan{eq}` | `bc_scan_open(lo = hi = prefix, BC_SCAN_PREFIX)` |
| A prefix and a range | `Scan{eq, Range}` | bounds from `bc_key_*`; an exclusive `lo` or an inclusive `hi` through `bc_key_successor` (`bc_plan.h`, *bounds*) |
| `DESC` order | `Scan::reverse` | `BC_SCAN_REVERSE` |
| A nullable ordered part, its NULLs placed as PostgreSQL places them | `NullSplitScan` | two `bc_scan_open`s. Present values: `bc_scan_open(lo = bc_key_successor(prefix + NULL), hi = bc_key_successor(prefix))`, no `BC_SCAN_PREFIX`. NULL part: `bc_scan_open(prefix + NULL, prefix + NULL, BC_SCAN_PREFIX)`, where `prefix + NULL` is the prefix's key with `bc_key_null` appended. `ASC`: present values forward, then the NULL part; `DESC`: the NULL part reversed, then the present values reversed. One loop body, one `LIMIT` counter |
| A covering read | `Scan::covering` | `bc_key_part_*` on `bc_scan_key`; otherwise `bc_scan_row` |
| A residual filter | `Loop::residual` | `bc_col_*` or `bc_key_part_*`, compared in an `if` that `continue`s; a NULL operand rejects the row |
| `+` / `-` | `Expr{Add, Sub}` | `bc_add` / `bc_sub`, then the range check of its type (*Additions*) |
| `SET col = col + $1` | `Update{set}` | `bc_row_from`, `bc_row_set_*` with the sum, `bc_update` |
| UPDATE or DELETE of many rows | `Update{rows}`, `Delete{rows}` with a `Scan` | the scan, with `bc_update` / `bc_delete` per row; `bc_emit_count` with the rows written |
| `ORDER BY` | the access path's order | none: the planner picks a path in that order |
| `LIMIT` | `Rows::limit` | an `int64_t` counter; `break` at the limit |
| `COUNT(*)`, `COUNT(col NOT NULL)`, no residual | `CountFromKeys` | `bc_count_range` over the access path's range |
| `COUNT`, `SUM` otherwise | `Aggregate` | an accumulator local, `bc_add`; a "seen" `bool` for SUM's NULL |
| `MIN` otherwise | `Aggregate` | a local holding the least non-NULL value, kept by comparison: `<` on integers, on doubles `<` with NaN taken as greater than every other value (`docs/types_design.md`, *Double arithmetic*); a "seen" `bool` for the NULL. Never a bytes column (*The dialect subset*) |
| `MIN(col)`, `col` an `ASC` part directly after the equality prefix, no residual | `MinFromFirstKey`, its `Scan` forward with `rest` = `PresentOnly` when `col` is nullable | the first entry of the scan, which starts past the NULL part when it is `PresentOnly`, then `break`. A `DESC` part (its first entry is the maximum) takes the `Aggregate` row |
| `COUNT(DISTINCT col)` | `Aggregate` | `bc_agg_group` keyed by the column, then a count of `bc_agg_next` |
| A join | `from` = outer, inner `Loop`s | the inner key built from the outer row: `bc_key_*` of `bc_col_*`; nested in the outer loop |
| `FOR UPDATE` | `Rows::for_update` | `bc_guard_row` on each returned row: `bc_guard_unchanged` on its primary key |
| Targets | `Rows::targets`, or `out` | `bc_emit_col`, `bc_emit_key_part`; for an aggregate, `bc_emit_i64` (`COUNT`, `SUM`, and `MIN` of an integer, `DECIMAL`, `DATE`, `TIMESTAMP` or `TIME`), `bc_emit_f64` (`MIN` of a `REAL` or `DOUBLE PRECISION`), and `bc_emit_null` over no rows |

A NULL that a key needs means no row can match, since `=` and the range
operators reject NULL:
- **a parameter:** the plan ends early, as today, before any read;
- **an outer row's column, in a join's inner key:** that outer row is
  skipped (`continue`), and the loop goes on to the next one.

**Additions to the plan API.** One is needed; the implementation PR designs
it.
- **A range check for computed integers and decimals.** `ol_o_id >= $3 - 20`
  computes an `integer` that never reaches a row builder, and PostgreSQL
  raises 22003 when such an operation leaves its type. Today
  `bc_add` / `bc_sub` check only `int64_t`, and the row builder checks only
  what it stores. The call takes the value and the SQL type the compiler
  resolved, and sets `BC_C_RANGE` when the value is outside it.
  - *The rule:* which type an operation is in is a decision, the
    compiler's operator resolution, so it shows in the plan text as one
    call. What the type's bounds are is mechanics, in the runtime with the
    row builder's.
  - Assignments need no call: the row builder checks the stored value
    already (`CONTRACT.md`, the row on values outside their declared type).

`COUNT(DISTINCT)` uses the aggregate structure `bc_plan.h` already
declares: its calls exist but set `BC_E_HANDLE` today, so Stock-Level's
slice implements them.

## The planner's choices for the MVP

All deliberately naive: rules, no cost model, chosen so every TPROC-C
statement gets the plan a person would write.

**Access path, per table.** A path is the primary key or one of the
table's indexes. It is *usable* when its first part is compared for
equality or bounded by a range. It *serves* an `ORDER BY` when the
ordered columns are its parts after its equality-compared ones, in its
order, all `ASC` (a forward scan) or all `DESC` (a reverse one); ordered
columns compared for equality may be left out. Where the NULLs of the
ordered columns fall is checked after the path is chosen (step 4).

A point lookup on the whole primary key serves any `ORDER BY`: it returns
at most one row.

1. Every primary-key column compared for equality: a point lookup.
2. Otherwise, with an `ORDER BY`, only the usable paths that serve it
   remain. None remaining: refused (*What the planner refuses*, row 5).
3. Of those, the one with the longest leading run of equality-compared
   parts wins; at equal runs, one with a range part after the run beats
   one without. Remaining ties go to the primary key, then to the covering
   path, then to the lower id.
4. Then the chosen path's NULLs are checked against PostgreSQL's
   placement (below). One it can't place is refused (row 6); the planner
   doesn't go back to try another path. This is deliberately naive.

So `WHERE a = $1 AND b = $2 ORDER BY d`, on a primary key `(a, b, c)` and
an index `(a, d)`, takes the index: one equality part and
`b = $2` as a residual, rather than the longer primary-key prefix, which
can't give the order.

**NULLs in an ordered part.** An index's order puts NULLs where PostgreSQL
doesn't: on an `ASC` index part the key codec sorts the NULL marker first
(`docs/types_design.md`), so a scan gives `NULLS FIRST` for `ASC` and
`NULLS LAST` for `DESC`, and PostgreSQL's defaults are the reverse. A
part the `WHERE` compares, by equality or a range, holds no NULLs in the
rows it can match, and counts as `NOT NULL` here. The chosen path places
NULLs as PostgreSQL does when:
- every ordered part is `NOT NULL`: one range; or
- exactly one is nullable, it is the first ordered part after the
  equality prefix, and it is an `ASC` index part: the two ranges of
  `NullSplitScan` (*The statement IR*), the present values and the NULL
  part, in PostgreSQL's order, with one `LIMIT` across both. Ordered parts
  after it are `NOT NULL`, so within each range the index order is
  PostgreSQL's.

Anything else is row 6: two nullable ordered parts would need a range per
combination of NULLs; a nullable part after another ordered part, its
NULLs placed within each value of that part; a nullable `DESC` index part
(its NULL marker and values inverted), other bounds. All are #533.

Take `WHERE c_w_id = $1 AND c_d_id = $2 ORDER BY c_first`. No usable path
of `customer` serves it: `customer_i2` is `(c_w_id, c_d_id, c_last,
c_first, c_id)`, and `c_last` lies between the prefix and `c_first`. So
it is row 5, before nullability is looked at. P6's `WHERE c_w_id = $1 AND
c_d_id = $2 AND c_last = $3 ORDER BY c_first` takes `customer_i2` with a
three-part prefix; `c_first` is nullable and directly after it, so the
plan is a `NullSplitScan`. `ORDER BY c_first, c_id` plans the same way,
`c_id` being `NOT NULL`.

TPROC-C's ordered columns are `c_first` (P6, O2), `o_id` (O4) and `no_o_id`
(D1). `o_id` and `no_o_id` are `NOT NULL` in the schema fixture. `c_first`
is nullable there, as in HammerDB's MySQL schema, so P6's and O2's golden
plans show the two-range form. HammerDB's default PostgreSQL schema
declares it `NOT NULL`; the fixture keeps the MySQL types and the plan
doesn't rely on either. No TPROC-C index has a `DESC` part.

`MIN` taken from the first entry needs an `ASC` part and no residual, and
starts past the NULL part (`PresentOnly`). Otherwise the scan runs to its
end and a local keeps the minimum.

`bc_count_range` cardinalities are not consulted. With TPROC-C's schema
every statement has a full key or a selective prefix, so a cost model
changes no plan.

**Where filters run.** Each conjunct the key expresses becomes a bound.
Every other conjunct is a residual, run on the row as soon as its loop
reaches it, before anything is counted, emitted, guarded or written: S2's
`s_quantity < $4` runs on each `stock` row the join fetches. Of two bounds
on one side, the first in `WHERE` order is the key bound and the other a
residual.

**`ORDER BY`** is served only by the access path's order, forward or
reverse, on a single table. Sorters (`bc_sorter_*`) are #533.

**Joins.** Index nested loop only, two tables only.
- The outer table is the one reachable from parameters alone, a point
  lookup over a scan. The inner one must be reachable from the outer row's
  columns and parameters, by a point lookup or a prefix.
- An `ORDER BY` on a join is refused in the MVP. No TPROC-C join has one.
- N1: both are point lookups; the order is the `FROM` order.
- S2: `order_line` is outer (a prefix and a range from parameters), and
  `stock` is a point lookup by `(ol_i_id, $1)`.
- Hash joins wait for a statement that needs one.

**Writes inside a statement.** An `UPDATE` or `DELETE` of many rows writes
each row as its scan reaches it. The statement reads the transaction's view
as it stood at the statement's start (`docs/transaction_design.md`,
*Statements inside a transaction*), so its own writes never feed back into
its scan.

### What the planner refuses

In the MVP the planner refuses a statement inside the subset that it can't
answer from an index scan in PostgreSQL's order (*Decisions*, 5; the gap
is #533). It checks in this order, and the first refusal is the
statement's only one:

| # | Refused | Message |
|---|---|---|
| 1 | A join with an `ORDER BY` | `no plan: ORDER BY on a join` |
| 2 | A join where neither table is reachable from parameters alone | `no plan: a full scan of <first table in FROM>` |
| 3 | A join whose inner table no key reaches from the outer row and the parameters | `no plan: no index of <inner table> joins on <columns>` |
| 4 | A single table with no usable path | `no plan: a full scan of <table>` |
| 5 | A single table whose usable paths don't give its `ORDER BY` (a point lookup on the whole primary key always does) | `no plan: no index of <table> gives ORDER BY <columns>` |
| 6 | A single table whose chosen path can't place the NULLs of its `ORDER BY` as PostgreSQL does (*NULLs in an ordered part*) | `no plan: ORDER BY <column> would need its NULLs reordered` |

The refusal is a SQL error with SQLSTATE 0A000 (`feature_not_supported`),
raised after binding and before anything runs. It carries no position: the
planner works on the bound statement, which keeps none. PostgreSQL runs
each of these; it is the one entry of *Known differences* that is a
feature gap.

## Tests

**Golden plans.** Statements are compiled against the schema fixture plus
`tests/golden/schema.sql`, and their plan text is checked in, as
`tests/golden/<statement>.sql` and `.c`. A test recompiles each, fails on
any difference, and compiles each plan with TinyCC. A change to the code
generator regenerates them with `SQL_LAYER_REGEN_GOLDEN=1`
(`tests/golden_test.cpp`), and the PR shows the
diff. Slice 0's goldens are the forms the compiler handled before the IR.
Each later slice adds the goldens of the TPROC-C statements it brings, and
with the last they are the MVP's TPROC-C plans: the hand-written ones of the
validation phase are dropped (#487).

**The proof machine against PostgreSQL** (`docs/testing_design.md`). Each
slice adds the rules for what it brings:
- tables with secondary indexes, unique or not (testing increment 2 folds
  in here);
- `SELECT` by a key prefix or a range, with residual filters, `ORDER BY`
  each way and `LIMIT`, on `NOT NULL` and nullable ordered columns, NULLs
  included. Rows are compared in `ORDER BY` order, and as a multiset
  within each run of equal sort keys;
- `UPDATE … SET col = col ± expr` and `UPDATE` / `DELETE` of many rows,
  with their counts;
- each aggregate, over empty and NULL inputs;
- two-table joins;
- arithmetic at the edges of `SMALLINT`, `INTEGER` and `DECIMAL`: both
  sides refuse with 22003, or both run;
- `FOR UPDATE`: on one connection the same rows as without it; refused with
  an aggregate on both sides.

Refusals of what the subset leaves out are tested with their message and
position, as `binder_test` tests today's. Each row of *What the planner
refuses* is tested with its message. The proof machine generates only
statements the planner serves, so it never compares a planner refusal with
PostgreSQL (`docs/testing_design.md`).

**Each transaction against PostgreSQL.** Each slice adds a test that runs
its TPROC-C transaction through SQL: a small fixture (one warehouse, scaled
down), a fixed seed, one thread. It sends the same statements and
parameters to PostgreSQL through psycopg, and compares every table after N
transactions. Concurrency is the MVP gate's: the harness at 1, 16 and 48
threads and Elle through SQL (#487, 2.6).

No micro-benchmarks: this is correctness work. `sql_layer_bench` runs only
for a speedup claim.

## Slices

Each slice is one vertical PR: the parser, the binder, the IR and the code
generator for what it adds, its proof-machine rules, its golden plans and
its transaction test.

0. **The IR, for today's forms only.** The planner builds the IR and the code
   generator prints from it; no new SQL. Golden plans for today's forms
   land here, and their diff against today's plans is reviewed
   (*Decisions*, 2). As built: `plan/ir.cppm` (the IR and `ir::check`),
   `plan/planner.cppm` (bound statement to IR) and `plan/codegen.cppm`
   (IR to text), with today's plans unchanged byte for byte. The IR holds
   only today's shapes, and a range's bounds are in key order: the planner
   resolves a `DESC` part and the NULL edge, so the printer needs neither.
1. **Payment and Order-Status:** `col + $n` on `DECIMAL`, equality
   prefixes on the primary key and on an index, `ORDER BY` from the index,
   reverse scans, the two-range scan of a nullable ordered column, `LIMIT`,
   `COUNT` from `bc_count_range`. Run statement by
   statement until #491's `Transaction` lands. As built, through
   `Transaction` (#491 landed first), with these differences from the
   design above:
   - `LIMIT` takes an integer constant, `ALL` or `NULL`; `LIMIT $n` is
     refused. No TPROC-C statement needs it, and a negative parameter
     would need a run-time error the plan API can't raise yet.
   - A comparison the chosen key does not express (a residual filter) is
     refused with `no plan:`, until slice 5 brings residuals. So is a
     second bound on one side of the ranged part, and an `UPDATE` or
     `DELETE` that does not name one row by the whole primary key.
   - A range after an equality prefix is built: it is the same bounds with
     the prefix's parts in front.
   - `COUNT(col)` on a column a matching row may hold NULL in counts by a
     loop over the scan, reading the key part when the key holds the
     column; `COUNT(*)` and a `NOT NULL` or compared column take
     `bc_count_range`. On a point lookup, `COUNT` is 0 or 1. One aggregate
     per `SELECT`.
   - `col + $n` and `col - $n` take a column of the assigned column's type,
     on integers and `DECIMAL` only. A `$n` has one SQL type across every
     use, as before; one used with two is refused, where PostgreSQL infers
     one (*Known differences* in `docs/testing_design.md`). The operand is
     range-checked as the column, a `DECIMAL` operand against 18 digits at
     the column's scale, so the sum stays inside `int64` and the row
     builder's check of it is 22003. A BIGINT overflow is still `BC_E_ARITH`, not 22003 (#540).
   - `FOR UPDATE` (#491, slice B) works with every scan here, `ORDER BY`
     and `LIMIT` included: each row returned is guarded, read from the
     row, never covering. With `COUNT` it is refused, as PostgreSQL
     refuses it with an aggregate.
   - A key path's parts are its key layout's: an index's parts, then every
     part of the primary key, one the index names among them; an equality
     prefix covers a repeated part with the same parameter.
   - The two-range scan prints its two cursors in two blocks, one `LIMIT`
     counter across both.
   - The refusals carry no SQLSTATE: `SqlError` has none yet (#541). Their
     messages are the table's.
2. **Multi-statement transactions** (#491, another session): the
   statements of slice 1 run in a `Transaction`.
3. **New-Order:** the two-point join, `FOR UPDATE`, integer literals,
   `col + 1`.
4. **Delivery:** the covering prefix scan with `LIMIT 1`, `MIN`, `UPDATE`
   of many rows, `SUM`.
5. **Stock-Level:** the join on an outer column, a range after a prefix,
   residual filters, a computed bound with its range check (the addition
   above), `COUNT(DISTINCT)` and the aggregate calls.

Then the MVP gate.

**The MVP gate** runs TPROC-C's transaction mix, with the statements above
(*Where the statements come from*), through our harness at 1, 16 and 48
threads, beside
`commit_probe` and with Elle through SQL (#487, 2.6). The harness calls the
embedded API in its own process, one `Transaction` per TPROC-C
transaction. It reports the layer's own numbers; there is no InnoDB
comparison for now (*Decisions*, 7).

## Known differences from PostgreSQL

| PostgreSQL | This layer | Why |
|---|---|---|
| `SUM` over `DECIMAL` is unbounded `numeric`. | Past 18 digits it is 22003. | `DECIMAL` is a scaled `int64_t` (`docs/types_design.md`). No TPROC-C sum comes near. |
| Comparing columns of different types casts one to the other's. | Refused, in a `WHERE` and a join. | One key encoding per comparison. TPROC-C compares columns of one type only. |
| A statement no index scan answers in its order runs: a full scan, a sort of the rows read, an `ORDER BY` on a join, or one whose NULLs the chosen index can't place as PostgreSQL does (*NULLs in an ordered part*). | Refused (*What the planner refuses*). | Each would add a sorter (`bc_sorter_*`) or an unbounded read to build and test before the MVP (*Decisions*, 5; #533). No TPROC-C statement needs one. |

All three are refusals rather than different answers. Everything else in
the subset matches PostgreSQL, and the proof machine checks that it does.

## Proposed `CONTRACT.md` changes

Each goes in with the slice that builds it, named in its PR. Slice 1 added
the rows on refusals, on `ORDER BY` and `LIMIT`, on `COUNT`, on `col ±
$n`'s range and on comparisons with NULL, each scoped to what it builds;
the planner's row names the `no plan:` message, not 0A000 (#541).

**Added (SQL):**
| Guarantee | Proved by |
|---|---|
| A statement outside the supported subset is refused with a SQL error naming the construct, before anything runs or is written. | to add |
| `ORDER BY` returns rows in that order, with NULLs placed as PostgreSQL places them (last for `ASC`, first for `DESC`) and ties in any order; `LIMIT n` returns the first `n`. In the MVP this covers the orders the planner serves. | to add |
| An aggregate query returns one row: `COUNT` 0 and `SUM` and `MIN` NULL over no rows. | to add |
| An `UPDATE` or `DELETE` affects exactly the rows that matched when it began, and reports how many. | to add |
| Arithmetic whose result leaves its type is SQLSTATE 22003, and the statement writes nothing. | to add |
| A comparison with NULL rejects the row. | to add |
| In the MVP, a statement inside the supported subset that no index scan answers in its order (a full scan, an `ORDER BY` no index gives, one on a join, one whose NULLs the chosen index can't place as PostgreSQL does, a join no index reaches) is refused with a SQL error, SQLSTATE 0A000, before anything runs or is written. | to add |

**Widened:** lookups by the whole primary key become lookups by any key
prefix or range, joins of two tables, and `UPDATE` and `DELETE` of many
rows.

## Decisions

The owner's answers to this design's open questions (2026-10-21):

1. **The statements.** We write our own PostgreSQL-dialect, procedure-free
   version of the per-row statements of HammerDB's MySQL client-side
   driver (*Where the statements come from*). HammerDB's PostgreSQL driver
   is not used: its set-oriented forms wait for procedures (#495).
2. **Slice 0 first.** Today's forms move onto the IR before any new SQL,
   reviewed against golden plans.
3. **The two rewrites and `history`'s key:** Delivery's `ORDER BY
   no_o_id`, Order-Status's flattened derived table, and `history.h_id`
   supplied by the harness.
4. **The range check on computed values** is added to the plan API,
   raising 22003 as PostgreSQL's operators do, so no key is built from a
   value its column cannot hold.
5. **Full scans and sorts no index provides are refused** in the MVP
   (*What the planner refuses*), a feature gap stated under *Known
   differences* and tracked in #533. The owner's condition: it must not
   block the TPROC-C run. No statement above needs one, and the golden
   plans fail if one is refused.
6. **No cost model.** Rule-based paths; `bc_count_range` cardinalities
   enter with the first workload whose plans a rule gets wrong (#534).
7. **No InnoDB comparison for now.** The MVP gate runs TPROC-C's
   transaction mix, with this design's statements, through our harness at
   1, 16 and 48 threads and reports the layer's own numbers.

## Later

Tracked in #534, except full scans and sorts, which are #533.

- `OR`, `IN`, `IS NULL`, subqueries, derived tables, `GROUP BY`, outer
  joins, three or more tables, `OFFSET`, `RETURNING`, multi-row `VALUES`,
  string and function expressions, `*` and `/`, `MIN` of a text or bytes
  column.
- Sorters and hash joins, with the bounds `bc_plan.h` gives them.
- A cost model from `bc_count_range`.
- `FOR UPDATE` on joins, and the serializable guards
  (`docs/transaction_design.md`, *Isolation*).
- `EXPLAIN` as the plan text, and the plan cache's design (#492).
- The set-oriented PostgreSQL forms of HammerDB's procedures, with
  procedures (#495).
