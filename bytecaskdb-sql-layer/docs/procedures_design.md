# Stored procedures in the SQL layer

Status: **design, approved by the owner on 2026-11-02** (#758, part of #487
and #757; it replaces the sketch in #495). Phase 1 is built: `CREATE [OR
REPLACE] PROCEDURE` and `DROP PROCEDURE` are parsed, checked and kept in the
catalog (*Catalog record*). Nothing runs a procedure yet: `CALL` is refused.
Phases 2 to 4 (*Phases*) are not built.

The goal is PL/pgSQL procedures compiled by the layer's own compiler, so that
HammerDB 6.0's stock PostgreSQL TPROC-C driver runs against
`sql_layer_server` unchanged. No C is written by hand.

## Why the layer needs procedures

HammerDB's PostgreSQL driver has no plain-SQL mode. With
`pg_storedprocs=true` it sends `call neword(...)` over the simple protocol.
With `false` it sends `PREPARE … AS select neword($1, …)`, which is still
PL/pgSQL on the server. `hammerdb/bytecask_tprocc.tcl` exists only because
of this gap.

## One generated file per procedure

A procedure compiles to **one generated C file**, compiled by TinyCC in one
call. It holds:

- one function `s<i>` per embedded SQL statement: the code the code
  generator prints today for that statement as a prepared statement, reading
  `bc_param_*` and emitting through `bc_emit_*`;
- the `sub<i>` functions those statements use, as plan files already have
  (`planner_design.md`, *Subqueries*);
- `void proc(bc_proc *px)`: the body's control flow and its expressions over
  variables;
- a constant table per statement: which variable feeds which `$n`, and which
  result column goes into which `INTO` variable.

```c
// PROCEDURE neword(...)   schema: catalog v12
#include "bc_proc.h"
enum { TBL_9 = 9, COL_9_3 = 3, V_CNT = 4, V_OL_I_ID = 11, V_PRICE = 14, S_ITEM = 3 };

/* SELECT i_price, i_name FROM item WHERE i_id = $1 */
static void s3(bc_ctx *cx) {
  bc_view row = bc_get(cx, TBL_9, bc_key_params(cx, TBL_9, 0, 1));
  ...
  bc_emit_begin(cx); bc_emit_col(cx, row, COL_9_3); ... bc_emit_end(cx);
}

void proc(bc_proc *px) {
  bc_range r = bc_range_open(px, 1, bc_var_i64(px, V_CNT));   /* bounds fixed on entry */
  while (bc_range_next(px, r)) {                             /* charged to the budget */
    bc_ctx *cx = bc_stmt_begin(px, S_ITEM);   /* $1 <- V_OL_I_ID */
    s3(cx);
    bc_stmt_end(px, S_ITEM);                  /* effects into the transaction; row -> V_PRICE, V_NAME; FOUND */
    bc_var_set_f64(px, V_AMOUNT, bc_mul_f64(px, bc_var_f64(px, V_QTY), bc_var_f64(px, V_PRICE)));
  }
}
```

## The statement boundary: `bc_stmt_begin` and `bc_stmt_end`

The body is not one plan, because PL/pgSQL needs statement semantics. Within
one plan run a statement never sees its own writes, and a second write of a
row is refused (`transaction_design.md`, *Statements inside a transaction*).
A procedure that updates a row and then reads or updates it again needs a
boundary in between. The pair does per statement what `Transaction` does
today:

- `bc_stmt_begin` opens the statement on the procedure's transaction
  (`bc_ctx_begin_txn`) and sets its parameters from the variables, as the
  statement's table says.
- `bc_stmt_end`:
  - appends the statement's effects to the transaction and applies them to
    the overlay (`bc_txn_add`);
  - copies the result row into the `INTO` variables, as copies the variables
    own, since the statement's views die at reset (#597);
  - records the row count for `FOUND`, and `STRICT`'s errors;
  - resets `written_rows`, the arena and the statement budget
    (`bc_ctx_reset`).
- Each statement is its own savepoint: a failed statement leaves the
  transaction's view as it was.

## The procedure language

The plan language's rules (`plan_api_design.md`, *The language*) apply, with
these changes:

- **Functions:** a procedure file may define `proc`, one `s<i>` per
  statement, and the `sub<i>` functions those use.
- **Variables** live in a frame the runtime owns, reached by slot number
  through `bc_var_*`, so there are still no pointers. Assigning a string
  copies it into the slot.
- **Loops** stay "only on an API iteration":
  - `FOR i IN a..b` is `bc_range_next`; its bounds are evaluated once on
    entry, so it always ends.
  - `WHILE` and `LOOP` are charged to the statement budget on each pass.
  - `FOR rec IN query` and cursor `FETCH` iterate over a statement's rows.
- **Expressions over variables** are printed by the code generator plans
  use, with variable reads in place of parameter reads. There is one
  evaluator, not a second one in C++. A function such as `||`, `substr` or
  `to_char` works in a procedure once the layer has it (#671, #685).

## Compilation pipeline

```
CREATE [OR REPLACE] PROCEDURE text
  ├─ pg_query_parse          → name, arguments, IN/INOUT/OUT modes
  └─ pg_query_parse_plpgsql  → body JSON: declarations, statements, embedded SQL and expressions as text
        │
   PL binder:
     - variables become typed slots;
     - each embedded statement is parsed and bound through the normal binder,
       with a scope hook: a name that is a variable becomes a $n;
       a name that is both a variable and a column is an error (42702)
        │
   procedure IR: Assign, If, ForRange, ForQuery, While, Exec(stmt, into), Open/Fetch/Close, Exit, Continue, Return
        │
   each statement through the normal pipeline (planner, physical plan, codegen) → s<i>
   the IR → proc() and the slot tables → one .c file → TinyCC
