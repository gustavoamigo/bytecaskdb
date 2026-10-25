// SELECT a, b, v FROM p WHERE a = $1 ORDER BY b DESC LIMIT 2 FOR UPDATE
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_0 p.a, COL_4_1 p.b, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_0 = 0, COL_4_1 = 1, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX | BC_SCAN_REVERSE);
  while (n < 2 && bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_guard_row(cx, TBL_4, row);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_4_0);
    bc_emit_col(cx, row, COL_4_1);
    bc_emit_col(cx, row, COL_4_2);
    bc_emit_end(cx);
    ++n;
  }
  bc_scan_close(cx, c);
}
