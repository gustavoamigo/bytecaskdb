// SELECT id FROM t WHERE qty IN (SELECT s FROM r UNION SELECT w FROM p)
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty, TBL_7 r, COL_7_1 r.s, TBL_4 p, COL_4_3 p.w
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2, TBL_7 = 7, COL_7_1 = 1, TBL_4 = 4, COL_4_3 = 3 };

static void sub0(bc_ctx *cx) {
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
  {
    bc_view lo = bc_empty(cx);
    bc_view hi = bc_empty(cx);
    bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);
    while (bc_scan_next(cx, c)) {
      bc_view row = bc_scan_row(cx, c);
      bool setn5 = bc_col_null(cx, row, COL_4_3);
      int64_t setx6 = INT64_C(0);
      if (!setn5) setx6 = bc_col_i64(cx, row, COL_4_3);
      bc_sort_key setk7 = bc_sort_key_begin(cx);
      if (setn5) {
        bc_sort_key_null(cx, setk7, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      } else {
        bc_sort_key_i64(cx, setk7, setx6, BC_SORT_ASC, BC_SORT_NULLS_LAST);
      }
      bc_group setg8 = bc_agg_group(cx, setu0, bc_sort_key_end(cx, setk7));
      if (bc_agg_null(cx, setg8, 0)) {
        bc_agg_add_i64(cx, setg8, 0, INT64_C(1));
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

void plan(bc_ctx *cx) {
  bc_sub_begin(cx, 0, BC_SUB_SET, BC_T_I64, 0);
  sub0(cx);
  bc_sub_end(cx);
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (!(((!(bc_col_null(cx, row, COL_1_2)) && bc_sub_has_i64(cx, 0, bc_col_i64(cx, row, COL_1_2))) || (!bc_param_null(cx, 0) && bc_param_bool(cx, 0))) && (!bc_param_null(cx, 1) && bc_param_bool(cx, 1)))) continue;
    bc_emit_begin(cx);
    bc_emit_col(cx, row, COL_1_0);
    bc_emit_end(cx);
  }
}
