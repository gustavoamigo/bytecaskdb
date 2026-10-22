// SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1
// isolation: snapshot   schema: catalog v13
// objects: IDX_17 orders_i2, COL_15_0 orders.o_id, COL_15_4 orders.o_carrier_id, COL_15_7 orders.o_entry_d
#include "bc_plan.h"
enum { IDX_17 = 17, COL_15_0 = 0, COL_15_4 = 4, COL_15_7 = 7 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  int64_t n = 0;
  bc_key k0 = bc_key_begin(cx, IDX_17);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_key_param(cx, k0, 2);
  bc_view lo = bc_key_end(cx, k0);
  bc_cursor c = bc_scan_open(cx, IDX_17, lo, lo, BC_SCAN_PREFIX | BC_SCAN_REVERSE);
  while (n < 1 && bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_15_0);
    bc_emit_col(cx, row, COL_15_4);
    bc_emit_col(cx, row, COL_15_7);
    bc_emit_end(cx);
    ++n;
  }
}
