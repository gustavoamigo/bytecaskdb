// SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4
// isolation: snapshot   schema: catalog v13
// objects: TBL_18 order_line, COL_18_4 order_line.ol_i_id, TBL_22 stock, COL_22_0 stock.s_i_id, COL_22_2 stock.s_quantity
#include "bc_plan.h"
enum { TBL_18 = 18, COL_18_4 = 4, TBL_22 = 22, COL_22_0 = 0, COL_22_2 = 2 };

void plan(bc_ctx *cx) {
  int64_t v0 = 0;
  if (!bc_param_null(cx, 2)) v0 = bc_check_int(cx, bc_sub(cx, bc_param_i64(cx, 2), INT64_C(20)), BC_INTEGER, TBL_18);
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2) || bc_param_null(cx, 3)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, 0);
    bc_emit_end(cx);
    return;
  }
  bc_agg groups = bc_agg_open(cx, 0);
  bc_key k0 = bc_key_begin(cx, TBL_18);
  bc_key_param(cx, k0, 0);
  bc_key_param(cx, k0, 1);
  bc_key_i64(cx, k0, v0);
  bc_view lo = bc_key_end(cx, k0);
  bc_view hi = bc_key_params(cx, TBL_18, 0, 3);
  bc_cursor c = bc_scan_open(cx, TBL_18, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (bc_col_null(cx, row, COL_18_4)) continue;
    bool found1 = false;
    bc_key k1 = bc_key_begin(cx, TBL_22);
    bc_key_i64(cx, k1, bc_col_i64(cx, row, COL_18_4));
    bc_key_param(cx, k1, 0);
    bc_view row1 = bc_get(cx, bc_key_end(cx, k1), &found1);
    if (!found1) continue;
    if (bc_col_null(cx, row1, COL_22_2) || bc_param_null(cx, 3) || !(bc_col_i64(cx, row1, COL_22_2) < bc_param_i64(cx, 3))) continue;
    bc_agg_group(cx, groups, bc_col_key(cx, row1, COL_22_0));
  }
  int64_t n = 0;
  while (bc_agg_next(cx, groups, 0, 0)) ++n;
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
