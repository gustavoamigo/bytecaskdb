// SELECT w, sum(v), avg(v) FROM p WHERE a = $1 GROUP BY w HAVING count(*) > $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_agg a0 = bc_agg_open(cx, 5);
  {
    bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
    bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bc_sort_key gb0 = bc_sort_key_begin(cx);
      bc_sort_key_col(cx, gb0, row, COL_4_3, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_view gr0 = bc_sort_key_end(cx, gb0);
      bc_group gg0 = bc_agg_group(cx, a0, gr0);
      if (bc_agg_null(cx, gg0, 0)) {
        bc_agg_min_i64(cx, gg0, 1, bc_col_i64(cx, row, COL_4_3));
      }
      bc_agg_count(cx, gg0, 0);
      if (!bc_col_null(cx, row, COL_4_2)) {
        bc_agg_add_i64(cx, gg0, 2, bc_col_i64(cx, row, COL_4_2));
      }
      if (!bc_col_null(cx, row, COL_4_2)) {
        bc_agg_add_i64(cx, gg0, 3, bc_col_i64(cx, row, COL_4_2));
        bc_agg_count(cx, gg0, 4);
      }
    }
  }
  bc_view gk0 = bc_empty(cx);
  while (bc_agg_next(cx, a0, &gk0, 0)) {
    bc_group g0 = bc_agg_group(cx, a0, gk0);
    if (!(bc_param_null(cx, 1) || !(bc_agg_i64(cx, g0, 0) > bc_param_i64(cx, 1)))) {
      bc_emit_begin(cx);
      bc_emit_i64(cx, bc_agg_i64(cx, g0, 1));
      if (bc_agg_null(cx, g0, 2)) {
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, bc_agg_i64(cx, g0, 2));
      }
      if (bc_agg_null(cx, g0, 3)) {
        bc_emit_null(cx);
        bc_emit_null(cx);
      } else {
        bc_emit_i64(cx, bc_agg_i64(cx, g0, 3));
        bc_emit_i64(cx, bc_agg_i64(cx, g0, 4));
      }
      bc_emit_end(cx);
    }
  }
}
