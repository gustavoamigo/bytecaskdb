# Plan API: a physical algebra for generated plans

Status: design proposal, not implemented. Nothing in the engine changes; this
document specifies a new component built on the public engine API.

## Purpose

A SQL compiler turns a statement into a physical plan: every access path,
join algorithm, sort and guard decided. This document specifies the API that
plan is written against. The plan is a C function in a restricted subset of C
that calls these functions and nothing else. The same file is the `EXPLAIN`
output, the executable, and the unit of test.

The API is the only boundary between the two halves of a SQL engine:

- Above it: parser, binder, optimizer, code generator. They read SQL and the
  catalog and write a C file. Anything else that writes the same C is a peer.
- Below it: the engine, a catalog, a key codec, a row format, and this API.
  No planning happens here. A plan says `bc_scan_open` on an index; the
  executor opens that index.

The C subset compiles with any C compiler, so the first execution tier is
clang linking plans into the test binary. Later tiers load the same text at
run time (an embedded C compiler, or an interpreter for hand-written and
untrusted plans). The API is the same in every tier.

What it is not: a general C API for the engine, a wire protocol, or a place
for SQL semantics. Collations, decimals and dates are the compiler's problem;
here every value is an integer, a double or bytes.

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
memory bound, a bad handle) the context records the error, and every later
call returns a value that ends the plan quickly: cursors report end of data,
gets report not found, writes are dropped. The runtime checks `bc_error`
after the function returns. Every loop in a plan is bounded by a cursor or
an ephemeral iteration, so a plan cannot spin after an error.

**Handles are small integers, views are plain structs.** Cursors, sorters,
hash tables, aggregates, key builders and row builders are indices into
per-context tables. The interpreter and debug builds check them; a
generation counter catches a stale handle. A view is a pointer and a length
passed by value, so the inline column decoders compile into the loop.

**Views borrow.** A view returned by a cursor is valid until that cursor
moves or closes. A view returned by `bc_get` or `bc_lookup` is valid until
the context is reset. A view into the arena is valid until reset. The
generated code respects this; the compiler that generates it is where the
rule is enforced. `bc_arena_copy` pins a view past its owner's lifetime.

**The arena is the only memory.** Keys, rows, result rows, sorter and hash
contents live in a bump arena owned by the context and reset after each
plan execution. There is no heap allocation on the plan's path and no
freeing. Blocking structures declare a byte bound at open and fail with
`BC_E_MEMORY` when they exceed it; there is no spill.

**One context, one thread.** A context is used by one thread at a time.
Contexts run concurrently on different threads; reads scale as the engine's
reads scale, and commits go through the engine's group commit.

