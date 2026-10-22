// SELECT d_next_o_id, d_tax FROM district WHERE d_id = $1 AND d_w_id = $2 FOR UPDATE
// isolation: snapshot   schema: catalog v13
// objects: TBL_6 district, COL_6_3 district.d_tax, COL_6_4 district.d_next_o_id
#include "bc_plan.h"
enum { TBL_6 = 6, COL_6_3 = 3, COL_6_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1) || bc_param_null(cx, 0)) {
    return;
  }
  bool found = false;
  bc_key k0 = bc_key_begin(cx, TBL_6);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view row = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    return;
  }
  bc_guard_row(cx, TBL_6, row);
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_6_4);
  bc_emit_col(cx, row, COL_6_3);
  bc_emit_end(cx);
}
