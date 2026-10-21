// INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1, $2, $3)
// isolation: snapshot   schema: catalog v13
// objects: TBL_13 new_order, COL_13_0 new_order.no_w_id, COL_13_1 new_order.no_d_id, COL_13_2 new_order.no_o_id
#include "bc_plan.h"
enum { TBL_13 = 13, COL_13_0 = 0, COL_13_1 = 1, COL_13_2 = 2 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_13);
  bc_row_set_param(cx, r, COL_13_0, 2);
  bc_row_set_param(cx, r, COL_13_1, 1);
  bc_row_set_param(cx, r, COL_13_2, 0);
  bc_insert(cx, TBL_13, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
