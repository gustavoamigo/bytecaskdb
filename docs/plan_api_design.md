# Plan API: a physical algebra for generated plans

Status: design proposal, not implemented. Nothing in the engine changes; this
document specifies a new component built on the public engine API.

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

A wrong plan can therefore do two things: return an error code, or return a
wrong answer. The second is what the oracle tests in *Validation* exist to
catch. Neither crashes the server, corrupts the engine, or holds a snapshot
past the budget. The only code that can still fault is the API's own
implementation, which is ordinary C++ under the engine's sanitizer and test
regime, and a plan cannot steer it except through checked arguments.

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
runtime reads `bc_error` after the function returns.

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
`bc_pin` copies a view into the arena to extend it.

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
is SQL statement semantics: a statement sees the state before it began.
Read-your-own-writes across statements in an interactive transaction is an
overlay the cursors read through and belongs to the session layer. See
*Deferred*.

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
  BC_E_HANDLE,      /* stale, foreign or out-of-range handle, or a bad column offset */
  BC_E_ARITH,       /* integer overflow, division by zero, out-of-range conversion */
  BC_E_LAYOUT,      /* plan incompatible with the live catalog, or a row violates its layout */
  BC_E_DEGRADED,    /* engine degraded; writes refused */
  BC_E_CLOSED,
};

enum bc_scan_flags {
  BC_SCAN_FORWARD      = 0,
  BC_SCAN_REVERSE      = 1u << 0,
  BC_SCAN_LO_EXCLUSIVE = 1u << 1,   /* default: lo inclusive */
  BC_SCAN_HI_INCLUSIVE = 1u << 2,   /* default: hi exclusive */
  BC_SCAN_PREFIX       = 1u << 3,   /* hi ignored: every key with lo as prefix */
};

enum bc_commit_result { BC_COMMITTED, BC_CONFLICT, BC_FAILED /* see bc_error */ };

typedef struct {
  uint64_t max_reads;       /* record reads and cursor steps, together */
  uint64_t deadline_ns;     /* monotonic; 0 = none */
} bc_budget;

/* What a plan was compiled against: the catalog version, and the tables and
   indexes it reads or writes. bc_ctx_run checks it against the live catalog. */
typedef struct {
  uint64_t catalog_version;
  const uint32_t *tables_read,  *tables_written;   uint32_t n_read, n_written;
} bc_schema_ref;
```

Table ids, index ids, layout ids and column offsets are constants the
compiler emits into each plan from the catalog, resolved from permanent
column ids (*Row format*). They are opaque to the plan and validated by the
compatibility check at `bc_ctx_run`; a column offset outside its row reads
as null (a shorter row) or is `BC_E_HANDLE` (a malformed one) at the access.

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
void    bc_ctx_run(bc_ctx *, bc_plan_fn plan, const bc_schema_ref *compiled_against);
enum bc_commit_result bc_ctx_commit(bc_ctx *, bool sync);
bool    bc_ctx_next_result(bc_ctx *, const uint8_t **row, uint32_t *len);     /* result rows, in emit order */
void    bc_ctx_reset(bc_ctx *);                                               /* drops snapshot, plan, arena */
enum bc_error_code bc_error(const bc_ctx *);
const char *bc_error_message(const bc_ctx *);
int     bc_execute(bc_ctx *, bc_plan_fn, const bc_schema_ref *, const bc_params *,
                   bc_budget, bool sync, uint32_t max_retries);               /* the retry loop */
```

