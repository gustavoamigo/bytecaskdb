// UPDATE customer SET c_balance = c_balance + $1 WHERE c_id = $2 AND c_d_id = $3 AND c_w_id = $4
// isolation: snapshot   schema: catalog v13
// objects: TBL_8 customer, COL_8_16 customer.c_balance
#include "bc_plan.h"
enum { TBL_8 = 8, COL_8_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 3) || bc_param_null(cx, 2) || bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_key k0 = bc_key_begin(cx, TBL_8);
  bc_key_param(cx, k0, 3);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_view old = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_8, old);
  if (bc_col_null(cx, old, COL_8_16) || bc_param_null(cx, 0)) {
    bc_row_set_null(cx, r, COL_8_16);
  } else {
    bc_row_set_i64(cx, r, COL_8_16, bc_add(cx, bc_col_i64(cx, old, COL_8_16), bc_param_i64(cx, 0)));
  }
  bc_update(cx, TBL_8, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
