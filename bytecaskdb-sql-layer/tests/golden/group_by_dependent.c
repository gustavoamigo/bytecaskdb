// SELECT id, qty, count(*) FROM t GROUP BY id
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  bool st0 = false;
  int64_t kv0_0 = 0;
  int64_t kv0_1 = 0;
  bool kn0_1 = false;
  int64_t x0_0 = 0;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    int64_t rk0_0 = bc_col_i64(cx, row, COL_1_0);
    bool rn0_1 = bc_col_null(cx, row, COL_1_2);
    int64_t rk0_1 = 0;
    if (!rn0_1) rk0_1 = bc_col_i64(cx, row, COL_1_2);
    if (st0 && (rk0_0 != kv0_0)) {
      bc_emit_begin(cx);
      bc_emit_i64(cx, kv0_0);
      if (kn0_1) {
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, kv0_1);
      }
      bc_emit_i64(cx, x0_0);
      bc_emit_end(cx);
      x0_0 = 0;
    }
    kv0_0 = rk0_0;
    kv0_1 = rk0_1;
    kn0_1 = rn0_1;
    st0 = true;
    ++x0_0;
  }
  if (st0) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, kv0_0);
    if (kn0_1) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, kv0_1);
    }
    bc_emit_i64(cx, x0_0);
    bc_emit_end(cx);
  }
}
