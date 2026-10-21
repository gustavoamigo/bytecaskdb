// SELECT id FROM t WHERE name >= $1 FOR UPDATE
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, IDX_3 t_by_name, COL_1_0 t.id
#include "bc_plan.h"
enum { TBL_1 = 1, IDX_3 = 3, COL_1_0 = 0 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_guard_row(cx, TBL_1, row);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1_0);
    bc_emit_end(cx);
  }
}
