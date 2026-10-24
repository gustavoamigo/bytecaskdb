/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Gustavo Amigo

   bc_plan.h — the plan API of docs/plan_api_design.md (revision 6). A plan is
   a function in a restricted subset of C that calls these functions and
   nothing else. Declarations are the document's, verbatim; bc_db, bc_catalog
   and bc_params are opaque here and built by the runtime
   (algebra/runtime.cppm, module bytecask.sql.runtime).

   Implemented: the runtime, parameters and results, arithmetic, keys, reads,
   rows and columns, writes, guards, and the aggregates' groups (bc_agg_open
   with no slots, bc_agg_group, bc_agg_next). The other ephemeral structures
   (sorters, hash tables) and the aggregates' slots are not implemented yet:
   each of their calls sets BC_E_HANDLE and returns a neutral value. */

#ifndef BC_PLAN_H
#define BC_PLAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
typedef struct {
  uint32_t table, col;
  uint8_t type;
} bc_col_ref;

/* What a plan was compiled or last checked against, and the types of the
   rows it emits, in emit order. bc_ctx_run checks the objects and columns
   against the catalog version the context pinned (docs/catalog_design.md,
   *Compatibility*) on every run; the result layout is the plan's own and
   is not a catalog object. */
typedef struct {
  uint64_t catalog_version;                                       /* compiled or last checked against; for messages */
  const uint32_t   *objects_read;    uint32_t n_read;             /* tables and indexes it names */
  const uint32_t   *tables_written;  uint32_t n_written;          /* tables whose rows it writes */
  const bc_col_ref *cols;            uint32_t n_cols;             /* every column it reads or sets */
  const uint8_t    *result_types;    uint32_t n_result;           /* bc_type codes */
} bc_schema_ref;

enum bc_type { BC_T_I64 = 1, BC_T_F64 = 2, BC_T_BOOL = 3, BC_T_BYTES = 4 };

/* Runtime: creating, running and committing. Called by the server, never by a plan. */
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

/* Transactions (docs/transaction_design.md). Internal: the embedded API's
   Transaction is their only caller, and plans never call them. A
   transaction's statements read its snapshot, pinned at the first statement
   with its catalog version, through the overlay of the earlier statements'
   writes; bc_txn_commit submits their writes as one batch.

   Preconditions, which the caller keeps and the runtime does not defend
   (use out of this order is unsupported):
   - one context per transaction, running one statement at a time;
   - each statement is bc_ctx_begin_txn, bc_ctx_run, bc_txn_add, then
     bc_ctx_reset. A statement refused before it ran with BC_E_LAYOUT and
     BC_L_INCOMPATIBLE did nothing, and may instead be reset and begun again;
   - after any other failed statement, the transaction is only freed;
   - bc_txn_commit with no statement open, once.

   A statement that fails, or that takes what the transaction holds past
   max_bytes (BC_E_LIMIT), aborts the transaction at bc_txn_add: it releases
   its snapshot and pin at once (bc_txn_catalog_version is then 0). An abort
   and bc_txn_free detach the context, which keeps its error and its
   finished statement's results. bc_txn_commit touches no context and
   reports through the transaction (bc_txn_error, BC_E_UNKNOWN when the
   outcome is unknown). */
typedef struct bc_txn bc_txn;
bc_txn *bc_txn_new(bc_db *db, const bc_catalog *cat, uint64_t max_bytes);
void    bc_txn_free(bc_txn *);                                                /* a transaction not committed writes nothing */
void    bc_ctx_begin_txn(bc_ctx *, bc_txn *, const bc_params *params, bc_budget budget);
bool    bc_txn_add(bc_ctx *);                     /* ends the statement: its writes join, or the transaction aborts */
uint64_t bc_txn_catalog_version(const bc_txn *);  /* the catalog version pinned at the first statement; 0 before
                                                     it and after an abort */
bool    bc_txn_repin(bc_txn *);                   /* before any statement was added, with no context attached:
                                                     drops the pin, so the next statement pins the live catalog
                                                     and a new snapshot; false, changing nothing, otherwise */
enum bc_commit_result bc_txn_commit(bc_txn *, bool sync);
enum bc_error_code bc_txn_error(const bc_txn *);  /* of the last bc_txn_commit */
const char *bc_txn_error_message(const bc_txn *);
uint32_t bc_txn_error_detail(const bc_txn *);

