// SELECT t.qty, count(*), sum(p.v) FROM t, p WHERE p.w = t.qty GROUP BY t.qty
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  bc_agg a0 = bc_agg_open(cx, 3);
  {
    bc_hash h0 = bc_hash_open(cx);  // built on TBL_4's rows
    {
      bc_view lo = bc_empty(cx);
      bc_view hi = bc_empty(cx);
      bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
      while (bc_scan_next(cx, c)) {
        bc_view row = bc_scan_row(cx, c);
        bc_sort_key hk0 = bc_sort_key_begin(cx);
        bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_4_3), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
        bc_hash_put(cx, h0, bc_sort_key_end(cx, hk0), row);
      }
    }
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      if (bc_col_null(cx, row, COL_1_2)) continue;
      bc_sort_key hk0 = bc_sort_key_begin(cx);
      bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_1_2), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_hash_iter q0 = bc_hash_find(cx, h0, bc_sort_key_end(cx, hk0));
      bc_view b0 = bc_empty(cx);
      while (bc_hash_next(cx, q0, &b0)) {
        bc_sort_key gb0 = bc_sort_key_begin(cx);
        bc_sort_key_col(cx, gb0, row, COL_1_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
        bc_view gr0 = bc_sort_key_end(cx, gb0);
        bc_group gg0 = bc_agg_group(cx, a0, gr0);
        if (bc_agg_null(cx, gg0, 0)) {
          if (!bc_col_null(cx, row, COL_1_2)) bc_agg_min_i64(cx, gg0, 1, bc_col_i64(cx, row, COL_1_2));
        }
        bc_agg_count(cx, gg0, 0);
        if (!bc_col_null(cx, b0, COL_4_2)) {
          bc_agg_add_i64(cx, gg0, 2, bc_col_i64(cx, b0, COL_4_2));
        }
      }
    }
  }
  bc_view gk0 = bc_empty(cx);
  while (bc_agg_next(cx, a0, &gk0, 0)) {
    bc_group g0 = bc_agg_group(cx, a0, gk0);
    bc_emit_begin(cx);
    if (bc_agg_null(cx, g0, 1)) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, bc_agg_i64(cx, g0, 1));
    }
    bc_emit_i64(cx, bc_agg_i64(cx, g0, 0));
    if (bc_agg_null(cx, g0, 2)) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, bc_agg_i64(cx, g0, 2));
    }
    bc_emit_end(cx);
  }
}