| Function | What it does |
|---|---|
| `bc_ctx_new` | Allocates a context bound to an open engine and a catalog, with an arena of `arena_bytes`. The arena is allocated once and reused. |
| `bc_ctx_begin` | Takes a snapshot, binds the parameters, sets the budget. Clears the error. Must precede `bc_ctx_run`. |
| `bc_ctx_run` | Checks that the schema version the plan was compiled against is compatible with the live catalog: every column id it reads still has its offset and type, every table and index it writes still exists with the same key. A plan that fails the check sets `BC_E_LAYOUT` and does not run; the server recompiles it from the SQL it cached. Adding a column never invalidates a plan; adding an index invalidates the write plans of that table, which must maintain it. Then calls the plan and returns when it returns. |
| `bc_ctx_commit` | Submits the write plan with `apply_batch`. `BC_COMMITTED` on success. `BC_CONFLICT` when the engine reports a guard or write-write conflict: nothing was written. `BC_FAILED` when `bc_error` is set, including an error raised during the plan: nothing is committed. A plan with no writes and no guards commits nothing and returns `BC_COMMITTED`. |
| `bc_ctx_next_result` | Iterates the result buffer. Rows are in the row format of the layout the plan emitted. The runtime, not a plan, sees raw pointers here; valid until `bc_ctx_reset`. |
| `bc_ctx_reset` | Releases the snapshot, drops the write plan and result buffer, resets the arena, bumps every generation so every handle and view the plan held is invalid. |
| `bc_execute` | `begin`, `run`, `commit`; on `BC_CONFLICT`, `reset` and repeat up to `max_retries`. Returns the commit result of the last attempt. This is the only place retry lives. |

### Parameters and results

```c
int64_t  bc_param_i64(bc_ctx *, uint32_t i);
double   bc_param_f64(bc_ctx *, uint32_t i);
bc_view  bc_param_bytes(bc_ctx *, uint32_t i);
bool     bc_param_null(bc_ctx *, uint32_t i);

void     bc_emit_begin(bc_ctx *, uint32_t layout);
void     bc_emit_i64(bc_ctx *, int64_t);
void     bc_emit_f64(bc_ctx *, double);
void     bc_emit_bytes(bc_ctx *, bc_view);
void     bc_emit_null(bc_ctx *);
void     bc_emit_end(bc_ctx *);
void     bc_emit_count(bc_ctx *, int64_t affected);   /* write statements: the row count */
```

| Function | What it does |
|---|---|
| `bc_param_*` | Reads parameter `i` as bound by `bc_ctx_begin`. A wrong index or type is `BC_E_HANDLE` and a neutral value. Parameter views live until reset. |
| `bc_emit_begin` … `bc_emit_end` | Appends one result row in `layout`'s row format. Columns are emitted in layout order; a count or type that does not match the layout is `BC_E_LAYOUT`. The row is copied into the result buffer, which is part of the arena. |
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
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_pk(bc_ctx *, bc_key, bc_view pk);     /* appends an already-encoded primary key */
bc_view  bc_key_end(bc_ctx *, bc_key);
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */
```

| Function | What it does |
|---|---|
| `bc_key_begin` | Starts a key for a table (its primary key) or an index. Writes the namespace byte and the 4-byte id. Returns a builder handle. |
| `bc_key_i64`, `bc_key_f64`, `bc_key_bytes`, `bc_key_null` | Appends one part. Nullability and direction come from the catalog entry for `table_or_index`, so the builder knows whether to write a null marker and whether to invert the bytes. A part of the wrong type, or more parts than the key has, is `BC_E_HANDLE`. |
| `bc_key_pk` | Appends the encoded primary-key suffix to a secondary index key, as `bc_scan_pk` or `bc_row_pk` returned it. Copied verbatim. |
| `bc_key_end` | Finalizes and returns the key. A partial key (fewer parts than the index has) is legal and is a prefix the compiler uses as a scan bound. |
| `bc_key_successor` | The exclusive upper bound of the prefix `key`: its last byte incremented, carrying. Turns a prefix into a half-open range where `BC_SCAN_PREFIX` does not fit. |

### Reads

Every read goes through the context's snapshot and charges the budget.

```c
bc_view   bc_get(bc_ctx *, bc_view key, bool *found);
bc_view   bc_lookup(bc_ctx *, uint32_t table, bc_view pk, bool *found);
bool      bc_exists(bc_ctx *, bc_view key);

