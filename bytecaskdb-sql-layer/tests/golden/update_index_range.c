// UPDATE p SET v = v + $2 WHERE w < $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, IDX_6 p_w, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, IDX_6 = 6, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_key_params(cx, IDX_6, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_6, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view old = bc_scan_row(cx, c);
    bc_row r = bc_row_from(cx, TBL_4, old);
    if (bc_col_null(cx, old, COL_4_2) || bc_param_null(cx, 1)) {
      bc_row_set_null(cx, r, COL_4_2);
    } else {
      bc_row_set_i64(cx, r, COL_4_2, bc_add(cx, bc_col_i64(cx, old, COL_4_2), bc_param_i64(cx, 1)));
    }
    bc_update(cx, TBL_4, old, bc_row_end(cx, r));
    ++n;
  }
  bc_emit_count(cx, n);
}
