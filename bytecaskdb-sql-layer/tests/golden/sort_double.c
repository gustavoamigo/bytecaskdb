// SELECT id, d FROM r WHERE id >= $1 ORDER BY d DESC, s LIMIT 3
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_0 r.id, COL_7_1 r.s, COL_7_2 r.d
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_0 = 0, COL_7_1 = 1, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  int64_t n = 0;
  bc_sorter s0 = bc_sorter_open(cx, 3);
  bc_view lo = bc_key_params(cx, TBL_7, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sort_key sk = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, sk, row, COL_7_2, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    bc_sort_key_col(cx, sk, row, COL_7_1, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);
  }
  bc_view p0 = bc_empty(cx);
  while (n < 3 && bc_sorter_next(cx, s0, &p0)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p0, COL_7_0);
    bc_emit_col(cx, p0, COL_7_2);
    bc_emit_end(cx);
    ++n;
  }
}
