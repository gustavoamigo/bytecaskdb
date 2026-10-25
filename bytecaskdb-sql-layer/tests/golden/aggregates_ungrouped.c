// SELECT count(*), max(v), avg(v) FROM p WHERE a = $1 HAVING count(*) > $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    if (!bc_param_null(cx, 1) && INT64_C(0) > bc_param_i64(cx, 1)) {
      bc_emit_begin(cx);
      bc_emit_i64(cx, 0);
      bc_emit_null(cx);
      bc_emit_null(cx);
      bc_emit_null(cx);
      bc_emit_end(cx);
    }
    return;
  }
  int64_t x0_0 = 0;
  int64_t x0_1 = 0;
  bool xs0_1 = false;
  int64_t x0_2 = 0;
  bool xs0_2 = false;
  int64_t xc0_2 = 0;
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    ++x0_0;
    if (!bc_col_null(cx, row, COL_4_2)) {
      int64_t v0_1 = bc_col_i64(cx, row, COL_4_2);
      if (!xs0_1 || v0_1 > x0_1) x0_1 = v0_1;
      xs0_1 = true;
    }
    if (!bc_col_null(cx, row, COL_4_2)) {
      x0_2 = bc_add(cx, x0_2, bc_col_i64(cx, row, COL_4_2));
      xs0_2 = true;
      ++xc0_2;
    }
  }
  if (!(bc_param_null(cx, 1) || !(x0_0 > bc_param_i64(cx, 1)))) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, x0_0);
    if (!xs0_1) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, x0_1);
    }
    if (!xs0_2) {
      bc_emit_null(cx);
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, x0_2);
      bc_emit_i64(cx, xc0_2);
    }
    bc_emit_end(cx);
  }
}
