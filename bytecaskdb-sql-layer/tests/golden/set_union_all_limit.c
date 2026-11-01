// SELECT qty FROM t WHERE id = $1 UNION ALL SELECT s FROM r WHERE id = $2 LIMIT 3
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_2 t.qty, TBL_7 r, COL_7_1 r.s
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_2 = 2, TBL_7 = 7, COL_7_1 = 1 };

void plan(bc_ctx *cx) {
  int64_t setcount = 0;
  {
    if (!(bc_param_null(cx, 0))) {
      bool found = false;
      bc_view row = bc_get(cx, bc_key_params(cx, TBL_1, 0, 1), &found);
      if (!(!found)) {
        bool setn0 = bc_col_null(cx, row, COL_1_2);
        int64_t setx1 = INT64_C(0);
        if (!setn0) setx1 = bc_col_i64(cx, row, COL_1_2);
        if (setcount < 3) {
          bc_emit_begin(cx);
          if (setn0) {
            bc_emit_null(cx);
          } else {
            bc_emit_i64(cx, setx1);
          }
          bc_emit_end(cx);
          setcount = bc_add(cx, setcount, INT64_C(1));
        }
      }
    }
  }
  {
    if (!(bc_param_null(cx, 1))) {
      bool found = false;
      bc_view row = bc_get(cx, bc_key_params(cx, TBL_7, 1, 1), &found);
      if (!(!found)) {
        bool setn2 = bc_col_null(cx, row, COL_7_1);
        int64_t setx3 = INT64_C(0);
        if (!setn2) setx3 = bc_col_i64(cx, row, COL_7_1);
        if (setcount < 3) {
          bc_emit_begin(cx);
          if (setn2) {
            bc_emit_null(cx);
          } else {
            bc_emit_i64(cx, setx3);
          }
          bc_emit_end(cx);
          setcount = bc_add(cx, setcount, INT64_C(1));
        }
      }
    }
  }
}
