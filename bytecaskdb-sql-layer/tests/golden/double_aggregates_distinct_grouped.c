// SELECT s, sum(DISTINCT d), count(DISTINCT d) FROM r GROUP BY s
// isolation: snapshot   schema: catalog v7
// objects: IDX_9 r_s_desc, COL_7_1 r.s, COL_7_2 r.d
#include "bc_plan.h"
enum { IDX_9 = 9, COL_7_1 = 1, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  bc_agg d0_0 = bc_agg_open(cx, 1);  // DISTINCT values seen per group
  bool st0 = false;
  int64_t kv0_0 = 0;
  bool kn0_0 = false;
  double x0_0 = 0;
  bool xs0_0 = false;
  int64_t x0_1 = 0;
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, IDX_9, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bool rn0_0 = bc_col_null(cx, row, COL_7_1);
    int64_t rk0_0 = 0;
    if (!rn0_0) rk0_0 = bc_col_i64(cx, row, COL_7_1);
    if (st0 && (rk0_0 != kv0_0 || (rn0_0 && !kn0_0) || (kn0_0 && !rn0_0))) {
      bc_emit_begin(cx);
      if (kn0_0) {
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, kv0_0);
      }
      if (!xs0_0) {
        bc_emit_null(cx);
      } else {
        bc_emit_f64(cx, x0_0);
      }
      bc_emit_i64(cx, x0_1);
      bc_emit_end(cx);
      x0_0 = 0;
      xs0_0 = false;
      x0_1 = 0;
      bc_agg_clear(cx, d0_0);
    }
    kv0_0 = rk0_0;
    kn0_0 = rn0_0;
    st0 = true;
    if (!bc_col_null(cx, row, COL_7_2)) {
      bc_sort_key dk0_0 = bc_sort_key_begin(cx);
      bc_sort_key_col(cx, dk0_0, row, COL_7_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_group dg0_0 = bc_agg_group(cx, d0_0, bc_sort_key_end(cx, dk0_0));
      if (bc_agg_null(cx, dg0_0, 0)) {
        bc_agg_count(cx, dg0_0, 0);
        x0_0 = bc_add_f64(cx, x0_0, bc_col_f64(cx, row, COL_7_2));
        xs0_0 = true;
        ++x0_1;
      }
    }
  }
  if (st0) {
    bc_emit_begin(cx);
    if (kn0_0) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, kv0_0);
    }
    if (!xs0_0) {
      bc_emit_null(cx);
    } else {
      bc_emit_f64(cx, x0_0);
    }
    bc_emit_i64(cx, x0_1);
    bc_emit_end(cx);
  }
}
