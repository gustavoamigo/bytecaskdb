/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Gustavo Amigo

   bc_plan.h — the plan API of docs/plan_api_design.md (revision 24). A plan is
   a function in a restricted subset of C that calls these functions and
   nothing else. Declarations are the document's, verbatim; bc_db, bc_catalog
   and bc_params are opaque here and built by the runtime
   (algebra/runtime.cppm, module bytecask.sql.runtime).

   Every call is implemented. */

#ifndef BC_PLAN_H
#define BC_PLAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc_ctx bc_ctx;                       /* opaque; one per executing plan */

/* Handles: an index and a generation, each in its own struct type. The
   generation is 64 bits: one is drawn per handle, and it never repeats
   within a context. */
typedef struct { uint32_t i; uint64_t g; } bc_view;   /* bytes the API owns */
typedef struct { uint32_t i; uint64_t g; } bc_cursor;
typedef struct { uint32_t i; uint64_t g; } bc_sorter;
typedef struct { uint32_t i; uint64_t g; } bc_hash;
typedef struct { uint32_t i; uint64_t g; } bc_hash_iter;
typedef struct { uint32_t i; uint64_t g; } bc_agg;
typedef struct { uint32_t i; uint64_t g; } bc_group;
typedef struct { uint32_t i; uint64_t g; } bc_key;    /* a table's or index's key under construction */
typedef struct { uint32_t i; uint64_t g; } bc_sort_key;  /* a sort key under construction */
typedef struct { uint32_t i; uint64_t g; } bc_row;    /* a row under construction */
/* A jsonb value: a view of one node of the binary layout (docs/jsonb_design.md,
   *Binary layout of a jsonb value*), without its format byte. Only bc_*jsonb*
   calls take or return one: its entry is tagged as jsonb, every jsonb call
   refuses a view that is not, and every bytes call one that is (BC_E_HANDLE),
   so text and binary never meet. */
typedef struct { uint32_t i; uint64_t g; } bc_jsonb;

typedef void (*bc_plan_fn)(bc_ctx *);               /* every plan has this signature */

enum bc_error_code {
  BC_OK = 0,
  BC_E_IO,          /* std::system_error from the engine */
  BC_E_CORRUPT,     /* CRC mismatch or unparseable record */
  BC_E_MEMORY,      /* an allocation failed (std::bad_alloc) */
  BC_E_BUDGET,      /* work budget or deadline exhausted, or the statement cancelled */
  BC_E_LIMIT,       /* key, value or write plan over the engine's limits, or the statement over its memory budget */
  BC_E_HANDLE,      /* stale, foreign or out-of-range handle, or a table, index or column the plan's
                       bc_schema_ref does not declare, or not of the type used: a plan bug */
  BC_E_ARITH,       /* integer overflow, division by zero, out-of-range conversion; its SQLSTATE in
                       bc_error_detail (BC_SQLSTATE), 0 for 22003 */
  BC_E_CONSTRAINT,  /* never retried; raised by the runtime (BC_C_UNIQUE from bc_insert and bc_update,
                       BC_C_RANGE and BC_C_NOT_NULL from the row builder) or by the plan with bc_raise */
  BC_E_LAYOUT,      /* plan incompatible with the live catalog, or a row violates its layout */
  BC_E_DEGRADED,    /* engine degraded; writes refused */
  BC_E_CLOSED,
  BC_E_UNKNOWN,     /* a commit failed and degraded the engine: its batch may have reached the data
                       file, so its outcome is unknown, never "nothing written" (SQLSTATE 08007) */
  BC_E_PATTERN,     /* bc_like reached a pattern's escape character at its end (SQLSTATE 22025) */
  BC_E_CARDINALITY, /* a BC_SUB_VALUE subquery emitted a second row, or bc_insert_or_existing
                       met a row this execution wrote (SQLSTATE 21000) */
  BC_E_READ_ONLY,   /* a write, guard or DDL in a transaction begun read-only (SQLSTATE 25006) */
  BC_E_DATA,        /* a value a call cannot take or make (SQLSTATE class 22 and others): its SQLSTATE
                       in bc_error_detail (BC_SQLSTATE), its DETAIL, HINT and CONTEXT in bc_error_note,
                       bc_error_hint and bc_error_context */
};

/* A SQLSTATE as bc_error_detail carries it for BC_E_DATA and BC_E_ARITH:
   PostgreSQL's MAKE_SQLSTATE, five characters of six bits each (the
   character less '0'), the first in the lowest bits, 30 bits in all. */
#define BC_SQLSTATE_SIXBIT(ch) (((ch) - '0') & 0x3Fu)
#define BC_SQLSTATE(c1, c2, c3, c4, c5)                                              \
  (BC_SQLSTATE_SIXBIT(c1) | (BC_SQLSTATE_SIXBIT(c2) << 6) | (BC_SQLSTATE_SIXBIT(c3) << 12) | \
   (BC_SQLSTATE_SIXBIT(c4) << 18) | (BC_SQLSTATE_SIXBIT(c5) << 24))

/* The isolation a transaction or an autocommit execution runs at
   (docs/transaction_design.md, *Serializable isolation*). SNAPSHOT: its
   reads are fixed by its snapshot and only its writes and guards are
   checked at commit. SERIALIZABLE: the runtime records every key and range
   its reads cover, and a commit that writes submits them as guards, so it
   commits only if what it read is unchanged at its commit. */
enum bc_isolation { BC_ISOLATION_SNAPSHOT = 0, BC_ISOLATION_SERIALIZABLE = 1 };

