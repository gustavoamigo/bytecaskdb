// UPDATE t SET qty = qty - $2 WHERE id = $1
// isolation: snapshot   schema: catalog v2
// objects: TBL_1 t, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_1, 0, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_1, old);
  if (bc_col_null(cx, old, COL_1_2) || bc_param_null(cx, 1)) {
    bc_row_set_null(cx, r, COL_1_2);
  } else {
    bc_row_set_i64(cx, r, COL_1_2, bc_sub(cx, bc_col_i64(cx, old, COL_1_2), bc_param_i64(cx, 1)));
  }
  bc_update(cx, TBL_1, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
