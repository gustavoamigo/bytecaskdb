/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Gustavo Amigo

   bc_plan.h — the plan API of docs/plan_api_design.md. A plan is a function
   in a restricted subset of C that calls these functions and nothing else.
   Declarations are the document's, verbatim; bc_db, bc_catalog and bc_params,
   which the document uses but does not declare, are opaque here and built by
   the runtime (runtime.hpp).

   prototype (#497): the calls the seven statements of the prototype need are
   implemented; every other call sets BC_E_HANDLE and returns a neutral value. */

#ifndef BC_PLAN_H
#define BC_PLAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc_ctx bc_ctx;                       /* opaque; one per executing plan */
typedef struct bc_db bc_db;                         /* opaque; an open engine */
typedef struct bc_catalog bc_catalog;               /* opaque; the live catalog */
typedef struct bc_params bc_params;                 /* opaque; bound parameters */

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

/* Runtime: creating, running and committing. Called by the server, never by a plan. */
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

/* Parameters and results */
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

/* Arithmetic and conversion */
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

/* Keys */
bc_key   bc_key_begin(bc_ctx *, uint32_t table_or_index);
void     bc_key_i64(bc_ctx *, bc_key, int64_t);
void     bc_key_f64(bc_ctx *, bc_key, double);
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_pk(bc_ctx *, bc_key, bc_view pk);     /* appends an already-encoded primary key */
bc_view  bc_key_end(bc_ctx *, bc_key);
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */

/* Reads */
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

/* Rows and columns */
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

/* Ephemeral structures */
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

/* Effects */
void bc_put(bc_ctx *, bc_view key, bc_view row);
void bc_del(bc_ctx *, bc_view key);
void bc_del_range(bc_ctx *, bc_view lo, bc_view hi);
void bc_guard_unchanged(bc_ctx *, bc_view key);
void bc_guard_range(bc_ctx *, uint32_t table_or_index, bc_view lo, bc_view hi);
void bc_guard_present(bc_ctx *, bc_view key);
void bc_guard_absent(bc_ctx *, bc_view key);

#ifdef __cplusplus
}
#endif

#endif /* BC_PLAN_H */
