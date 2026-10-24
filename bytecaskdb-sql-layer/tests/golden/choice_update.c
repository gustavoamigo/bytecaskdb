// UPDATE p SET v = $3 WHERE a = $1 AND w = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, IDX_6 p_w, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, IDX_6 = 6, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  int64_t n = 0;
  bc_key k0 = bc_key_begin(cx, IDX_6);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view lo0 = bc_key_end(cx, k0);
  bc_view hi0 = bc_key_successor(cx, lo0);
  int64_t count0 = bc_count_range(cx, IDX_6, lo0, hi0, 50001);
  if (count0 < 50001) {
    bc_cursor c = bc_scan_open(cx, IDX_6, lo0, lo0, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view old = bc_scan_row(cx, c);
      bc_row r = bc_row_from(cx, TBL_4, old);
      bc_row_set_param(cx, r, COL_4_2, 2);
      bc_update(cx, TBL_4, old, bc_row_end(cx, r));
      ++n;
    }
  } else {
    bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
    bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view old = bc_scan_row(cx, c);
      if (bc_col_null(cx, old, COL_4_3) || bc_param_null(cx, 1) || !(bc_col_i64(cx, old, COL_4_3) == bc_param_i64(cx, 1))) continue;
      bc_row r = bc_row_from(cx, TBL_4, old);
      bc_row_set_param(cx, r, COL_4_2, 2);
      bc_update(cx, TBL_4, old, bc_row_end(cx, r));
      ++n;
    }
  }
  bc_emit_count(cx, n);
}
