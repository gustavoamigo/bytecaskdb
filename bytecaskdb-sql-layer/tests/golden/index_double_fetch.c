// SELECT d FROM r WHERE d >= $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_10 r_d, COL_7_2 r.d
#include "bc_plan.h"
enum { IDX_10 = 10, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_10, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_10, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_7_2);
    bc_emit_end(cx);
  }
}
