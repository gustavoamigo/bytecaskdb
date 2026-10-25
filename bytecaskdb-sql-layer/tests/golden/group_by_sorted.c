// SELECT qty, max(id) FROM t GROUP BY qty ORDER BY qty DESC LIMIT 3
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  int64_t n = 0;
  bc_sorter s0 = bc_sorter_open(cx, 3);
  bc_agg a0 = bc_agg_open(cx, 3);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bc_sort_key gb0 = bc_sort_key_begin(cx);
      bc_sort_key_col(cx, gb0, row, COL_1_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_view gr0 = bc_sort_key_end(cx, gb0);
      bc_group gg0 = bc_agg_group(cx, a0, gr0);
      if (bc_agg_null(cx, gg0, 0)) {
        if (!bc_col_null(cx, row, COL_1_2)) bc_agg_min_i64(cx, gg0, 1, bc_col_i64(cx, row, COL_1_2));
      }
      bc_agg_count(cx, gg0, 0);
      bc_agg_max_i64(cx, gg0, 2, bc_col_i64(cx, row, COL_1_0));
    }
  }
  bc_view gk0 = bc_empty(cx);
  while (bc_agg_next(cx, a0, &gk0, 0)) {
    bc_group g0 = bc_agg_group(cx, a0, gk0);
    bc_sort_key sk = bc_sort_key_begin(cx);
    if (bc_agg_null(cx, g0, 1)) {
      bc_sort_key_null(cx, sk, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    } else {
      bc_sort_key_i64(cx, sk, bc_agg_i64(cx, g0, 1), BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    }
    bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), gk0);
  }
  bc_view p0 = bc_empty(cx);
  while (n < 3 && bc_sorter_next(cx, s0, &p0)) {
    bc_group g0 = bc_agg_group(cx, a0, p0);
    bc_emit_begin(cx);
    if (bc_agg_null(cx, g0, 1)) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, bc_agg_i64(cx, g0, 1));
    }
    if (bc_agg_null(cx, g0, 2)) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, bc_agg_i64(cx, g0, 2));
    }
    bc_emit_end(cx);
    ++n;
  }
}
