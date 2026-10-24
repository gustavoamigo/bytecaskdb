// SELECT p.v FROM r, p WHERE r.id = $1 AND p.a = $2 AND p.v = r.s ORDER BY p.b DESC NULLS LAST
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_1 r.s, TBL_4 p, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_1 = 1, TBL_4 = 4, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_7, 0, 1), &found);
  if (!found) {
    return;
  }
  bc_view lo = bc_key_params(cx, TBL_4, 1, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX | BC_SCAN_REVERSE);
  while (bc_scan_next(cx, c)) {
    bc_view row1 = bc_scan_row(cx, c);
    if (bc_col_null(cx, row1, COL_4_2) || bc_col_null(cx, row, COL_7_1) || !(bc_col_i64(cx, row1, COL_4_2) == bc_col_i64(cx, row, COL_7_1))) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, row1, COL_4_2);
    bc_emit_end(cx);
  }
}
