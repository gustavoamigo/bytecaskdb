// SELECT id FROM r WHERE d >= $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_10 r_d, PART_10_0 r_d.id
#include "bc_plan.h"
enum { IDX_10 = 10, PART_10_0 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_10, 0, 1);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_10, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, IDX_10, key, PART_10_0);
    bc_emit_end(cx);
  }
}
