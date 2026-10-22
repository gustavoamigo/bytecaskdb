// SELECT no_o_id FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 ORDER BY no_o_id LIMIT 1
// isolation: snapshot   schema: catalog v13
// objects: TBL_13 new_order, PART_13_2 new_order.no_o_id
#include "bc_plan.h"
enum { TBL_13 = 13, PART_13_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  int64_t n = 0;
  bc_view lo = bc_key_params(cx, TBL_13, 0, 2);
  bc_cursor c = bc_scan_open(cx, TBL_13, lo, lo, BC_SCAN_PREFIX);
  while (n < 1 && bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, TBL_13, key, PART_13_2);
    bc_emit_end(cx);
    ++n;
  }
}
