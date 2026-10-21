// INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
// isolation: snapshot   schema: catalog v13
// objects: TBL_18 order_line, COL_18_0 order_line.ol_w_id, COL_18_1 order_line.ol_d_id, COL_18_2 order_line.ol_o_id, COL_18_3 order_line.ol_number, COL_18_4 order_line.ol_i_id, COL_18_6 order_line.ol_amount, COL_18_7 order_line.ol_supply_w_id, COL_18_8 order_line.ol_quantity, COL_18_9 order_line.ol_dist_info
#include "bc_plan.h"
enum { TBL_18 = 18, COL_18_0 = 0, COL_18_1 = 1, COL_18_2 = 2, COL_18_3 = 3, COL_18_4 = 4, COL_18_6 = 6, COL_18_7 = 7, COL_18_8 = 8, COL_18_9 = 9 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_18);
  bc_row_set_param(cx, r, COL_18_0, 2);
  bc_row_set_param(cx, r, COL_18_1, 1);
  bc_row_set_param(cx, r, COL_18_2, 0);
  bc_row_set_param(cx, r, COL_18_3, 3);
  bc_row_set_param(cx, r, COL_18_4, 4);
  bc_row_set_param(cx, r, COL_18_6, 7);
  bc_row_set_param(cx, r, COL_18_7, 5);
  bc_row_set_param(cx, r, COL_18_8, 6);
  bc_row_set_param(cx, r, COL_18_9, 8);
  bc_insert(cx, TBL_18, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
