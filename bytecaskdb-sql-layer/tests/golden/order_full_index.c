// SELECT id, d FROM r WHERE id >= $1 ORDER BY d DESC LIMIT 3
// isolation: snapshot   schema: catalog v7
// objects: IDX_10 r_d, COL_7_0 r.id, COL_7_2 r.d
#include "bc_plan.h"
enum { IDX_10 = 10, COL_7_0 = 0, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  int64_t n = 0;
  {
    bc_key k0 = bc_key_begin(cx, IDX_10);
    bc_key_null(cx, k0);
    bc_view lo = bc_key_end(cx, k0);
    bc_cursor c = bc_scan_open(cx, IDX_10, lo, lo, BC_SCAN_PREFIX | BC_SCAN_REVERSE);
    while (n < 3 && bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      if (bc_col_null(cx, row, COL_7_0) || bc_param_null(cx, 0) || !(bc_col_i64(cx, row, COL_7_0) >= bc_param_i64(cx, 0))) continue;
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_7_0);
      bc_emit_col(cx, row, COL_7_2);
      bc_emit_end(cx);
      ++n;
    }
    bc_scan_close(cx, c);
  }
  {
    bc_key k1 = bc_key_begin(cx, IDX_10);
    bc_key_null(cx, k1);
    bc_view lo = bc_key_successor(cx, bc_key_end(cx, k1));
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, IDX_10, lo, hi, BC_SCAN_REVERSE);
    while (n < 3 && bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      if (bc_col_null(cx, row, COL_7_0) || bc_param_null(cx, 0) || !(bc_col_i64(cx, row, COL_7_0) >= bc_param_i64(cx, 0))) continue;
      bc_emit_begin(cx);
      bc_emit_col(cx, row, COL_7_0);
      bc_emit_col(cx, row, COL_7_2);
      bc_emit_end(cx);
      ++n;
    }
    bc_scan_close(cx, c);
  }
}
