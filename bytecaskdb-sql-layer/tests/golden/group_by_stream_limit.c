// SELECT a, b, max(w) FROM p WHERE a = $1 GROUP BY a, b LIMIT 2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_0 p.a, COL_4_1 p.b, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_0 = 0, COL_4_1 = 1, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  int64_t n = 0;
  bool st0 = false;
  int64_t kv0_0 = 0;
  int64_t kv0_1 = 0;
  int64_t x0_0 = 0;
  bool xs0_0 = false;
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  while (n < 2 && bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    int64_t rk0_0 = bc_col_i64(cx, row, COL_4_0);
    int64_t rk0_1 = bc_col_i64(cx, row, COL_4_1);
    if (st0 && (rk0_0 != kv0_0 || rk0_1 != kv0_1)) {
      bc_emit_begin(cx);
      bc_emit_i64(cx, kv0_0);
      bc_emit_i64(cx, kv0_1);
      if (!xs0_0) {
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, x0_0);
      }
      bc_emit_end(cx);
      ++n;
      x0_0 = 0;
      xs0_0 = false;
    }
    kv0_0 = rk0_0;
    kv0_1 = rk0_1;
    st0 = true;
    int64_t v0_0 = bc_col_i64(cx, row, COL_4_3);
    if (!xs0_0 || v0_0 > x0_0) x0_0 = v0_0;
    xs0_0 = true;
  }
  bc_scan_close(cx, c);
  if (n < 2 && st0) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, kv0_0);
    bc_emit_i64(cx, kv0_1);
    if (!xs0_0) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, x0_0);
    }
    bc_emit_end(cx);
    ++n;
  }
}
