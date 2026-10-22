// SELECT d_next_o_id FROM district WHERE d_w_id = $1 AND d_id = $2
// isolation: snapshot   schema: catalog v13
// objects: TBL_6 district, COL_6_4 district.d_next_o_id
#include "bc_plan.h"
enum { TBL_6 = 6, COL_6_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_6, 0, 2), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_6_4);
  bc_emit_end(cx);
}
