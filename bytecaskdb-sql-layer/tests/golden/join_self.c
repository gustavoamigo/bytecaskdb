// SELECT x.id, y.id FROM t x JOIN t y ON y.id = x.qty WHERE x.name = $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, COL_1073741824_0 x.id, COL_1073741824_2 x.qty, TBL_1073741825 y, COL_1073741825_0 y.id
#include "bc_plan.h"
enum { IDX_3 = 3, COL_1073741824_0 = 0, COL_1073741824_2 = 2, TBL_1073741825 = 1, COL_1073741825_0 = 0 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (bc_col_null(cx, row, COL_1073741824_2)) continue;
    bool found1 = false;
    bc_key k0 = bc_key_begin(cx, TBL_1073741825);
    bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_1073741824_2));
    bc_view row1 = bc_get(cx, bc_key_end(cx, k0), &found1);
    if (!found1) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1073741824_0);
    bc_emit_col(cx, row1, COL_1073741825_0);
    bc_emit_end(cx);
  }
}