bc_cursor bc_scan_open(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi, uint32_t flags);
bool      bc_scan_next(bc_ctx *, bc_cursor);
bc_view   bc_scan_key(bc_ctx *, bc_cursor);
bc_view   bc_scan_value(bc_ctx *, bc_cursor);
bc_view   bc_scan_pk(bc_ctx *, bc_cursor);
void      bc_scan_close(bc_ctx *, bc_cursor);

int64_t   bc_count_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi, int64_t limit);
```

| Function | What it does | Returns and lifetime | Cost |
|---|---|---|---|
| `bc_get` | Reads the record at `key`. | The value; `*found` false and an empty view when absent. Lives until reset. | 1 record read |
| `bc_lookup` | Builds the primary key of `table` from an encoded pk suffix and reads the row. The secondary-index path: `bc_scan_pk` feeds it. | The row; `*found` false when absent. Lives until reset. | 1 record read |
| `bc_exists` | Whether `key` is present. | `bool`. | 1 record read (the blind key directory confirms a key from its record) |
| `bc_scan_open` | Opens a cursor over `[lo, hi)` on a table (primary key order) or an index, direction and bounds per `flags`. An empty `lo` means the start, an empty `hi` the end. With `BC_SCAN_PREFIX`, `hi` is ignored and the range is every key having `lo` as a prefix. Positioned before the first entry. | A cursor handle. Closed by `bc_ctx_reset`; `bc_scan_close` releases one early. | 0 |
| `bc_scan_next` | Advances in the scan's direction, charging one unit of budget. Invalidates the cursor's views. | `true` while an entry is available; `false` at the end, on error, or when the budget is spent. | 1 record read (the key directory holds no key bytes, so each step reads the entry's record; on a table scan the same read yields the value) |
| `bc_scan_key` | The current entry's full encoded key. | View valid until the cursor steps or closes. | 0 |
| `bc_scan_value` | The current entry's value: the row on a table scan, empty on a secondary index. | View valid until the cursor steps or closes. | 0 |
| `bc_scan_pk` | On a secondary index, the encoded primary-key suffix of the current entry, parsed with the index's catalog layout. On a table scan, the primary-key part of the key. | View valid until the cursor steps or closes. | 0 |
| `bc_count_range` | Counts the live entries in `[lo, hi)` without walking them, stopping at `limit`. Compiles to `Snapshot::count_keys`. The planner's cardinality source and `COUNT(*)` over a key range. | `min(count, limit)`. | at most 2 record reads, whatever the range holds |

A range scan on a secondary index with a fetch of each row costs one read
per step plus one per `bc_lookup`. A covering index, where the plan reads
only `bc_scan_pk` and the index columns from `bc_scan_key`, costs one.

### Rows and columns

Rows use the format in *Row format*. The compiler emits each column's
offset or slot as a constant from the catalog. Decoders resolve the view,
check its generation, check that the access lies within the row, then load;
a violation is `BC_E_HANDLE` and a neutral value. They are inline, and the
checks are a compare and a bounds test.

```c
bool     bc_col_null(bc_ctx *, bc_view row, uint32_t col);
int64_t  bc_col_i64(bc_ctx *, bc_view row, uint32_t off);
int64_t  bc_col_i32(bc_ctx *, bc_view row, uint32_t off);    /* widened: the subset has one integer type */
double   bc_col_f64(bc_ctx *, bc_view row, uint32_t off);
bc_view  bc_col_bytes(bc_ctx *, bc_view row, uint32_t slot); /* a sub-view; same lifetime as row */
bc_view  bc_row_pk(bc_ctx *, uint32_t table, bc_view row);   /* the row's encoded primary key */

