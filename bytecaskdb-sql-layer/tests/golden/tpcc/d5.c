// UPDATE order_line SET ol_delivery_d = $1 WHERE ol_o_id = $2 AND ol_d_id = $3 AND ol_w_id = $4
// isolation: snapshot   schema: catalog v13
// objects: TBL_18 order_line, COL_18_5 order_line.ol_delivery_d
#include "bc_plan.h"
enum { TBL_18 = 18, COL_18_5 = 5 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 2) || bc_param_null(cx, 3)) {
    bc_emit_count(cx, 0);
    return;
  }
  int64_t n = 0;
  bc_key k0 = bc_key_begin(cx, TBL_18);
  bc_key_param(cx, k0, 3);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_view lo = bc_key_end(cx, k0);
  bc_cursor c = bc_scan_open(cx, TBL_18, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view old = bc_scan_row(cx, c);
    bc_row r = bc_row_from(cx, TBL_18, old);
    bc_row_set_param(cx, r, COL_18_5, 0);
    bc_update(cx, TBL_18, old, bc_row_end(cx, r));
    ++n;
  }
  bc_emit_count(cx, n);
}
