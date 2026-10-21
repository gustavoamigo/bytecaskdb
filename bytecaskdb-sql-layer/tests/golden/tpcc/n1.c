// SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = $1 AND c_w_id = $1 AND c_d_id = $2 AND c_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_8 customer, COL_8_5 customer.c_last, COL_8_13 customer.c_credit, COL_8_15 customer.c_discount, TBL_4 warehouse, COL_4_2 warehouse.w_tax
#include "bc_plan.h"
enum { TBL_8 = 8, COL_8_5 = 5, COL_8_13 = 13, COL_8_15 = 15, TBL_4 = 4, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_8, 0, 3), &found);
  if (!found) {
    return;
  }
  bool found1 = false;
  bc_view row1 = bc_get(cx, bc_key_params(cx, TBL_4, 0, 1), &found1);
  if (!found1) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_8_15);
  bc_emit_col(cx, row, COL_8_5);
  bc_emit_col(cx, row, COL_8_13);
  bc_emit_col(cx, row1, COL_4_2);
  bc_emit_end(cx);
}
