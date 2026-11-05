/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Gustavo Amigo

   bc_proc.h — what a procedure's file calls beyond the plan API
   (docs/procedures_design.md, *The procedure language*). A procedure file
   defines proc(), one s<i> per embedded statement, and their sub
   functions; proc() reaches its variables only through these calls, by slot
   number, and so holds no pointer.

   Once the procedure has failed, every call does nothing: bc_stmt_begin
   hands back a failed context, on which the plan API does nothing, and
   bc_loop and bc_range_next return false, so every loop ends and proc()
   returns. */

#ifndef BC_PROC_H
#define BC_PROC_H

#include "bc_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc_proc bc_proc;  /* opaque; one per running procedure */
typedef void (*bc_proc_fn)(bc_proc *);

/* The frame: a context whose parameter $n is slot n, a variable or a
   constant of the procedure. Expressions in proc() are evaluated on it. */
bc_ctx *bc_proc_ctx(bc_proc *);

/* Statement `stmt` of the procedure: begun on the procedure's transaction,
   its parameters set from the variables the statement's table names; then
   s<stmt> runs on the context it returns. bc_stmt_end adds its effects to
   the transaction, copies its first row into the INTO variables, and sets
   FOUND. */
bc_ctx *bc_stmt_begin(bc_proc *, uint32_t stmt);
void    bc_stmt_end(bc_proc *, uint32_t stmt);

/* var := value, checked against the variable's declared type (22003, 22001
   past it). A string is copied into the slot. */
void bc_var_set_i64(bc_proc *, uint32_t var, int64_t v);
void bc_var_set_f64(bc_proc *, uint32_t var, double v);
void bc_var_set_bool(bc_proc *, uint32_t var, bool v);
void bc_var_set_bytes(bc_proc *, uint32_t var, bc_view v);
void bc_var_set_null(bc_proc *, uint32_t var);

/* FOR targets IN query: after bc_stmt_end of the query's statement, each
   call copies its next row into the targets and is a pass of the loop,
   charged; false when the rows are done or the procedure has failed. The
   rows are the query's as it ran: the body's writes do not change them.
   bc_rows_done, after the loop however it ended, sets FOUND: whether the
   query returned a row. */
bool bc_rows_next(bc_proc *, uint32_t stmt);
void bc_rows_done(bc_proc *, uint32_t stmt);

/* A pass of WHILE or LOOP, charged to the procedure's budget: false once the
   procedure has failed or its budget is spent. */
bool bc_loop(bc_proc *);

/* FOR var IN lo..hi (REVERSE: hi down to lo), its bounds fixed here; a NULL
   bound is 22004. bc_range_next sets `var` to the next value and is a pass
   of the loop, charged; false when the range is done or the procedure has
   failed. `range` numbers the loop. */
void bc_range_open(bc_proc *, uint32_t range, int64_t lo, bool lo_null, int64_t hi, bool hi_null, bool reverse);
bool bc_range_next(bc_proc *, uint32_t range, uint32_t var);

#ifdef __cplusplus
}
#endif

#endif /* BC_PROC_H */
