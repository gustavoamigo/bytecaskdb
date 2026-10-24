// SELECT max(b) FROM p WHERE a = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, PART_4_1 p.b
#include "bc_plan.h"
enum { TBL_4 = 4, PART_4_1 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_begin(cx);
    bc_emit_null(cx);
    bc_emit_end(cx);
    return;
  }
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX | BC_SCAN_REVERSE);
  if (bc_scan_next(cx, c)) {
    bc_emit_begin(cx);
    bc_emit_key_part(cx, TBL_4, bc_scan_key(cx, c), PART_4_1);
    bc_emit_end(cx);
  } else {
    bc_emit_begin(cx);
    bc_emit_null(cx);
    bc_emit_end(cx);
  }
  bc_scan_close(cx, c);
}
