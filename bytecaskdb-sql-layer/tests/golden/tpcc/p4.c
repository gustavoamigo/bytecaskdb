// SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district WHERE d_w_id = $1 AND d_id = $2
// isolation: snapshot   schema: catalog v13
// objects: TBL_6 district, COL_6_5 district.d_name, COL_6_6 district.d_street_1, COL_6_7 district.d_street_2, COL_6_8 district.d_city, COL_6_9 district.d_state, COL_6_10 district.d_zip
#include "bc_plan.h"
enum { TBL_6 = 6, COL_6_5 = 5, COL_6_6 = 6, COL_6_7 = 7, COL_6_8 = 8, COL_6_9 = 9, COL_6_10 = 10 };

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
  bc_emit_col(cx, row, COL_6_6);
  bc_emit_col(cx, row, COL_6_7);
  bc_emit_col(cx, row, COL_6_8);
  bc_emit_col(cx, row, COL_6_9);
  bc_emit_col(cx, row, COL_6_10);
  bc_emit_col(cx, row, COL_6_5);
  bc_emit_end(cx);
}
