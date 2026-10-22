// UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3
// isolation: snapshot   schema: catalog v13
// objects: TBL_6 district, COL_6_2 district.d_ytd
#include "bc_plan.h"
enum { TBL_6 = 6, COL_6_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_6, 1, 2), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_6, old);
  if (bc_col_null(cx, old, COL_6_2) || bc_param_null(cx, 0)) {
    bc_row_set_null(cx, r, COL_6_2);
  } else {
    bc_row_set_i64(cx, r, COL_6_2, bc_add(cx, bc_col_i64(cx, old, COL_6_2), bc_param_i64(cx, 0)));
  }
  bc_update(cx, TBL_6, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
