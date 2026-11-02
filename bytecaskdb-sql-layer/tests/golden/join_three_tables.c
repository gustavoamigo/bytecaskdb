// SELECT t.id, p.v, r.s FROM t, p, r WHERE t.name = $1 AND p.a = t.qty AND p.b = $2 AND r.id = p.w
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, COL_1_0 t.id, COL_1_2 t.qty, TBL_4 p, COL_4_2 p.v, COL_4_3 p.w, TBL_7 r, COL_7_1 r.s
#include "bc_plan.h"
enum { IDX_3 = 3, COL_1_0 = 0, COL_1_2 = 2, TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3, TBL_7 = 7, COL_7_1 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (bc_col_null(cx, row, COL_1_2)) continue;
    bool found1 = false;
    bc_key k0 = bc_key_begin(cx, TBL_4);
    bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_1_2));
    bc_key_param(cx, k0, 1);
    bc_view row1 = bc_get(cx, bc_key_end(cx, k0), &found1);
    if (!found1) continue;
    bool found2 = false;
    bc_key k1 = bc_key_begin(cx, TBL_7);
    bc_key_i64(cx, k1, bc_col_i64(cx, row1, COL_4_3));
    bc_view row2 = bc_get(cx, bc_key_end(cx, k1), &found2);
    if (!found2) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1_0);
    bc_emit_col(cx, row1, COL_4_2);
    bc_emit_col(cx, row2, COL_7_1);
    bc_emit_end(cx);
  }
}
