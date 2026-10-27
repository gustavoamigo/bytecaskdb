// SELECT qty FROM t EXCEPT ALL SELECT s FROM r
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_7 r, COL_7_1 r.s
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_7 = 7, COL_7_1 = 1 };

void plan(bc_ctx *cx) {
  bc_agg setu0 = bc_agg_open(cx, 2);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn1 = bc_col_null(cx, row, COL_7_1);
      int64_t setx2 = INT64_C(0);
      if (!setn1) setx2 = bc_col_i64(cx, row, COL_7_1);
      bc_sort_key setk3 = bc_sort_key_begin(cx);
      if (setn1) {
        bc_sort_key_null(cx, setk3, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk3, setx2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg4 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk3));
      bc_agg_add_i64(cx, setg4, 0, INT64_C(1));
    }
  }
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn5 = bc_col_null(cx, row, COL_1_2);
      int64_t setx6 = INT64_C(0);
      if (!setn5) setx6 = bc_col_i64(cx, row, COL_1_2);
      bc_sort_key setk7 = bc_sort_key_begin(cx);
      if (setn5) {
        bc_sort_key_null(cx, setk7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk7, setx6, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg8 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk7));
      if (bc_agg_i64(cx, setg8, 1) < bc_agg_i64(cx, setg8, 0)) {
        bc_agg_add_i64(cx, setg8, 1, INT64_C(1));
      } else {
        bc_emit_begin(cx);
        if (setn5) {
          bc_emit_null(cx);
        } else {
          bc_emit_i64(cx, setx6);
        }
        bc_emit_end(cx);
      }
    }
  }
}
