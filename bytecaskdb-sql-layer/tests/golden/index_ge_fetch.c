// SELECT id, qty FROM t WHERE name >= $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { IDX_3 = 3, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1_0);
    bc_emit_col(cx, row, COL_1_2);
    bc_emit_end(cx);
  }
}
