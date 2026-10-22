// SELECT o_c_id FROM orders WHERE o_id = $1 AND o_d_id = $2 AND o_w_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_15 orders, COL_15_3 orders.o_c_id
#include "bc_plan.h"
enum { TBL_15 = 15, COL_15_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 2) || bc_param_null(cx, 1) || bc_param_null(cx, 0)) {
    return;
  }
  bool found = false;
  bc_key k0 = bc_key_begin(cx, TBL_15);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view row = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_15_3);
  bc_emit_end(cx);
}
