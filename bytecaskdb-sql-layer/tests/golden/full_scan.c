// SELECT id, qty FROM t WHERE qty = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (bc_col_null(cx, row, COL_1_2) || bc_param_null(cx, 0) || !(bc_col_i64(cx, row, COL_1_2) == bc_param_i64(cx, 0))) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1_0);
    bc_emit_col(cx, row, COL_1_2);
    bc_emit_end(cx);
  }
}
