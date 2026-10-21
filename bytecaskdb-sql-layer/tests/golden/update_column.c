// UPDATE p SET v = w, w = $3 WHERE a = $1 AND b = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_4, 0, 2), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_4, old);
  bc_row_set_i64(cx, r, COL_4_2, bc_col_i64(cx, old, COL_4_3));
  bc_row_set_param(cx, r, COL_4_3, 2);
  bc_update(cx, TBL_4, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
