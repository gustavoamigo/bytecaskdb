// SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 FROM stock WHERE s_i_id = $1 AND s_w_id = $2
// isolation: snapshot   schema: catalog v13
// objects: TBL_22 stock, COL_22_2 stock.s_quantity, COL_22_3 stock.s_dist_01, COL_22_4 stock.s_dist_02, COL_22_5 stock.s_dist_03, COL_22_6 stock.s_dist_04, COL_22_7 stock.s_dist_05, COL_22_8 stock.s_dist_06, COL_22_9 stock.s_dist_07, COL_22_10 stock.s_dist_08, COL_22_11 stock.s_dist_09, COL_22_12 stock.s_dist_10, COL_22_16 stock.s_data
#include "bc_plan.h"
enum { TBL_22 = 22, COL_22_2 = 2, COL_22_3 = 3, COL_22_4 = 4, COL_22_5 = 5, COL_22_6 = 6, COL_22_7 = 7, COL_22_8 = 8, COL_22_9 = 9, COL_22_10 = 10, COL_22_11 = 11, COL_22_12 = 12, COL_22_16 = 16 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_22, 0, 2), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_22_2);
  bc_emit_col(cx, row, COL_22_16);
  bc_emit_col(cx, row, COL_22_3);
  bc_emit_col(cx, row, COL_22_4);
  bc_emit_col(cx, row, COL_22_5);
  bc_emit_col(cx, row, COL_22_6);
  bc_emit_col(cx, row, COL_22_7);
  bc_emit_col(cx, row, COL_22_8);
  bc_emit_col(cx, row, COL_22_9);
  bc_emit_col(cx, row, COL_22_10);
  bc_emit_col(cx, row, COL_22_11);
  bc_emit_col(cx, row, COL_22_12);
  bc_emit_end(cx);
}
