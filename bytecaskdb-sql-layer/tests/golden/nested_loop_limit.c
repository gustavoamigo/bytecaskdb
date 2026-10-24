// SELECT t.id, p.v FROM t, p WHERE t.name = $1 AND p.w = t.qty LIMIT 10
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, COL_1_0 t.id, COL_1_2 t.qty, TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { IDX_3 = 3, COL_1_0 = 0, COL_1_2 = 2, TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (n < 10 && bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c1 = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
    while (n < 10 && bc_scan_next(cx, c1)) {
      bc_view row1 = bc_scan_row(cx, c1);
      if (bc_col_null(cx, row1, COL_4_3) || bc_col_null(cx, row, COL_1_2) || !(bc_col_i64(cx, row1, COL_4_3) == bc_col_i64(cx, row, COL_1_2))) continue;
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_1_0);
      bc_emit_col(cx, row1, COL_4_2);
      bc_emit_end(cx);
      ++n;
    }
    bc_scan_close(cx, c1);
  }
  bc_scan_close(cx, c);
}
