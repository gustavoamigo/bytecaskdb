// SELECT i_price, i_name, i_data FROM item WHERE i_id = $1
// isolation: snapshot   schema: catalog v13
// objects: TBL_20 item, COL_20_2 item.i_name, COL_20_3 item.i_price, COL_20_4 item.i_data
#include "bc_plan.h"
enum { TBL_20 = 20, COL_20_2 = 2, COL_20_3 = 3, COL_20_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_20, 0, 1), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_20_3);
  bc_emit_col(cx, row, COL_20_2);
  bc_emit_col(cx, row, COL_20_4);
  bc_emit_end(cx);
}
