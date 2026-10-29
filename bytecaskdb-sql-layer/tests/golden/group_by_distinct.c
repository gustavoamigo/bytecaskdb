// SELECT a, count(DISTINCT v) FROM p GROUP BY a
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_0 p.a, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_0 = 0, COL_4_2 = 2 };

void plan(bc_ctx *cx) {
  bc_agg d0_0 = bc_agg_open(cx, 1);  // DISTINCT values seen per group
  bool st0 = false;
  int64_t kv0_0 = 0;
  int64_t x0_0 = 0;
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
      bc_emit_end(cx);
      x0_0 = 0;
      bc_agg_clear(cx, d0_0);
    }
    kv0_0 = rk0_0;
    st0 = true;
    if (!bc_col_null(cx, row, COL_4_2)) {
      bc_sort_key dk0_0 = bc_sort_key_begin(cx);
      bc_sort_key_col(cx, dk0_0, row, COL_4_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_group dg0_0 = bc_agg_group(cx, d0_0, bc_sort_key_end(cx, dk0_0));
      if (bc_agg_null(cx, dg0_0, 0)) {
        bc_agg_count(cx, dg0_0, 0);
        ++x0_0;
      }
    }
  }
  if (st0) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, kv0_0);
    bc_emit_i64(cx, x0_0);
    bc_emit_end(cx);
  }
}