bc_row   bc_row_begin(bc_ctx *, uint32_t layout);
bc_row   bc_row_from(bc_ctx *, uint32_t layout, bc_view old);  /* copy, then overwrite columns */
void     bc_row_set_i64(bc_ctx *, bc_row, uint32_t col, int64_t);
void     bc_row_set_f64(bc_ctx *, bc_row, uint32_t col, double);
void     bc_row_set_bytes(bc_ctx *, bc_row, uint32_t col, bc_view);
void     bc_row_set_null(bc_ctx *, bc_row, uint32_t col);
bc_view  bc_row_end(bc_ctx *, bc_row);
bc_view  bc_pin(bc_ctx *, bc_view);                 /* copy into the arena: lives until reset */
```

| Function | What it does |
|---|---|
| `bc_col_null` | True if column `col`'s bit is set in the null bitmap, or if the column lies past the end of the row (written before the column existed). The compiler emits it before any decoder of a nullable column, and before any column added after the table was created. |
| `bc_col_i64`, `bc_col_i32`, `bc_col_f64` | Loads the fixed-width column at byte offset `off`. A null column, or one past the end of a shorter row, reads as zero; `bc_col_null` decides first and reports both as null. Little-endian in the row. |
| `bc_col_bytes` | The variable-length column in slot `slot`, as a view with the row's lifetime. |
| `bc_row_pk` | Encodes the row's primary key from its columns, for an `UPDATE` or `DELETE` reached through a scan or a lookup. Built in the arena. |
| `bc_row_begin` | Starts an empty row of `layout`: every column null. |
| `bc_row_from` | Starts a row as a copy of `old`, which must be of `layout` (`BC_E_LAYOUT` otherwise). The `UPDATE` pattern: copy, set the assigned columns, end. |
| `bc_row_set_*`, `bc_row_set_null` | Sets column `col`. Setting twice keeps the last value. A type that does not match the layout is `BC_E_LAYOUT`. |
| `bc_row_end` | Finalizes the row in the arena and returns its view. Every non-nullable column must have been set (`BC_E_LAYOUT` otherwise); a row over the engine's value limit is `BC_E_LIMIT`. |
| `bc_pin` | Copies a view into the arena so it outlives its cursor. The compiler emits it where a plan keeps a cursor's bytes across a step. |

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
| `bc_sorter_add` | Adds a row with its sort key, a mem-comparable byte string the plan built with `bc_key_*` (a `DESC` column inverted, nulls first or last per the key's layout). Both are copied. |
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

### Effects

Each appends to the write plan. Nothing reaches the engine until
`bc_ctx_commit`. A failed guard is reported by the commit, not by the call.

```c
void bc_put(bc_ctx *, bc_view key, bc_view row);
void bc_del(bc_ctx *, bc_view key);
void bc_del_range(bc_ctx *, bc_view lo, bc_view hi);
void bc_guard_unchanged(bc_ctx *, bc_view key);
void bc_guard_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi);
void bc_guard_present(bc_ctx *, bc_view key);
void bc_guard_absent(bc_ctx *, bc_view key);
```

| Function | What it does | Engine operation |
|---|---|---|
| `bc_put` | Writes `row` at `key`, overwriting. The compiler emits one per table row and one per secondary index entry the statement creates or moves; index maintenance is visible in the plan. | `WritePlan::put` |
| `bc_del` | Deletes `key`. One per table row and per index entry removed. | `WritePlan::del` |
| `bc_del_range` | Deletes every key in `[lo, hi)` in one append. `TRUNCATE`, `DROP`, and range deletes the compiler can express as a key range. | `WritePlan::del_range` |
| `bc_guard_unchanged` | The commit fails if `key` changed since the snapshot. Serializable plans emit one for every key they read and did not write. Keys the plan writes are checked by the engine without a guard. | `ensure_unchanged` |
| `bc_guard_range` | The commit fails if any key in `[lo, hi)` changed. Serializable plans emit one per range scanned; it covers phantoms. | `ensure_range_unchanged` |
| `bc_guard_present`, `bc_guard_absent` | Existence preconditions. `INSERT` emits `absent` on the primary key and on each unique index key. | `ensure_present`, `ensure_absent` |

A plan under snapshot isolation emits no guards. The two plans for the same
SQL differ only in guard calls, which is how isolation reads in the text.
The write plan is bounded by the engine's 1 GiB plan limit; exceeding it is
`BC_E_LIMIT` at the call.

## The language

A plan file is a program in a subset of C defined by a checker that every
tier runs before compiling or interpreting it. The checker is a type checker
for the subset; a file it rejects never reaches a compiler. The rules are
the language's definition, and the C compiler is a backend for programs that
already passed them.

**Structure.** One comment with the SQL text, the isolation level and the
schema version the plan was compiled against; `#include "bc_plan.h"` and nothing else; one `enum` of
catalog constants; one function `void plan(bc_ctx *cx)`. No other
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
/* SELECT c_id, c_first, c_balance FROM customer
   WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3
   ORDER BY c_first LIMIT 1 OFFSET (count / 2)
   isolation: serializable   schema: customer v7   */
