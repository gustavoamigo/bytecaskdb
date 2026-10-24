// SELECT a, count(*), sum(v) FROM p GROUP BY a
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_0 p.a, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_0 = 0, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  bool st0 = false;
  int64_t kv0_0 = 0;
  int64_t x0_0 = 0;
  int64_t x0_1 = 0;
  bool xs0_1 = false;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    int64_t rk0_0 = bc_col_i64(cx, row, COL_4_0);
    if (st0 && (rk0_0 != kv0_0)) {
      bc_emit_begin(cx);
      bc_emit_i64(cx, kv0_0);
      bc_emit_i64(cx, x0_0);
      if (!xs0_1) {
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, x0_1);
      }
      bc_emit_end(cx);
      x0_0 = 0;
      x0_1 = 0;
      xs0_1 = false;
    }
    kv0_0 = rk0_0;
    st0 = true;
    ++x0_0;
    if (!bc_col_null(cx, row, COL_4_2)) {
      x0_1 = bc_add(cx, x0_1, bc_col_i64(cx, row, COL_4_2));
      xs0_1 = true;
    }
  }
  if (st0) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, kv0_0);
    bc_emit_i64(cx, x0_0);
    if (!xs0_1) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, x0_1);
    }
    bc_emit_end(cx);
  }
}
