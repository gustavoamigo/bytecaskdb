// SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_8 customer, COL_8_3 customer.c_first, COL_8_4 customer.c_middle, COL_8_5 customer.c_last, COL_8_6 customer.c_street_1, COL_8_7 customer.c_street_2, COL_8_8 customer.c_city, COL_8_9 customer.c_state, COL_8_10 customer.c_zip, COL_8_11 customer.c_phone, COL_8_12 customer.c_since, COL_8_13 customer.c_credit, COL_8_14 customer.c_credit_lim, COL_8_15 customer.c_discount, COL_8_16 customer.c_balance
#include "bc_plan.h"
enum { TBL_8 = 8, COL_8_3 = 3, COL_8_4 = 4, COL_8_5 = 5, COL_8_6 = 6, COL_8_7 = 7, COL_8_8 = 8, COL_8_9 = 9, COL_8_10 = 10, COL_8_11 = 11, COL_8_12 = 12, COL_8_13 = 13, COL_8_14 = 14, COL_8_15 = 15, COL_8_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_8, 0, 3), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_8_3);
  bc_emit_col(cx, row, COL_8_4);
  bc_emit_col(cx, row, COL_8_5);
  bc_emit_col(cx, row, COL_8_6);
  bc_emit_col(cx, row, COL_8_7);
  bc_emit_col(cx, row, COL_8_8);
  bc_emit_col(cx, row, COL_8_9);
  bc_emit_col(cx, row, COL_8_10);
  bc_emit_col(cx, row, COL_8_11);
  bc_emit_col(cx, row, COL_8_13);
  bc_emit_col(cx, row, COL_8_14);
  bc_emit_col(cx, row, COL_8_15);
  bc_emit_col(cx, row, COL_8_16);
  bc_emit_col(cx, row, COL_8_12);
  bc_emit_end(cx);
}
