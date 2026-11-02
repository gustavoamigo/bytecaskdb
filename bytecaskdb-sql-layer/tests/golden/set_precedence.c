// SELECT qty FROM t UNION SELECT s FROM r INTERSECT SELECT w FROM p
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_7 r, COL_7_1 r.s, TBL_4 p, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_7 = 7, COL_7_1 = 1, TBL_4 = 4, COL_4_3 = 3 };

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
      bc_sort_key setk3 = bc_sort_key_begin(cx);
      if (setn1) {
        bc_sort_key_null(cx, setk3, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk3, setx2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg4 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk3));
      if (bc_agg_null(cx, setg4, 0)) {
        bc_agg_add_i64(cx, setg4, 0, INT64_C(1));
        bc_emit_begin(cx);
        if (setn1) {
          bc_emit_null(cx);
        } else {
          bc_emit_i64(cx, setx2);
        }
        bc_emit_end(cx);
      }
    }
  }
  bc_agg setu5 = bc_agg_open(cx, 2);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn6 = bc_col_null(cx, row, COL_7_1);
      int64_t setx7 = INT64_C(0);
      if (!setn6) setx7 = bc_col_i64(cx, row, COL_7_1);
      bc_sort_key setk8 = bc_sort_key_begin(cx);
      if (setn6) {
        bc_sort_key_null(cx, setk8, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk8, setx7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg9 = bc_agg_group(cx, setu5, bc_sort_key_end(cx, setk8));
      bc_agg_add_i64(cx, setg9, 0, INT64_C(1));
    }
  }
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn10 = bc_col_null(cx, row, COL_4_3);
      int64_t setx11 = INT64_C(0);
      if (!setn10) setx11 = bc_col_i64(cx, row, COL_4_3);
      bc_sort_key setk12 = bc_sort_key_begin(cx);
      if (setn10) {
        bc_sort_key_null(cx, setk12, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk12, setx11, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg13 = bc_agg_group(cx, setu5, bc_sort_key_end(cx, setk12));
      if (!bc_agg_null(cx, setg13, 0)) {
        if (bc_agg_null(cx, setg13, 1)) {
          bc_agg_add_i64(cx, setg13, 1, INT64_C(1));
          bc_sort_key setk14 = bc_sort_key_begin(cx);
          if (setn10) {
            bc_sort_key_null(cx, setk14, BC_SORT_ASC, BC_SORT_NULLS_LAST);
          } else {
            bc_sort_key_i64(cx, setk14, setx11, BC_SORT_ASC, BC_SORT_NULLS_LAST);
          }
          bc_group setg15 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk14));
          if (bc_agg_null(cx, setg15, 0)) {
            bc_agg_add_i64(cx, setg15, 0, INT64_C(1));
            bc_emit_begin(cx);
            if (setn10) {
              bc_emit_null(cx);
            } else {
              bc_emit_i64(cx, setx11);
            }
            bc_emit_end(cx);
          }
        }
      }
    }
  }
}
