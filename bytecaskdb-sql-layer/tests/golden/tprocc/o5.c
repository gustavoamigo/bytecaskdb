// SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_18 order_line, COL_18_4 order_line.ol_i_id, COL_18_5 order_line.ol_delivery_d, COL_18_6 order_line.ol_amount, COL_18_7 order_line.ol_supply_w_id, COL_18_8 order_line.ol_quantity
#include "bc_plan.h"
enum { TBL_18 = 18, COL_18_4 = 4, COL_18_5 = 5, COL_18_6 = 6, COL_18_7 = 7, COL_18_8 = 8 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  bc_key k0 = bc_key_begin(cx, TBL_18);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view lo = bc_key_end(cx, k0);
  bc_cursor c = bc_scan_open(cx, TBL_18, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_18_4);
    bc_emit_col(cx, row, COL_18_7);
    bc_emit_col(cx, row, COL_18_8);
    bc_emit_col(cx, row, COL_18_6);
    bc_emit_col(cx, row, COL_18_5);
    bc_emit_end(cx);
  }
}
