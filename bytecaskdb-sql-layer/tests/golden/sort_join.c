// SELECT p.v, r.s FROM p, r WHERE p.a = $1 AND r.id = p.w ORDER BY r.s, p.v DESC
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v, COL_4_3 p.w, TBL_7 r, COL_7_1 r.s
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3, TBL_7 = 7, COL_7_1 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_sorter s0 = bc_sorter_open(cx, 0);
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bool found1 = false;
    bc_key k0 = bc_key_begin(cx, TBL_7);
    bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_4_3));
    bc_view row1 = bc_get(cx, bc_key_end(cx, k0), &found1);
    if (!found1) continue;
    bc_sort_key sk = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, sk, row1, COL_7_1, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sort_key_col(cx, sk, row, COL_4_2, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);
  }
  bc_view p0 = bc_empty(cx);
  while (bc_sorter_next(cx, s0, &p0)) {
    bool found1 = false;
    bc_key k1 = bc_key_begin(cx, TBL_7);
    bc_key_i64(cx, k1, bc_col_i64(cx, p0, COL_4_3));
    bc_view row1 = bc_get(cx, bc_key_end(cx, k1), &found1);
    if (!found1) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, p0, COL_4_2);
    bc_emit_col(cx, row1, COL_7_1);
    bc_emit_end(cx);
  }
}
