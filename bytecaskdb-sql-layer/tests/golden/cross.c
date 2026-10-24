// SELECT s FROM r, t WHERE r.id = $1 AND name = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_1 r.s, IDX_3 t_by_name
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_1 = 1, IDX_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_7, 0, 1), &found);
  if (!found) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 1, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row1 = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_7_1);
    bc_emit_end(cx);
  }
}