/* What a subquery's rows are folded into, from bc_sub_begin to bc_sub_end:
   parameters from `param` on, which the caller passes as NULL, and for
   BC_SUB_SET the set bc_sub_has_* tests. Each row is one column of `type`;
   an EXISTS row of any columns, none of them kept. */
enum bc_sub_kind {
  BC_SUB_VALUE = 0,   /* param: its one row's value, NULL for none; a second row is BC_E_CARDINALITY */
  BC_SUB_EXISTS = 1,  /* param: a BOOL, whether it emitted a row */
  BC_SUB_BOUNDS = 2,  /* param: the largest non-NULL value, param+1 the smallest (NULL for none);
                         param+2: NULL if a row was NULL, else FALSE; param+3: a BOOL, whether it emitted a row */
  BC_SUB_SET = 3,     /* the set of its non-NULL values; param and param+1 as BC_SUB_BOUNDS's +2 and +3 */
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
  BC_C_CHECK_OPTION,   /* a row written through a view WITH CHECK OPTION that a checked WHERE does not hold
                          true of; the object id is the view's (bc_raise_row) */
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
  const uint8_t *cancel;    /* nonzero once another thread cancels the statement, read with an
                               atomic load where deadline_ns is checked; null = none */
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

/* BC_T_NUM: PostgreSQL's numeric as its text (numeric_out), a view as BYTES
   is; a result, a subquery or a NUMERIC column's, never a key's part. A
   column of it is read with bc_col_bytes and set with bc_row_set_bytes or
   bc_row_set_param, which store the text as numeric_in reads it and refuse
   text that is no number (BC_C_RANGE). */
/* BC_T_JSONB: a jsonb value, a bc_jsonb; a result or a jsonb column's, never a
   key's part. Read with bc_col_jsonb, set with bc_row_set_jsonb or
   bc_row_set_param, emitted as its text (jsonb_out's). */
enum bc_type { BC_T_I64 = 1, BC_T_F64 = 2, BC_T_BOOL = 3, BC_T_BYTES = 4, BC_T_NUM = 5, BC_T_JSONB = 6 };

/* Runtime: creating, running and committing. Called by the server, never by a plan. */
bc_ctx *bc_ctx_new(bc_db *db, const bc_catalog *cat);
void    bc_ctx_free(bc_ctx *);
void    bc_ctx_begin(bc_ctx *, const bc_params *params, bc_budget budget);  /* takes the snapshot */
void    bc_ctx_run(bc_ctx *, bc_plan_fn plan, const bc_schema_ref *checked_against);
bool    bc_ctx_check(bc_ctx *, const bc_schema_ref *checked_against);      /* bc_ctx_run's verification alone */
enum bc_commit_result bc_ctx_commit(bc_ctx *, bool sync);
bool    bc_ctx_next_result(bc_ctx *, const uint8_t **row, uint32_t *len);     /* result rows, in emit order */
void    bc_ctx_reset(bc_ctx *);                                               /* drops snapshot, plan, arena */
/* A window on the next run's result rows (docs/cursor_design.md, *The replay
   cursor*), set after bc_ctx_begin* and before bc_ctx_run, once: the first
   `skip` rows the plan ends are dropped, unencoded and uncharged; the next
   `limit` are kept; after the last the run stops, a normal end, not an error.
   limit 0 stops it, once verified, before it reads anything. A stopped run leaves the
   statement's writes as the rows it reached made them: use it for reads.
   bc_ctx_emitted is the rows the plan ended, dropped ones included; when the
   run was not stopped it is the result's whole size. */
void    bc_ctx_window(bc_ctx *, uint64_t skip, uint64_t limit);
uint64_t bc_ctx_emitted(const bc_ctx *);
bool    bc_ctx_stopped(const bc_ctx *);
enum bc_error_code bc_error(const bc_ctx *);
const char *bc_error_message(const bc_ctx *);
const char *bc_error_note(const bc_ctx *);                 /* the error's DETAIL line: bc_raise_row's failing row; "" for none */
const char *bc_error_hint(const bc_ctx *);                 /* the error's HINT line; "" for none */
const char *bc_error_context(const bc_ctx *);              /* the error's CONTEXT line; "" for none */
uint32_t bc_error_detail(const bc_ctx *);                  /* BC_E_CONSTRAINT: the bc_constraint in the high byte, the object id (below 2^24) in the low 24 bits; BC_E_LAYOUT: a bc_layout_detail;
                                                              BC_E_DATA, BC_E_ARITH: the SQLSTATE (BC_SQLSTATE), 0 for none */
int64_t bc_ctx_affected(const bc_ctx *);                   /* what the last run's bc_emit_count recorded, or -1 */
int     bc_execute(bc_ctx *, bc_plan_fn, const bc_schema_ref *, const bc_params *,
                   bc_budget, bool sync, uint32_t max_retries);               /* the retry loop */
/* The isolation of the context's autocommit executions, from its next
   bc_ctx_run until set again; SNAPSHOT for a new context. An execution
   whose plan writes no table records nothing at any level: it commits
   nothing, and is serialized at its snapshot. A context begun on a
   transaction runs at the transaction's level instead. */
void    bc_ctx_set_isolation(bc_ctx *, enum bc_isolation);

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
   - after any other failed statement, the transaction is only freed, or
     rolled back to a mark (bc_txn_rollback_to);
   - bc_txn_commit with no statement open, once.

