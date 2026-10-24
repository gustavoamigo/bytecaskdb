// SELECT t.id FROM t, p WHERE t.name = $1 AND p.w = $2 ORDER BY p.v
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, COL_1_0 t.id, IDX_6 p_w, COL_4_2 p.v
#include "bc_plan.h"
enum { IDX_3 = 3, COL_1_0 = 0, IDX_6 = 6, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bc_sorter s0 = bc_sorter_open(cx, 0);
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_view lo = bc_key_params(cx, IDX_6, 1, 1);
    bc_cursor c1 = bc_scan_open(cx, IDX_6, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c1)) {
      bc_view row1 = bc_scan_row(cx, c1);
      bc_sort_key sk = bc_sort_key_begin(cx);
      bc_sort_key_col(cx, sk, row1, COL_4_2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);
    }
  }
  bc_view p0 = bc_empty(cx);
  while (bc_sorter_next(cx, s0, &p0)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p0, COL_1_0);
    bc_emit_end(cx);
  }
}
