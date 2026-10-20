# Plan API: a physical algebra for generated plans

Status: design, revision 5 (#487, stage 2.1). Nothing in the engine
changes; this document specifies the plan API of `bytecaskdb-sql-layer`, a
top-level component beside the MariaDB plugin, built on the public engine
API and MIT-licensed like the core. `bytecaskdb-sql-layer/algebra/`
implements it, except the ephemeral structures; the revisions that the
prototype (#497, merged as #498) and the layers after it forced are listed
under *Revision history*.

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

The subset compiles with any C compiler, so the first execution tier is
clang linking plans into the test binary. Later tiers load the same text at
run time: an embedded C compiler, or an interpreter for hand-written and
untrusted plans. The API and the guarantees are the same in every tier.

What it is not: a general C API for the engine, a wire protocol, or a place
for SQL semantics. Collations, decimals and dates are the compiler's problem;
here every value is a 64-bit integer, a double, a boolean or bytes.

## The guarantee

A plan that passes the checker (*The language*, below) has these
properties in every tier, whatever its logic does:

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
- **Bounded in memory.** The arena and every ephemeral structure have
  declared limits. Exceeding one is an error, never a spill and never a
  larger allocation.
- **Effect-free until commit.** The only outputs are a write plan and a
  result buffer, both discarded on any error and committed only by the
  runtime after the plan returns.
- **Index-consistent.** A plan writes rows, never index entries. The
  runtime writes every entry of every index the table has when the plan
  runs, so a committed write leaves each index holding exactly one entry per
  row, whatever the plan does.

A wrong plan can therefore do two things: return an error code, or return
or write a wrong answer. The second is what the oracle tests in *Validation* exist to
catch. Neither crashes the server, corrupts the engine, or holds a snapshot
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
whose owner has moved on. A stale or foreign handle sets `BC_E_HANDLE`.

**Views are handles with a lifetime the API tracks.** A view names bytes
the API owns: a cursor's current record, a parameter, a value returned by a
read, or something built in the arena. A cursor's view is invalidated when
that cursor steps or closes; every other view lives until the context is
reset. Reading through an invalidated view is an error, not a read.
`bc_pin` copies a view into the arena to extend it. The zero handle is the
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

**The arena is the only memory.** Keys, rows, result rows, sorter and hash
contents live in a bump arena owned by the context and reset after each
execution. There is no heap allocation on the plan's path and no freeing.
The arena has a size, blocking structures declare a byte bound at open, and
exceeding either sets `BC_E_MEMORY`.

**The API meters the work.** `bc_ctx_begin` sets a budget in record reads
and cursor steps and a deadline. Every read and every step charges the
budget, and the API checks the deadline periodically. Exhausting either sets
`BC_E_BUDGET` and the plan unwinds. Compute between API calls is
straight-line and finite, so the budget bounds the whole execution without
help from the compiler. The interpreter adds a fuel counter on backward
branches, which compiled plans do not need.

**One context, one thread.** A context is used by one thread at a time.
Contexts run concurrently on different threads; reads scale as the engine's
reads scale, and commits go through the engine's group commit. A plan never
blocks: reads are lock-free and the commit happens after it returns.

**Within a plan, reads see the snapshot, not the plan's own writes.** This
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
entries. Writing a row in several statements of one transaction needs
#491's own-writes overlay; until then the second write is refused within
a context. The affected-row count is each run's own. A unique
secondary key two of one statement's inserts share is not caught yet: a
stage-2 limit, unreachable from the stage-1 compiler, which emits one write
per plan (#507).
Read-your-own-writes across statements in an interactive transaction is an
overlay the cursors read through and belongs to the session layer. See
*Deferred*.

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

/* Handles: an index and a generation, each in its own struct type. */
typedef struct { uint32_t i, g; } bc_view;          /* bytes the API owns */
typedef struct { uint32_t i, g; } bc_cursor;
typedef struct { uint32_t i, g; } bc_sorter;
typedef struct { uint32_t i, g; } bc_hash;
typedef struct { uint32_t i, g; } bc_hash_iter;
typedef struct { uint32_t i, g; } bc_agg;
typedef struct { uint32_t i, g; } bc_group;
typedef struct { uint32_t i, g; } bc_key;           /* a key under construction */
typedef struct { uint32_t i, g; } bc_row;           /* a row under construction */

typedef void (*bc_plan_fn)(bc_ctx *);               /* every plan has this signature */

enum bc_error_code {
  BC_OK = 0,
  BC_E_IO,          /* std::system_error from the engine */
  BC_E_CORRUPT,     /* CRC mismatch or unparseable record */
  BC_E_MEMORY,      /* arena exhausted or an ephemeral bound exceeded */
  BC_E_BUDGET,      /* work budget or deadline exhausted */
  BC_E_LIMIT,       /* key or value over the engine's limits */
  BC_E_HANDLE,      /* stale, foreign or out-of-range handle, or a table, index or column the plan's
                       bc_schema_ref does not declare, or not of the type used: a plan bug */
  BC_E_ARITH,       /* integer overflow, division by zero, out-of-range conversion */
  BC_E_CONSTRAINT,  /* never retried; raised by the runtime (BC_C_UNIQUE from bc_insert and bc_update,
                       BC_C_RANGE and BC_C_NOT_NULL from the row builder) or by the plan with bc_raise */
  BC_E_LAYOUT,      /* plan incompatible with the live catalog, or a row violates its layout */
  BC_E_DEGRADED,    /* engine degraded; writes refused */
  BC_E_CLOSED,
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
  uint64_t max_reads;       /* record reads and cursor steps, together */
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
bc_ctx *bc_ctx_new(bc_db *db, const bc_catalog *cat, uint32_t arena_bytes);
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
| `bc_ctx_new` | Allocates a context bound to an open engine and a catalog, with an arena of `arena_bytes`. The arena is allocated once and reused. |
| `bc_ctx_begin` | Pins a catalog version and takes a snapshot that agrees with it (no DDL committed between the two), binds the parameters, sets the budget. Clears the error. Must precede `bc_ctx_run`. |
| `bc_ctx_check` | Verifies a `bc_schema_ref` against the pinned version as `bc_ctx_run` does, and runs nothing. False with `BC_E_LAYOUT` where `bc_ctx_run` would refuse the plan. What a server calls to learn whether a cached plan still fits the catalog before it checks parameters against that plan. |
| `bc_ctx_run` | Verifies the plan's `bc_schema_ref` against the catalog version the context pinned at `bc_ctx_begin`, then calls the plan and returns when it returns. Compatible means: every table and index it names exists; every column it reads or sets exists in its table with the type it was compiled with; every table it writes exists. Verifying also maps each declared column to its offset or slot in the current layout. A null reference is `BC_E_HANDLE`. A plan that fails sets `BC_E_LAYOUT` and does not run; the server recompiles it from the SQL it cached. Adding a column or an index never invalidates a plan, since the runtime maintains whatever indexes the table has; a drop invalidates the plans that name what it drops. There is no version-equality check. |
| `bc_ctx_commit` | Submits the write plan with `apply_batch`, guarded by `ensure_unchanged` on the catalog record of every table whose keys it writes, so a DDL on such a table committed since the snapshot makes it a conflict: the index entries the runtime wrote follow the pinned version's indexes, and a `CREATE INDEX` committed after the snapshot would otherwise miss the row. `BC_COMMITTED` on success. `BC_CONFLICT` when the engine reports a guard or write-write conflict: nothing was written. `BC_FAILED` when `bc_error` is set, including an error raised during the plan: nothing is committed. A plan with no writes and no guards commits nothing and returns `BC_COMMITTED`. |
| `bc_ctx_next_result` | Iterates the result buffer. Rows are in the row format of the layout the plan emitted. The runtime, not a plan, sees raw pointers here; valid until `bc_ctx_reset`. |
| `bc_ctx_reset` | Releases the snapshot, drops the write plan and result buffer, resets the arena, bumps every generation so every handle and view the plan held is invalid. |
| `bc_error_detail`, `bc_ctx_affected` | The constraint kind and object id of a raised error, or for `BC_E_LAYOUT` its class: `bc_ctx_run` refusing the reference (`BC_L_INCOMPATIBLE`, the only one a recompile cures), a row used with a table or column it is not of (`BC_L_ROW`), a malformed plan or reference (`BC_L_PLAN`), or a row of a retired layout generation (`BC_L_RETIRED`). The runtime sets every `BC_E_LAYOUT` with its detail; one without cannot be compiled; the affected-row count the last run recorded. Both read by the runtime after the plan returns. |
| `bc_execute` | `begin`, `run`, `commit`; on `BC_CONFLICT`, `reset` and repeat up to `max_retries`. A `BC_E_CONSTRAINT` raised by the plan is final: no retry. Returns the commit result of the last attempt. This is the only place retry lives. |

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
| `bc_param_*` | Reads parameter `i` as bound by `bc_ctx_begin`. A wrong index or type is `BC_E_HANDLE` and a neutral value. Parameter views live until reset. |
| `bc_emit_begin` … `bc_emit_end` | Appends one result row, in the row format, with the types the plan's `bc_schema_ref` declares in `result_types`. Columns are emitted in that order; a count or type that does not match is `BC_E_LAYOUT`. The row is copied into the result buffer, which is part of the arena. |
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
compiler emits comparisons on sort keys it built, never on raw text.

### Keys

Keys are built in the arena in the mem-comparable encoding of *Key encoding*
below. A builder is finalized once; its view lives until reset.

```c
bc_key   bc_key_begin(bc_ctx *, uint32_t table_or_index);
void     bc_key_i64(bc_ctx *, bc_key, int64_t);
void     bc_key_f64(bc_ctx *, bc_key, double);
void     bc_key_bool(bc_ctx *, bc_key, bool);
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_param(bc_ctx *, bc_key, uint32_t i);  /* parameter i as the next part, NULL included */
bc_view  bc_key_end(bc_ctx *, bc_key);
bc_view  bc_key_params(bc_ctx *, uint32_t table_or_index, uint32_t first, uint32_t n);  /* parts from params first.. */
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */
```

| Function | What it does |
|---|---|
| `bc_key_begin` | Starts a key for a table (its primary key) or an index. Writes the namespace byte and the 4-byte id. Returns a builder handle. |
| `bc_key_i64`, `bc_key_f64`, `bc_key_bool`, `bc_key_bytes`, `bc_key_null` | Appends one part. Nullability and direction come from the catalog entry for `table_or_index`, so the builder knows whether to write a null marker and whether to invert the bytes. A part of the wrong type, or more parts than the key has, is `BC_E_HANDLE`. |
| `bc_key_param` | Appends parameter `i` as the next part: its value, or the null part when it is NULL. A type that is not the part's, or a NULL for a part that is not nullable, is `BC_E_HANDLE`. A value outside the type of the part's column, without its modifier, is `BC_C_RANGE` with the table's id, as for `bc_key_i64`, `bc_key_f64` and `bc_key_bytes` and each part of `bc_key_params`: a lookup or bound is refused, never wrapped or rounded to a neighbour. A `REAL` part is keyed as its float rounding. |
| `bc_key_end` | Finalizes and returns the key. A partial key (fewer parts than the index has) is legal and is a prefix the compiler uses as a scan bound. A key over the engine's hard ceiling (65,535 bytes) is `BC_E_LIMIT`. |
| `bc_key_params` | The key, or prefix, of `table_or_index` whose first `n` parts are parameters `first` to `first + n - 1`, each as `bc_key_param` appends it: `bc_key_begin`, `n` parts and `bc_key_end` in one call. `(cx, TBL_9, 0, 3)` is a composite primary key `(w_id, d_id, c_id)`. The builder remains for keys whose parts come from anywhere else. |
| `bc_key_successor` | The exclusive upper bound of the prefix `key`: its last byte incremented, carrying. Turns a prefix into a half-open range where `BC_SCAN_PREFIX` does not fit. |

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
| `bc_get` | Reads the record at `key`. On a row key the view is a stored row of that table. | The value; `*found` false and an empty view when absent. Lives until reset. | 1 record read |
| `bc_exists` | Whether `key` is present. | `bool`. | 1 record read (the blind key directory confirms a key from its record) |
| `bc_scan_open` | Opens a cursor over `[lo, hi)` on a table (primary key order) or an index, direction per `flags`. An empty `lo` (`bc_empty`) means the start of the table or index, an empty `hi` the end. With `BC_SCAN_PREFIX`, `hi` is ignored and the range is every key having `lo` as a prefix. Positioned before the first entry. | A cursor handle. Closed by `bc_ctx_reset`; `bc_scan_close` releases one early. | 0 |
| `bc_scan_next` | Advances in the scan's direction, charging one unit of budget. Invalidates the cursor's views. | `true` while an entry is available; `false` at the end, on error, or when the budget is spent. | 1 record read (the key directory holds no key bytes, so each step reads the entry's record; on a table scan the same read yields the value) |
| `bc_scan_key` | The current entry's full encoded key. | View valid until the cursor steps or closes. | 0 |
| `bc_scan_row` | The current entry's row, a stored row of the cursor's table. On a table scan it is the entry's value. On a secondary index the runtime builds the row key from the entry's primary-key suffix and reads it, once per entry however often it is asked: the fetch of a non-covering read. An index entry whose row is missing is `BC_E_CORRUPT`, never skipped: the runtime keeps every entry paired with its row, so a missing one is damage. | View valid until the cursor steps or closes. | 0 on a table, 1 record read on an index |
| `bc_empty` | The empty view. | The zero handle, valid always. | 0 |
| `bc_key_part_*` | Decodes one part of an encoded key, by position in the key's catalog layout. On a secondary index the index columns come first, then the primary key's parts. `bc_key_part_null` reports a null part; the typed decoders return zero or empty for one. A part number or type that disagrees with the layout is `BC_E_HANDLE`, and so is `bc_key_part_f64` on any part: the key canonicalises a `DOUBLE` (`-0.0` is `0.0`, every NaN one NaN), so it is read from the row. A key that does not parse is `BC_E_CORRUPT`. | Values; a bytes part is unescaped into the arena and lives until reset. | 0 |
| `bc_count_range` | Counts the live entries in `[lo, hi)` without walking them, stopping at `limit`. Compiles to `Snapshot::count_keys`. The planner's cardinality source and `COUNT(*)` over a key range. | `min(count, limit)`. | at most 2 record reads, whatever the range holds |

A range scan on a secondary index with a fetch of each row costs one read
per step plus one per `bc_scan_row`. A covering read, where the plan reads
the columns it needs from `bc_scan_key` with `bc_emit_key_part` or
`bc_key_part_*`, costs one. Covering or fetch is the optimizer's decision,
and the text shows which: a plan that never calls `bc_scan_row` reads no
row. A `DOUBLE` column is never read from a key: the encoding writes `-0.0`
as `0.0` and every NaN as one, so only the row holds the exact value.

### Rows and columns

Rows use the format in *Row format*. A plan names a column by its permanent
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
bc_view  bc_pin(bc_ctx *, bc_view);                 /* copy into the arena: lives until reset */
```

| Function | What it does |
|---|---|
| `bc_col_null` | True if column `col`'s bit in the bitmap is clear. The bitmap says *present*: a column set null and a column the row predates both have the bit clear, so one test covers both (`docs/types_design.md`, *Row format*). A set bit for a column the row does not hold, or a variable column whose two `var_end` words are out of order or past the row, is `BC_E_CORRUPT`, as every column reader reports it; the value itself is not copied, so a null test before a read does not decode the column twice. The compiler emits it in one place: an `UPDATE` that assigns from a nullable column (`SET c = src`, `SET c = src - $n`), before it reads `src`. |
| `bc_col_i64`, `bc_col_f64`, `bc_col_bool` | Loads the 8-byte column. A null column, or one a shorter row predates, reads as zero; `bc_col_null` decides first and reports both as null. A column marked present that the row does not hold is `BC_E_CORRUPT`. Little-endian in the row. |
| `bc_col_bytes` | The variable-length column, as a view with the row's lifetime. A column marked present whose slot the row does not hold, or whose `var_end` runs past the row, is `BC_E_CORRUPT`. |
| `bc_row_key` | The table's full primary key (namespace, table id, parts), encoded from the row's columns: for a `bc_get`, a guard, or a scan bound reached from a row. Built in the arena. |
| `bc_row_begin` | Starts a row of `table`'s current layout with every column at its default: the catalog's default for a column that has one, null for the rest. A plan compiled before a column was added writes the column's default without naming it. |
| `bc_row_from` | Starts a row of `table` as a copy of `old`, a row of that table and of its current layout generation (otherwise as for the writes below: `BC_L_ROW`, `BC_L_RETIRED` or `BC_E_CORRUPT`; a `REBUILD`, #503, widens this to any generation). The `UPDATE` pattern: copy, set the assigned columns, end. |
| `bc_row_set_*`, `bc_row_set_null` | Sets column `col`, which must be of the builder's table. Setting twice keeps the last value. A value outside the column's declared type is `BC_E_CONSTRAINT` with `BC_C_RANGE` and the table's id, and sets nothing: a SMALLINT or INTEGER out of range, a REAL that overflows or underflows float, a TEXT or VARCHAR that is not UTF-8 or holds a NUL, a VARCHAR(n) of more than n code points, a DECIMAL(p, s) of more than p digits, a DATE, TIMESTAMP or TIME outside PostgreSQL's range for it (a TIME from 00:00 to 24:00). A NULL for a NOT NULL column is set, and refused by `bc_row_end`. |
| `bc_row_set_param` | Sets column `col` to parameter `i`: its value, or null when it is NULL. One call where a nullable column took an `if` and two sets. A parameter whose type is not the column's is `BC_E_HANDLE`; one outside the column's declared type is `BC_C_RANGE`, as above. |
| `bc_row_end` | Finalizes the row in the arena and returns its view, a built row of the builder's table. A NOT NULL column that is null, set so or never set and with no default, is `BC_E_CONSTRAINT` with `BC_C_NOT_NULL` and the table's id: checked here, after every value set passed its range check, so a row holding both is `BC_C_RANGE`, as PostgreSQL reports it. A row over the engine's value limit is `BC_E_LIMIT`. |
| `bc_pin` | Copies a view into the arena so it outlives its cursor, keeping what it is: a pinned row is still a row of its table. The compiler emits it where a plan keeps a cursor's bytes across a step. |

### Ephemeral structures

All three copy their inputs into the arena and declare a byte bound.
Exceeding it sets `BC_E_MEMORY`. Their contents live until reset. Iteration
order is deterministic: insertion order for hash tables, first-appearance
order for aggregate groups, sort order for sorters.

```c
bc_sorter bc_sorter_open(bc_ctx *, uint32_t bytes);
void      bc_sorter_add(bc_ctx *, bc_sorter, bc_view sort_key, bc_view payload);
int64_t   bc_sorter_count(bc_ctx *, bc_sorter);
bc_view   bc_sorter_nth(bc_ctx *, bc_sorter, int64_t n);
bool      bc_sorter_next(bc_ctx *, bc_sorter, bc_view *payload);

bc_hash      bc_hash_open(bc_ctx *, uint32_t bytes);
void         bc_hash_put(bc_ctx *, bc_hash, bc_view key, bc_view payload);
bc_hash_iter bc_hash_find(bc_ctx *, bc_hash, bc_view key);
bool         bc_hash_next(bc_ctx *, bc_hash_iter, bc_view *payload);

bc_agg    bc_agg_open(bc_ctx *, uint32_t bytes, uint32_t slots);
bc_group  bc_agg_group(bc_ctx *, bc_agg, bc_view group_key);
void      bc_agg_add_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);   /* overflow → BC_E_ARITH */
void      bc_agg_add_f64(bc_ctx *, bc_group, uint32_t slot, double);
void      bc_agg_min_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_max_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_count(bc_ctx *, bc_group, uint32_t slot);
bool      bc_agg_next(bc_ctx *, bc_agg, bc_view *group_key, bc_group *g);
int64_t   bc_agg_i64(bc_ctx *, bc_group, uint32_t slot);
double    bc_agg_f64(bc_ctx *, bc_group, uint32_t slot);
```

| Function | What it does |
|---|---|
| `bc_sorter_open` | A sorter for `ORDER BY` the plan cannot get from an index. |
| `bc_sorter_add` | Adds a row with its sort key, a mem-comparable byte string the plan built with `bc_key_*` (a `DESC` column inverted, nulls first or last per the key's layout). Both are copied; a payload that is a row view comes back from the sorter a row of its table, as `bc_pin` keeps it one. |
| `bc_sorter_count` | Rows added so far. |
| `bc_sorter_nth` | The payload of the `n`-th row in sort order, zero-based; out of range is `BC_E_HANDLE` and an empty view. The first `nth` or `next` sorts; adding after that is `BC_E_HANDLE`. |
| `bc_sorter_next` | Streams payloads in sort order. Each call charges one unit of budget. |
| `bc_hash_open`, `bc_hash_put` | A multimap for the build side of a hash join. Duplicate keys are kept. |
| `bc_hash_find`, `bc_hash_next` | Probes: an iterator over every payload stored under `key`, in insertion order. Each `next` charges one unit. |
| `bc_agg_open` | Grouped accumulators with `slots` numeric slots per group, for `GROUP BY` the plan cannot get from index order. Ungrouped aggregates use one group with an empty key. |
| `bc_agg_group` | Finds or creates the group for `group_key`, a mem-comparable byte string. Slots start at zero and null. |
| `bc_agg_add_*`, `bc_agg_min_*`, `bc_agg_max_*`, `bc_agg_count` | Folds a value into a slot. `SUM`, `MIN`, `MAX`, `COUNT`; `AVG` is a sum slot and a count slot divided at emit. Integer sums are checked. |
| `bc_agg_next` | Iterates groups in first-appearance order, one unit each. |
| `bc_agg_i64`, `bc_agg_f64` | Reads a slot. |

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
void bc_guard_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi);
void bc_guard_present(bc_ctx *, bc_view key);
void bc_guard_absent(bc_ctx *, bc_view key);
void bc_raise(bc_ctx *, uint32_t constraint, uint32_t object_id);   /* a user error: unwinds, never retried */
```

| Function | What it does | Engine operation |
|---|---|---|
| `bc_guard_unchanged` | The commit fails if `key` changed since the snapshot. Serializable plans emit one for every key they read and did not write. Keys the plan writes are checked by the engine without a guard. | `ensure_unchanged` |
| `bc_guard_range` | The commit fails if any key in `[lo, hi)` changed. Serializable plans emit one per range scanned; it covers phantoms. Empty bounds are the start and end of `table_or_index`, as for `bc_scan_open`; `lo >= hi` guards nothing. | `ensure_range_unchanged` |
| `bc_guard_present`, `bc_guard_absent` | Existence preconditions on a key the plan reads and does not write. | `ensure_present`, `ensure_absent` |
| `bc_raise` | Sets `BC_E_CONSTRAINT` with a `bc_constraint` kind and the id of the constraint, column or index concerned, as the compiler emitted them; an unknown kind or an id of 2^24 or more is `BC_E_HANDLE`. The plan unwinds; the runtime reports it and does not retry. A `CHECK` the compiler evaluates is raised this way. | none: nothing reaches the engine |

A plan under snapshot isolation emits no guards. The two plans for the same
SQL differ only in guard calls, which is how isolation reads in the text.
The write plan is bounded by the engine's 1 GiB plan limit; exceeding it is
`BC_E_LIMIT`.

## The language

A plan file is a program in a subset of C defined by a checker that every
tier runs before compiling or interpreting it. The checker is a type checker
for the subset; a file it rejects never reaches a compiler. The rules are
the language's definition, and the C compiler is a backend for programs that
already passed them.

**Structure.** Three `//` comment lines: the SQL text, the isolation level
and schema version the plan was compiled against, and the objects its
constants stand for, every byte of user text outside printable ASCII (and
`\` and `?`) written as `\xNN` so none can end or splice a line; `#include "bc_plan.h"` and nothing else; one `enum` of
catalog constants; one function `void plan(bc_ctx *cx)`. A plan file is its
own compilation unit, and `plan` is the only symbol it exports; a loader
compiles one file per plan, and a build that links several plans into one
binary renames the symbol per file (`-Dplan=plan_<name>`), as the
prototype's tests do. No other
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

**Tiers.** Compiled tiers run checked plans produced by the SQL compiler;
the C compiler is trusted as the engine's own compiler is. The interpreter
runs any checked plan, including hand-written and user-supplied ones, and
adds a fuel counter on loop back-edges so a plan is bounded even if an API
call were to misreport.

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

  bc_sorter s = bc_sorter_open(cx, 65536);
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

## Key encoding

Every key is `namespace(1) table_or_index_id(4, big-endian) parts…`. Parts
are encoded so that `memcmp` order is the typed order, and so that a key can
be parsed back into its parts without a length prefix.

| Part type | Encoding |
|---|---|
| Nullable part | A marker byte first: `0x00` null, `0x01` present. Nulls sort first. Non-nullable parts have no marker. |
| `i64` | 8 bytes big-endian with the sign bit flipped. Narrower integers, decimals and times are `i64` by the time they reach the API. |
| `f64` | IEEE 754 bits, big-endian; positive values have the sign bit flipped, negative values have every bit flipped. `-0.0` is encoded as `0.0` and every NaN as one canonical NaN, above `+inf`. |
| `bool` | One byte, `0x00` or `0x01`. |
| `bytes` | Escaped: each `0x00` becomes `0x00 0xFF`, followed by the terminator `0x00 0x00`. Self-delimiting, and a prefix of a longer string sorts first. |
| `DESC` part | The encoded bytes above, marker included, with every bit inverted. Nulls sort last. |

A secondary index key is the index columns followed by the primary key's
parts. The primary key suffix is what `bc_scan_row` turns into the row's
key. Because every part is self-delimiting, the suffix is
found by skipping the index columns with the index's catalog layout.

Namespaces: `0x01` catalog, `0x02` table rows, `0x03` index entries, as in
the MariaDB plugin. Text columns are compared as bytes in stage 1; under a
collation they are not stored as text in keys: the compiler encodes the
collation's sort key (weights) as `bytes`, as the plugin does, and the row
carries the original value. The encoding is specified in full, with the
reasons, in `docs/types_design.md`.

## Row format

```
u16   layout_generation     ; which offset table decodes this row (see below)
u16   bitmap_bytes          ; size of the null bitmap; a multiple of 8
u16   var_table             ; offset of var_end: where this row's fixed region ends
u16   nvar                  ; entries in var_end
u32   row_bytes             ; length of the row; columns beyond it are null
u8[]  null_bitmap           ; bit set = present, indexed by permanent column id; sized at create
      fixed columns         ; 8 bytes each, at permanent offsets, little-endian
u32[] var_end               ; nvar end offsets, relative to the start of var data
u8[]  var data
```

Fixed columns are `i64`, `f64` and `bool`, one 8-byte word each. A variable
column `k` occupies `[var_end[k-1], var_end[k])`.

`bitmap_bytes`, `var_table` and `nvar` are in the header because the
short-row rule below needs them: appending a fixed column lengthens the
fixed region, so a reader holding an older layout's constants cannot find
`var_end` from them alone. The row says where it is. The null bitmap is
sized when the layout is created, with room to spare (64 bits at least), so
that appending a column does not move the fixed offsets either; a layout
whose bitmap is full needs a `REBUILD`. The bitmap's bit means *present*, so
a column the row predates reads as null by the same test as one set null. A
trailer at the row's end was weighed against these header fields and
rejected; the row's length is 32 bits so that the row's ceiling is the
engine's value limit. The format is specified in full, with the reasons, in
`docs/types_design.md`, *Row format*.

**Columns have permanent ids.** A column is given a small integer id when it
is created, and the id is never reused, even after the column is dropped.
Plans, rows and the catalog speak ids; names exist only in the compiler's
binder. This is protobuf's field number, and it is what lets several schema
versions of one table be served at once (see *Deferred*).

**Positions are permanent and rows may be short.** Within a layout
generation, a column's byte offset or variable slot is assigned when the
column is created and never changes. New columns append; a dropped column
leaves its offset or slot reserved. A row may be shorter than the current
layout: a column the row predates, a fixed column past its fixed region or
a variable slot past the end of `var_end`, has its bit clear and reads as
null; with its bit set the row is damaged, and reading the column is
`BC_E_CORRUPT`. So a plan compiled against an
older version of the table decodes rows written by a newer one, ignoring
the bytes it does not know, and a plan compiled against a newer version
decodes older rows, seeing null where the column did not yet exist. Both
use the same offsets, which the runtime maps from column ids once per
verified plan, and no per-row indirection; this is Cap'n Proto's evolution
rule rather than FlatBuffers' per-row offset table.

**Generations bound the cost of the rule.** Dropped fixed columns leave
holes in every new row, and a type cannot change in place: a type change is
a new column id. When the holes are worth reclaiming, an explicit
`ALTER TABLE … REBUILD` starts a new layout generation with fresh offsets.
`layout_generation` in the header says which generation's offsets decode
the row. A plan names columns by id, not by position, so a new generation
invalidates no plan: the runtime maps ids to the current generation's
offsets when it verifies the plan, and a row of another generation takes
the slow path, mapping each column id through that generation's offset
table in the catalog. Rebuilds are rare and explicit,
and updates and vacuum rewrite rows into the current generation, so the
slow path stays rare.

One decoder, `row_column`, reads every column and checks every access
against `row_bytes` and the offset table: a column whose bit is clear is
null, a row whose header does not parse and a present column the row does
not hold (outside its regions, or a `var_end` past the row) are
`BC_E_CORRUPT` on every path, and a column the plan's reference does not
declare is `BC_E_HANDLE`; never a read past the end, and never 0, empty or
null for damage. Values over the engine's limit are refused at
`bc_row_end` with `BC_E_LIMIT`; the engine's value ceiling applies to the
whole row.

## Validation plan

The end-to-end prototype (#497, merged as #498) came first: seven statements
from SQL text through libpg_query, a binder, a three-rule planner, a
template code generator and TinyCC to `bc_execute`, surviving a reopen, with
the codec, a minimal catalog and this API's subset implemented in
`bytecaskdb-sql-layer/`. Its write-up produced the revisions in *Revision
history*. The validation phase proper follows, against the revised API, with
hand-written plans before any compiler, compiled by clang and linked into
the test binary. CI must build and run `sql_layer_tests`; the prototype
merged without a job doing so.

1. The checker for the subset, with a corpus of rejected files: one per
   rule, each a plan that would be undefined or unbounded without it.
2. The key codec and row format, with property tests: encode, decode, and
   `memcmp` order equal to typed order, for each type, for composite keys,
   and with nulls.
3. The TPROC-C schema, created through SQL DDL by the catalog
   (`tests/tpcc_test.cpp`); a plan's constants are what
   `Catalog::constants` gives for its tables.
4. Ten hand-written plans covering each API surface once: primary-key
   select; secondary range with lookup, sort and limit; a sum over a range;
   an index nested-loop join; an insert into a table with two indexes; an
   update changing an indexed column; a delete; a range delete; a count by `bc_count_range`; one update under
   snapshot isolation and the same with a guard. Then New Order and Payment
   as whole procedures.
5. A result oracle for the hand-written plans: something independent that
   runs the SQL in each plan's header over the same rows, so result rows and
   post-write table contents can be compared. Which one is not decided and
   does not need to be yet. Candidates: an existing SQL engine loaded with
   the same data, or hand-computed expected results for the fixture, which
   is enough for ten plans and two procedures. The Python reference
   implementation is not extended to the relational layer. The first plans
   can be checked by hand while the choice is made.
6. Fault tests for the guarantee: each plan run with a stale view, a wrong
   column offset, an exhausted budget, an exhausted arena and an injected
   I/O error, under the sanitizers, asserting an error code and untouched
   engine state every time.
7. A harness in the shape of `commit_probe`: closed-loop threads running a
   plan through `bc_execute`, reporting plan time, record reads per plan
   from the engine's counters, and commits per second at 1, 16 and 48
   threads. The remainder after subtracting the engine's own reads and
   commit is what this API costs, checks included.
8. Only then, run-time loaders for the same files (`libtcc`, MIR), then an
   interpreter for the subset.

Layout: `bytecaskdb-sql-layer/algebra/` holds `bc_plan.h` and its C++
implementation; `codec/` the key and row codecs; `catalog/` the catalog
(`docs/catalog_design.md`); `plans/` the hand-written plan files, each with its SQL
in the header comment; `sql/` the parser transformer and binder; `plan/` the
planner, code generator and loader; `api/` the embedded API; `check/` the
checker; `tests/` the property tests, fault tests, the harness and the
TPROC-C schema fixture (`tests/tpcc_schema.cppm`). Files
marked `// prototype:` are disposable and are replaced in place. The C++ is
organized as C++23 modules (`bytecask.sql.*`), as the engine is; `bc_plan.h`
is a header because C reads it, and the only other headers are the ones
another toolchain consumes: the C ABI for the bindings and the header bridge
for the plugin, the counterpart of `include/bytecask.hpp`. The catalog, transaction and codec directories are the half the
MariaDB plugin links: the plugin depends on the sql-layer, never the
reverse, and nothing under `bytecaskdb-sql-layer/` includes a MariaDB header.
The plugin keeps its own key and row codecs until the shared ones are
proven, then adopts them through its per-table format versions.

Questions the hand-written plans settle: whether `bc_pin` appears at all
(the prototype's five plans needed it zero times); whether the budget and
ephemeral bounds a compiler would choose are predictable; whether the
checked decoders and the per-row copy cost anything measurable beside the
record read.

## Deferred

- **Several schema versions served at once.** The row format and the
  permanent column ids above are the storage half, and are in the first
  phase. The rest is catalog and DDL: the catalog keeps every version of a
  table; a session declares the version it speaks and the binder resolves
  that version's names and defaults; generated plans always write the
  current layout and maintain the current indexes, filling defaults for
  columns the client's version does not know, and read rows of any version
  by the short-row rule. `ADD COLUMN` with a default and `DROP COLUMN` are
  compatible changes and bump the version; `ALTER TABLE … REBUILD` starts a
  layout generation; the catalog records which versions are still served so
  an old one can be retired. Nothing is migrated for a client to keep
  working.
- **Read-your-own-writes across statements.** An interactive transaction is
  a snapshot and an accumulating write plan; later statements' cursors read
  through an overlay of the earlier writes. The overlay is the session
  layer's and sits under `bc_get`, `bc_scan_row` and the cursors without
  changing their signatures.
- **Sequence allocation.** Auto-increment is a runtime service invoked at
  commit, outside the plan, so the plan stays pure. Until then ids come from
  rows the plan reads and guards.
- **Streaming results.** The result buffer lives in the arena, so a result
  set is bounded by it. A statement that returns more than the arena holds
  is `BC_E_MEMORY`; streaming rows to the client as they are emitted is a
  session-layer change behind the same `bc_emit_*` calls.
- **Spilling sorters and hash tables.** Bounds without spill are right for
  OLTP. An analytic workload would add a spilling variant behind the same
  calls.

## Revision history

**Revision 2, after the prototype (#497).** Every item below came from a
plan the prototype's generator wrote or a test it ran.

- `bc_empty`: a plan could not make an empty view, so an open scan bound
  and an index entry's empty value were unreachable.
- `bc_key_part_*`: no call decoded a key part, so a covering index read was
  impossible and every index scan fetched the row.
- `bc_raise`, `BC_E_CONSTRAINT`, `bc_constraint`, `bc_error_detail`: a
  duplicate key failed `bc_guard_absent` and surfaced as `BC_CONFLICT`,
  retried in vain and misreported. The engine cannot tell a failed
  precondition from a race, so the plan checks the snapshot first and
  raises.
- The result layout moved from the catalog to the plan's `bc_schema_ref`;
  `bc_emit_begin` takes no layout id. Interning result layouts in the
  catalog gave ids stable only within a process.
- Row header: `var_table` and `nvar` added, bitmap sized at create, for the
  short-row rule to hold with variable columns.
- `bc_col_null` takes the column's position, so it can see past the end of
  a shorter row.
- `BC_SCAN_LO_EXCLUSIVE` and `BC_SCAN_HI_INCLUSIVE` removed: wrong on a
  partial key, since the primary-key suffix follows the bound.
- `bc_db`, `bc_catalog`, `bc_params` declared; `bc_ctx_affected` added.
- Plan naming: one symbol per file, renamed per file when linked together.
- Views copy by default, as the prototype implemented them; lending the
  engine's buffers is a later optimization behind the same calls.

**Revision 3, with the types and codec design (#490,
`docs/types_design.md`).** The row and key formats are complete,
and the API changed where implementing them showed a gap.

- The null bitmap's bit means *present*, so a column the row predates reads
  as null with no further test; `bc_col_null` loses the position argument
  revision 2 gave it, which could not tell a fixed offset from a variable
  slot anyway.
- Row header: `row_bytes` is 32 bits, so the row's ceiling is the engine's
  value limit, not 64 KiB; `bitmap_bytes` is in the header, so every
  structural check is local to the row. The header is 12 bytes.
- `bool` had a type code and no calls: `bc_param_bool`, `bc_emit_bool`,
  `bc_key_bool`, `bc_key_part_bool`, `bc_col_bool`, `bc_row_set_bool`.
- `bc_col_i32` removed: every integer is widened to `i64` before it reaches
  a row, so there is no 4-byte column to read.
- `BC_C_RANGE`: a value outside its declared type (`INTEGER`, `VARCHAR(n)`,
  `DECIMAL(p, s)`) is a constraint of its own kind, not a `CHECK`.
- `bc_error_detail` packs the constraint kind in the high byte and the
  object id in the low 24 bits; catalog object ids stay below 2^24, and
  `bc_raise` with an unknown kind or a wider id is `BC_E_HANDLE`.
- `BC_SCAN_PREFIX` keeps bit 3: revision 2 moved it to bit 1, the bit
  `LO_EXCLUSIVE` had, so a plan still passing that flag would have scanned a
  prefix silently instead of being refused.
- A row a column decoder cannot parse is `BC_E_CORRUPT`, not null: before,
  `bc_col_null` read a damaged row as null and the query returned NULL for
  data that was there. The empty view passed as a row is `BC_E_HANDLE`.
- `bc_key_part_bytes` unescapes into the arena rather than lending a
  sub-view: an escaped part is not a sub-range of the key.
- `bc_del_range` refuses an empty bound (`BC_E_HANDLE`): it names no
  table, so an open bound would reach the whole keyspace. `bc_del_range` and
  `bc_guard_range` treat `lo >= hi` as an empty range, as SQL does, rather
  than letting the engine refuse the commit.

**Revision 4, with the catalog (#489, `docs/catalog_design.md`).**

- `bc_schema_ref` lists what a plan depends on (objects read, tables
  written and the indexes it maintains, columns with their positions and
  types, as `bc_col_ref`) instead of a catalog version to match: the
  version-equality check recompiled every plan on every DDL, and the
  per-column rule the design always stated needs the columns. The
  embedded API skips the walk for a cached plan while the version is
  unchanged; `bc_ctx_run` always walks.
- `bc_ctx_begin` pins a catalog version with its snapshot, and
  `bc_ctx_commit` guards the catalog record of every table it writes, so a
  plan never commits rows shaped by a catalog the engine has moved past.
- `bc_row_begin` fills the catalog's defaults, so `ADD COLUMN … DEFAULT`
  invalidates no plan.
- A unique secondary index is checked on its prefix (a prefix scan, then
  `bc_guard_range`): its entries carry the primary key, so `bc_exists` and
  `bc_guard_absent` on one key cannot see a duplicate.
- Secondary keys stay `0x03 index_id`; the plugin's table-id prefix is not
  adopted.
- A plan's constants are named from ids (`TBL_<table>`, `LAY_<layout>`,
  `IDX_<index>`, `COL_<table>_<column>_OFF|_SLOT|_COL`), with the objects
  they stand for listed in the header: a SQL name need not be a C
  identifier, and two names cannot produce the same constant. The header
  is `//` lines with user bytes outside printable ASCII, `\` and `?`
  escaped as `\xNN`, so no name or SQL text can end or splice it.

**Revision 5, a plan carries decisions, not mechanics (#487, stage 2.1).**
The rule in *Decisions, not mechanics* replaces the choice, made in
revision 1, to show index maintenance in the plan. That choice bought no
optimization: every call costs a handle resolution, a generation check, a
sticky-error check and a type check whatever its size, and `insert.c` made
about twenty of them for one row. It existed because the C subset has no
arrays, structs or varargs. It also left a gap: a plan declared the indexes
it maintained in `bc_schema_ref`, and nothing checked that its code wrote
them.

- `bc_insert`, `bc_update`, `bc_delete` and `bc_row_key`: writes are
  table-level, and the runtime writes the index entries, runs the unique
  checks (a prefix probe, `BC_C_UNIQUE`, a range guard; for the primary key,
  existence) and moves only the entries whose keys changed, encoding no
  entry of an index whose columns did not change. The unique
  checks no longer show in the text; they are still charged to the budget.
  `bc_update` and `bc_delete` take a stored row, so the entries they remove
  are the snapshot's.
- `bc_put`, `bc_del`, `bc_lookup`, `bc_scan_value`, `bc_scan_pk`,
  `bc_key_pk` and `bc_row_pk` removed. Rows and index entries are the whole
  keyspace a plan writes, and both go through the calls above; the others
  served index maintenance by hand, or the fetch `bc_scan_row` makes.
- `bc_del_range` names its table and deletes the rows' index entries too:
  a range delete of rows alone would leave entries without rows. Its empty
  bounds now mean the table's start and end, as for the scans, instead of
  being refused.
- `bc_insert` writes no `ensure_absent` on the primary key. The engine's
  check of the plan's own write set already makes a concurrent insert of the
  key a conflict, and a test shows it.
- A NULL set into a NOT NULL column, or a NOT NULL column left unset with
  no default, is `BC_E_CONSTRAINT` with `BC_C_NOT_NULL` and the table's id,
  never retried. It was a `BC_E_LAYOUT` at `bc_row_end`, and an explicit
  NULL cleared the column's default until then.
- `bc_scan_row`: an index entry to its row. A missing row is `BC_E_CORRUPT`;
  the prototype's generated range plan skipped it, which hid damage.
- `bc_key_param`, `bc_key_params`, `bc_row_set_param`, `bc_emit_col`,
  `bc_emit_key_part`: a parameter or a column copied with its NULL, where a
  plan wrote an `if` and two calls.
- Columns are named by id, one constant per column (`COL_<t>_<c>`, the
  column id), instead of an offset or slot constant and a column constant.
  The runtime maps ids to positions when it verifies the reference, so a
  layout generation never reaches a plan, and `REBUILD` will invalidate
  none. `bc_col_ref` loses `layout` and `pos` and gains `table`.
  `bc_row_begin` and `bc_row_from` take the table, not a layout id, and the
  `LAY_` constants are gone; `PART_<k>_<c>` constants name key parts.
- Every table, index and column a plan names must be declared in its
  reference, a table it writes in `tables_written`, and a row view knows its
  table: a decoder on another view, another table's
  column or another type is `BC_E_HANDLE`. A setter of another type was
  `BC_E_LAYOUT`.
- `bc_schema_ref` loses `indexes_written`, and `CREATE INDEX` invalidates no
  plan: the runtime maintains whatever indexes the pinned version has.
  Every run still verifies its reference; the cost is measured and stated
  in *Runtime*, and caching is deferred. `BC_E_LAYOUT` gains a detail on every error:
  `BC_L_INCOMPATIBLE` for the check's refusal, `BC_L_ROW`, `BC_L_PLAN` and
  `BC_L_RETIRED` for the rest, so the embedded API no longer
  checks a compatible plan itself: it recompiles exactly when `bc_ctx_run`
  refuses one, or, when the catalog version has moved, when `bc_ctx_check`
  refuses it before the run. Only then does it check the parameters, their
  types and their ranges, so they are checked against the plan that runs
  and the catalog it was verified against: a key recreated from INTEGER as
  BIGINT takes a BIGINT the stale plan's types would have refused. A plan
  that still fits is marked with the version it was checked against, so it
  is checked once per catalog version, not on every run after a DDL.
- The compiler emits covering reads: a range plan whose result columns are
  all in the index key, none of them `DOUBLE`, reads no row.
