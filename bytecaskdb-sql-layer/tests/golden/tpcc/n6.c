// UPDATE stock SET s_quantity = $1 WHERE s_i_id = $2 AND s_w_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_22 stock, COL_22_2 stock.s_quantity
#include "bc_plan.h"
enum { TBL_22 = 22, COL_22_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_22, 1, 2), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_22, old);
  bc_row_set_param(cx, r, COL_22_2, 0);
  bc_update(cx, TBL_22, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
