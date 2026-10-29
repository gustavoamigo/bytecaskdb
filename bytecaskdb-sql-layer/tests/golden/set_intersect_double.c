// SELECT qty FROM t INTERSECT SELECT d FROM r
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_7 r, COL_7_2 r.d
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_7 = 7, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  bc_agg setu0 = bc_agg_open(cx, 2);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn1 = bc_col_null(cx, row, COL_1_2);
      int64_t setx2 = INT64_C(0);
      if (!setn1) setx2 = bc_col_i64(cx, row, COL_1_2);
      double setx3 = bc_i64_to_f64(setx2);
      bc_sort_key setk4 = bc_sort_key_begin(cx);
      if (setn1) {
        bc_sort_key_null(cx, setk4, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk4, setx3, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg5 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk4));
      bc_agg_add_i64(cx, setg5, 0, INT64_C(1));
    }
  }
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn6 = bc_col_null(cx, row, COL_7_2);
      double setx7 = 0.0;
      if (!setn6) setx7 = bc_col_f64(cx, row, COL_7_2);
      bc_sort_key setk8 = bc_sort_key_begin(cx);
      if (setn6) {
        bc_sort_key_null(cx, setk8, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk8, setx7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg9 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk8));
      if (!bc_agg_null(cx, setg9, 0)) {
        if (bc_agg_null(cx, setg9, 1)) {
          bc_agg_add_i64(cx, setg9, 1, INT64_C(1));
          bc_emit_begin(cx);
          if (setn6) {
            bc_emit_null(cx);
          } else {
            bc_emit_f64(cx, setx7);
          }
          bc_emit_end(cx);
        }
      }
    }
  }
}