   A statement that fails, or that takes what the transaction holds past
   max_bytes (BC_E_LIMIT), aborts the transaction at bc_txn_add: it releases
   its snapshot and pin at once (bc_txn_catalog_version is then 0), unless
   a savepoint's mark keeps them (below). An abort
   and bc_txn_free detach the context, which keeps its error and its
   finished statement's results. bc_txn_commit touches no context and
   reports through the transaction (bc_txn_error, BC_E_UNKNOWN when the
   outcome is unknown). */
typedef struct bc_txn bc_txn;
/* `read_only`: every write, guard and DDL of its statements is BC_E_READ_ONLY,
   and at SERIALIZABLE it records nothing, since it can submit nothing. */
bc_txn *bc_txn_new(bc_db *db, const bc_catalog *cat, uint64_t max_bytes, enum bc_isolation isolation,
                   bool read_only);
/* Changes the level and the read-only flag before the transaction's first
   statement, cursor pin and savepoint, and before it has read anything:
   false, and nothing changed, after them. */
bool    bc_txn_set_mode(bc_txn *, enum bc_isolation isolation, bool read_only);
/* Whether the transaction has run or is running a statement, pinned a cursor
   or read anything: PostgreSQL's "any query" for SET TRANSACTION. */
bool    bc_txn_queried(const bc_txn *);
void    bc_txn_free(bc_txn *);                                                /* a transaction not committed writes nothing */
void    bc_ctx_begin_txn(bc_ctx *, bc_txn *, const bc_params *params, bc_budget budget);
bool    bc_txn_add(bc_ctx *);                     /* ends the statement: its writes join, or the transaction aborts */
uint64_t bc_txn_catalog_version(const bc_txn *);  /* the catalog version pinned at the first statement; 0 before
                                                     it and after an abort */
bool    bc_txn_repin(bc_txn *);                   /* before any statement was added, with no context attached:
                                                     drops the pin, so the next statement pins the live catalog
                                                     and a new snapshot; false, changing nothing, otherwise,
                                                     and while its read set holds a read */
/* Cursors (docs/cursor_design.md, *What a cursor sees*). A pin is the
   transaction's overlay as of the call, shared copy-on-write: the statements
   added later write to a copy and the pin never changes. The call also pins
   the snapshot and catalog if no statement did. A pin reserves the overlay's
   bound against max_bytes until it is released; one that would pass it is
   BC_E_LIMIT and aborts the transaction, any other failure also, and the
   call returns null (bc_txn_error says why). A pin is a read: a context
   begun on the transaction reads through it after bc_ctx_use_overlay, which
   is called after bc_ctx_begin_txn and before bc_ctx_run, and the statement
   must not write. bc_txn_repin is refused while a pin is live. Release every
   pin before bc_txn_free. */
typedef struct bc_overlay bc_overlay;
bc_overlay *bc_txn_overlay_pin(bc_txn *);
void    bc_txn_overlay_release(bc_overlay *);
void    bc_ctx_use_overlay(bc_ctx *, const bc_overlay *);
/* A hold (docs/cursor_design.md, WITH HOLD): what a cursor that outlives its
   transaction reads, owned by the cursor. bc_txn_hold takes, from a
   transaction with a live overlay pin, a clone of its snapshot, its pinned
   catalog and the pin's overlay, so the transaction's commit may consume its
   own snapshot and the transaction may end: a hold needs nothing of the
   transaction after the call. A hold keeps the engine's snapshot, and so every
   tree node and data file it reaches, until bc_txn_hold_free. Failure is as
   bc_txn_overlay_pin's: null, bc_txn_error says why, the transaction aborted.
   bc_ctx_begin_hold begins a context, which belongs to no transaction, on a
   hold: it reads the hold's snapshot through its overlay and writes nothing,
   so the statement must be a read. Free every context begun on a hold before
   bc_txn_hold_free; a hold may outlive its bc_txn and its pin. */
typedef struct bc_hold bc_hold;
bc_hold *bc_txn_hold(bc_txn *, bc_overlay *);
void    bc_txn_hold_free(bc_hold *);
void    bc_ctx_begin_hold(bc_ctx *, const bc_hold *, const bc_params *, bc_budget);
/* Savepoints (docs/transaction_design.md, *Savepoints*). A mark is the
   transaction as it stands between statements, with no context attached:
   its overlay, shared copy-on-write as a pin's, and its guards, claims,
   locks and tables. bc_txn_savepoint pushes one and returns its depth, 1
   for the oldest; 0 when refused (an aborted or ended transaction, a
   statement open) or out of memory, the transaction unchanged and
   bc_txn_error saying why. It charges nothing: the first write after it
   clones the overlay, and the copy the mark keeps counts against max_bytes
   from that statement on, as a pin's does (BC_E_LIMIT at the call that
   would pass it). An abort of a transaction with marks keeps its snapshot,
   catalog and marks, and goes back to its newest mark.
   bc_txn_rollback_to puts the transaction back as it stood at mark `depth`,
   which stays, and drops the marks above it; it takes an aborted
   transaction back to open. bc_txn_release drops mark `depth` and those
   above it, keeping the writes since; it needs an open transaction. Both
   leave the pins and the snapshot alone, return false for a mark that
   does not exist or a statement open, changing nothing, and allocate
   nothing. */
uint32_t bc_txn_savepoint(bc_txn *);
bool    bc_txn_rollback_to(bc_txn *, uint32_t depth);
bool    bc_txn_release(bc_txn *, uint32_t depth);
enum bc_commit_result bc_txn_commit(bc_txn *, bool sync);
enum bc_error_code bc_txn_error(const bc_txn *);  /* of the last bc_txn_commit, bc_txn_savepoint, bc_txn_rollback_to or bc_txn_release */
const char *bc_txn_error_message(const bc_txn *);
uint32_t bc_txn_error_detail(const bc_txn *);

/* Parameters and results */
int64_t  bc_param_i64(bc_ctx *, uint32_t i);
double   bc_param_f64(bc_ctx *, uint32_t i);
bool     bc_param_bool(bc_ctx *, uint32_t i);
bc_view  bc_param_bytes(bc_ctx *, uint32_t i);
bool     bc_param_null(bc_ctx *, uint32_t i);
bc_jsonb bc_param_jsonb(bc_ctx *, uint32_t i);   /* parameter i's text, parsed as jsonb_in reads it, once per
                                                    execution; lives until reset. Text that is no JSON is
                                                    BC_E_DATA (22P02, 22P05, 22003, 54001, 22021) */

void     bc_emit_begin(bc_ctx *);                     /* a row of the plan's result layout */
void     bc_emit_i64(bc_ctx *, int64_t);
void     bc_emit_f64(bc_ctx *, double);
void     bc_emit_bool(bc_ctx *, bool);
void     bc_emit_bytes(bc_ctx *, bc_view);
void     bc_emit_num(bc_ctx *, bc_view);              /* a BC_T_NUM column: numeric text, as bc_num_quotient's */
void     bc_emit_jsonb(bc_ctx *, bc_jsonb);           /* a BC_T_JSONB column: the value's text (jsonb_out's) */
void     bc_emit_null(bc_ctx *);
void     bc_emit_col(bc_ctx *, bc_view row, uint32_t col);                /* the column, NULL as NULL; a jsonb's text */
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
int64_t bc_add_bigint(bc_ctx *, int64_t, int64_t, uint32_t object_id);   /* leaves BIGINT: BC_E_CONSTRAINT,
                                                   BC_C_RANGE on object_id, "bigint out of range" */
int64_t bc_sub_bigint(bc_ctx *, int64_t, int64_t, uint32_t object_id);
enum bc_int_type { BC_SMALLINT = 2, BC_INTEGER = 4, BC_BIGINT = 8 };
int64_t bc_check_int(bc_ctx *, int64_t v, uint32_t int_type, uint32_t object_id);  /* v outside the type:
                                                   BC_E_CONSTRAINT, BC_C_RANGE on object_id; else v */
double  bc_i64_to_f64(int64_t);                 /* always defined */
double  bc_add_f64(bc_ctx *, double, double);   /* PostgreSQL's float8 sum: finite operands that overflow → error */
double  bc_min_f64(double, double);             /* PostgreSQL's order: NaN above +inf; of equal values (-0.0, 0.0) the second, as float8smaller */
double  bc_max_f64(double, double);
int     bc_cmp_f64(double, double);              /* -1, 0 or 1 in PostgreSQL's float8 order: -0.0 = 0.0, NaN = NaN and above +inf */
bool    bc_bytes_eq(bc_ctx *, bc_view, bc_view);
int     bc_bytes_cmp(bc_ctx *, bc_view, bc_view);       /* memcmp order */
bool    bc_bytes_prefix(bc_ctx *, bc_view prefix, bc_view);
/* Text built from values (docs/frontend_design.md, *Expressions over
   columns*), each a new view of bytes the statement holds:
   - bc_concat: a, then b;
   - bc_substr: PostgreSQL's substring(s FROM from FOR count) of UTF-8 text,
     in characters: a start before the first character takes from it, those
     before counting toward `count`; INT64_MAX is to the end; a negative
     count is BC_E_ARITH, "negative substring length not allowed" (22011);
   - bc_text_case: upper (true) or lower case, of ASCII letters only, as
     COLLATE "C" folds;
   - bc_trim: s without the characters of `set` from its start
     (BC_TRIM_LEADING), its end (BC_TRIM_TRAILING) or both; with
     BC_TRIM_SPACES the set is a space and `set` is not read.
   And numbers of text: bc_char_length, its characters; bc_octet_length, its
   bytes; bc_position, the 1-based character `needle` first starts at in s,
   0 for none and 1 for an empty needle. */
bc_view bc_concat(bc_ctx *, bc_view a, bc_view b);
/* jsonb and text, the two crossings between the forms (docs/jsonb_design.md,
   *One form for a jsonb value at each boundary*). bc_jsonb_in: text parsed as
   jsonb_in reads it (BC_E_DATA with its SQLSTATE, DETAIL and CONTEXT for text
   that is no JSON); bc_jsonb_text: the value's text (jsonb_out's) in a new
   view. Both charged; a text past 1 GiB - 1 is BC_E_LIMIT, before it is
   allocated. */
bc_jsonb bc_jsonb_in(bc_ctx *, bc_view text);
bc_view  bc_jsonb_text(bc_ctx *, bc_jsonb);
/* jsonb's accessors and casts out (docs/jsonb_design.md, stage 3). Each that
   can give SQL NULL returns false for it, with nothing written to *out; a
   NULL out is BC_E_HANDLE. A value found is a slice of j, with j's
   lifetime and no copy; a lookup reads only what it needs, O(log n) by key
   and O(1) by index, and a damaged value is BC_E_CORRUPT. A node is a
   jsonb value of its own: a scalar is an array of itself to an index.
   - bc_jsonb_field: j -> key, NULL for no such key or j no object;
   - bc_jsonb_element: j -> i, i from the end when negative (-1 the last);
     NULL outside the array or for an object;
   - the _text forms, ->> : a string's own bytes, de-escaped, with no copy;
     NULL for a JSON null; the text (jsonb_out's) of any other value;
   - bc_jsonb_path_in: a text[]'s text, read as array_in reads it, as a path
     (every dimension flat): 22P02 "malformed array literal" with its
     DETAIL, 54000, 2202E or 22003 as array_in (BC_E_DATA); the empty view
     is the empty path;
   - bc_jsonb_path_add: the path with one more element, NULL when `null`;
   - bc_jsonb_path: j #> path, as jsonb_get_element walks it: a key of an
     object, an index of an array (its text as strtoint reads it), NULL at a
     scalar, a NULL element or a missing step; the empty path is j. A view
     that is no path is BC_E_HANDLE;
   - bc_jsonb_typeof: jsonb_typeof's name, a new view;
   - bc_jsonb_array_length: its elements; a scalar or an object is
     BC_E_DATA 22023, "cannot get array length of a scalar" ("of a non-array");
   - the casts: a value of another type is BC_E_DATA 22023, "cannot cast
     jsonb string to type integer"; bc_jsonb_to_i64 rounds half away from
     zero, and outside `int_type` (enum bc_int_type) raises BC_C_RANGE on
     object_id, as bc_check_int; bc_jsonb_to_f64 reads the number's text as
     float8in (float4in, `real`), its range error BC_E_DATA 22003;
     bc_jsonb_to_num gives its text, numeric_out's (BC_T_NUM). */
bool     bc_jsonb_field(bc_ctx *, bc_jsonb j, bc_view key, bc_jsonb *out);
bool     bc_jsonb_field_text(bc_ctx *, bc_jsonb j, bc_view key, bc_view *out);
bool     bc_jsonb_element(bc_ctx *, bc_jsonb j, int64_t i, bc_jsonb *out);
bool     bc_jsonb_element_text(bc_ctx *, bc_jsonb j, int64_t i, bc_view *out);
bc_view  bc_jsonb_path_in(bc_ctx *, bc_view text);
bc_view  bc_jsonb_path_add(bc_ctx *, bc_view path, bc_view element, bool null);
bool     bc_jsonb_path(bc_ctx *, bc_jsonb j, bc_view path, bc_jsonb *out);
bool     bc_jsonb_path_text(bc_ctx *, bc_jsonb j, bc_view path, bc_view *out);
bc_view  bc_jsonb_typeof(bc_ctx *, bc_jsonb j);
int64_t  bc_jsonb_array_length(bc_ctx *, bc_jsonb j);
bool     bc_jsonb_to_bool(bc_ctx *, bc_jsonb j);
int64_t  bc_jsonb_to_i64(bc_ctx *, bc_jsonb j, uint32_t int_type, uint32_t object_id);
double   bc_jsonb_to_f64(bc_ctx *, bc_jsonb j, bool real);
bc_view  bc_jsonb_to_num(bc_ctx *, bc_jsonb j);
/* Casts computed per row (docs/planner_design.md, *Plan API calls for runtime
   casts*), each over the same functions the binder folds a constant's cast
   with (sql/text.cppm), so a constant, a parameter and a row's value cast
   alike. A SQL type is a bc_sql_type (the layer's SqlKind codes); a DECIMAL
   travels as its scaled value with its precision and scale.
   - bc_text_in_i64: text read by the input function of an integer, DECIMAL
     (rounded half away from zero to `scale`, checked against `precision`)
     or date or time type (UTC);
   - bc_text_in_f64: by float8in, or float4in for BC_TYPE_REAL (the REAL as
     a double);
   - bc_text_in_bool: by boolin;
   - bc_text_in_bytes: by byteain (BC_TYPE_BYTEA), or namein (BC_TYPE_NAME,
     cut to 63 bytes), into the step's arena;
   - bc_value_text: an integer, a DECIMAL at `scale`, a date or time type's
     output, or a BOOLEAN's (v 0 or 1) as booltext writes it ("true") at
     scale 0 and boolout ("t") at scale 1, into the step's arena;
   - bc_bytea_text: byteaout's hex text, into the step's arena;
   - bc_f64_to_int: rint, then the bc_int_type's range;
   - bc_decimal_rescale: a DECIMAL at from_scale (an integer at 0) to an
     integer type (to_type) or to DECIMAL(precision, to_scale), rounded half
     away from zero;
   - bc_decimal_to_f64: its decimal text read correctly rounded, as a double
     or (real) the nearest REAL;
   - bc_f64_to_decimal: float8_numeric (float4_numeric for real): 15 (6)
     significant digits read as DECIMAL(precision, scale);
   - bc_f64_to_f32: the nearest REAL, as a double; bc_i64_to_f32: an
     integer's nearest REAL, rounded once;
   - bc_temporal_cast: date to timestamp(tz) at midnight, timestamp(tz) to
     date or time, timestamp to timestamptz and back, time to timetz and
     back, timestamptz to timetz, as PostgreSQL in UTC.
   An integer or DECIMAL result outside its type is BC_E_CONSTRAINT,
   BC_C_RANGE on object_id, with PostgreSQL's message (and a DECIMAL's
   DETAIL in bc_error_note), as bc_check_int; every other error, text that
   is no value of the type included, is BC_E_DATA with its SQLSTATE,
   message and DETAIL (22P02, 22007, 22008, a float's 22003). An input
   helper charges its text's size to the statement while it reads it
   (54000 past the budget, before it parses); a type or scale outside these
   lists is BC_E_HANDLE. */
enum bc_sql_type {
  BC_TYPE_SMALLINT = 1, BC_TYPE_INTEGER, BC_TYPE_BIGINT, BC_TYPE_REAL, BC_TYPE_DOUBLE, BC_TYPE_BOOLEAN,
  BC_TYPE_BYTEA, BC_TYPE_TEXT, BC_TYPE_VARCHAR, BC_TYPE_DECIMAL, BC_TYPE_TIMESTAMP, BC_TYPE_DATE, BC_TYPE_TIME,
  BC_TYPE_NUMERIC, BC_TYPE_TIMESTAMPTZ, BC_TYPE_CHAR, BC_TYPE_NAME, BC_TYPE_TIMETZ, BC_TYPE_JSONB
};
int64_t bc_text_in_i64(bc_ctx *, bc_view text, uint32_t sql_type, uint32_t precision, uint32_t scale, uint32_t object_id);
double  bc_text_in_f64(bc_ctx *, bc_view text, uint32_t sql_type);
bool    bc_text_in_bool(bc_ctx *, bc_view text);
bc_view bc_text_in_bytes(bc_ctx *, bc_view text, uint32_t sql_type);
bc_view bc_value_text(bc_ctx *, int64_t v, uint32_t sql_type, uint32_t scale);
bc_view bc_bytea_text(bc_ctx *, bc_view b);
int64_t bc_f64_to_int(bc_ctx *, double v, uint32_t int_type, uint32_t object_id);
int64_t bc_decimal_rescale(bc_ctx *, int64_t v, uint32_t from_scale, uint32_t to_type, uint32_t precision, uint32_t to_scale,
                           uint32_t object_id);
double  bc_decimal_to_f64(bc_ctx *, int64_t v, uint32_t scale, bool real);
int64_t bc_f64_to_decimal(bc_ctx *, double v, bool real, uint32_t precision, uint32_t scale, uint32_t object_id);
double  bc_f64_to_f32(bc_ctx *, double v);
double  bc_i64_to_f32(int64_t v);
int64_t bc_temporal_cast(bc_ctx *, int64_t v, uint32_t from_type, uint32_t to_type);
/* PostgreSQL's AVG of an integer or a DECIMAL, as numeric text (BC_T_NUM):
   numeric_div of sum (a total at `scale`) by count, at the scale
   select_div_scale chooses, rounded half away from zero. count < 1 or a
   scale past 18 is BC_E_HANDLE. */
bc_view bc_num_quotient(bc_ctx *, int64_t sum, int64_t count, uint32_t scale);
bc_view bc_substr(bc_ctx *, bc_view s, int64_t from, int64_t count);
bc_view bc_text_case(bc_ctx *, bc_view s, bool upper);
enum bc_trim_how { BC_TRIM_LEADING = 1, BC_TRIM_TRAILING = 2, BC_TRIM_SPACES = 4 };
bc_view bc_trim(bc_ctx *, bc_view s, bc_view set, uint32_t how);
int64_t bc_char_length(bc_ctx *, bc_view s);
int64_t bc_octet_length(bc_ctx *, bc_view s);
int64_t bc_position(bc_ctx *, bc_view s, bc_view needle);
/* PostgreSQL 17's random(lo, hi): an integer drawn uniformly from lo to hi,
   both included; lo > hi is BC_E_ARITH, "lower bound must be less than or
   equal to upper bound" (22023). Not repeatable: each thread draws from its
   own generator, seeded at random. */
int64_t bc_random(bc_ctx *, int64_t lo, int64_t hi);
/* PostgreSQL 17's EXTRACT(field FROM v) of a value not NULL: v a DATE, TIME
   or TIMESTAMP (a TIMESTAMPTZ in UTC) as `source` says, the result a
   DECIMAL's scaled value, at scale 6 for SECOND and for EPOCH of a TIME,
   3 for MILLISECONDS, 0 otherwise. A field the source has no such value
   for (HOUR of a DATE), EPOCH or JULIAN of a timestamp (not built) or a
   code outside the enums is BC_E_HANDLE. */
enum bc_date_field {
  BC_FIELD_MICROSECONDS = 1, BC_FIELD_MILLISECONDS, BC_FIELD_SECOND, BC_FIELD_MINUTE, BC_FIELD_HOUR,
  BC_FIELD_DAY, BC_FIELD_MONTH, BC_FIELD_QUARTER, BC_FIELD_WEEK, BC_FIELD_YEAR, BC_FIELD_ISOYEAR,
  BC_FIELD_DECADE, BC_FIELD_CENTURY, BC_FIELD_MILLENNIUM, BC_FIELD_DOW, BC_FIELD_ISODOW, BC_FIELD_DOY,
  BC_FIELD_JULIAN, BC_FIELD_EPOCH, BC_FIELD_TIMEZONE, BC_FIELD_TIMEZONE_HOUR, BC_FIELD_TIMEZONE_MINUTE
};
enum bc_date_source { BC_SOURCE_DATE = 1, BC_SOURCE_TIME, BC_SOURCE_TIMESTAMP, BC_SOURCE_TIMESTAMPTZ };
int64_t bc_extract(bc_ctx *, int64_t v, uint32_t field, uint32_t source);
bool    bc_like(bc_ctx *, bc_view text, bc_view pattern, int32_t escape);  /* PostgreSQL's LIKE of UTF-8 text:
                                                   _ one character, % any run, the escape (a code point, -1 for
                                                   none) quoting the next; an escape it reaches at the pattern's
                                                   end is BC_E_PATTERN */
bool    bc_ilike(bc_ctx *, bc_view text, bc_view pattern, int32_t escape);  /* bc_like with ASCII letters of the
                                                   pattern's literals matching either case, as ILIKE under COLLATE "C" */
int32_t bc_like_escape(bc_ctx *, bc_view escape);  /* an ESCAPE's code point, -1 for '' (none); more than one
                                                   character is BC_E_PATTERN, "invalid escape string" */

/* Keys */
bc_key   bc_key_begin(bc_ctx *, uint32_t table_or_index);
void     bc_key_i64(bc_ctx *, bc_key, int64_t);
void     bc_key_f64(bc_ctx *, bc_key, double);
void     bc_key_bool(bc_ctx *, bc_key, bool);
void     bc_key_bytes(bc_ctx *, bc_key, bc_view);
void     bc_key_null(bc_ctx *, bc_key);
void     bc_key_param(bc_ctx *, bc_key, uint32_t i);  /* parameter i as the next part, NULL included; an integer
                                                       past the part's width is BC_E_LIMIT, as in bc_key_end */
bc_view  bc_key_end(bc_ctx *, bc_key);
bool     bc_key_fits(bc_ctx *, bc_key);  /* the key so far is within the engine's key ceiling and each integer
                                           within its part's width (2 bytes for SMALLINT, 4 for INTEGER and
                                           DATE): past either, no row has it, and bc_key_end is BC_E_LIMIT.
                                           Compiled plans ask it of keys built from a row's strings; the
                                           search keys a join only by an integer the part holds */
bc_view  bc_key_params(bc_ctx *, uint32_t table_or_index, uint32_t first, uint32_t n);  /* parts from params first..;
                                                                                         BC_E_LIMIT as bc_key_param */
bc_view  bc_key_successor(bc_ctx *, bc_view key);     /* smallest key greater than every key with this prefix */

/* Sort keys: what a sorter orders by, built from parts the plan chooses, each
   with a direction and a NULL placement. memcmp order on the key is
   PostgreSQL's ORDER BY order, part by part, with text compared bytewise
   (the layer's one collation). Each part is a marker byte, never inverted,
   then the value, inverted under DESC: NULLS FIRST or LAST whatever the
   direction. A sort key is no table's or index's: its handle is its own
   type, which bc_key_* do not take. A part that could take the key past
   UINT32_MAX bytes is BC_E_LIMIT. */
enum bc_sort_dir   { BC_SORT_ASC = 0, BC_SORT_DESC = 1 };
enum bc_sort_nulls { BC_SORT_NULLS_LAST = 0, BC_SORT_NULLS_FIRST = 1 };
bc_sort_key bc_sort_key_begin(bc_ctx *);
void     bc_sort_key_i64(bc_ctx *, bc_sort_key, int64_t, uint32_t dir, uint32_t nulls);
void     bc_sort_key_f64(bc_ctx *, bc_sort_key, double, uint32_t dir, uint32_t nulls);   /* -0.0 is 0.0; NaN above +inf */
void     bc_sort_key_bool(bc_ctx *, bc_sort_key, bool, uint32_t dir, uint32_t nulls);
void     bc_sort_key_bytes(bc_ctx *, bc_sort_key, bc_view, uint32_t dir, uint32_t nulls);
void     bc_sort_key_null(bc_ctx *, bc_sort_key, uint32_t dir, uint32_t nulls);
void     bc_sort_key_col(bc_ctx *, bc_sort_key, bc_view row, uint32_t col, uint32_t dir, uint32_t nulls);  /* NULL included */
bc_view  bc_sort_key_end(bc_ctx *, bc_sort_key);

/* Sort-key decoders: read a part back from a key built by bc_sort_key_*, the
   key itself or a copy of its bytes (a sorter's payload). The key carries no
   layout, so parts are read in order, 0 first: part n's type is learned when
   it is read, and reading part n+1 before n is BC_E_HANDLE, as is reading a
   part as a type other than the one it was read as. For each part, call
   bc_sort_key_part_null, then the typed decoder if it is not NULL (a typed
   decoder of a NULL part returns zero or empty). Only a part built BC_SORT_ASC
   decodes: a DESC part is inverted, and reads as another value. A DOUBLE
   reads back as the key holds it, so -0.0 is 0.0 and every NaN one NaN. A key
   that does not parse is BC_E_CORRUPT. A bytes part is unescaped into the
   arena, the step's, charged. */
int64_t  bc_sort_key_part_i64(bc_ctx *, bc_view key, uint32_t part);
double   bc_sort_key_part_f64(bc_ctx *, bc_view key, uint32_t part);
bool     bc_sort_key_part_bool(bc_ctx *, bc_view key, uint32_t part);
bc_view  bc_sort_key_part_bytes(bc_ctx *, bc_view key, uint32_t part);
bool     bc_sort_key_part_null(bc_ctx *, bc_view key, uint32_t part);

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
bc_view  bc_col_bytes(bc_ctx *, bc_view row, uint32_t col);  /* a sub-view; same lifetime as row; a NUMERIC's text */
bc_jsonb bc_col_jsonb(bc_ctx *, bc_view row, uint32_t col);  /* a jsonb column's node, its format byte checked
                                                                (BC_E_CORRUPT otherwise): a sub-view, same lifetime
                                                                as row */
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
void     bc_row_set_bytes(bc_ctx *, bc_row, uint32_t col, bc_view);  /* a NUMERIC column: text read as numeric_in */
void     bc_row_set_null(bc_ctx *, bc_row, uint32_t col);
void     bc_row_set_param(bc_ctx *, bc_row, uint32_t col, uint32_t i);   /* parameter i, NULL included; into a jsonb
                                                                            column, its text parsed (BC_E_DATA) */
void     bc_row_set_jsonb(bc_ctx *, bc_row, uint32_t col, bc_jsonb);    /* after the full check of the value
                                                                            (BC_E_CORRUPT), its format byte and node */
bc_view  bc_row_end(bc_ctx *, bc_row);
bc_view  bc_pin(bc_ctx *, bc_view);                 /* a copy that lives until reset */
bc_view  bc_null_row(bc_ctx *, uint32_t table);     /* a row of the table's layout, every column NULL: an outer
                                                       join's inner side where it matches nothing; lives until reset */

/* Subqueries: what the plan emits between bc_sub_begin and bc_sub_end is
   folded as `kind` says (enum bc_sub_kind), never a result row. One at a
   time; `sub` names its set for bc_sub_has_*, which lives until reset or
   until `sub` runs again, which starts it empty. bc_sub_end releases what
   the run made: its handles, sorters, hash tables and aggregates. */
void bc_sub_begin(bc_ctx *, uint32_t sub, int32_t kind, int32_t type, uint32_t param);  /* type: a bc_type */
void bc_sub_end(bc_ctx *);
/* An argument of a subquery's next run: sets parameter i, which the caller
   passed as NULL (another is BC_E_HANDLE), until it is set again. A view of
   the old value is then stale. */
void bc_sub_arg_null(bc_ctx *, uint32_t i);
void bc_sub_arg_i64(bc_ctx *, uint32_t i, int64_t);
void bc_sub_arg_f64(bc_ctx *, uint32_t i, double);
void bc_sub_arg_bool(bc_ctx *, uint32_t i, bool);
void bc_sub_arg_bytes(bc_ctx *, uint32_t i, bc_view);  /* copied */
bool bc_sub_has_i64(bc_ctx *, uint32_t sub, int64_t);   /* whether the BC_SUB_SET holds the value */
bool bc_sub_has_f64(bc_ctx *, uint32_t sub, double);    /* -0.0 is 0.0, and NaN is one value, as PostgreSQL's = */
bool bc_sub_has_bool(bc_ctx *, uint32_t sub, bool);
bool bc_sub_has_bytes(bc_ctx *, uint32_t sub, bc_view);

/* Ephemeral structures */
bc_sorter bc_sorter_open(bc_ctx *, int64_t keep);        /* keep the first `keep` in sort order; 0 = all */
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
void      bc_agg_clear(bc_ctx *, bc_agg);                              /* drops every group, releasing its bytes */
void      bc_agg_add_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);   /* overflow → BC_E_ARITH */
void      bc_agg_add_f64(bc_ctx *, bc_group, uint32_t slot, double);
void      bc_agg_min_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_max_i64(bc_ctx *, bc_group, uint32_t slot, int64_t);
void      bc_agg_min_f64(bc_ctx *, bc_group, uint32_t slot, double);
void      bc_agg_max_f64(bc_ctx *, bc_group, uint32_t slot, double);
void      bc_agg_min_bytes(bc_ctx *, bc_group, uint32_t slot, bc_view);  /* bytewise; the value copied and charged */
void      bc_agg_max_bytes(bc_ctx *, bc_group, uint32_t slot, bc_view);
void      bc_agg_count(bc_ctx *, bc_group, uint32_t slot);
bool      bc_agg_next(bc_ctx *, bc_agg, bc_view *group_key, bc_group *g);
int64_t   bc_agg_i64(bc_ctx *, bc_group, uint32_t slot);
double    bc_agg_f64(bc_ctx *, bc_group, uint32_t slot);
bc_view   bc_agg_bytes(bc_ctx *, bc_group, uint32_t slot);  /* a copy that lives until the step ends */
bool      bc_agg_null(bc_ctx *, bc_group, uint32_t slot);   /* no value folded in: SUM, MIN, MAX, AVG are NULL */

