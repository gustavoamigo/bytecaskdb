// SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_8 customer, COL_8_20 customer.c_data
#include "bc_plan.h"
enum { TBL_8 = 8, COL_8_20 = 20 };

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
  bc_emit_col(cx, row, COL_8_20);
  bc_emit_end(cx);
}
