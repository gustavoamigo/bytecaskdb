// SELECT id FROM r WHERE s > $1 AND s <= $2
// isolation: snapshot   schema: catalog v7
// objects: IDX_9 r_s_desc, PART_9_0 r_s_desc.id
#include "bc_plan.h"
enum { IDX_9 = 9, PART_9_0 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_9, 1, 1);
  bc_view hi = bc_key_params(cx, IDX_9, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_9, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, IDX_9, key, PART_9_0);
    bc_emit_end(cx);
  }
}
