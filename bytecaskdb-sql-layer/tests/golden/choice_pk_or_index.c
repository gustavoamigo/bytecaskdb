// SELECT v FROM p WHERE a = $1 AND w = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, IDX_6 p_w, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, IDX_6 = 6, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  int64_t count0 = 0;
  {
    bc_key k0 = bc_key_begin(cx, IDX_6);
    bc_key_param(cx, k0, 1);
    bc_key_param(cx, k0, 0);
    bc_view lo = bc_key_end(cx, k0);
    bc_view hi = bc_key_successor(cx, lo);
    count0 = bc_count_range(cx, IDX_6, lo, hi, 50001);
  }
  if (count0 < 50001) {
    bc_key k1 = bc_key_begin(cx, IDX_6);
    bc_key_param(cx, k1, 1);
    bc_key_param(cx, k1, 0);
    bc_view lo = bc_key_end(cx, k1);
    bc_cursor c = bc_scan_open(cx, IDX_6, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_4_2);
      bc_emit_end(cx);
    }
  } else {
    bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
    bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      if (bc_col_null(cx, row, COL_4_3) || bc_param_null(cx, 1) || !(bc_col_i64(cx, row, COL_4_3) == bc_param_i64(cx, 1))) continue;
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_4_2);
      bc_emit_end(cx);
    }
  }
}
