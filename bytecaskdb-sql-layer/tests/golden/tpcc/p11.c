// INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
// isolation: snapshot   schema: catalog v13
// objects: TBL_11 history, COL_11_0 history.h_id, COL_11_1 history.h_c_id, COL_11_2 history.h_c_d_id, COL_11_3 history.h_c_w_id, COL_11_4 history.h_d_id, COL_11_5 history.h_w_id, COL_11_6 history.h_date, COL_11_7 history.h_amount, COL_11_8 history.h_data
#include "bc_plan.h"
enum { TBL_11 = 11, COL_11_0 = 0, COL_11_1 = 1, COL_11_2 = 2, COL_11_3 = 3, COL_11_4 = 4, COL_11_5 = 5, COL_11_6 = 6, COL_11_7 = 7, COL_11_8 = 8 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_11);
  bc_row_set_param(cx, r, COL_11_0, 0);
  bc_row_set_param(cx, r, COL_11_1, 3);
  bc_row_set_param(cx, r, COL_11_2, 1);
  bc_row_set_param(cx, r, COL_11_3, 2);
  bc_row_set_param(cx, r, COL_11_4, 4);
  bc_row_set_param(cx, r, COL_11_5, 5);
  bc_row_set_param(cx, r, COL_11_6, 6);
  bc_row_set_param(cx, r, COL_11_7, 7);
  bc_row_set_param(cx, r, COL_11_8, 8);
  bc_insert(cx, TBL_11, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
