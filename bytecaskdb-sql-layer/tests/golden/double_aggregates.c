// SELECT sum(d), avg(d), min(d), max(d) FROM r
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_2 r.d
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  double x0_0 = 0;
  bool xs0_0 = false;
  double x0_1 = 0;
  bool xs0_1 = false;
  int64_t xc0_1 = 0;
  double x0_2 = 0;
  bool xs0_2 = false;
  double x0_3 = 0;
  bool xs0_3 = false;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (!bc_col_null(cx, row, COL_7_2)) {
      x0_0 = bc_add_f64(cx, x0_0, bc_col_f64(cx, row, COL_7_2));
      xs0_0 = true;
    }
    if (!bc_col_null(cx, row, COL_7_2)) {
      x0_1 = bc_add_f64(cx, x0_1, bc_col_f64(cx, row, COL_7_2));
      xs0_1 = true;
      ++xc0_1;
    }
    if (!bc_col_null(cx, row, COL_7_2)) {
      double v0_2 = bc_col_f64(cx, row, COL_7_2);
      x0_2 = xs0_2 ? bc_min_f64(x0_2, v0_2) : v0_2;
      xs0_2 = true;
    }
    if (!bc_col_null(cx, row, COL_7_2)) {
      double v0_3 = bc_col_f64(cx, row, COL_7_2);
      x0_3 = xs0_3 ? bc_max_f64(x0_3, v0_3) : v0_3;
      xs0_3 = true;
    }
  }
  bc_emit_begin(cx);
  if (!xs0_0) {
    bc_emit_null(cx);
  } else {
    bc_emit_f64(cx, x0_0);
  }
  if (!xs0_1) {
    bc_emit_null(cx);
  } else {
    bc_emit_f64(cx, x0_1 / bc_i64_to_f64(xc0_1));
  }
  if (!xs0_2) {
    bc_emit_null(cx);
  } else {
    bc_emit_f64(cx, x0_2);
  }
  if (!xs0_3) {
    bc_emit_null(cx);
  } else {
    bc_emit_f64(cx, x0_3);
  }
  bc_emit_end(cx);
}
