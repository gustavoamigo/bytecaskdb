// DELETE FROM p WHERE a = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p
#include "bc_plan.h"
enum { TBL_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view old = bc_scan_row(cx, c);
    bc_delete(cx, TBL_4, old);
    ++n;
  }
  bc_emit_count(cx, n);
}
