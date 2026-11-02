// SELECT d, s FROM r UNION ALL SELECT d, s FROM r ORDER BY s DESC NULLS LAST, d
// isolation: snapshot   schema: catalog v7
// objects: TBL_7 r, COL_7_1 r.s, COL_7_2 r.d
#include "bc_plan.h"
enum { TBL_7 = 7, COL_7_1 = 1, COL_7_2 = 2 };

void plan(bc_ctx *cx) {
  bc_sorter sets0 = bc_sorter_open(cx, 0);
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn1 = bc_col_null(cx, row, COL_7_2);
      double setx2 = 0.0;
      if (!setn1) setx2 = bc_col_f64(cx, row, COL_7_2);
      bool setn3 = bc_col_null(cx, row, COL_7_1);
      int64_t setx4 = INT64_C(0);
      if (!setn3) setx4 = bc_col_i64(cx, row, COL_7_1);
      bc_sort_key setk5 = bc_sort_key_begin(cx);
      if (setn3) {
        bc_sort_key_null(cx, setk5, BC_SORT_DESC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk5, setx4, BC_SORT_DESC, BC_SORT_NULLS_LAST);
      }
      if (setn1) {
        bc_sort_key_null(cx, setk5, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk5, setx2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_view setv6 = bc_sort_key_end(cx, setk5);
      bc_sort_key setk7 = bc_sort_key_begin(cx);
      if (setn1) {
        bc_sort_key_null(cx, setk7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk7, setx2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_sort_key_bool(cx, setk7, !setn1 && setx2 == 0.0 && 1.0 / setx2 < 0.0, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      if (setn3) {
        bc_sort_key_null(cx, setk7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk7, setx4, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_sorter_add(cx, sets0, setv6, bc_sort_key_end(cx, setk7));
    }
  }
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_7, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn8 = bc_col_null(cx, row, COL_7_2);
      double setx9 = 0.0;
      if (!setn8) setx9 = bc_col_f64(cx, row, COL_7_2);
      bool setn10 = bc_col_null(cx, row, COL_7_1);
      int64_t setx11 = INT64_C(0);
      if (!setn10) setx11 = bc_col_i64(cx, row, COL_7_1);
      bc_sort_key setk12 = bc_sort_key_begin(cx);
      if (setn10) {
        bc_sort_key_null(cx, setk12, BC_SORT_DESC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk12, setx11, BC_SORT_DESC, BC_SORT_NULLS_LAST);
      }
      if (setn8) {
        bc_sort_key_null(cx, setk12, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk12, setx9, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_view setv13 = bc_sort_key_end(cx, setk12);
      bc_sort_key setk14 = bc_sort_key_begin(cx);
      if (setn8) {
        bc_sort_key_null(cx, setk14, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_f64(cx, setk14, setx9, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_sort_key_bool(cx, setk14, !setn8 && setx9 == 0.0 && 1.0 / setx9 < 0.0, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      if (setn10) {
        bc_sort_key_null(cx, setk14, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk14, setx11, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_sorter_add(cx, sets0, setv13, bc_sort_key_end(cx, setk14));
    }
  }
  bc_view setp15 = bc_empty(cx);
  while (bc_sorter_next(cx, sets0, &setp15)) {
    bool setn16 = bc_sort_key_part_null(cx, setp15, 0);
    double setx17 = 0.0;
    if (!setn16) setx17 = bc_sort_key_part_f64(cx, setp15, 0);
    if (bc_sort_key_part_bool(cx, setp15, 1)) setx17 *= -1.0;
    bool setn18 = bc_sort_key_part_null(cx, setp15, 2);
    int64_t setx19 = INT64_C(0);
    if (!setn18) setx19 = bc_sort_key_part_i64(cx, setp15, 2);
    bc_emit_begin(cx);
    if (setn16) {
      bc_emit_null(cx);
    } else {
      bc_emit_f64(cx, setx17);
    }
    if (setn18) {
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, setx19);
    }
    bc_emit_end(cx);
  }
}
