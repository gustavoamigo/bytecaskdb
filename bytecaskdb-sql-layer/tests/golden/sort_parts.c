// SELECT a, b, v FROM p WHERE a = $1 ORDER BY v DESC, w NULLS FIRST, b
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_0 p.a, COL_4_1 p.b, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_0 = 0, COL_4_1 = 1, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_sorter s0 = bc_sorter_open(cx, 0);
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sort_key sk = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, sk, row, COL_4_2, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    bc_sort_key_col(cx, sk, row, COL_4_3, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_sort_key_col(cx, sk, row, COL_4_1, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);
  }
  bc_view p0 = bc_empty(cx);
  while (bc_sorter_next(cx, s0, &p0)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p0, COL_4_0);
    bc_emit_col(cx, p0, COL_4_1);
    bc_emit_col(cx, p0, COL_4_2);
    bc_emit_end(cx);
  }
}
