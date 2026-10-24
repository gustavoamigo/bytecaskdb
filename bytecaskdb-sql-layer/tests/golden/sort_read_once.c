// SELECT p.v FROM r, p WHERE r.id = $1 AND p.a = $2 AND p.w = r.s ORDER BY p.v NULLS FIRST LIMIT 5
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_1 r.s, TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_1 = 1, TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  int64_t n = 0;
  bc_sorter s0 = bc_sorter_open(cx, 5);
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_7, 0, 1), &found);
  if (!found) {
    return;
  }
  bc_view lo = bc_key_params(cx, TBL_4, 1, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row1 = bc_scan_row(cx, c);
    if (bc_col_null(cx, row1, COL_4_3) || bc_col_null(cx, row, COL_7_1) || !(bc_col_i64(cx, row1, COL_4_3) == bc_col_i64(cx, row, COL_7_1))) continue;
    bc_sort_key sk = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, sk, row1, COL_4_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row1);
  }
  bc_view p0 = bc_empty(cx);
  while (n < 5 && bc_sorter_next(cx, s0, &p0)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p0, COL_4_2);
    bc_emit_end(cx);
    ++n;
  }
}