#include "bc_plan.h"
enum { TBL_CUSTOMER = 7, IDX_CUST_BY_NAME = 12, LAY_RESULT = 31,
       CUST_ID_OFF = 24, CUST_BALANCE_OFF = 40, CUST_FIRST_SLOT = 1 };

void plan(bc_ctx *cx) {
  bc_key kb = bc_key_begin(cx, IDX_CUST_BY_NAME);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_key_i64(cx, kb, bc_param_i64(cx, 1));
  bc_key_bytes(cx, kb, bc_param_bytes(cx, 2));
  bc_view prefix = bc_key_end(cx, kb);

  bc_sorter s = bc_sorter_open(cx, 65536);
  bc_cursor c = bc_scan_open(cx, IDX_CUST_BY_NAME, prefix, prefix, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {                                   /* 1 read per step */
    bool found = false;
    bc_view row = bc_lookup(cx, TBL_CUSTOMER, bc_scan_pk(cx, c), &found);   /* 1 read */
    bc_sorter_add(cx, s, bc_col_bytes(cx, row, CUST_FIRST_SLOT), row);
  }
  bc_guard_range(cx, IDX_CUST_BY_NAME, prefix, bc_key_successor(cx, prefix));  /* serializable */

  int64_t n = bc_sorter_count(cx, s);
  if (n == 0) return;
  bc_view mid = bc_sorter_nth(cx, s, bc_div(cx, n, 2));
  bc_emit_begin(cx, LAY_RESULT);
  bc_emit_i64(cx, bc_col_i64(cx, mid, CUST_ID_OFF));
  bc_emit_bytes(cx, bc_col_bytes(cx, mid, CUST_FIRST_SLOT));
  bc_emit_f64(cx, bc_col_f64(cx, mid, CUST_BALANCE_OFF));
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
| `i64` | 8 bytes big-endian with the sign bit flipped. |
| `i32` and narrower | Widened to `i64`. |
| `f64` | IEEE 754 bits, big-endian; positive values have the sign bit flipped, negative values have every bit flipped. |
| `bytes` | Escaped: each `0x00` becomes `0x00 0xFF`, followed by the terminator `0x00 0x00`. Self-delimiting, and a prefix of a longer string sorts first. |
| `DESC` part | The encoded bytes above with every bit inverted. |

A secondary index key is the index columns followed by the primary key's
parts. The primary key suffix is what `bc_scan_pk` returns and `bc_lookup`
and `bc_key_pk` accept. Because every part is self-delimiting, the suffix is
found by skipping the index columns with the index's catalog layout.

Namespaces: `0x01` catalog, `0x02` table rows, `0x03` index entries, as in
the MariaDB plugin. Text columns are not stored as text in keys: the
compiler encodes a collation's sort key (weights) as `bytes`, as the plugin
does, and the row carries the original value.

## Row format

```
u16   layout_generation     ; which offset table decodes this row (see below)
u16   row_bytes             ; length of the row; columns beyond it are null
u8[]  null_bitmap           ; ceil(ncols / 8) bytes, bit set = null
      fixed columns         ; at permanent offsets, little-endian, 8-byte aligned region
u32[] var_end               ; nvar end offsets, relative to the start of var data
u8[]  var data
```

Fixed columns are `i64`, `i32`, `f64`, `bool`, and fixed-width binary. A
variable column `k` occupies `[var_end[k-1], var_end[k])`.

**Columns have permanent ids.** A column is given a small integer id when it
is created, and the id is never reused, even after the column is dropped.
Plans, rows and the catalog speak ids; names exist only in the compiler's
binder. This is protobuf's field number, and it is what lets several schema
versions of one table be served at once (see *Deferred*).

**Positions are permanent and rows may be short.** Within a layout
generation, a column's byte offset or variable slot is assigned when the
column is created and never changes. New columns append; a dropped column
leaves its offset or slot reserved. A row may be shorter than the current
layout: a fixed column whose offset is at or past `row_bytes`, or a variable
slot past the end of `var_end`, reads as null. So a plan compiled against an
older version of the table decodes rows written by a newer one, ignoring
the bytes it does not know, and a plan compiled against a newer version
decodes older rows, seeing null where the column did not yet exist. Both
use the same compile-time offsets and no per-row indirection; this is
Cap'n Proto's evolution rule rather than FlatBuffers' per-row offset table.

**Generations bound the cost of the rule.** Dropped fixed columns leave
holes in every new row, and a type cannot change in place: a type change is
a new column id. When the holes are worth reclaiming, an explicit
`ALTER TABLE … REBUILD` starts a new layout generation with fresh offsets.
`layout_generation` in the header says which generation's offsets decode
the row. A plan's constants are its own generation's; a row of another
generation takes the slow path, mapping each column id through that
generation's offset table in the catalog. Rebuilds are rare and explicit,
and updates and vacuum rewrite rows into the current generation, so the
slow path stays rare.

The decoders check every access against `row_bytes` and the offset table,
so a row that disagrees with its layout is null or `BC_E_HANDLE`, never a
read past the end. Values over the engine's limit are refused at
`bc_row_end` with `BC_E_LIMIT`; the engine's value ceiling applies to the
whole row.

## Validation plan

Hand-written plans come before any compiler, compiled by clang and linked
into the test binary.

1. The checker for the subset, with a corpus of rejected files: one per
   rule, each a plan that would be undefined or unbounded without it.
2. The key codec and row format, with property tests: encode, decode, and
   `memcmp` order equal to typed order, for each type, for composite keys,
   and with nulls.
3. A catalog for the TPROC-C schema as C constants.
4. Ten hand-written plans covering each API surface once: primary-key
   select; secondary range with lookup, sort and limit; a sum over a range;
   an index nested-loop join; an insert maintaining two indexes; an update
   changing an indexed column (delete and reinsert of the index entry); a
   delete; a range delete; a count by `bc_count_range`; one update under
   snapshot isolation and the same with a guard. Then New Order and Payment
   as whole procedures.
5. SQLite as the oracle: the same rows loaded there, the SQL in each plan's
   header run, result rows and post-write table contents compared.
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

Questions the hand-written plans settle: whether `bc_pin` appears rarely or
everywhere; whether the budget and ephemeral bounds a compiler would choose
are predictable; whether the checked decoders cost anything measurable
beside the record read.

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
  layer's and sits under `bc_get`, `bc_lookup` and the cursors without
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
- **The catalog in the database.** The first phase hard-codes it. Its
  persistent form is rows in namespace `0x01`, as in the plugin.
