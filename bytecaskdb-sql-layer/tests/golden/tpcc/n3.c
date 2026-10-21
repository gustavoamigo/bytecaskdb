// UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = $1 AND d_w_id = $2
// isolation: snapshot   schema: catalog v13
// objects: TBL_6 district, COL_6_4 district.d_next_o_id
#include "bc_plan.h"
enum { TBL_6 = 6, COL_6_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_key k0 = bc_key_begin(cx, TBL_6);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view old = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_6, old);
  if (bc_col_null(cx, old, COL_6_4)) {
    bc_row_set_null(cx, r, COL_6_4);
  } else {
    bc_row_set_i64(cx, r, COL_6_4, bc_add(cx, bc_col_i64(cx, old, COL_6_4), INT64_C(1)));
  }
  bc_update(cx, TBL_6, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