```

**A name that is both a variable and a column is refused**, as under
PostgreSQL's default `plpgsql.variable_conflict = error`: SQLSTATE 42702,
naming both. Qualifying the column (`t.col`) or renaming the variable
resolves it. `#variable_conflict` directives are refused.

**When the body is checked.** `CREATE PROCEDURE` checks the body's PL/pgSQL
syntax (`pg_query_parse_plpgsql`), as PostgreSQL does with
`check_function_bodies` on; from phase 2 it also builds the procedure IR,
refusing a PL/pgSQL statement the layer does not run. It does not bind the
embedded SQL: as in PostgreSQL, a procedure may name a table created after
it. Binding happens at the first `CALL` (*Running a procedure*).

## Catalog record

A procedure is kept the way a view is (`catalog_design.md`, *Views*): the
text is the definition. Its record is under `01 05 <id>`; its name entry is
`01 06 <name>`, in a namespace of its own, so a procedure and a table may
share a name, as in PostgreSQL (`pg_proc` is not `pg_class`).

```
u16 format                   ; kProcedureRecordFormat, 1
u32 id, str name
blob definition              ; the CREATE PROCEDURE statement, as the client sent it
u16 nargs { u8 mode, str name, u8 kind, u32 length, u8 precision, u8 scale }
```

- **The signature is stored** beside the text: the argument modes (1 `IN`,
  2 `INOUT`, 3 `OUT`), names and types, with the type fields a column's
  entry has. A `CALL` and the wire reply's row description read it without
  parsing the text again.
- **No dependency list.** PostgreSQL records no dependency from a PL/pgSQL
  body on the tables it names: `DROP TABLE` succeeds and the next call
  fails. The layer agrees, so a view's RESTRICT has no counterpart here.
- **Compiled code is kept only in memory.** It is built at the first call
  and rebuilt after any DDL, as the embedded API rebuilds a prepared
  statement's plan. No compiled artifact is ever persisted.
- **One procedure per name.** PostgreSQL overloads by argument types; the
  layer does not. `CREATE PROCEDURE` of a name in use is 42723.
  `CREATE OR REPLACE` replaces the record, keeping its id, when the
  arguments are the ones it has (modes, names and types), and is refused
  with 0A000 otherwise: "drop it first". PostgreSQL would create an overload
  for other types, and refuses a renamed argument (42P13); the layer keeps
  neither apart.
- **Ids come from the one counter**, so a procedure dropped and created
  again is another object.
- **A record that does not parse is refused at load**, with a view's checks:
  a truncated field, trailing bytes, a record under another id's key, an id
  used twice, a name entry that disagrees, a mode or type that is not one.
- **A build from before procedures opens a database holding one**, and does
  not see it: it reads neither `01 05` nor `01 06`. Its ids still come from
  the one counter, so nothing it creates collides with a procedure. Table
  records do not change.

### What `CREATE PROCEDURE` accepts

| Part | Accepted | Refused |
|---|---|---|
| Arguments | `IN` (the default), `INOUT`, `OUT`, named, of any column type of `types_design.md` | `VARIADIC`; an unnamed argument; a `DEFAULT`; a type the layer lacks, such as `NUMERIC` without a precision (0A000 or 42601, as a column of that type is); two arguments of one name (42P13) |
| Language | `LANGUAGE plpgsql` | any other language, `BEGIN ATOMIC` bodies (0A000) |
| Options | none | `SECURITY DEFINER`, `SET`, any other option (0A000) |
| Body | PL/pgSQL that `pg_query_parse_plpgsql` reads | a body that does not parse (42601) |

