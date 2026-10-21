// SELECT sum(w) FROM p WHERE w >= $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_6 p_w, PART_6_3 p_w.w
#include "bc_plan.h"
enum { IDX_6 = 6, PART_6_3 = 0 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_begin(cx);
    bc_emit_null(cx);
    bc_emit_end(cx);
    return;
  }
  int64_t sum = 0;
  bool seen = false;
  bc_view lo = bc_key_params(cx, IDX_6, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_6, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    sum = bc_add(cx, sum, bc_key_part_i64(cx, IDX_6, key, PART_6_3));
    seen = true;
  }
  bc_emit_begin(cx);
  if (seen) {
    bc_emit_i64(cx, sum);
  } else {
    bc_emit_null(cx);
  }
  bc_emit_end(cx);
}
