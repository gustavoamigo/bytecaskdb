// SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3 ORDER BY c_first
// isolation: snapshot   schema: catalog v13
// objects: IDX_10 customer_i2, COL_8_0 customer.c_id, COL_8_3 customer.c_first, COL_8_4 customer.c_middle, COL_8_16 customer.c_balance
#include "bc_plan.h"
enum { IDX_10 = 10, COL_8_0 = 0, COL_8_3 = 3, COL_8_4 = 4, COL_8_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  {
    bc_key k0 = bc_key_begin(cx, IDX_10);
    bc_key_param(cx, k0, 2);
    bc_key_param(cx, k0, 1);
    bc_key_param(cx, k0, 0);
    bc_key_null(cx, k0);
    bc_view lo = bc_key_successor(cx, bc_key_end(cx, k0));
    bc_key k1 = bc_key_begin(cx, IDX_10);
    bc_key_param(cx, k1, 2);
    bc_key_param(cx, k1, 1);
    bc_key_param(cx, k1, 0);
    bc_view hi = bc_key_successor(cx, bc_key_end(cx, k1));
    bc_cursor c = bc_scan_open(cx, IDX_10, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_8_16);
      bc_emit_col(cx, row, COL_8_3);
      bc_emit_col(cx, row, COL_8_4);
      bc_emit_col(cx, row, COL_8_0);
      bc_emit_end(cx);
    }
  }
  {
    bc_key k2 = bc_key_begin(cx, IDX_10);
    bc_key_param(cx, k2, 2);
    bc_key_param(cx, k2, 1);
    bc_key_param(cx, k2, 0);
    bc_key_null(cx, k2);
    bc_view lo = bc_key_end(cx, k2);
    bc_cursor c = bc_scan_open(cx, IDX_10, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_8_16);
      bc_emit_col(cx, row, COL_8_3);
      bc_emit_col(cx, row, COL_8_4);
      bc_emit_col(cx, row, COL_8_0);
      bc_emit_end(cx);
    }
  }
}