**Argument types keep their modifiers.** PostgreSQL drops an argument's
modifier (`VARCHAR(10)` is `VARCHAR`); the layer stores the type as written,
and from phase 2 checks a value assigned to it as a column of that type.
This is stricter than PostgreSQL. The layer has no `NUMERIC` without a
precision, and HammerDB's own signatures use one, so full fidelity needs it
(#758, *Full fidelity*).

### `DROP PROCEDURE`

`DROP PROCEDURE [IF EXISTS] name [(argtype, …)]`: one procedure, RESTRICT.
An argument list, when given, names the types of the input arguments (`IN`,
`INOUT`) or of all of them, in order; a type marked `OUT` in the list is
left out, as libpg_query leaves it out. Types match by kind: a modifier is
not compared, as PostgreSQL ignores it, and `VARCHAR` is `TEXT`. A list that
matches neither is 42883, as is a procedure that does not exist. `IF EXISTS`
makes either a no-op. `CASCADE` and several names are refused.

Both statements are DDL: each is one atomic batch, guarded by the catalog
version, refused inside a transaction block, and survives a reopen.

## Running a procedure

- **`CALL` in autocommit:** `Database::call` opens one transaction, runs
  `proc`, then commits. On 40001 the body reruns from its arguments under
  the retry policy of autocommit statements: three attempts in all on a
  fresh snapshot, then 40001 to the client (`transaction_design.md`,
  *Commit and conflicts*). There is no separate cap or counter.
- **`CALL` inside the client's `BEGIN` block:** the procedure runs in that
  transaction, and the client handles any retry.
- **A failed statement** aborts the procedure, as it aborts a transaction
  today.
- **`EXCEPTION` blocks are refused** until full fidelity: `CREATE PROCEDURE`
  of a body with one is 0A000. Full fidelity makes each block a savepoint, a
  mark in the log plus a copy of the overlay (`transaction_design.md`,
  *Later*). Conflicts surface only at commit, so a `serialization_failure`
  handler would never fire inside the body.
- **Transaction control inside a body** (`COMMIT`, `ROLLBACK`) is refused
  (0A000).

## Wire protocol for `CALL`

- **Built in phase 1:** `CREATE PROCEDURE` and `DROP PROCEDURE` over either
  protocol, with their command tags. The simple protocol's split of a
  `Query` into statements skips dollar-quoted text (`$$…$$`, `$tag$…$tag$`),
  so a `;` inside a body ends nothing.
- **Request:** `CALL name(args)` over the simple protocol, with literal
  arguments, `TO_TIMESTAMP(text, fmt)` and the `::timestamp` cast, as
  HammerDB sends them.
- **Reply:** one row of the `INOUT` and `OUT` values, with the command tag
  `CALL`.
- **Monitor queries:** HammerDB's monitor sends
  `select sum(xact_commit + xact_rollback) from pg_stat_database` outside a
  `catch`, so a stub fed by the server's commit and rollback counters is
  required. It also sends `select sum(d_next_o_id) from district`,
  `select max(w_id) from warehouse` and `select max(d_id) from district`.
  `set CLIENT_MIN_MESSAGES` and the `pg_proc` query run inside a `catch`.
- **Later:** `CREATE FUNCTION` called as `SELECT f(...)`, for
  `pg_storedprocs=false`.

## Phases

The MVP runs HammerDB's driver unchanged, with our own TPROC-C procedure
bodies under HammerDB's names and signatures (`neword`, `payment`, `ostat`,
`delivery`, `slev`), in the SQL the layer runs today, loaded with
`sql_layer_tprocc`.

| Phase | Content | Status |
|---|---|---|
| 1 | This document; `CREATE [OR REPLACE] PROCEDURE` and `DROP PROCEDURE` parsed (signature and PL/pgSQL syntax) and kept in the catalog | built |
| 2 | PL binder, procedure IR, codegen of the one file, `bc_proc.h` and its runtime (`bc_stmt_begin/end`, `bc_var_*`, `bc_range_*`), `Database::call` with the retry | not built |
| 3 | `CALL` over the wire, the OUT row, the `pg_stat_database` stub, `TO_TIMESTAMP` and `::` in arguments | not built |
| 4 | `hammerdb/tprocc_procs.sql`, a stock-driver mode in `run-gate.sh` (HammerDB 6.0, #556), a proof test running the same procedures on PostgreSQL 17 with the consistency conditions | not built |

**Full fidelity**, later: HammerDB's own procedure bodies and schema build,
unchanged. Almost all of it is SQL rather than PL/pgSQL: arrays,
`RETURNING … INTO`, data-modifying CTEs, `UPDATE … FROM`,
`DELETE … USING`, `INSERT … SELECT`, correlated scalar subqueries and
derived tables (#712), expressions in `SET` and `WHERE` (#685), string
functions (#671), `to_char`, `NUMERIC` without a precision, `EXCEPTION`
blocks as savepoints, a user function (`DBMS_RANDOM`) callable from SQL,
and the schema build's `COPY FROM STDIN`, catalog lookups, `CREATE USER`,
`CREATE DATABASE`, `ANALYZE` and `USING BTREE`.

## Tests

Phase 1: `tests/parser_test.cpp` (the signature, modes, types and the
refused forms), `tests/catalog_test.cpp` (the record round-trips, survives a
reopen, is refused when damaged; `OR REPLACE` and `DROP`), and
`tests/api_test.cpp` (the statements through `Database`, with their
SQLSTATEs and commands), and `tests/server/test_wire.py` (the command tags,
a dollar-quoted body in a simple `Query`, `CALL` refused).