/* Writes: rows, with every index entry the runtime derives from them */
void bc_insert(bc_ctx *, uint32_t table, bc_view row);
/* INSERT … ON CONFLICT DO NOTHING: the row, inserted as bc_insert inserts it, unless its key under
   `arbiter` is taken, when nothing is written and it returns false. `arbiter` is the table (its
   primary key), one of its unique indexes, or BC_ARBITER_ANY (the primary key and every unique
   index); a named one stands for every unique key over its columns, as a set. Taken: a row this execution wrote has the key, or the view holds it and this execution
   did not vacate it. Deciding takes no claim (at SERIALIZABLE each arbiter key it probes is
   a read), and an arbiter key the row takes is no insert claim: a concurrent commit of the key is BC_CONFLICT. A key of another unique index is checked
   as bc_insert checks it (BC_C_UNIQUE). False, too, once an error is set. */
#define BC_ARBITER_ANY UINT32_C(0xFFFFFFFF)
bool bc_insert_on_conflict(bc_ctx *, uint32_t table, bc_view row, uint32_t arbiter);
/* INSERT … ON CONFLICT DO UPDATE: as bc_insert_on_conflict, but where the row's key under
   `arbiter` is taken, *existing is the row the view holds at that key, of the primary key first,
   then of the unique indexes in their order: the row the plan then updates with bc_update. A key a
   row this execution wrote holds, or one a row it wrote held in the view and kept, is
   BC_E_CARDINALITY: one statement affects a row once. A key its writes vacated is free. *existing is the empty view when the row is inserted or an
   error is set. */
bool bc_insert_or_existing(bc_ctx *, uint32_t table, bc_view row, uint32_t arbiter, bc_view *existing);
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
void bc_raise_row(bc_ctx *, uint32_t constraint, uint32_t object_id, uint32_t table, bc_view row);
                                                                    /* bc_raise, naming `row`, a built row of `table`,
                                                                       in bc_error_note */

#ifdef __cplusplus
}
#endif

#endif /* BC_PLAN_H */
