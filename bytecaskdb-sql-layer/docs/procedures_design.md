# Stored procedures in the SQL layer

Status: **design, approved by the owner on 2026-11-02** (#758, part of #487
and #757; it replaces the sketch in #495; revised on 2026-11-03: the gate
drives the procedures with `bcdboltp.tcl`, HammerDB 6.0's PostgreSQL
driver logic in the layer's call syntax). Phases 1 to 3, 4a and 4b are built:
`CREATE [OR REPLACE] PROCEDURE` and `DROP PROCEDURE` are kept in the catalog
(*Catalog record*), the embedded API runs a procedure (`Database::call`,
`Transaction::call`) from one generated file, and `sql_layer_server` takes
`CALL` as HammerDB's driver sends it, with its monitor's queries (*Wire
protocol for `CALL`*), a body loops over a query's rows and builds text
with `||` and `substr`. Phase 4c, the TPROC-C procedures with their driver
and gate, is not built (*Phases*).

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

A procedure compiles to **one generated C file** (`plan/procedure.cppm`),
compiled by TinyCC in one call (`TccPlan::compile_procedure`). It holds:

- one function `s<i>` per embedded SQL statement: the code the code
  generator prints for that statement as a prepared statement, under that
  name (`generate_plan`'s `fn`), reading `bc_param_*` and emitting through
  `bc_emit_*`;
- the subquery functions those statements use, named `s<i>_sub<k>`
  (`planner_design.md`, *Subqueries*);
- one `enum` of the constants every statement uses, each name once: a name
  comes from an id (`TBL_9`, `COL_9_3`), so two statements that use it agree
  on its value;
- `void proc(bc_proc *px)`: the body's control flow and its expressions.

Which variable feeds each `$n` of a statement, and which variables its row
goes into, are not in the file: the compiler keeps them beside it
(`ProcedureStatementPlan`), with what a prepared statement's compile keeps
(the parameters' range checks, the constants), and the runtime's hooks read
them.

```c
// PROCEDURE p   schema: catalog v12
#include "bc_proc.h"
enum { TBL_1 = 1, COL_1_2 = 2, PART_1_0 = 0 };

// SELECT qty FROM t WHERE id = $1
static void s0(bc_ctx *cx) { ... }

void proc(bc_proc *px) {
  bc_ctx *cx = bc_proc_ctx(px);  /* the frame: $n is slot n */
  s0(bc_stmt_begin(px, 0));      /* $1 <- k; INTO q */
  bc_stmt_end(px, 0);
  {
    bc_range_open(px, 0, 1, 0, (bc_param_null(cx, 3)) ? 0 : bc_param_i64(cx, 3), bc_param_null(cx, 3), 0);
    while (bc_range_next(px, 0, 4)) {  /* i, slot 4; a pass charged */
      s1(bc_stmt_begin(px, 1));
      bc_stmt_end(px, 1);
    }
  }
}
```

## The statement boundary: `bc_stmt_begin` and `bc_stmt_end`

The body is not one plan, because PL/pgSQL needs statement semantics. Within
one plan run a statement never sees its own writes, and a second write of a
row is refused (`transaction_design.md`, *Statements inside a transaction*).
A procedure that updates a row and then reads or updates it again needs a
boundary in between. The pair does per statement what `Transaction` does
for a client's statement. The runtime (`bc_proc` in
`algebra/runtime.cppm`) calls two hooks the embedded API sets
(`Database::run_procedure`), so the parameter checks and the result decoding
are a client statement's own:

- `bc_stmt_begin`:
  - takes each `$n` from its variable's slot and checks it as a client's
    parameter is checked (`check_params`: the range of the column it meets);
  - begins the statement on the procedure's transaction
    (`bc_ctx_begin_txn`) and verifies its plan against the transaction's
    pinned catalog: a plan that does not fit is a DDL since the transaction
    began, 40001.
- `bc_stmt_end`:
  - adds the statement's effects to the transaction (`bc_txn_add`); a failed
    statement aborts it, and the procedure with it;
  - decodes its rows as a client's are, and copies the first into the
    `INTO` variables, each checked against the variable's type (22003,
    22001), as copies the slots own;
  - sets `FOUND`: a row returned, or a row written;
  - with `STRICT`, no row is P0002 and a second row P0003.

## The procedure language

The plan language's rules (`plan_api_design.md`, *The language*) apply, with
these changes, declared in `algebra/bc_proc.h`:

- **Functions:** a procedure file defines `proc`, one `s<i>` per statement,
  and their subquery functions.
- **Variables** are slots of a frame the runtime owns: a context whose
  parameter `$n` is slot `n` (`bc_proc_ctx`). The arguments come first, in
  order, then `FOUND`, then the declared variables and the `FOR` variables,
  then the constants of `proc()`'s expressions. `proc()` reads a slot as a
  parameter and sets one with `bc_var_set_*`, so there are still no
  pointers. Setting a value checks it against the variable's declared type
  (22003, 22001); a string is copied into the slot.
- **Loops** stay "only on an API iteration":
  - `FOR i IN a..b [REVERSE]` is `bc_range_open` and `bc_range_next`; its
    bounds are evaluated once on entry, so it always ends. A NULL bound is
    22004.
  - `FOR a, b IN query LOOP` runs the query's statement once
    (`bc_stmt_begin`, `bc_stmt_end`), whose rows the runtime keeps; then
    each `bc_rows_next` copies the next row into the variables, a pass of
    the loop. The rows are the query's as it ran, so the body's writes do
    not change them, as in PostgreSQL. `bc_rows_done`, after the loop
    however it ended, sets `FOUND`: whether the query returned a row. A
    statement that returns no rows is 42601. A record variable (`FOR rec
    IN`) and cursor `FETCH` are not built.
  - `WHILE` and `LOOP` are `while (bc_loop(px))`. Each pass of every loop is
    charged to the procedure's budget, its deadline and cancel flag
    included.
  - Once the procedure has failed, `bc_loop`, `bc_range_next` and `bc_rows_next` return
    false and every other call does nothing, so `proc()` returns.
- **Expressions** are printed by the code generator's expression printer,
  the one a select list uses, reading slots as parameters: there is one
  evaluator, not a second one in C++. What a select list may compute, a
  procedure may (`frontend_design.md`, *Expressions over columns*); a
  function such as `||`, `substr` or `to_char` works in a procedure once the
  layer has it (#671, #685).

## Compilation pipeline

```
CREATE [OR REPLACE] PROCEDURE text
  ├─ pg_query_parse          → name, arguments, IN/INOUT/OUT modes
  └─ pg_query_parse_plpgsql  → body JSON (sql/json.cppm), read into the procedure IR (sql/plpgsql.cppm)
        │
   at the first call after any DDL (plan/procedure.cppm):
     - variables become typed slots of the frame, a table of no catalog;
     - each expression is bound over the frame (bound::bind_procedure_expr);
     - each embedded statement is parsed, its variables rewritten to $n, and
       bound and planned as a prepared statement (bound::bind_procedure_statement)
        │
   proc IR: Assign, If, ForRange, ForQuery, While (LOOP: no condition), Exit (EXIT, CONTINUE), Exec, Return
        │
   each statement → s<i>; the IR → proc() → one .c file → TinyCC
```

**A name that is both a variable and a column is refused**, as under
PostgreSQL's default `plpgsql.variable_conflict = error`: SQLSTATE 42702.
The columns checked are those of every table and view the statement names,
but an `INSERT`'s target, whose columns its `VALUES` cannot name (so
`INSERT INTO t VALUES (id, v)` with variables `id` and `v` runs, as in
PostgreSQL). This is stricter than PostgreSQL where a subquery's table is
not in scope of the whole statement. Qualifying the column (`t.col`)
resolves it.

**A variable is its parameter's value as it is.** No conversion is built:
a variable used as a `$n`, assigned a value, or given a column by `INTO`
must have the same physical type and decimal scale as the other side, and
be an integer of any width, text of any length, or of the same kind
(`bound::assignable`). A narrower side is checked when the value is stored.
Anything else is refused (42601, as the binder refuses a form not built
yet; 0A000 for an assignment or `INTO`).

**When the body is checked.** `CREATE PROCEDURE` reads the body into the
procedure IR, refusing a PL/pgSQL statement the layer does not run (0A000):
`EXCEPTION` blocks, `COMMIT` and `ROLLBACK`, `RAISE`, `EXECUTE`, cursors,
records (`FOR rec IN query` among them), labels, `FOR … BY`, `RETURN` with a value, and a variable that
shadows another. It does not bind anything: as in PostgreSQL, a procedure
may name a table created after it. Binding happens at the first call after
any DDL, which reports what binding refuses: a name that does not exist
(42P01, 42703), 42702, a condition that is not BOOLEAN (42804), a `SELECT`
with neither `INTO` nor `PERFORM` (42601), `EXIT` outside a loop (42601), a
`CONSTANT` assigned (42601), a `NOT NULL` variable (0A000).

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

- **The embedded API:** `Transaction::call(name, args)` runs the procedure
  in the transaction; `Database::call(name, args)` in a transaction of its
  own, then commits. `args` has a value per argument, `OUT` ones included
  and ignored, as PostgreSQL's `CALL` takes them; each is checked against
  its argument's type. The result is one row of the `INOUT` and `OUT`
  arguments' final values, named and typed as the arguments, or no row.
- **In autocommit**, on 40001 the body reruns from its arguments under the
  retry policy of autocommit statements: three attempts in all, each on a
  fresh snapshot, then 40001 to the client (`transaction_design.md`, *Commit
  and conflicts*). There is no separate cap or counter.
- **Inside the client's transaction**, the procedure's writes are the
  transaction's, and the client handles any retry.
- **A failed statement** aborts the procedure and its transaction, as it
  aborts a transaction today; so does any other error of the body.
- **Compiled code** is cached per procedure and built again at the first
  call after any DDL, as a view's plan is; it is never stored.
- **`EXCEPTION` blocks are refused** until full fidelity: `CREATE PROCEDURE`
  of a body with one is 0A000. Full fidelity makes each block a savepoint, a
  mark in the log plus a copy of the overlay (`transaction_design.md`,
  *Later*). Conflicts surface only at commit, so a `serialization_failure`
  handler would never fire inside the body.
- **Transaction control inside a body** (`COMMIT`, `ROLLBACK`) is refused
  (0A000).
- **Known limit:** every read of a text variable in `proc()` takes a view
  in the frame, held until the call ends and charged to its budget, so a
  long loop over text variables can reach the statement memory limit
  (54000). Releasing a frame's views between passes is #762.

## Wire protocol for `CALL`

- **Built in phase 1:** `CREATE PROCEDURE` and `DROP PROCEDURE` over either
  protocol, with their command tags. The simple protocol's split of a
  `Query` into statements skips dollar-quoted text (`$$…$$`, `$tag$…$tag$`),
  so a `;` inside a body ends nothing.
HammerDB 6.0's driver (`src/postgresql/pgoltp.tcl`, `pg_storedprocs=true`)
sends, for instance, `call neword(1,1,3,1123,9,0.0,'','',0.0,0.0,0,
TO_TIMESTAMP('20261103120530','YYYYMMDDHH24MISS')::timestamp without time
zone)`. Phase 3 serves that (`server/pg_session.cppm`, `run_call`):

- **Request:** `CALL name(args)` as a simple `Query`, in or out of a
  transaction block. It is not a statement the embedded API prepares, so
  the session reads it (`ast::parse_call`) as it reads `BEGIN`; over the
  extended protocol it is 0A000.
- **Arguments** are constants (`bound::call_arguments`): literals, `NULL`,
  casts, and functions of constants, `TO_TIMESTAMP` among them. Each is
  read as its argument's type as an `INSERT` reads a constant for a
  column: an unknown literal as that type, a number rounded to a
  `DECIMAL`'s scale, a value outside the type refused (22003, 22001). An
  `OUT` argument's value is ignored. A count other than the procedure's is
  42883; an argument that is not a constant, 42601.
- **`TO_TIMESTAMP(text, 'YYYYMMDDHH24MISS')`** folds to a `timestamptz`, in
  UTC, the layer's one time zone (`frontend_design.md`, *The dialect*);
  another format is refused (42601). `::timestamp without time zone` is the
  existing cast between timestamps.
- **Reply:** a `RowDescription` and one `DataRow` of the `INOUT` and `OUT`
  arguments, named and typed as the arguments, then the command tag
  `CALL`; a procedure without them sends only the tag. Out of a block the
  call is `Database::call`, retried on 40001; in one, `Transaction::call`,
  and an error fails the block as a statement's does. In a `READ ONLY`
  block `CALL` is refused (25006), where PostgreSQL would refuse only the
  writes.
- **Monitor queries:**
  - `select sum(xact_commit + xact_rollback) from pg_stat_database`, which
    the monitor sends outside a `catch`, is answered by the session, matched
    without white space and case: one `numeric` column `sum`, the
    transactions ended since the database opened
    (`Database::transaction_counts`: each autocommit statement and DDL is
    one, a failed one rolled back). The query itself is not counted. Any
    other query of `pg_stat_database` is 42P01.
  - `select sum(d_next_o_id) from district`, `select max(w_id) from
    warehouse` and `select max(d_id) from district` are ordinary queries.
  - `set CLIENT_MIN_MESSAGES TO 'ERROR'` and the `pg_proc` detection are
    refused (0A000, 42P01); the driver ignores both (its `pg_exec` does not
    raise, and the detection is in a `catch`).
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
| 2 | PL binder, procedure IR, codegen of the one file, `bc_proc.h` and its runtime (`bc_stmt_begin/end`, `bc_var_*`, `bc_range_*`), `Database::call` with the retry | built |
| 3 | `CALL` over the wire, the OUT row, the `pg_stat_database` stub, `TO_TIMESTAMP` and `::` in arguments | built |
| 4a | `FOR a, b IN query LOOP`: TPROC-C's customer by last name, the row at position ceil(n / 2) | built |
| 4b | `\|\|` and `substr` over columns and variables: Payment's `h_data` and `c_data` (#671, in part) | built |
| 4c | `hammerdb/tprocc_procs.sql`; `hammerdb/bcdboltp.tcl`, HammerDB 6.0's PostgreSQL stored-procedure driver logic with the layer's call syntax, run as a `customscript`; `CALL` over the extended protocol; `run-gate.sh` running it (#556); a proof test running the same procedures and driver on PostgreSQL 17 with the consistency conditions | not built |
| 5 | `bytecask_tprocc.tcl` and `check_driver_statements.py` removed, once the gate passes with `bcdboltp.tcl` | not built |

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

- Phase 1: `tests/parser_test.cpp` (the signature, modes, types, the refused
  forms and bodies), `tests/catalog_test.cpp` (the record round-trips,
  survives a reopen, is refused when damaged; `OR REPLACE` and `DROP`),
  `tests/api_test.cpp` (the statements through `Database`, with their
  SQLSTATEs and commands), and `tests/server/test_wire.py` (the command
  tags, a dollar-quoted body in a simple `Query`, `CALL` refused).
- Phase 2: `tests/codegen_test.cpp` (the file's shape: `proc()`, `s<i>`,
  one constants block, the slots), and `tests/api_test.cpp`: a procedure's
  statements see the earlier ones' writes in one transaction and write
  nothing when one fails; its errors at the call (P0002, P0003, 42702,
  22003, 22004, 42804, 42601, 42883, 08P01); recompiled after a DDL; and a
  conflict rerun in autocommit, 40001 after the last attempt, and left to
  the client inside a transaction.
- Phase 3: `tests/parser_test.cpp` (`parse_call`), `tests/binder_test.cpp`
  (`call_arguments`: types, rounding, ranges, `TO_TIMESTAMP`, the refusals),
  `tests/api_test.cpp` (`transaction_counts`), and `tests/server/test_wire.py`
  (HammerDB's form of `CALL`, its row and tag, in and out of a block, its
  errors; the monitor's queries).
- Phase 4a: `tests/api_test.cpp`, "FOR targets IN query runs the body per
  row of the query as it ran": the middle customer by `c_first`, the rows
  unchanged by the body's writes, `FOUND` after the loop, `EXIT` out of it,
  and a statement with no rows refused.
