// UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2
// isolation: snapshot   schema: catalog v13
// objects: TBL_4 warehouse, COL_4_1 warehouse.w_ytd
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_1 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_4, 1, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_4, old);
  if (bc_col_null(cx, old, COL_4_1) || bc_param_null(cx, 0)) {
    bc_row_set_null(cx, r, COL_4_1);
  } else {
    bc_row_set_i64(cx, r, COL_4_1, bc_add(cx, bc_col_i64(cx, old, COL_4_1), bc_param_i64(cx, 0)));
  }
  bc_update(cx, TBL_4, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
