// SELECT min(v) FROM p WHERE a = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_begin(cx);
    bc_emit_null(cx);
    bc_emit_end(cx);
    return;
  }
  int64_t m = 0;
  bool seen = false;
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (!bc_col_null(cx, row, COL_4_2)) {
      int64_t v = bc_col_i64(cx, row, COL_4_2);
      if (!seen || v < m) m = v;
      seen = true;
    }
  }
  bc_emit_begin(cx);
  if (seen) {
    bc_emit_i64(cx, m);
  } else {
    bc_emit_null(cx);
  }
  bc_emit_end(cx);
}
