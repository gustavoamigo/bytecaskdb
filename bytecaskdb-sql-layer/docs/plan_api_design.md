# Plan API: a physical algebra for generated plans

Status: **built**, revision 9 (revision 8 with the sort-key builder,
`bc_sort_key_*` (#579);
revision 8 built the sorters, the hash
tables and the aggregates' slots, `bc_sorter_open`'s `keep`, and
`bc_agg_null`;
revision 7 added the lifetime rule of a loop's storage, `planner_design.md`,
*Memory*, 64-bit handle generations, and the write set bounded as it
grows). This specifies the plan API of `bytecaskdb-sql-layer`: the calls a
generated plan makes, the guarantees they give, and the language plans are
written in. `algebra/` implements all of it. Nothing in the engine changes
for it.

**Plans are only ever ours** (owner decision, 2026-10-20). The layer runs
only plans its own compiler generates, and nothing is designed for plan
text from anywhere else: there is no checker for the subset, and the
runtime runs only the compiler's plans. Plans written by hand for the
plan API's own tests are ours too, and are fine there. The runtime's checks (handles,
the budget, sticky errors, the arithmetic helpers, `BC_E_CORRUPT`) guard
against the compiler's own bugs and against damaged data.

## Purpose

A SQL compiler turns a statement into a physical plan: every access path,
join algorithm, sort and guard decided. This document specifies the API that
plan is written against, and the language the plan is written in. The plan
is a function in a restricted subset of C that calls these functions and
nothing else. The same file is the `EXPLAIN` output, the executable, and
the unit of test.

The API is the only boundary between the two halves of a SQL engine:

- Above it: parser, binder, optimizer, code generator. They read SQL and the
  catalog and write a plan file. Anything else that writes the same file is
  a peer.
- Below it: the engine, a catalog, a key codec, a row format, and this API.
  No planning happens here. A plan says `bc_scan_open` on an index; the
  executor opens that index.

The subset compiles with any C compiler. TinyCC, linked into the layer,
compiles each plan at run time (`plan/tcc_loader.cppm`), and the plan API's
tests link a few hand-written plans with clang (`plans/`). An interpreter,
should a target need one (a WASM build: TinyCC has no WASM backend), is not
built. The API and the guarantees are the same whatever compiles the plan.

What it is not: a general C API for the engine, a wire protocol, or a place
for SQL semantics. Collations, decimals and dates are the compiler's problem;
here every value is a 64-bit integer, a double, a boolean or bytes.

## The guarantee

A plan in the subset (*The language*, below), as the compiler generates
it, has these properties in every tier, whatever its logic does:

- **Memory safe.** It cannot read or write memory outside buffers the API
  manages, and it cannot read a buffer after its owner has released it.
  The plan holds no pointers; every reference is a checked handle.
- **Free of undefined behaviour.** No signed overflow, no division by zero,
  no out-of-range conversion, no uninitialized read, no unsequenced
  modification, no shift, no cast. Each is either impossible in the subset
  or routed through a helper that defines it.
- **Terminating and bounded.** Every loop is driven by an API iteration,
  there is no recursion and no counted loop, and the API enforces a work
  budget and a deadline. A plan ends within its budget whatever its shape.
- **Bounded in memory.** A statement's memory has a budget, the
  `max_bytes` of its `bc_budget`. Every byte the API allocates for the
  statement is charged to it, and passing it is an error, never a spill
  (*Memory*, below).
- **Effect-free until commit.** The only outputs are a write plan and a
  result buffer, both discarded on any error and committed only by the
  runtime after the plan returns.
- **Index-consistent.** A plan writes rows, never index entries. The
  runtime writes every entry of every index the table has when the plan
  runs, so a committed write leaves each index holding exactly one entry per
  row, whatever the plan does.

A wrong plan can therefore do two things: return an error code, or return
or write a wrong answer. The second is what the proof machine's comparison
with PostgreSQL exists to catch (`testing_design.md`). Neither crashes the server, corrupts the engine, or holds a snapshot
past the budget. The only code that can still fault is the API's own
implementation, which is ordinary C++ under the engine's sanitizer and test
regime, and a plan cannot steer it except through checked arguments.

## Decisions, not mechanics

**A plan carries decisions, not mechanics.** This rule is the test for
anything added to the API.

- *Decisions* stay in the plan text, one line each: the access path (a point
  lookup or an index scan), the bounds and the direction, covering read or
  fetch, joins, guards, and NULL semantics. They are what the optimizer
  chose, what `EXPLAIN` must show, and what a reader checks against the SQL.
- *Mechanics* live in the runtime: key encoding, index maintenance and the
  unique checks it needs, column positions, defaults and NULL markers. Each
  follows from the catalog and a decision already on the page, so writing
  it out adds lines a reader skims and a compiler can get wrong, and buys no
  optimization: every API call resolves its handles, checks its generation
  and the sticky error, and type-checks its part, whatever its size.

So a write is one call per row (`bc_insert`, `bc_update`, `bc_delete`), and
the runtime derives the index entries from the table the catalog has when
the plan runs. A key from consecutive parameters is one call
(`bc_key_params`), a column copied to the result one call (`bc_emit_col`),
NULL included. The fine-grained builders stay where the parts come from
several sources, as New-Order's order key, whose `o_id` is read from the
district row. The early return on a NULL parameter stays in the text: in
SQL `id = NULL` matches no row, while a key built from a NULL would match
the NULL entries of a nullable index. That is three-valued logic, a
decision the compiler makes.

## Model

**A plan is a pure function of its parameters and one snapshot.** It reads
only through the snapshot, and its only effects are appending to a write
plan and to a result buffer. It cannot commit, see the clock, draw
randomness or allocate ids. The runtime commits the write plan when the
function returns and, on a conflict, discards both buffers and runs the
function again on a fresh snapshot. Retry is sound because nothing a plan
does is visible until the commit.

**Errors are sticky and returns are neutral.** C has no exceptions and
generated code carries no error checks. When a call fails (I/O, CRC, a
memory bound, a stale handle, overflow, the budget) the context records the
first error, and every later call returns a value that ends the plan: cursor
steps report end of data, iterations report done, gets report not found,
arithmetic returns zero, writes are dropped. Because every loop condition is
an API iteration, the plan unwinds within one pass of each open loop. The
runtime reads `bc_error` after the function returns. No exception ever
crosses into a plan: the whole body of every call is inside one handler,
so an allocation that fails anywhere in a call, a message being built
included, is `BC_E_MEMORY` and not an unwind through the plan's frames,
which TinyCC compiles without unwind tables. A test fails each allocation
of a set of plans in turn and checks this.

**Handles are checked indices.** Cursors, sorters, hash tables, aggregates,
groups, key builders, row builders and views are indices into per-context
tables, each wrapped in its own struct type so the C compiler rejects a
cursor passed as a sorter, with a generation tag so the API rejects a handle
whose owner has moved on. A stale or foreign handle sets `BC_E_HANDLE`. The
generation is 64 bits, drawn once per handle from a counter the context
never resets, so it never repeats: a stale handle never matches a slot a
later handle reuses. Plans are regenerated from SQL, never stored, so the
handles' layout can change with the API.

**Views are handles with a lifetime the API tracks.** A view names bytes
the API owns: a cursor's current record, a parameter, a value returned by a
read, or something built. It lives as *Memory* below says: a view made in a
loop goes when that loop steps, a cursor's view also when that cursor
steps or closes, and a view made outside every loop lives until the context
is reset. Reading through an invalidated view is an error, not a read.
`bc_pin` copies a view to extend it to the reset. The zero handle is the
empty view: every neutral return yields it, and `bc_empty` makes one on
purpose.

**A row view knows its table.** A view returned by `bc_get` on a row key or
by `bc_scan_row` is a *stored* row of that key's table; a view returned by
`bc_row_end` is a *built* row of the builder's table. The column decoders
read only row views, and only columns of the view's table;
`bc_update` and `bc_delete` take only a stored row as the old one, so the
index entries they remove are the ones the snapshot holds. Any other view
passed as a row is `BC_E_HANDLE`.

**Views copy by default.** A cursor copies each entry's key and value into
buffers it owns as it steps; `bc_get` copies the record into the arena. The engine's own read API already copies into a caller buffer,
so this costs one copy per row, tens of nanoseconds against a record read
of several hundred, and makes a stale view fail its generation check rather
than touch a buffer-pool frame the engine may have released. Lending the
engine's buffers instead is an optimization behind the same calls, taken
only if the harness shows the copy.

**A constraint error is never retried.** `BC_E_CONSTRAINT`, with a detail
code, is a sticky error of its own family: the plan unwinds like any other
failure, and the runtime reports it and does not retry. `bc_insert` and
`bc_update` set it for a duplicate primary or unique key, and the row
builder for a value outside its column's declared type (`BC_C_RANGE`) and
for a NULL in a NOT NULL column (`BC_C_NOT_NULL`), both with the table's
id, since column ids are per table; the message names the column. A plan
sets it itself with `bc_raise`. This is how a duplicate reaches the client as an
error rather than as a conflict: the engine reports every failed
precondition of a batch as the same conflict, so the runtime checks the
snapshot first, raises if the key is taken, and keeps a guard for the race.
On a retry after a genuine race the check runs again on the fresh snapshot
and reports the duplicate correctly.

**A loop's step owns what it allocates.** This is the lifetime rule, the
contract every plan is written to:
- A loop is driven by an API iteration: `bc_scan_next` on a cursor,
  `bc_sorter_next` on a sorter, `bc_hash_next` on a hash table probe, or
  `bc_agg_next` on an aggregate. A step is from one call that moves the
  loop to an entry to the next call on it.
- What the API allocates for the plan during a step is the step's: views,
  key and row builders and what they hold, copies (`bc_get`'s records, keys
  from `bc_key_params` and `bc_key_successor`, `bc_row_key`'s, decoded key
  parts, `bc_col_key`'s), and cursors and hash table probes opened in it.
  A view of a structure's contents (`bc_sorter_next`'s, `bc_sorter_nth`'s,
  `bc_hash_next`'s, `bc_agg_next`'s group key) is a handle like any other:
  it reads the structure's copy, and goes with the step it was made in.
  A sorter's `bc_sorter_nth` view also goes at the next `bc_sorter_nth` on
  that sorter, which takes its slot, so a loop over `nth` holds one view.
- All of it goes when the loop steps again, reaches its end, or its cursor
  closes. Loops nest: an outer loop's step also ends every step inside it.
  So a key built for one probe, an inner row and a group key all go when
  the outer row moves on.
- What is made during a step is that step's, the innermost one's, whatever
  it was made from: `bc_col_key` on an outer row inside an inner loop
  makes a key of the inner step, which goes when the inner loop steps.
- A loop the plan can leave before its iteration reports the end (an `if`
  of one step, as MIN's first entry is, or a LIMIT's counter) is left on
  an entry, with its step still open. The plan closes its cursor right
  after such a loop, and codegen emits that `bc_scan_close`. Until the
  close, what the plan makes is still that step's, and goes with it.
- What outlives a step: pins (`bc_pin`), the contents of blocking
  structures (sorters, hash tables, aggregate groups), the write set, and
  result rows. A view made outside every loop, such as a scan's bounds,
  lives until reset.

A handle used after its step ended is `BC_E_HANDLE`, not a read: each
handle carries a generation its entry holds, drawn from a counter the
context never resets.

**The rule assumes loops nest.** A loop's step is opened when its iteration
moves to an entry and closed when the loop moves again, and the open steps
are a stack: a step ending ends every step above it. Stepping an outer loop
inside an inner one, or two loops in turn, would end steps out of order,
and release what a loop still on its entry made. Plans are ours, and
codegen emits only nested loops, so nothing checks for interleaved stepping. Streaming operators therefore hold memory bounded by
the depth of the pipeline, not by the number of rows; only pins,
structures and results grow.

The API keeps this without the plan's help: it marks its handle tables and
its arena of 64 KiB chunks when a loop moves to an entry, and cuts them
back to the mark when the step ends. Pins go in an arena of their own.
The arena keeps one chunk across resets.

**Memory is the statement's, and charged to its budget.** Every byte the
API allocates for the statement is charged to its memory budget,
`bc_budget::max_bytes` (0: none), as `max_reads` charges work, and released
when its step ends or the context is reset:
- the keys it builds, as their parts are added, and the copies above;
- each row a builder finishes;
- a cursor's bounds and the entry it is on: its key, its value and an index
  entry's fetched row, released as it steps;
- each handle it gives the plan, a view, a pin, a key or row builder, a
  cursor or a hash table probe, a fixed amount per handle:
  `kViewEntryBytes`, `kKeyEntryBytes`, `kRowEntryBytes`, `kCursorEntryBytes`
  and `kHashIterEntryBytes` (`algebra/runtime.cppm`);
- each sorter, hash table and aggregate opened, `kSorterBytes`,
  `kHashBytes` or `kAggBytes`, and an aggregate a byte per slot for its
  slots' kinds;
- each aggregate group's key, `kGroupEntryBytes` for the aggregate's own
  node, and `kAggSlotBytes` per slot;
- each sorter entry's sort key and payload, and `kSortEntryBytes` for its
  node; a top-n sorter that replaces an entry is charged the difference in
  bytes, and one that refuses an entry nothing;
- each hash table payload, and `kHashEntryBytes` for its node, and each key
  once, when it is new to the table;
- each result row, with its 4-byte length.

It counts bytes requested and the fixed amounts, not the allocator's
overhead, so the accounting is deterministic: a plan holds the same bytes
on every run over the same data. Two things are not charged:
- **The write set.** A row passed to `bc_insert`, `bc_update` or
  `bc_delete` is copied into it; its built copy is released with its step
  like any other. The write set has a bound of its own instead, below.
- **A row still being built**, until `bc_row_end` finishes it.

**The write set is bounded as it grows.** It is what the statement keeps
until commit: its effects (puts, deletes, range deletes and guards), its
insert claims and locked rows, and the row keys, unique prefixes and
ranges it wrote. The call that adds to it counts what it adds, as a
transaction counts what it holds: each buffer's size plus the allocator's
overhead, and each entry's node or its slot at twice its vector's size, a
conservative bound on the memory. The bound is:
- in autocommit, the engine's 1 GiB batch limit;
- in a transaction, its `max_bytes`, less what its earlier statements
  hold. `bc_txn_add` checks the whole again before it takes the statement
  in.

The call that passes it fails with `BC_E_LIMIT` (SQLSTATE 54000), as the
statement budget does, so a 20M-row `UPDATE` stops at the row that passes
1 GiB rather than build several first. A bulk `UPDATE` of 100 MB in
autocommit runs under the 64 MiB statement budget, since only the step's
rows are charged to it.

A charge past the budget is `BC_E_LIMIT` (SQLSTATE 54000), a limit
the layer sets: checked before the bytes are allocated wherever their size
is known first, and otherwise right after, so a statement passes it by at
most one record. A real allocation failure, `std::bad_alloc`, stays
`BC_E_MEMORY` (53200). Either error is sticky, so nothing the statement
did is committed, and inside a transaction it aborts at `bc_txn_add`.
There is no spill. The embedded API passes `DatabaseOptions::
statement_memory_bytes`, 64 MiB by default.

**The API meters the work.** `bc_ctx_begin` sets a budget in record reads
and cursor steps and a deadline. Every read and every step charges the
budget, and the API checks the deadline periodically. Exhausting either sets
`BC_E_BUDGET` and the plan unwinds. Compute between API calls is
straight-line and finite, so the budget bounds the whole execution without
help from the compiler. An interpreter, if a target ever needs one, would
add a fuel counter on backward branches, which compiled plans do not need.

**One context, one thread.** A context is used by one thread at a time.
Contexts run concurrently on different threads; reads scale as the engine's
reads scale, and commits go through the engine's group commit. A plan never
blocks: reads are lock-free and the commit happens after it returns.

**Within a plan, reads see the snapshot, not the plan's own writes.** In a
transaction, "the snapshot" is the transaction's view: its snapshot plus
the writes of its earlier statements (`docs/transaction_design.md`). This
is SQL statement semantics: a statement sees the state before it began. The
runtime's unique checks read the snapshot too. A second write of a row the
plan already wrote is refused (`BC_E_HANDLE`): `bc_update` and `bc_delete`
derive the entries they remove from the snapshot's row, so a second write
would leave the first one's entries without a row. Only a wrong plan
reaches it, since one SQL statement affects each row at most once
(PostgreSQL refuses an `ON CONFLICT DO UPDATE` that would affect a row
twice). The exception is a `bc_insert` of a key the plan already put
there: that is `INSERT … VALUES (1, …), (1, …)`, a duplicate key, and it
raises `BC_C_UNIQUE` (SQLSTATE 23505, as PostgreSQL reports it), never
retried. The tracking spans the execution, every run between
`bc_ctx_begin` and the commit: those runs share the snapshot and the write
plan, and each derives the entries it removes from the snapshot's row, so
a row written by one run and again by a later one would keep both runs'
entries. In a transaction the tracking spans one statement: a later
statement reads the earlier one's row through the transaction's view and
derives its entries from that. The affected-row count is each run's own.
Two writes of one execution that give two rows one unique value are
`BC_C_UNIQUE` too: the runtime records each unique prefix the execution
writes, which its snapshot can't see.
Read-your-own-writes across statements in an interactive transaction is an
overlay the reads and cursors read through: `docs/transaction_design.md`,
*The transaction's view*.

**A row a plan reads and then writes needs no guard.** The row's key is in
the write set, and the engine refuses a write plan with a snapshot when
any key it writes changed since that snapshot (the engine's `CONTRACT.md`). So
`UPDATE … SET qty = qty - 1` is first-committer-wins: a concurrent update
or delete of the row makes the commit a conflict, retried on a fresh
snapshot, and no update is lost or applied to a deleted row (tests: "plan
writes: concurrent read-then-write of one row loses no update", "… a delete
racing updates of one row has one outcome"). A guard
(`bc_guard_unchanged`, `bc_guard_range`) is needed only for what a plan
reads and does *not* write: another row whose value decided the write, a
range that decided which rows qualify, a key found absent. Without those
guards a plan has snapshot isolation; with them, serializable conflict
detection.

## Types and constants

```c
#include <stdint.h>
#include <stdbool.h>

typedef struct bc_ctx bc_ctx;                       /* opaque; one per executing plan */

/* Handles: an index and a generation, each in its own struct type. The
   generation is 64 bits: one is drawn per handle, and it never repeats. */
typedef struct { uint32_t i; uint64_t g; } bc_view;   /* bytes the API owns */
typedef struct { uint32_t i; uint64_t g; } bc_cursor;
typedef struct { uint32_t i; uint64_t g; } bc_sorter;
typedef struct { uint32_t i; uint64_t g; } bc_hash;
typedef struct { uint32_t i; uint64_t g; } bc_hash_iter;
typedef struct { uint32_t i; uint64_t g; } bc_agg;
typedef struct { uint32_t i; uint64_t g; } bc_group;
typedef struct { uint32_t i; uint64_t g; } bc_key;    /* a key under construction */
typedef struct { uint32_t i; uint64_t g; } bc_row;    /* a row under construction */

typedef void (*bc_plan_fn)(bc_ctx *);               /* every plan has this signature */

enum bc_error_code {
  BC_OK = 0,
  BC_E_IO,          /* std::system_error from the engine */
  BC_E_CORRUPT,     /* CRC mismatch or unparseable record */
  BC_E_MEMORY,      /* an allocation failed (std::bad_alloc) */
  BC_E_BUDGET,      /* work budget or deadline exhausted */
  BC_E_LIMIT,       /* key or value over the engine's limits, or the statement over its memory budget */
  BC_E_HANDLE,      /* stale, foreign or out-of-range handle, or a table, index or column the plan's
                       bc_schema_ref does not declare, or not of the type used: a plan bug */
  BC_E_ARITH,       /* integer overflow, division by zero, out-of-range conversion */
  BC_E_CONSTRAINT,  /* never retried; raised by the runtime (BC_C_UNIQUE from bc_insert and bc_update,
                       BC_C_RANGE and BC_C_NOT_NULL from the row builder) or by the plan with bc_raise */
  BC_E_LAYOUT,      /* plan incompatible with the live catalog, or a row violates its layout */
  BC_E_DEGRADED,    /* engine degraded; writes refused */
  BC_E_CLOSED,
  BC_E_UNKNOWN,     /* a commit failed and degraded the engine: its batch may have reached the data
                       file, so its outcome is unknown, never "nothing written" (SQLSTATE 08007) */
};

enum bc_scan_flags {
  BC_SCAN_FORWARD = 0,
  BC_SCAN_REVERSE = 1u << 0,
  BC_SCAN_PREFIX  = 1u << 3,   /* hi ignored: every key with lo as prefix */
};
/* Bits 1 and 2 were LO_EXCLUSIVE and HI_INCLUSIVE in revision 1; they stay
   unassigned so a plan that still passes them is refused, not misread. */
/* Bounds are always [lo, hi). There are no exclusive-lo or inclusive-hi
   flags: an index key carries the primary key after the bounded part, so an
   entry equal to a partial bound still sorts after it. "name > $1" is
   lo = bc_key_successor(key(name)); "name <= $2" is hi = successor. */

enum bc_constraint {   /* the detail of BC_E_CONSTRAINT; the compiler emits the object id with it */
  BC_C_UNIQUE = 1, BC_C_NOT_NULL, BC_C_CHECK, BC_C_FOREIGN_KEY,
  BC_C_RANGE,          /* a value outside its declared type: SMALLINT, INTEGER, REAL, TEXT and
                          VARCHAR(n) (UTF-8, n code points), DECIMAL(p, s), DATE, TIMESTAMP, TIME */
};

enum bc_layout_detail {  /* the detail of BC_E_LAYOUT; every BC_E_LAYOUT carries one */
  BC_L_ROW = 0,           /* a row written to a table it is not of, or a column its layout lacks */
  BC_L_INCOMPATIBLE = 1,  /* bc_ctx_run refused the plan's bc_schema_ref: recompile it */
  BC_L_PLAN = 2,          /* the plan or its bc_schema_ref is malformed, a compiler bug: an unknown result
                             type, a result row too wide, result rows emitted out of order or off their
                             declared types */
  BC_L_RETIRED = 3,       /* a stored row of a layout generation REBUILD retired */
};

enum bc_commit_result { BC_COMMITTED, BC_CONFLICT, BC_FAILED /* see bc_error */ };

typedef struct {
  uint64_t max_reads;       /* record reads and cursor steps, together; 0 = none */
  uint64_t max_bytes;       /* memory the statement holds, as bytes requested; 0 = none */
  uint64_t deadline_ns;     /* monotonic; 0 = none */
} bc_budget;

/* The runtime's objects, opaque to a plan: the open engine, the live
   catalog, and the parameters bound for one execution. */
typedef struct bc_db bc_db;
typedef struct bc_catalog bc_catalog;
typedef struct bc_params bc_params;

/* A column a plan reads or sets, as compiled: its table, its permanent
   column id, and its bc_type. No position: the runtime finds that. */
typedef struct { uint32_t table, col; uint8_t type; } bc_col_ref;

/* What a plan was compiled or last checked against, and the types of the
   rows it emits, in emit order. bc_ctx_run checks the objects and columns
   against the pinned catalog version (docs/catalog_design.md,
   *Compatibility*) on every run; the result layout is the plan's own and
   is not a catalog object. */
typedef struct {
  uint64_t catalog_version;                                     /* compiled or last checked against; for messages */
  const uint32_t   *objects_read;    uint32_t n_read;           /* tables and indexes it names */
  const uint32_t   *tables_written;  uint32_t n_written;        /* tables whose rows it writes */
  const bc_col_ref *cols;            uint32_t n_cols;           /* every column it reads or sets */
  const uint8_t    *result_types;    uint32_t n_result;         /* bc_type codes */
} bc_schema_ref;

enum bc_type { BC_T_I64 = 1, BC_T_F64 = 2, BC_T_BOOL = 3, BC_T_BYTES = 4 };
```

A plan's constants are ids, emitted by the compiler from the catalog
(`Catalog::constants`) and named from ids too, so that no SQL name has to be
a C identifier:

| Constant | Value |
|---|---|
| `TBL_<t>` | the table id |
| `IDX_<i>` | the index id |
| `COL_<t>_<c>` | the column's permanent id `c` |
| `PART_<k>_<c>` | the position of column `c` in the key of table or index `k`, for the key-part decoders |

None is a position in a row. The runtime maps each column id the schema
reference declares to its offset or slot in the table's current layout,
when it verifies the reference (*Runtime*), so a layout generation never
reaches plan text. A column a plan names but its reference does not declare
is `BC_E_HANDLE` at the access, and so is a table or index it names but does
not declare, or a table it writes but does not list in `tables_written`: the
reference is complete by construction, and the compatibility check covers
every object and column the plan touches. A declared column's table must
be declared too (`BC_E_LAYOUT` with `BC_L_PLAN` otherwise), and an index
cursor hands out its table's rows only when the table is declared. Every
run has a reference: `bc_ctx_run` without one is `BC_E_HANDLE`.

## Function reference

Cost is counted in record reads, the engine's unit of work: a key directory
descent plus one read of the record, served from the buffer pool or the
page cache. Everything that is not a record read is a few nanoseconds,
including the handle and bounds checks.

### Runtime: creating, running and committing

Called by the server, never by a plan.

```c
bc_ctx *bc_ctx_new(bc_db *db, const bc_catalog *cat);
void    bc_ctx_free(bc_ctx *);
void    bc_ctx_begin(bc_ctx *, const bc_params *params, bc_budget budget);  /* takes the snapshot */
void    bc_ctx_run(bc_ctx *, bc_plan_fn plan, const bc_schema_ref *checked_against);
bool    bc_ctx_check(bc_ctx *, const bc_schema_ref *checked_against);      /* bc_ctx_run's verification alone */
enum bc_commit_result bc_ctx_commit(bc_ctx *, bool sync);
bool    bc_ctx_next_result(bc_ctx *, const uint8_t **row, uint32_t *len);     /* result rows, in emit order */
void    bc_ctx_reset(bc_ctx *);                                               /* drops snapshot, plan, arena */
enum bc_error_code bc_error(const bc_ctx *);
const char *bc_error_message(const bc_ctx *);
uint32_t bc_error_detail(const bc_ctx *);                  /* BC_E_CONSTRAINT: the bc_constraint in the high byte, the object id (below 2^24) in the low 24 bits; BC_E_LAYOUT: a bc_layout_detail */
int64_t bc_ctx_affected(const bc_ctx *);                   /* what the last run's bc_emit_count recorded, or -1 */
int     bc_execute(bc_ctx *, bc_plan_fn, const bc_schema_ref *, const bc_params *,
                   bc_budget, bool sync, uint32_t max_retries);               /* the retry loop */
```

| Function | What it does |
|---|---|
| `bc_ctx_new` | Allocates a context bound to an open engine and a catalog. Its memory is bounded per statement, by the `bc_budget` each begins with, not per context. |
| `bc_ctx_begin` | Pins a catalog version and takes a snapshot that agrees with it (no DDL committed between the two), binds the parameters, sets the budgets of work and memory. Clears the error. Must precede `bc_ctx_run`. |
| `bc_ctx_check` | Verifies a `bc_schema_ref` against the pinned version as `bc_ctx_run` does, and runs nothing. False with `BC_E_LAYOUT` where `bc_ctx_run` would refuse the plan. What a server calls to learn whether a cached plan still fits the catalog before it checks parameters against that plan. |
| `bc_ctx_run` | Verifies the plan's `bc_schema_ref` against the catalog version the context pinned at `bc_ctx_begin`, then calls the plan and returns when it returns. Compatible means: every table and index it names exists; every column it reads or sets exists in its table with the type it was compiled with; every table it writes exists. Verifying also maps each declared column to its offset or slot in the current layout. A null reference is `BC_E_HANDLE`. A plan that fails sets `BC_E_LAYOUT` and does not run; the server recompiles it from the SQL it cached. Adding a column or an index never invalidates a plan, since the runtime maintains whatever indexes the table has; a drop invalidates the plans that name what it drops. There is no version-equality check. |
| `bc_ctx_commit` | Submits the write plan with `apply_batch`, guarded by `ensure_unchanged` on the catalog record of every table whose keys it writes, so a DDL on such a table committed since the snapshot makes it a conflict: the index entries the runtime wrote follow the pinned version's indexes, and a `CREATE INDEX` committed after the snapshot would otherwise miss the row. `BC_COMMITTED` on success. `BC_CONFLICT` when the engine reports a guard or write-write conflict: nothing was written. `BC_FAILED` when `bc_error` is set, including an error raised during the plan: nothing is committed. A plan with no writes commits nothing and returns `BC_COMMITTED`, whatever it guarded: guards protect writes, and what a read-only execution read is its snapshot's either way. |
| `bc_ctx_next_result` | Iterates the result buffer. Rows are in the row format of the layout the plan emitted. The runtime, not a plan, sees raw pointers here; valid until `bc_ctx_reset`. |
| `bc_ctx_reset` | Releases the snapshot, drops the write plan and result buffer, frees the statement's memory (the arena keeps one chunk) and its charge, and releases every handle, so every handle and view the plan held is invalid. |
| `bc_error_detail`, `bc_ctx_affected` | The constraint kind and object id of a raised error, or for `BC_E_LAYOUT` its class: `bc_ctx_run` refusing the reference (`BC_L_INCOMPATIBLE`, the only one a recompile cures), a row used with a table or column it is not of (`BC_L_ROW`), a malformed plan or reference (`BC_L_PLAN`), or a row of a retired layout generation (`BC_L_RETIRED`). The runtime sets every `BC_E_LAYOUT` with its detail; one without cannot be compiled; the affected-row count the last run recorded. Both read by the runtime after the plan returns. |
| `bc_execute` | `begin`, `run`, `commit`; on `BC_CONFLICT`, `reset` and repeat up to `max_retries`. A `BC_E_CONSTRAINT` raised by the plan is final: no retry. Returns the commit result of the last attempt, except that a last attempt's conflict on a key it inserted, taken by a concurrent commit, is the duplicate it is: `BC_FAILED` with `BC_C_UNIQUE` on the table or index, as a retry's own check would raise (`docs/transaction_design.md`, *Commit and conflicts*). This is the only place retry lives. |

**Verification runs on every execution.** `bc_ctx_run` checks the
reference against the version it pinned each time, and builds what the
run's calls look up: the declared objects and the written tables sorted
by id, the column map, the written tables' key slots. Each access a plan
makes is then a binary search or an index, not a scan of the reference.
The context keeps these buffers across runs, and rebuilds the result
layout only when the result types change. The check is a known cost, paid
by every execution: 42–104 ns for a point read's reference (2 columns, 1
object) and 0.97–1.26 µs for a New-Order-sized one (36 columns, 10
objects), measured by `sql_layer_bench`. Caching verified references is
deferred until a workload shows the need.

### Parameters and results

```c
int64_t  bc_param_i64(bc_ctx *, uint32_t i);
double   bc_param_f64(bc_ctx *, uint32_t i);
bool     bc_param_bool(bc_ctx *, uint32_t i);
bc_view  bc_param_bytes(bc_ctx *, uint32_t i);
bool     bc_param_null(bc_ctx *, uint32_t i);

void     bc_emit_begin(bc_ctx *);                     /* a row of the plan's result layout */
void     bc_emit_i64(bc_ctx *, int64_t);
void     bc_emit_f64(bc_ctx *, double);
void     bc_emit_bool(bc_ctx *, bool);
void     bc_emit_bytes(bc_ctx *, bc_view);
void     bc_emit_null(bc_ctx *);
void     bc_emit_col(bc_ctx *, bc_view row, uint32_t col);                /* the column, NULL as NULL */
void     bc_emit_key_part(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);   /* not a DOUBLE part: BC_E_HANDLE */
void     bc_emit_end(bc_ctx *);
void     bc_emit_count(bc_ctx *, int64_t affected);   /* write statements: the row count */
```

| Function | What it does |
|---|---|
| `bc_param_*` | Reads parameter `i` as bound by `bc_ctx_begin`. A wrong index or type is `BC_E_HANDLE` and a neutral value. A parameter view is a handle like any other: made in a loop, it goes with the step. |
| `bc_emit_begin` … `bc_emit_end` | Appends one result row, in the row format, with the types the plan's `bc_schema_ref` declares in `result_types`. Columns are emitted in that order; a count or type that does not match is `BC_E_LAYOUT`. The row is copied into the result buffer and charged to the memory budget. |
| `bc_emit_col` | Emits column `col` of a row view: its value, or NULL when the column is null or the row predates it; `BC_E_CORRUPT` for a column marked present that the row does not hold. The type is the column's, and must be the next result column's. One call where a nullable column took an `if` and two emits. |
| `bc_emit_key_part` | Emits part `part` of an encoded key of `table_or_index`, NULL as NULL: a covering read's column, from `bc_scan_key`, with no row read. A `DOUBLE` part is `BC_E_HANDLE`: the key canonicalises `-0.0` and NaN, so the value comes from the row. |
| `bc_emit_count` | Records the affected-row count a write statement reports. At most once per plan. |

### Arithmetic and conversion

The subset has no raw integer arithmetic (*The language*). These helpers
define the cases C leaves undefined. On failure they set `BC_E_ARITH` and
return zero.

```c
int64_t bc_add(bc_ctx *, int64_t, int64_t);     /* overflow → error */
int64_t bc_sub(bc_ctx *, int64_t, int64_t);
int64_t bc_mul(bc_ctx *, int64_t, int64_t);
int64_t bc_div(bc_ctx *, int64_t, int64_t);     /* zero divisor, INT64_MIN / -1 → error */
int64_t bc_mod(bc_ctx *, int64_t, int64_t);
int64_t bc_neg(bc_ctx *, int64_t);              /* INT64_MIN → error */
int64_t bc_f64_to_i64(bc_ctx *, double);        /* NaN or out of range → error; truncates */
enum bc_int_type { BC_SMALLINT = 2, BC_INTEGER = 4 };
int64_t bc_check_int(bc_ctx *, int64_t v, uint32_t int_type, uint32_t object_id);  /* outside the type: BC_C_RANGE */
double  bc_i64_to_f64(int64_t);                 /* always defined */
bool    bc_bytes_eq(bc_ctx *, bc_view, bc_view);
int     bc_bytes_cmp(bc_ctx *, bc_view, bc_view);       /* memcmp order */
bool    bc_bytes_prefix(bc_ctx *, bc_view prefix, bc_view);
```

The header implements the integer helpers on the compiler's overflow
builtins where available and on a portable compare otherwise, so a plan file
never depends on a C23 compiler. Double arithmetic stays plain: it is
defined for every input, and NaN and infinities propagate as SQL expects the
compiler to handle. Byte comparisons are for encoded or binary columns; the
compiler emits comparisons on sort keys it built, never on raw text, and on
TEXT and VARCHAR columns compared under `COLLATE "C"`, byte order.

`bc_check_int` is the range check of a computed integer (added in the
front end's slice 5): it returns `v` when it is inside the SQL type the
compiler resolved, `BC_SMALLINT` or `BC_INTEGER`, and otherwise sets
`BC_E_CONSTRAINT` with `BC_C_RANGE` on `object_id` and PostgreSQL's words
("integer out of range"), SQLSTATE 22003. Which type an operation is in is
the compiler's decision, so it shows in the plan text; the bounds are the
runtime's.

### Keys

Keys are built in the mem-comparable encoding of *Keys and
rows* below. A builder is finalized once; it and its view are its step's.

```c
bc_key   bc_key_begin(bc_ctx *, uint32_t table_or_index);
void     bc_key_i64(bc_ctx *, bc_key, int64_t);
void     bc_key_f64(bc_ctx *, bc_key, double);
void     bc_key_bool(bc_ctx *, bc_key, bool);
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_param(bc_ctx *, bc_key, uint32_t i);  /* parameter i as the next part, NULL included */
bc_view  bc_key_end(bc_ctx *, bc_key);
bool     bc_key_fits(bc_ctx *, bc_key);
bc_view  bc_key_params(bc_ctx *, uint32_t table_or_index, uint32_t first, uint32_t n);  /* parts from params first.. */
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */
```

| Function | What it does |
|---|---|
| `bc_key_begin` | Starts a key for a table (its primary key) or an index. Writes the namespace byte and the 4-byte id. Returns a builder handle. |
| `bc_key_i64`, `bc_key_f64`, `bc_key_bool`, `bc_key_bytes`, `bc_key_null` | Appends one part. Nullability and direction come from the catalog entry for `table_or_index`, so the builder knows whether to write a null marker and whether to invert the bytes. A part of the wrong type, or more parts than the key has, is `BC_E_HANDLE`. |
| `bc_key_param` | Appends parameter `i` as the next part: its value, or the null part when it is NULL. A type that is not the part's, or a NULL for a part that is not nullable, is `BC_E_HANDLE`. A `REAL` part is keyed as its float rounding. Whether the value is inside the column's type is checked before the plan runs, by the embedded API: every compared parameter has one SQL type, and a parameter compared with columns of two is refused at bind (`docs/types_design.md`). |
| `bc_key_end` | Finalizes and returns the key. A partial key (fewer parts than the index has) is legal and is a prefix the compiler uses as a scan bound. A key over the engine's hard ceiling (65,535 bytes) is `BC_E_LIMIT`. |
| `bc_key_fits` | Whether the key built so far is within the engine's key ceiling. A key past it names no row, so a join asks before `bc_key_end` and skips an outer row whose long value would make the inner key one: PostgreSQL finds no inner row there, where `bc_key_end` would fail the statement (front end slice 5). |
| `bc_key_params` | The key, or prefix, of `table_or_index` whose first `n` parts are parameters `first` to `first + n - 1`, each as `bc_key_param` appends it: `bc_key_begin`, `n` parts and `bc_key_end` in one call. `(cx, TBL_9, 0, 3)` is a composite primary key `(w_id, d_id, c_id)`. The builder remains for keys whose parts come from anywhere else. |
| `bc_key_successor` | The exclusive upper bound of the prefix `key`: its last byte incremented, carrying. Turns a prefix into a half-open range where `BC_SCAN_PREFIX` does not fit. |

### Sort keys

A sort key is what a sorter orders by (`bc_sorter_add`), built from parts
the plan chooses, each with its own direction and NULL placement: `ORDER
BY a DESC, b` over any columns, whatever index the table has. `memcmp`
order on the key is PostgreSQL's `ORDER BY` order, part by part, for
every type the layer has, and equal values make equal keys, so the
sorter's stable sort keeps the rows PostgreSQL ties in the order added.
Text compares bytewise, the layer's one collation (`types_design.md`,
*Text is valid UTF-8*): the order of PostgreSQL's `COLLATE "C"`. The encoding is
`types_design.md`, *Sort keys*.

```c
enum bc_sort_dir   { BC_SORT_ASC = 0, BC_SORT_DESC = 1 };
enum bc_sort_nulls { BC_SORT_NULLS_LAST = 0, BC_SORT_NULLS_FIRST = 1 };
bc_key   bc_sort_key_begin(bc_ctx *);
void     bc_sort_key_i64(bc_ctx *, bc_key, int64_t, uint32_t dir, uint32_t nulls);
void     bc_sort_key_f64(bc_ctx *, bc_key, double, uint32_t dir, uint32_t nulls);   /* -0.0 is 0.0; NaN above +inf */
void     bc_sort_key_bool(bc_ctx *, bc_key, bool, uint32_t dir, uint32_t nulls);
void     bc_sort_key_bytes(bc_ctx *, bc_key, bc_view, uint32_t dir, uint32_t nulls);
void     bc_sort_key_null(bc_ctx *, bc_key, uint32_t dir, uint32_t nulls);
void     bc_sort_key_col(bc_ctx *, bc_key, bc_view row, uint32_t col, uint32_t dir, uint32_t nulls);  /* NULL included */
bc_view  bc_sort_key_end(bc_ctx *, bc_key);
```

| Function | What it does |
|---|---|
| `bc_sort_key_begin` | Starts an empty sort key: no namespace, no id and no layout. Returns a key builder handle, charged `kKeyEntryBytes` and owned by its step as any key builder. |
| `bc_sort_key_i64`, `bc_sort_key_f64`, `bc_sort_key_bool`, `bc_sort_key_bytes`, `bc_sort_key_null` | Appends one part: a value, or NULL, with its direction and NULL placement. Every call of one part takes the same `dir` and `nulls`, a NULL's too, so the plan writes a part's placement once. A `DOUBLE` orders as PostgreSQL's `float8`: `-0.0` and `0.0` make one key, every NaN one key above `+∞`. The bytes are charged as the part is added. A `dir` or `nulls` outside its enum is `BC_E_HANDLE`. |
| `bc_sort_key_col` | Appends a row's column as the next part, NULL included: what a plan uses for `ORDER BY col`. A column the plan's `bc_schema_ref` does not declare is `BC_E_HANDLE`, as for every column decoder. |
| `bc_sort_key_end` | Finalizes the key and returns its view, the step's. There is no key ceiling: a sort key never reaches the engine, and its bytes are bounded by the memory budget. |

A sort key and a table's or index's key do not mix: `bc_key_*` parts,
`bc_key_end` and `bc_key_fits` on a sort key's builder, and `bc_sort_key_*`
on a table's or index's, are `BC_E_HANDLE`. The type of a part is the
plan's to keep the same in every row: the key holds no type tag.

### Reads

Every read goes through the context's snapshot and charges the budget.

```c
bc_view   bc_get(bc_ctx *, bc_view key, bool *found);
bool      bc_exists(bc_ctx *, bc_view key);

bc_cursor bc_scan_open(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi, uint32_t flags);
bool      bc_scan_next(bc_ctx *, bc_cursor);
bc_view   bc_scan_key(bc_ctx *, bc_cursor);
bc_view   bc_scan_row(bc_ctx *, bc_cursor);                   /* the entry's row; on an index, fetched */
void      bc_scan_close(bc_ctx *, bc_cursor);
bc_view   bc_empty(bc_ctx *);                                 /* the empty view, for an open bound */

/* Key-part decoders: read part `part` of an encoded key of `table_or_index`
   (index columns first, then the primary key's parts). What makes a
   covering index read possible: the columns an index key holds are read
   from bc_scan_key without fetching the row. */
int64_t   bc_key_part_i64(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);
double    bc_key_part_f64(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);  /* always BC_E_HANDLE: a DOUBLE part is canonicalised; read the row */
bool      bc_key_part_bool(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);
bc_view   bc_key_part_bytes(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);
bool      bc_key_part_null(bc_ctx *, uint32_t table_or_index, bc_view key, uint32_t part);

int64_t   bc_count_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi, int64_t limit);
```

| Function | What it does | Returns and lifetime | Cost |
|---|---|---|---|
| `bc_get` | Reads the record at `key`. On a row key the view is a stored row of that table. | The value; `*found` false and an empty view when absent. The step's. | 1 record read |
| `bc_exists` | Whether `key` is present. | `bool`. | 1 record read (the blind key directory confirms a key from its record) |
| `bc_scan_open` | Opens a cursor over `[lo, hi)` on a table (primary key order) or an index, direction per `flags`. An empty `lo` (`bc_empty`) means the start of the table or index, an empty `hi` the end. With `BC_SCAN_PREFIX`, `hi` is ignored and the range is every key having `lo` as a prefix. Positioned before the first entry. | A cursor handle. Released with the step it was opened in, or by `bc_ctx_reset`; `bc_scan_close` releases its entry early. | 0 |
| `bc_scan_next` | Advances in the scan's direction, charging one unit of budget. Ends the step: the cursor's views, and everything the step allocated, go. | `true` while an entry is available; `false` at the end, on error, or when the budget is spent. | 1 record read (the key directory holds no key bytes, so each step reads the entry's record; on a table scan the same read yields the value) |
| `bc_scan_key` | The current entry's full encoded key. | View valid until the cursor steps or closes. | 0 |
| `bc_scan_row` | The current entry's row, a stored row of the cursor's table. On a table scan it is the entry's value. On a secondary index the runtime builds the row key from the entry's primary-key suffix and reads it, once per entry however often it is asked: the fetch of a non-covering read. An index entry whose row is missing is `BC_E_CORRUPT`, never skipped: the runtime keeps every entry paired with its row, so a missing one is damage. | View valid until the cursor steps or closes. | 0 on a table, 1 record read on an index |
| `bc_empty` | The empty view. | The zero handle, valid always. | 0 |
| `bc_key_part_*` | Decodes one part of an encoded key, by position in the key's catalog layout. On a secondary index the index columns come first, then the primary key's parts. `bc_key_part_null` reports a null part; the typed decoders return zero or empty for one. A part number or type that disagrees with the layout is `BC_E_HANDLE`, and so is `bc_key_part_f64` on any part: the key canonicalises a `DOUBLE` (`-0.0` is `0.0`, every NaN one NaN), so it is read from the row. A key that does not parse is `BC_E_CORRUPT`. | Values; a bytes part is unescaped into the arena, the step's, charged. | 0 |
| `bc_count_range` | Counts the live entries in `[lo, hi)` without walking them, stopping at `limit`. Compiles to `Snapshot::count_keys`. The planner's cardinality source and `COUNT(*)` over a key range. | `min(count, limit)`. | at most 2 record reads, whatever the range holds |

A range scan on a secondary index with a fetch of each row costs one read
per step plus one per `bc_scan_row`. A covering read, where the plan reads
the columns it needs from `bc_scan_key` with `bc_emit_key_part` or
`bc_key_part_*`, costs one. Covering or fetch is the optimizer's decision,
and the text shows which: a plan that never calls `bc_scan_row` reads no
row. A `DOUBLE` column is never read from a key: the encoding writes `-0.0`
as `0.0` and every NaN as one, so only the row holds the exact value.

### Rows and columns

Rows use the format in *Keys and rows*. A plan names a column by its permanent
id (`COL_<t>_<c>`), and only the columns its `bc_schema_ref` declares; the
runtime maps the id to the offset or slot in the row's layout generation,
from the map it built when it verified the reference. A decoder resolves the
view, checks it is a row of the column's table and that its header parses,
then loads, bounds-checked against the row. A column the reference does not
declare, of another table than the row's, or of another type than the call's
is `BC_E_HANDLE` and a neutral value; a row that does not parse is
`BC_E_CORRUPT`.

```c
bool     bc_col_null(bc_ctx *, bc_view row, uint32_t col);   /* the bitmap bit: clear means null, or a column the row predates;
                                                                set for a column the row does not hold, or whose value
                                                                runs past the row, BC_E_CORRUPT */
int64_t  bc_col_i64(bc_ctx *, bc_view row, uint32_t col);
double   bc_col_f64(bc_ctx *, bc_view row, uint32_t col);
bool     bc_col_bool(bc_ctx *, bc_view row, uint32_t col);
bc_view  bc_col_bytes(bc_ctx *, bc_view row, uint32_t col); /* a sub-view; same lifetime as row */
bc_view  bc_col_key(bc_ctx *, bc_view row, uint32_t col);   /* the value as a mem-comparable key part */
bc_view  bc_row_key(bc_ctx *, uint32_t table, bc_view row);  /* the row's primary key, from its columns */

bc_row   bc_row_begin(bc_ctx *, uint32_t table);
bc_row   bc_row_from(bc_ctx *, uint32_t table, bc_view old);  /* copy, then overwrite columns */
void     bc_row_set_i64(bc_ctx *, bc_row, uint32_t col, int64_t);
void     bc_row_set_f64(bc_ctx *, bc_row, uint32_t col, double);
void     bc_row_set_bool(bc_ctx *, bc_row, uint32_t col, bool);
void     bc_row_set_bytes(bc_ctx *, bc_row, uint32_t col, bc_view);
void     bc_row_set_null(bc_ctx *, bc_row, uint32_t col);
void     bc_row_set_param(bc_ctx *, bc_row, uint32_t col, uint32_t i);   /* parameter i, NULL included */
bc_view  bc_row_end(bc_ctx *, bc_row);
bc_view  bc_pin(bc_ctx *, bc_view);                 /* a copy that lives until reset */
```

| Function | What it does |
|---|---|
| `bc_col_null` | True if column `col`'s bit in the bitmap is clear. The bitmap says *present*: a column set null and a column the row predates both have the bit clear, so one test covers both (`docs/types_design.md`, *Row format*). A set bit for a column the row does not hold, or a variable column whose two `var_end` words are out of order or past the row, is `BC_E_CORRUPT`: it reads the column through the one decoder every reader uses, so it never disagrees with them. A variable column's bytes are viewed, not copied. The compiler emits it in one place: an `UPDATE` that assigns from a nullable column (`SET c = src`, `SET c = src - $n`), before it reads `src`. |
| `bc_col_i64`, `bc_col_f64`, `bc_col_bool` | Loads the 8-byte column. A null column, or one a shorter row predates, reads as zero; `bc_col_null` decides first and reports both as null. A column marked present that the row does not hold is `BC_E_CORRUPT`. Little-endian in the row. |
| `bc_col_bytes` | The variable-length column, as a view with the row's lifetime. A column marked present whose slot the row does not hold, or whose `var_end` runs past the row, is `BC_E_CORRUPT`. |
| `bc_col_key` | The column's value, NULL included, encoded as an ascending nullable key part of its type: equal values give equal bytes, a DOUBLE's `-0.0` and `0.0` and every NaN alike. A group key for `bc_agg_group` (front end slice 5), so it has no key ceiling. It is the innermost step's, whichever row it was read from, so a scan holds one entry's at a time, and an outer row's key read in an inner loop goes when the inner loop steps. |
| `bc_row_key` | The table's full primary key (namespace, table id, parts), encoded from the row's columns: for a `bc_get`, a guard, or a scan bound reached from a row. Built in the arena, the step's. |
| `bc_row_begin` | Starts a row of `table`'s current layout with every column at its default: the catalog's default for a column that has one, null for the rest. A plan compiled before a column was added writes the column's default without naming it. |
| `bc_row_from` | Starts a row of `table` as a copy of `old`, a row of that table and of its current layout generation (otherwise as for the writes below: `BC_L_ROW`, `BC_L_RETIRED` or `BC_E_CORRUPT`; a `REBUILD`, #503, widens this to any generation). The `UPDATE` pattern: copy, set the assigned columns, end. |
| `bc_row_set_*`, `bc_row_set_null` | Sets column `col`, which must be of the builder's table. Setting twice keeps the last value. A value outside the column's declared type is `BC_E_CONSTRAINT` with `BC_C_RANGE` and the table's id, and sets nothing: a SMALLINT or INTEGER out of range, a REAL that overflows or underflows float, a TEXT or VARCHAR that is not UTF-8 or holds a NUL, a VARCHAR(n) of more than n code points, a DECIMAL(p, s) of more than p digits, a DATE, TIMESTAMP or TIME outside PostgreSQL's range for it (a TIME from 00:00 to 24:00). A NULL for a NOT NULL column is set, and refused by `bc_row_end`. |
| `bc_row_set_param` | Sets column `col` to parameter `i`: its value, or null when it is NULL. One call where a nullable column took an `if` and two sets. A parameter whose type is not the column's is `BC_E_HANDLE`; one outside the column's declared type is `BC_C_RANGE`, as above. |
| `bc_row_end` | Finalizes the row and returns its view, a built row of the builder's table. The row is held with its builder, the step's, and charged to the memory budget; a write copies it into the write set, and it is released with its step. A NOT NULL column that is null, set so or never set and with no default, is `BC_E_CONSTRAINT` with `BC_C_NOT_NULL` and the table's id: checked here, after every value set passed its range check, so a row holding both is `BC_C_RANGE`, as PostgreSQL reports it. A row over the engine's value limit is `BC_E_LIMIT`. |
| `bc_pin` | Copies a view, charged, so it outlives its step and its cursor and lives until reset, keeping what it is: a pinned row is still a row of its table. The compiler emits it where a plan keeps a step's bytes across the next. |

### Ephemeral structures

All three copy their inputs and charge them to the statement's memory
budget: past it, `BC_E_LIMIT` (SQLSTATE 54000), never a spill. Their
contents outlive the steps that added them and live until reset. Iterating
one is a loop, whose steps own what they allocate (*Memory*): a view
`bc_sorter_next` or `bc_hash_next` returns lives until that loop's next
step, and one `bc_sorter_nth` returns lives until the next
`bc_sorter_nth` on that sorter, or until the step it was made in ends.
Iteration order is deterministic: insertion order for hash tables,
first-appearance order for aggregate groups, sort order for sorters.

```c
bc_sorter bc_sorter_open(bc_ctx *, int64_t keep);       /* keep the first `keep` in sort order; 0 = all */
void      bc_sorter_add(bc_ctx *, bc_sorter, bc_view sort_key, bc_view payload);
int64_t   bc_sorter_count(bc_ctx *, bc_sorter);
bc_view   bc_sorter_nth(bc_ctx *, bc_sorter, int64_t n);
bool      bc_sorter_next(bc_ctx *, bc_sorter, bc_view *payload);

bc_hash      bc_hash_open(bc_ctx *);
void         bc_hash_put(bc_ctx *, bc_hash, bc_view key, bc_view payload);
bc_hash_iter bc_hash_find(bc_ctx *, bc_hash, bc_view key);
bool         bc_hash_next(bc_ctx *, bc_hash_iter, bc_view *payload);

bc_agg    bc_agg_open(bc_ctx *, uint32_t slots);
bc_group  bc_agg_group(bc_ctx *, bc_agg, bc_view group_key);
void      bc_agg_add_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);   /* overflow → BC_E_ARITH */
void      bc_agg_add_f64(bc_ctx *, bc_group, uint32_t slot, double);
void      bc_agg_min_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_max_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_count(bc_ctx *, bc_group, uint32_t slot);
bool      bc_agg_next(bc_ctx *, bc_agg, bc_view *group_key, bc_group *g);
int64_t   bc_agg_i64(bc_ctx *, bc_group, uint32_t slot);
double    bc_agg_f64(bc_ctx *, bc_group, uint32_t slot);
bool      bc_agg_null(bc_ctx *, bc_group, uint32_t slot);
```

| Function | What it does |
|---|---|
| `bc_sorter_open` | A sorter for `ORDER BY` the plan cannot get from an index. With `keep` n above 0 it holds at most the first n entries in sort order: `ORDER BY … LIMIT n` (with `OFFSET m`, `keep` is n + m), held in n entries whatever the input. An entry that sorts after the last of n held is neither copied nor charged; one before it takes the last one's place. `keep` 0 holds every entry, so `LIMIT 0` is the plan's to answer without a sorter; a negative `keep` is `BC_E_HANDLE`. |
| `bc_sorter_add` | Adds a row with its sort key. Entries are ordered by the sort key's bytes, `memcmp` order. Plans build sort keys with `bc_sort_key_*` (*Sort keys*), whose order is PostgreSQL's `ORDER BY` order with each part's direction and NULL placement. Equal keys keep the order they were added in: the sort is stable, and a top-n sorter keeps the earliest of equal keys. Both are copied; a payload that is a row view comes back from the sorter a row of its table, as `bc_pin` keeps it one. |
| `bc_sorter_count` | The entries held: those added, or at most `keep`. |
| `bc_sorter_nth` | The payload of the `n`-th entry in sort order, zero-based; out of range is `BC_E_HANDLE` and an empty view. The view lives until the next `bc_sorter_nth` on the same sorter, which takes its slot, so a loop over `nth` holds one view. The first `nth` or `next` sorts; adding after that is `BC_E_HANDLE`. |
| `bc_sorter_next` | Streams payloads in sort order, once: a loop, each step its own. Each call charges one unit of budget. A plan runs it to its end; one it leaves early keeps its step open until reset, as a scan left without `bc_scan_close` does. |
| `bc_hash_open`, `bc_hash_put` | A multimap for the build side of a hash join. Keys are compared as bytes. Duplicate keys are kept: a key holds every payload put under it. |
| `bc_hash_find`, `bc_hash_next` | Probes: `find` makes a probe, a handle of the step it is made in, charged `kHashIterEntryBytes`; `next` iterates every payload stored under `key`, in the order put, a loop whose steps own what they allocate. A key never put finds none. Each `next` charges one unit. |
| `bc_agg_open` | Grouped accumulators with `slots` numeric slots per group (0 for `COUNT(DISTINCT)`, which needs only the groups), for `GROUP BY` the plan cannot get from index order. Ungrouped aggregates use one group with an empty key. Opening one charges `kAggBytes` and a byte per slot, as `bc_sorter_open` charges `kSorterBytes` and `bc_hash_open` `kHashBytes`: past the budget, the open is `BC_E_LIMIT`. Each group's key is held once, and a new group charges its key's bytes, `kGroupEntryBytes` and `kAggSlotBytes` per slot. |
| `bc_agg_group` | Finds or creates the group for `group_key`, a mem-comparable byte string. Slots start at zero, with no value folded in. |
| `bc_agg_add_*`, `bc_agg_min_*`, `bc_agg_max_*`, `bc_agg_count` | Folds a value into a slot: `SUM`, `MIN`, `MAX`, `COUNT`; `AVG` is a sum slot and a count slot divided at emit. A slot's first fold fixes its kind, the same for every group; a fold of another kind, or a slot past `slots`, is `BC_E_HANDLE`. An integer sum, or a count, that overflows is `BC_E_ARITH` ("bigint out of range"); a double sum that overflows from finite values is `BC_E_ARITH` ("value out of range: overflow"), as PostgreSQL's `float8` sum, and an infinite or NaN input carries through. |
| `bc_agg_next` | Iterates groups in first-appearance order, one unit each. |
| `bc_agg_i64`, `bc_agg_f64` | Reads a slot: `bc_agg_f64` a double sum, `bc_agg_i64` any other. A slot read as the other type is `BC_E_HANDLE`; one never folded reads zero either way. |
| `bc_agg_null` | True when no value was folded into the slot in this group, of any kind; a slot past `slots` is `BC_E_HANDLE`. |

**PostgreSQL's NULLs are the plan's.** The slots hold numbers, never
NULL, so a plan keeps PostgreSQL's semantics this way:
- A NULL input is skipped: the plan tests `bc_col_null` and folds nothing
  for it, as `SUM`, `MIN`, `MAX` and `COUNT(col)` ignore NULLs.
  `COUNT(*)` counts every row.
- `SUM`, `MIN`, `MAX` and `AVG` of a group no value reached are NULL. The
  plan emits NULL where `bc_agg_null` is true for the slot; no count slot
  is needed for it. `AVG` still divides its sum slot by a count slot.
  `COUNT` of such a group is 0.
- An aggregate without `GROUP BY` returns one row over no input: the plan
  makes its group, with the empty key, before the input loop, so it exists
  when none arrives. With `GROUP BY`, no input is no groups and no rows.
- `SUM` of a `BIGINT` is a `numeric` in PostgreSQL and never overflows;
  here an integer sum past 64 bits is `BC_E_ARITH`, SQLSTATE 22003. `SUM`
  of a `SMALLINT` or `INTEGER` is a `bigint` in both, and overflows alike.

Ordered aggregation, where the scan's order groups the keys, needs none of
this: the compiler keeps accumulators in locals and emits when the group key
changes.

### Writes

A plan writes rows; the runtime writes what follows from them. Each call
appends to the write plan, and nothing reaches the engine until
`bc_ctx_commit`. The index entries are those of every index the table has
in the pinned catalog version, so an index created after the plan was
compiled is maintained by it, and `bc_ctx_commit`'s guard on the table's
catalog record makes a commit that raced a `CREATE INDEX` a conflict.

```c
void    bc_insert(bc_ctx *, uint32_t table, bc_view row);
void    bc_update(bc_ctx *, uint32_t table, bc_view old, bc_view row);
void    bc_delete(bc_ctx *, uint32_t table, bc_view old);
void    bc_del_range(bc_ctx *, uint32_t table, bc_view lo, bc_view hi);
```

| Function | What it does | Reads | Engine operations |
|---|---|---|---|
| `bc_insert` | Inserts `row`, a row of `table`'s current layout. Checks on the snapshot that its primary key is free and that, for each unique index, no entry has the row's index parts (skipped when one of them is null: nulls are distinct); a duplicate is `BC_E_CONSTRAINT` with `BC_C_UNIQUE` and the table's or the index's id. Then writes the row and one entry per index, and a range guard over each checked unique prefix for the race. | 1, plus 1 per unique index | `put` per row and entry, `ensure_range_unchanged` per unique index |
| `bc_update` | Replaces `old`, a stored row of `table`, with `row`. Writes the row; for each index, compares the entry `old` has with the one `row` has and, only where they differ, deletes the old and writes the new. An index none of whose columns changed encoding is not encoded at all, nor is the new primary key when its columns did not change, so an update of unindexed columns builds no index key. A unique index is checked, as for `bc_insert`, only when its prefix changed, so a row never collides with itself. A changed primary key moves the row: the old key deleted, the new one checked and written, and every entry moved, since each carries the primary key. | 1 per unique index whose prefix changed, plus 1 if the primary key changed | `put`, `del` |
| `bc_delete` | Deletes `old`, a stored row of `table`, and its entry in every index. | 0 | `del` per row and entry |
| `bc_del_range` | Deletes the rows of `table` whose primary keys are in `[lo, hi)`, row keys or prefixes of `table` (empty: its start or its end), with their index entries. `lo >= hi` deletes nothing. A table with no secondary index takes one range delete whatever the range holds; otherwise the runtime scans the range to delete each row's entries. | 0 without secondary indexes; else 1 per row in the range, plus 1 for the record that ends it, each charged before it is read | `del_range`, then `del` per entry |

`old` must be a stored row, read in this execution by `bc_get` or
`bc_scan_row`: the entries removed are derived from it, so a row the plan
built would remove entries the snapshot does not hold and leave its real
ones behind. Anything else is `BC_E_HANDLE`, and so is a second write of a
row key this plan already wrote, for the same reason. A `bc_del_range` is a
write of every row in its range: it is refused over a row the plan already
wrote or a range it already deleted, and a later write of a row in its range
is refused. A row of another table is `BC_E_LAYOUT` with `BC_L_ROW`. A row
not of its table's current layout generation is `BC_E_LAYOUT` with
`BC_L_RETIRED` when `REBUILD` (#503) retired that generation, and
`BC_E_CORRUPT` when the table never had it: that row is damage, not a
stale plan. Neither is cured by a recompile, which only `BC_L_INCOMPATIBLE`
asks for.

Every key a write touches is in the write plan, and the engine checks each
one for a change since the snapshot, so a concurrent writer of the same
row, entry or range makes the commit a conflict without a guard. That
includes a concurrent insert of the same primary key: `bc_insert` writes no
`ensure_absent` for it, since a key absent in the snapshot and present at
commit is a change (test: "plan writes: a concurrent insert of the same key
conflicts with no absent guard"). The unique indexes need their range
guards, because a concurrent duplicate is another row's entry, a key this
plan never writes.

Raw `bc_put` and `bc_del` are gone from the language. The plan's keyspace
holds rows and index entries, and both are written only through the calls
above; the catalog's namespace is written only by DDL. A namespace added
later gets calls of its own, decided by the same rule.

### Guards and raised errors

Each appends to the write plan; a failed guard is reported by the commit,
not by the call.

```c
void bc_guard_unchanged(bc_ctx *, bc_view key);
void bc_guard_row(bc_ctx *, uint32_t table, bc_view row);
void bc_guard_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi);
void bc_guard_present(bc_ctx *, bc_view key);
void bc_guard_absent(bc_ctx *, bc_view key);
void bc_raise(bc_ctx *, uint32_t constraint, uint32_t object_id);   /* a user error: unwinds, never retried */
```

| Function | What it does | Engine operation |
|---|---|---|
| `bc_guard_unchanged` | The commit fails if `key` changed since the snapshot. Serializable plans emit one for every key they read and did not write. Keys the plan writes are checked by the engine without a guard. | `ensure_unchanged` |
| `bc_guard_row` | `bc_guard_unchanged` on the primary key of `row`, a row of `table`, encoded into the guard without the arena: what `SELECT … FOR UPDATE` emits for each row it returns, however many. | `ensure_unchanged` |
| `bc_guard_range` | The commit fails if any key in `[lo, hi)` changed. Serializable plans emit one per range scanned; it covers phantoms. Empty bounds are the start and end of `table_or_index`, as for `bc_scan_open`; `lo >= hi` guards nothing. | `ensure_range_unchanged` |
| `bc_guard_present`, `bc_guard_absent` | Existence preconditions on a key the plan reads and does not write. | `ensure_present`, `ensure_absent` |
| `bc_raise` | Sets `BC_E_CONSTRAINT` with a `bc_constraint` kind and the id of the constraint, column or index concerned, as the compiler emitted them; an unknown kind or an id of 2^24 or more is `BC_E_HANDLE`. The plan unwinds; the runtime reports it and does not retry. A `CHECK` the compiler evaluates is raised this way. | none: nothing reaches the engine |

A plan under snapshot isolation emits no guards. The two plans for the same
SQL differ only in guard calls, which is how isolation reads in the text.
The write plan is bounded by the engine's 1 GiB plan limit, counted as it
grows (*Memory is the statement's*); exceeding it is `BC_E_LIMIT`.

## The language

A plan file is a program in a subset of C. The rules below are the
language's definition, and the compiler generates only plans inside it; the
C compiler is a backend for those plans. The checker this design proposed,
a type checker that would refuse other text before any tier compiled it, is
not planned: only the compiler's plans are supported.

**Structure.** Three `//` comment lines: the SQL text, the isolation level
and schema version the plan was compiled against, and the objects its
constants stand for, every byte of user text outside printable ASCII (and
`\` and `?`) written as `\xNN` so none can end or splice a line; `#include "bc_plan.h"` and nothing else; one `enum` of
catalog constants; one function `void plan(bc_ctx *cx)`. A plan file is its
own compilation unit, and `plan` is the only symbol it exports; a loader
compiles one file per plan, and a build that links several plans into one
binary renames the symbol per file (`-Dplan=plan_<name>`), as the
tests' clang-linked plans do. No other
declarations: no globals, no `static`, no other functions, no macros, no
`typedef`.

**Types.** Locals are `int64_t`, `double`, `bool`, `bc_view` or one of the
handle types. There is one integer type; narrow columns are widened by their
decoders. No pointers, arrays, structs of the plan's own, unions, enums
other than the constants block, or `uint32_t` outside the constants.

**Declarations.** Every local is initialized where declared. Each is
assigned only by a plain `=` statement, never inside an expression.

**Expressions.** Comparisons on integers and doubles; `&& || !` on booleans;
`+ - * /` on doubles; calls to header functions with arguments of the
declared types; integer and `bool` literals; constants from the block. No
raw integer `+ - * / % ~ << >>`, no unary minus on integers, no casts, no
`?:`, no comma operator, no `++ --`, no compound assignment, no string or
character literals, no `sizeof`.

**Statements.** `if` / `else`, `while`, `break`, `continue`, `return`, and
expression statements that are calls. A `while` condition is exactly a call
to `bc_scan_next`, `bc_sorter_next`, `bc_hash_next` or `bc_agg_next`,
optionally conjoined with a boolean local. No `for`, `do`, `switch`,
`goto`, labels, or blocks that are not the body of one of these.

**Why each rule.** No pointers and no casts: every memory access is a
checked API call. No raw integer arithmetic: overflow and division by zero
are undefined in C and defined by the helpers. Loops only on iteration
calls, no counted loops, no recursion: termination follows from finite
cursors and bounded structures, and the budget bounds the total. Initialized
locals, no nested assignment, no `++`: no uninitialized read and no
unsequenced modification. Handles in distinct struct types: the C compiler
itself rejects a handle of the wrong kind.

**Tiers.** Every tier runs plans produced by the SQL compiler; the C
compiler is trusted as the engine's own compiler is. Nothing runs
user-supplied plans; hand-written ones exist only in the plan API's tests.
An interpreter for the compiler's plans, with a fuel counter on loop
back-edges, stays possible for a target the C compiler lacks (WASM), and is
not built.

### Example

Payment's customer lookup by last name, serializable:

```c
// SELECT c_id, c_first, c_balance FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3 ORDER BY c_first LIMIT 1 OFFSET (count / 2)
// isolation: serializable   schema: catalog v7
// objects: TBL_7 customer, IDX_12 customer_i2, COL_7_0 customer.c_id, COL_7_3 customer.c_first, COL_7_16 customer.c_balance
#include "bc_plan.h"
enum { TBL_7 = 7, IDX_12 = 12, COL_7_0 = 0, COL_7_3 = 3, COL_7_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  bc_view prefix = bc_key_params(cx, IDX_12, 0, 3);

  bc_sorter s = bc_sorter_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, IDX_12, prefix, prefix, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {                                   /* 1 read per step */
    bc_view row = bc_scan_row(cx, c);                             /* 1 read */
    bc_sorter_add(cx, s, bc_col_bytes(cx, row, COL_7_3), row);
  }
  bc_guard_range(cx, IDX_12, prefix, bc_key_successor(cx, prefix));  /* serializable */

  int64_t n = bc_sorter_count(cx, s);
  if (n == 0) {
    return;
  }
  bc_view mid = bc_sorter_nth(cx, s, bc_div(cx, n, 2));
  bc_emit_begin(cx);
  bc_emit_col(cx, mid, COL_7_0);
  bc_emit_col(cx, mid, COL_7_3);
  bc_emit_col(cx, mid, COL_7_16);
  bc_emit_end(cx);
}
```

A reader sees the index, the direction, one record read per entry plus one
per row, the sort and its bound, the guard, and the result. The cost model
is on the page, and nothing on it can fault.

## Keys and rows

Every key is `namespace(1) table_or_index_id(4, big-endian) parts…`, with
parts encoded so that `memcmp` order is the typed order and a key parses back
into its parts without a length prefix. A secondary index key is the index
columns followed by the primary key's parts, which `bc_scan_row` turns into
the row's key. Namespaces: `0x01` catalog, `0x02` table rows, `0x03` index
entries, as in the MariaDB plugin. Rows carry a header (layout generation,
bitmap size, the variable region's table, length), a null bitmap indexed by
permanent column id, fixed columns and variable data, and a row shorter than
its layout reads the missing columns as null.

Both formats are specified in full, with their reasons, in
`types_design.md` (*Key encoding*, *Row format*).

## Source layout

`bytecaskdb-sql-layer/algebra/` holds `bc_plan.h` and its C++
implementation; `codec/` the key and row codecs; `catalog/` the catalog
(`catalog_design.md`); `plans/` the hand-written plans the tests link with
clang, each with its SQL in the header comment; `sql/` the parser transformer and binder; `plan/` the
planner, code generator and loader; `api/` the embedded API; `tests/` the property tests, fault tests, the harness and the
TPROC-C schema fixture (`tests/tprocc_schema.cppm`). Files
marked `// prototype:` are disposable and are replaced in place. The C++ is
organized as C++23 modules (`bytecask.sql.*`), as the engine is; `bc_plan.h`
is a header because C reads it. The only other headers would be ones
another toolchain consumes: a C ABI for the bindings, and a header bridge
for the plugin, the counterpart of `include/bytecask.hpp`. The catalog, transaction and codec directories are the half the
MariaDB plugin links: the plugin depends on the sql-layer, never the
reverse, and nothing under `bytecaskdb-sql-layer/` includes a MariaDB header.
The plugin keeps its own key and row codecs, and would adopt the shared
ones through its per-table format versions (`types_design.md`, *What the
plugin adopts*; #488).

## Deferred

- **Several schema versions served at once.** The row format and the
  permanent column ids are its storage half; the rest is the catalog's
  (`catalog_design.md`, *Several schema versions*).
- **Sequence allocation.** Auto-increment is a runtime service invoked at
  commit, outside the plan, so the plan stays pure. Until then ids come from
  rows the plan reads and guards.
- **Streaming results.** The result buffer is charged to the statement's
  memory budget, so a result set is bounded by it. A statement that returns
  more is `BC_E_LIMIT`. Streaming rows to the client as they are emitted is
  part of the transport design (#487, *Next*), behind the same `bc_emit_*`
  calls.
- **Spilling sorters and hash tables.** Bounds without spill are right for
  OLTP (`planner_design.md`, *Memory*). An analytic workload would add a
  spilling variant behind the same calls.
