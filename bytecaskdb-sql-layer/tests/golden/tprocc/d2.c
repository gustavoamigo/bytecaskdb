// DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_13 new_order
#include "bc_plan.h"
enum { TBL_13 = 13 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_13, 0, 3), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_delete(cx, TBL_13, old);
  bc_emit_count(cx, 1);
}
