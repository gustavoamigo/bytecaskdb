// SELECT qty FROM t UNION SELECT s FROM r ORDER BY 1 DESC LIMIT 2
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_7 r, COL_7_1 r.s
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_7 = 7, COL_7_1 = 1 };

void plan(bc_ctx *cx) {
  bc_sorter sets0 = bc_sorter_open(cx, 2);
  bc_agg setu1 = bc_agg_open(cx, 2);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn2 = bc_col_null(cx, row, COL_1_2);
      int64_t setx3 = INT64_C(0);
      if (!setn2) setx3 = bc_col_i64(cx, row, COL_1_2);
      bc_sort_key setk4 = bc_sort_key_begin(cx);
      if (setn2) {
        bc_sort_key_null(cx, setk4, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk4, setx3, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg5 = bc_agg_group(cx, setu1, bc_sort_key_end(cx, setk4));
      if (bc_agg_null(cx, setg5, 0)) {
        bc_agg_add_i64(cx, setg5, 0, INT64_C(1));
        bc_sort_key setk6 = bc_sort_key_begin(cx);
        if (setn2) {
          bc_sort_key_null(cx, setk6, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
        } else {
          bc_sort_key_i64(cx, setk6, setx3, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
        }
        bc_view setv7 = bc_sort_key_end(cx, setk6);
        bc_sort_key setk8 = bc_sort_key_begin(cx);
        if (setn2) {
          bc_sort_key_null(cx, setk8, BC_SORT_ASC, BC_SORT_NULLS_LAST);
        } else {
          bc_sort_key_i64(cx, setk8, setx3, BC_SORT_ASC, BC_SORT_NULLS_LAST);
        }
        bc_sorter_add(cx, sets0, setv7, bc_sort_key_end(cx, setk8));
      }
    }
  }
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn9 = bc_col_null(cx, row, COL_7_1);
      int64_t setx10 = INT64_C(0);
      if (!setn9) setx10 = bc_col_i64(cx, row, COL_7_1);
      bc_sort_key setk11 = bc_sort_key_begin(cx);
      if (setn9) {
        bc_sort_key_null(cx, setk11, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk11, setx10, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg12 = bc_agg_group(cx, setu1, bc_sort_key_end(cx, setk11));
      if (bc_agg_null(cx, setg12, 0)) {
        bc_agg_add_i64(cx, setg12, 0, INT64_C(1));
        bc_sort_key setk13 = bc_sort_key_begin(cx);
        if (setn9) {
          bc_sort_key_null(cx, setk13, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
        } else {
          bc_sort_key_i64(cx, setk13, setx10, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
        }
        bc_view setv14 = bc_sort_key_end(cx, setk13);
        bc_sort_key setk15 = bc_sort_key_begin(cx);
        if (setn9) {
          bc_sort_key_null(cx, setk15, BC_SORT_ASC, BC_SORT_NULLS_LAST);
        } else {
          bc_sort_key_i64(cx, setk15, setx10, BC_SORT_ASC, BC_SORT_NULLS_LAST);
        }
        bc_sorter_add(cx, sets0, setv14, bc_sort_key_end(cx, setk15));
      }
    }
  }
  bc_view setp16 = bc_empty(cx);
  while (bc_sorter_next(cx, sets0, &setp16)) {
    bool setn17 = bc_sort_key_part_null(cx, setp16, 0);
    int64_t setx18 = INT64_C(0);
    if (!setn17) setx18 = bc_sort_key_part_i64(cx, setp16, 0);
    bc_emit_begin(cx);
    if (setn17) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, setx18);
    }
    bc_emit_end(cx);
  }
}
