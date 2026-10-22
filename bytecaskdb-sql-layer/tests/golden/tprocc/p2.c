// SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = $1
// isolation: snapshot   schema: catalog v13
// objects: TBL_4 warehouse, COL_4_3 warehouse.w_name, COL_4_4 warehouse.w_street_1, COL_4_5 warehouse.w_street_2, COL_4_6 warehouse.w_city, COL_4_7 warehouse.w_state, COL_4_8 warehouse.w_zip
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_3 = 3, COL_4_4 = 4, COL_4_5 = 5, COL_4_6 = 6, COL_4_7 = 7, COL_4_8 = 8 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_4, 0, 1), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_4_4);
  bc_emit_col(cx, row, COL_4_5);
  bc_emit_col(cx, row, COL_4_6);
  bc_emit_col(cx, row, COL_4_7);
  bc_emit_col(cx, row, COL_4_8);
  bc_emit_col(cx, row, COL_4_3);
  bc_emit_end(cx);
}