/* Parameters and results */
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

/* Arithmetic and conversion */
int64_t bc_add(bc_ctx *, int64_t, int64_t);     /* overflow → error */
int64_t bc_sub(bc_ctx *, int64_t, int64_t);
int64_t bc_mul(bc_ctx *, int64_t, int64_t);
int64_t bc_div(bc_ctx *, int64_t, int64_t);     /* zero divisor, INT64_MIN / -1 → error */
int64_t bc_mod(bc_ctx *, int64_t, int64_t);
int64_t bc_neg(bc_ctx *, int64_t);              /* INT64_MIN → error */
int64_t bc_f64_to_i64(bc_ctx *, double);        /* NaN or out of range → error; truncates */
enum bc_int_type { BC_SMALLINT = 2, BC_INTEGER = 4 };
int64_t bc_check_int(bc_ctx *, int64_t v, uint32_t int_type, uint32_t object_id);  /* v outside the type:
                                                   BC_E_CONSTRAINT, BC_C_RANGE on object_id; else v */
double  bc_i64_to_f64(int64_t);                 /* always defined */
bool    bc_bytes_eq(bc_ctx *, bc_view, bc_view);
int     bc_bytes_cmp(bc_ctx *, bc_view, bc_view);       /* memcmp order */
bool    bc_bytes_prefix(bc_ctx *, bc_view prefix, bc_view);

/* Keys */
bc_key   bc_key_begin(bc_ctx *, uint32_t table_or_index);
void     bc_key_i64(bc_ctx *, bc_key, int64_t);
void     bc_key_f64(bc_ctx *, bc_key, double);
void     bc_key_bool(bc_ctx *, bc_key, bool);
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_param(bc_ctx *, bc_key, uint32_t i);  /* parameter i as the next part, NULL included */
bc_view  bc_key_end(bc_ctx *, bc_key);
bool     bc_key_fits(bc_ctx *, bc_key);  /* the key so far is within the engine's key ceiling: past it, no row has it */
bc_view  bc_key_params(bc_ctx *, uint32_t table_or_index, uint32_t first, uint32_t n);  /* parts from params first.. */
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */

/* Reads */
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

/* Rows and columns. A column is named by its permanent id, and must be
   declared in the plan's bc_schema_ref. */
bool     bc_col_null(bc_ctx *, bc_view row, uint32_t col);   /* the bitmap bit: clear means null, or a column the row predates;
                                                                set for a column the row does not hold, or whose value
                                                                runs past the row, BC_E_CORRUPT */
int64_t  bc_col_i64(bc_ctx *, bc_view row, uint32_t col);
double   bc_col_f64(bc_ctx *, bc_view row, uint32_t col);
bool     bc_col_bool(bc_ctx *, bc_view row, uint32_t col);
bc_view  bc_col_bytes(bc_ctx *, bc_view row, uint32_t col);  /* a sub-view; same lifetime as row */
bc_view  bc_col_key(bc_ctx *, bc_view row, uint32_t col);    /* the value as a mem-comparable key part, NULL
                                                                included: equal values give equal bytes. A
                                                                group key, with no key ceiling; same lifetime
                                                                as row */
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

/* Ephemeral structures */
bc_sorter bc_sorter_open(bc_ctx *);
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

/* Writes: rows, with every index entry the runtime derives from them */
void bc_insert(bc_ctx *, uint32_t table, bc_view row);
void bc_update(bc_ctx *, uint32_t table, bc_view old, bc_view row);
void bc_delete(bc_ctx *, uint32_t table, bc_view old);
void bc_del_range(bc_ctx *, uint32_t table, bc_view lo, bc_view hi);

/* Guards and raised errors */
void bc_guard_unchanged(bc_ctx *, bc_view key);
void bc_guard_row(bc_ctx *, uint32_t table, bc_view row);           /* bc_guard_unchanged on the row's primary key */
void bc_guard_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi);
void bc_guard_present(bc_ctx *, bc_view key);
void bc_guard_absent(bc_ctx *, bc_view key);
void bc_raise(bc_ctx *, uint32_t constraint, uint32_t object_id);   /* a user error: unwinds, never retried */

#ifdef __cplusplus
}
#endif

#endif /* BC_PLAN_H */
