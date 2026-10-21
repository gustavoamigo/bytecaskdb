// SELECT a FROM p WHERE w < $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_6 p_w, PART_6_0 p_w.a
#include "bc_plan.h"
enum { IDX_6 = 6, PART_6_0 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_key_params(cx, IDX_6, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_6, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, IDX_6, key, PART_6_0);
    bc_emit_end(cx);
  }
}
