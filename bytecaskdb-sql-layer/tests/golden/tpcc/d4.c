// UPDATE orders SET o_carrier_id = $1 WHERE o_id = $2 AND o_d_id = $3 AND o_w_id = $4
// isolation: snapshot   schema: catalog v13
// objects: TBL_15 orders, COL_15_4 orders.o_carrier_id
#include "bc_plan.h"
enum { TBL_15 = 15, COL_15_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 3) || bc_param_null(cx, 2) || bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_key k0 = bc_key_begin(cx, TBL_15);
  bc_key_param(cx, k0, 3);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_view old = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_15, old);
  bc_row_set_param(cx, r, COL_15_4, 0);
  bc_update(cx, TBL_15, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
