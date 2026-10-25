// SELECT count(DISTINCT p.v) FROM t, p WHERE t.name > $1 AND p.w = t.qty
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, IDX_3 t_by_name, COL_1_2 t.qty, TBL_4 p, COL_4_2 p.v, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_1 = 1, IDX_3 = 3, COL_1_2 = 2, TBL_4 = 4, COL_4_2 = 2, COL_4_3 = 3 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, 0);
    bc_emit_end(cx);
    return;
  }
  bc_agg groups = bc_agg_open(cx, 0);
  bc_hash h0 = bc_hash_open(cx);  // built on TBL_1's rows
  {
    bc_view lo = bc_key_successor(cx, bc_key_params(cx, IDX_3, 0, 1));
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, IDX_3, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      if (bc_col_null(cx, row, COL_1_2)) continue;
      bc_sort_key hk0 = bc_sort_key_begin(cx);
      bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_1_2), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
      bc_hash_put(cx, h0, bc_sort_key_end(cx, hk0), row);
    }
  }
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sort_key hk0 = bc_sort_key_begin(cx);
    bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_4_3), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_hash_iter q0 = bc_hash_find(cx, h0, bc_sort_key_end(cx, hk0));
    bc_view b0 = bc_empty(cx);
    while (bc_hash_next(cx, q0, &b0)) {
      if (!bc_col_null(cx, row, COL_4_2)) bc_agg_group(cx, groups, bc_col_key(cx, row, COL_4_2));
    }
  }
  int64_t n = 0;
  while (bc_agg_next(cx, groups, 0, 0)) ++n;
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