**Within a plan, reads see the snapshot, not the plan's own writes.** This
is SQL statement semantics: a statement sees the state before it began.
Read-your-own-writes across statements in an interactive transaction is an
overlay the cursors read through (the plugin's merge iterator) and belongs
to the session layer, not this API. See *Deferred*.

## Types and constants

```c
#include <stdint.h>
#include <stdbool.h>

typedef struct { const uint8_t *p; uint32_t len; } bc_view;   /* p may be NULL when len == 0 */

typedef struct bc_ctx bc_ctx;                 /* opaque; one per executing plan */
typedef uint32_t bc_cursor;                   /* handles: indices, checked in debug */
typedef uint32_t bc_sorter;
typedef uint32_t bc_hash;
typedef uint32_t bc_hash_iter;
typedef uint32_t bc_agg;
typedef uint32_t bc_group;
typedef uint32_t bc_key;                      /* a key under construction */
typedef uint32_t bc_row;                      /* a row under construction */

typedef void (*bc_plan_fn)(bc_ctx *);         /* every plan has this signature */

enum bc_error_code {
  BC_OK = 0,
  BC_E_IO,          /* std::system_error from the engine */
  BC_E_CORRUPT,     /* CRC mismatch or unparseable record */
  BC_E_MEMORY,      /* arena exhausted or an ephemeral bound exceeded */
  BC_E_LIMIT,       /* key or value over the engine's limits */
  BC_E_HANDLE,      /* bad or stale handle (debug and interpreter builds) */
  BC_E_LAYOUT,      /* plan fingerprint does not match the catalog */
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
```

Table ids, index ids, layout ids and column offsets are constants the
compiler emits into each plan from the catalog. They are opaque to the plan
and checked by the fingerprint.

## Function reference

Cost is counted in record reads, the engine's unit of work: a key directory
descent plus one read of the record, served from the buffer pool or the
page cache. Everything that is not a record read is a few nanoseconds.

### Runtime: creating, running and committing

Called by the server, never by a plan.

```c
bc_ctx *bc_ctx_new(bc_db *db, const bc_catalog *cat, uint32_t arena_bytes);
void    bc_ctx_free(bc_ctx *);
void    bc_ctx_begin(bc_ctx *, const bc_params *params);        /* takes the snapshot */
void    bc_ctx_run(bc_ctx *, bc_plan_fn plan, uint64_t plan_fingerprint);
enum bc_commit_result bc_ctx_commit(bc_ctx *, bool sync);
bool    bc_ctx_next_result(bc_ctx *, bc_view *row);               /* result rows, in emit order */
void    bc_ctx_reset(bc_ctx *);                                   /* drops snapshot, plan, arena */
enum bc_error_code bc_error(const bc_ctx *);
const char *bc_error_message(const bc_ctx *);
int     bc_execute(bc_ctx *, bc_plan_fn, uint64_t fingerprint, const bc_params *,
                   bool sync, uint32_t max_retries);              /* the retry loop */
```

| Function | What it does |
|---|---|
| `bc_ctx_new` | Allocates a context bound to an open engine and a catalog, with an arena of `arena_bytes`. The arena is allocated once and reused. |
| `bc_ctx_begin` | Takes a snapshot and binds the parameters. Clears the error. Must precede `bc_ctx_run`. |
| `bc_ctx_run` | Checks `plan_fingerprint` against the catalog (`BC_E_LAYOUT` if it differs, and the plan does not run), then calls the plan. Returns when the plan returns. |
| `bc_ctx_commit` | Submits the write plan with `apply_batch`. `BC_COMMITTED` on success. `BC_CONFLICT` when the engine reports a guard or write-write conflict: nothing was written. `BC_FAILED` when `bc_error` is set, including an error raised during the plan: nothing is committed. A plan with no writes and no guards commits nothing and returns `BC_COMMITTED`. |
| `bc_ctx_next_result` | Iterates the result buffer. Rows are in the row format of the layout the plan emitted. Valid until `bc_ctx_reset`. |
| `bc_ctx_reset` | Releases the snapshot, drops the write plan and result buffer, resets the arena, invalidates every handle and view. |
| `bc_execute` | `begin`, `run`, `commit`; on `BC_CONFLICT`, `reset` and repeat up to `max_retries`. Returns the commit result of the last attempt. This is the only place retry lives. |

### Parameters and results

```c
int64_t  bc_param_i64(const bc_ctx *, uint32_t i);
double   bc_param_f64(const bc_ctx *, uint32_t i);
bc_view  bc_param_bytes(const bc_ctx *, uint32_t i);
bool     bc_param_null(const bc_ctx *, uint32_t i);

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
| `bc_param_*` | Reads parameter `i` as bound by `bc_ctx_begin`. The compiler knows each parameter's type; reading the wrong type is a plan bug, caught in debug builds. Parameter views are valid for the whole execution. |
| `bc_emit_begin` … `bc_emit_end` | Appends one result row in `layout`'s row format. Columns are emitted in layout order; the count and types must match the layout. The row is copied into the result buffer, so views passed to `bc_emit_bytes` need only be valid during the call. |
| `bc_emit_count` | Records the affected-row count a write statement reports. At most once per plan. |

### Keys

Keys are built in the arena in the mem-comparable encoding of *Key encoding*
below. A builder is finalized once; its view is valid until reset.

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
| `bc_key_i64`, `bc_key_f64`, `bc_key_bytes`, `bc_key_null` | Appends one part. The part's nullability and direction come from the catalog entry for `table_or_index`, so the builder knows whether to write a null marker and whether to invert the bytes. Appending more parts than the key has is `BC_E_HANDLE`. |
| `bc_key_pk` | Appends the encoded primary-key suffix to a secondary index key, as `bc_scan_pk` or `bc_row_pk` returned it. The suffix is already encoded; it is copied verbatim. |
| `bc_key_end` | Finalizes and returns the key's bytes. A partial key (fewer parts than the index has) is legal and is a prefix: the compiler uses it as a scan bound. |
| `bc_key_successor` | Returns the exclusive upper bound of the prefix `key`: the key with its last byte incremented, carrying. Used to turn a prefix into a half-open range when `BC_SCAN_PREFIX` does not fit. |

### Reads

Every read goes through the context's snapshot.

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
| `bc_get` | Reads the record at `key`. | The value; `*found` false and an empty view when absent. Valid until reset. | 1 record read |
| `bc_lookup` | Builds the primary key of `table` from an encoded pk suffix and reads the row. The secondary-index path: `bc_scan_pk` feeds it. | The row; `*found` false when absent (an index entry whose row is gone is a plan bug and asserts in debug). Valid until reset. | 1 record read |
| `bc_exists` | Whether `key` is present. | `bool`. | 1 record read (the blind key directory confirms a key from its record) |
| `bc_scan_open` | Opens a cursor over `[lo, hi)` on a table (primary key order) or an index, direction and bounds per `flags`. `lo` and `hi` are keys or key prefixes from `bc_key_end`; an empty `lo` means the start of the table or index, an empty `hi` the end. With `BC_SCAN_PREFIX`, `hi` is ignored and the range is every key having `lo` as a prefix. The cursor is positioned before the first entry. | A cursor handle. Cursors are closed by `bc_ctx_reset`; `bc_scan_close` releases one early. | 0 |
| `bc_scan_next` | Advances to the next entry in the scan's direction. | `true` while an entry is available; `false` at the end or on error. | 1 record read (the key directory holds no key bytes, so each step reads the entry's record to learn its key; on a table scan the same read yields the value) |
| `bc_scan_key` | The current entry's full encoded key. | View valid until the cursor moves or closes. | 0 |
| `bc_scan_value` | The current entry's value: the row on a table scan, empty on a secondary index. | View valid until the cursor moves or closes. | 0 |
| `bc_scan_pk` | On a secondary index, the encoded primary-key suffix of the current entry, parsed from the key with the index's catalog layout. On a table scan, the primary-key part of the key. | View valid until the cursor moves or closes. | 0 |
| `bc_count_range` | Counts the live entries in `[lo, hi)` without walking them, stopping at `limit`. Compiles to `Snapshot::count_keys`. The planner's cardinality source and `COUNT(*)` over a key range. | `min(count, limit)`. | at most 2 record reads, whatever the range holds |

A range scan on a secondary index with a fetch of each row costs one read
per step plus one per `bc_lookup`. A covering index, where the plan reads
only `bc_scan_pk` and the index columns from `bc_scan_key`, costs one.

### Rows and columns

Rows use the format in *Row format*. Decoding is inline; the compiler emits
each column's offset or slot as a constant from the catalog.

```c
static inline bool    bc_col_null(bc_view row, uint32_t col);
static inline int64_t bc_col_i64(bc_view row, uint32_t off);
static inline int32_t bc_col_i32(bc_view row, uint32_t off);
static inline double  bc_col_f64(bc_view row, uint32_t off);
static inline bc_view bc_col_bytes(bc_view row, uint32_t slot);
bc_view  bc_row_pk(bc_ctx *, uint32_t table, bc_view row);    /* the row's encoded primary key */

bc_row   bc_row_begin(bc_ctx *, uint32_t layout);
bc_row   bc_row_from(bc_ctx *, uint32_t layout, bc_view old);  /* copy, then overwrite columns */
void     bc_row_set_i64(bc_ctx *, bc_row, uint32_t col, int64_t);
void     bc_row_set_f64(bc_ctx *, bc_row, uint32_t col, double);
void     bc_row_set_bytes(bc_ctx *, bc_row, uint32_t col, bc_view);
void     bc_row_set_null(bc_ctx *, bc_row, uint32_t col);
bc_view  bc_row_end(bc_ctx *, bc_row);
```

| Function | What it does |
|---|---|
| `bc_col_null` | Tests column `col`'s bit in the null bitmap. The compiler emits it before any decoder of a nullable column. |
| `bc_col_i64`, `bc_col_i32`, `bc_col_f64` | Loads the fixed-width column at byte offset `off`. Undefined on a null column; the bitmap decides first. Little-endian in the row, so a load on the host. |
| `bc_col_bytes` | Returns the variable-length column in slot `slot` as a view into the row. Valid as long as `row` is. |
| `bc_row_pk` | Encodes the row's primary key from its columns, for an `UPDATE` or `DELETE` reached through a table scan or a lookup. Built in the arena. |
| `bc_row_begin` | Starts an empty row of `layout`: every column null. |
| `bc_row_from` | Starts a row as a copy of `old`, which must be of `layout`. The `UPDATE` pattern: copy, set the assigned columns, end. |
| `bc_row_set_*`, `bc_row_set_null` | Sets column `col`. Setting a column twice keeps the last value. Type must match the layout; checked in debug builds. |
| `bc_row_end` | Finalizes the row in the arena and returns its view. Every non-nullable column must have been set (`BC_E_LAYOUT` otherwise). |

### Ephemeral structures

All three copy their inputs into the arena and declare a byte bound.
Exceeding it sets `BC_E_MEMORY`. Their contents are valid until reset.

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
void      bc_agg_add_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
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
| `bc_sorter_nth` | The payload of the `n`-th row in sort order, zero-based. The first call to `nth` or `next` sorts; adding after that is `BC_E_HANDLE`. Random access for `OFFSET` and for medians. |
| `bc_sorter_next` | Streams payloads in sort order. |
| `bc_hash_open`, `bc_hash_put` | A multimap for the build side of a hash join. Duplicate keys are kept. |
| `bc_hash_find`, `bc_hash_next` | Probes: an iterator over every payload stored under `key`, in insertion order. |
| `bc_agg_open` | Grouped accumulators with `slots` numeric slots per group, for `GROUP BY` the plan cannot get from index order. Ungrouped aggregates use one group with an empty key. |
| `bc_agg_group` | Finds or creates the group for `group_key`, a mem-comparable byte string. Returns its handle; slots start at zero and null. |
| `bc_agg_add_*`, `bc_agg_min_*`, `bc_agg_max_*`, `bc_agg_count` | Folds a value into a slot. `SUM`, `MIN`, `MAX`, `COUNT`; `AVG` is a sum slot and a count slot divided at emit. Null inputs are skipped, as SQL does. |
| `bc_agg_next` | Iterates groups in an unspecified order; the plan sorts them if the statement orders them. |
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

### Arena and miscellany

```c
bc_view bc_arena_copy(bc_ctx *, bc_view);          /* pin a view until reset */
bool    bc_bytes_eq(bc_view, bc_view);
int     bc_bytes_cmp(bc_view, bc_view);           /* memcmp order */
bool    bc_bytes_prefix(bc_view prefix, bc_view);
```

`bc_arena_copy` is for the case a plan must hold a cursor's view across a
step of that cursor. The compiler emits it; it is the explicit copy. The
byte helpers are for predicates on encoded or binary columns; the compiler
is responsible for collation and emits comparisons on sort keys it built,
never on raw text.

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
u16   layout_version        ; the catalog's version of this table's layout
u8[]  null_bitmap           ; ceil(ncols / 8) bytes, bit set = null
      fixed columns         ; at constant offsets, little-endian, 8-byte aligned region
u32[] var_end               ; nvar end offsets, relative to the start of var data
u8[]  var data
```

Fixed columns are `i64`, `i32`, `f64`, `bool`, and fixed-width binary. A
variable column `k` occupies `[var_end[k-1], var_end[k])`. The layout
version lets `ADD COLUMN` be a catalog change: a row at an older version
decodes with that version's layout and new columns read as null or default.
Values over the engine's limit are refused at `bc_row_end` with
`BC_E_LIMIT`; the engine's value ceiling applies to the whole row.

## Generated plan files

A plan file contains, in order: a comment with the SQL text, the isolation
level and the catalog fingerprint; `#include "bc_plan.h"`; an `enum` of the
catalog constants it uses; one function of type `bc_plan_fn`. It uses no
other include and no identifier outside this header and its own locals.

The C subset: `int64_t`, `int32_t`, `double`, `bool`, `bc_view` and the
handle types; `if`, `else`, `for`, `while`, `break`, `continue`, `return`;
arithmetic, comparison and logical operators; calls to this header. No
pointer arithmetic or address-of, no arrays, no casts between handle types,
no `goto`, no `switch`, no string literals except in comments, no libc.

Payment's customer lookup by last name, serializable:

```c
/* SELECT c_id, c_first, c_balance FROM customer
   WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3
   ORDER BY c_first LIMIT 1 OFFSET (count / 2)
   isolation: serializable   catalog: 0x5a1c…   */
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
  while (bc_scan_next(cx, c)) {                                 /* 1 read per step */
    bool found;
    bc_view row = bc_lookup(cx, TBL_CUSTOMER, bc_scan_pk(cx, c), &found);   /* 1 read */
    bc_sorter_add(cx, s, bc_col_bytes(row, CUST_FIRST_SLOT), row);
  }
  bc_guard_range(cx, IDX_CUST_BY_NAME, prefix, bc_key_successor(cx, prefix));  /* serializable */

  int64_t n = bc_sorter_count(cx, s);
  if (n == 0) return;
  bc_view mid = bc_sorter_nth(cx, s, n / 2);
  bc_emit_begin(cx, LAY_RESULT);
  bc_emit_i64(cx, bc_col_i64(mid, CUST_ID_OFF));
  bc_emit_bytes(cx, bc_col_bytes(mid, CUST_FIRST_SLOT));
  bc_emit_f64(cx, bc_col_f64(mid, CUST_BALANCE_OFF));
  bc_emit_end(cx);
}
```

A reader sees the index, the direction, one record read per entry plus one
per row, the sort and its bound, the guard, and the result. The cost model
is on the page.

## Validation plan

Hand-written plans come before any compiler, compiled by clang and linked
into the test binary.

1. The key codec and row format, with property tests: encode, decode, and
   `memcmp` order equal to typed order, for each type, for composite keys,
   and with nulls.
2. A catalog for the TPROC-C schema as C constants.
3. Ten hand-written plans covering each API surface once: primary-key
   select; secondary range with lookup, sort and limit; a sum over a range;
   an index nested-loop join; an insert maintaining two indexes; an update
   changing an indexed column (delete and reinsert of the index entry); a
   delete; a range delete; a count by `bc_count_range`; one update under
   snapshot isolation and the same with a guard. Then New Order and Payment
   as whole procedures.
4. SQLite as the oracle: the same rows loaded there, the SQL in each plan's
   header run, result rows and post-write table contents compared.
5. A harness in the shape of `commit_probe`: closed-loop threads running a
   plan through `bc_execute`, reporting plan time, record reads per plan
   from the engine's counters, and commits per second at 1, 16 and 48
   threads. The remainder after subtracting the engine's own reads and
   commit is what this API costs.
6. Only then, run-time loaders for the same files (`libtcc`, MIR), then an
   interpreter for the subset.

Questions the hand-written plans settle: whether the view-lifetime contract
is livable for a code generator or `bc_arena_copy` appears everywhere;
whether the sticky-error model leaves any loop unbounded; whether the
ephemeral bounds a compiler would choose are predictable.

## Deferred

- **Read-your-own-writes across statements.** An interactive transaction is
  a snapshot and an accumulating write plan; later statements' cursors read
  through an overlay of the earlier writes. The overlay is the session
  layer's and sits under `bc_get`, `bc_lookup` and the cursors without
  changing their signatures.
- **Sequence allocation.** Auto-increment is a runtime service invoked at
  commit, outside the plan, so the plan stays pure. Until then ids come from
  rows the plan reads and guards.
- **Spilling sorters and hash tables.** Bounds without spill are right for
  OLTP. An analytic workload would add a spilling variant behind the same
  calls.
- **The catalog in the database.** The first phase hard-codes it. Its
  persistent form is rows in namespace `0x01`, as in the plugin.
