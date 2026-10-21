// UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4
// isolation: snapshot   schema: catalog v13
// objects: TBL_8 customer, COL_8_16 customer.c_balance
#include "bc_plan.h"
enum { TBL_8 = 8, COL_8_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 2) || bc_param_null(cx, 3)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_8, 1, 3), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_8, old);
  bc_row_set_param(cx, r, COL_8_16, 0);
  bc_update(cx, TBL_8, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
