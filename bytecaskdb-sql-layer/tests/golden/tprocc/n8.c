// INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES ($1, $2, $3, $4, $5, $6, $7)
// isolation: snapshot   schema: catalog v13
// objects: TBL_15 orders, COL_15_0 orders.o_id, COL_15_1 orders.o_w_id, COL_15_2 orders.o_d_id, COL_15_3 orders.o_c_id, COL_15_5 orders.o_ol_cnt, COL_15_6 orders.o_all_local, COL_15_7 orders.o_entry_d
#include "bc_plan.h"
enum { TBL_15 = 15, COL_15_0 = 0, COL_15_1 = 1, COL_15_2 = 2, COL_15_3 = 3, COL_15_5 = 5, COL_15_6 = 6, COL_15_7 = 7 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_15);
  bc_row_set_param(cx, r, COL_15_0, 0);
  bc_row_set_param(cx, r, COL_15_1, 2);
  bc_row_set_param(cx, r, COL_15_2, 1);
  bc_row_set_param(cx, r, COL_15_3, 3);
  bc_row_set_param(cx, r, COL_15_5, 5);
  bc_row_set_param(cx, r, COL_15_6, 6);
  bc_row_set_param(cx, r, COL_15_7, 4);
  bc_insert(cx, TBL_15, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
