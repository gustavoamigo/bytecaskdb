// UPDATE t SET name = $1 WHERE qty = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_1 t.name, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_1 = 1, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view old = bc_scan_row(cx, c);
    if (bc_col_null(cx, old, COL_1_2) || bc_param_null(cx, 1) || !(bc_col_i64(cx, old, COL_1_2) == bc_param_i64(cx, 1))) continue;
    bc_row r = bc_row_from(cx, TBL_1, old);
    bc_row_set_param(cx, r, COL_1_1, 0);
    bc_update(cx, TBL_1, old, bc_row_end(cx, r));
    ++n;
  }
  bc_emit_count(cx, n);
}
