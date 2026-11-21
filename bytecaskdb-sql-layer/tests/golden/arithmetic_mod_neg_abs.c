// SELECT id, qty % 3, -qty, abs(qty) FROM t WHERE id = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_1, 0, 1), &found);
  if (!found) {
    return;
  }
  int64_t v0 = 0;
  bool n0 = bc_col_null(cx, row, COL_1_2) || bc_param_null(cx, 1);
  if (!n0) {
    v0 = bc_mod(cx, bc_col_i64(cx, row, COL_1_2), bc_param_i64(cx, 1));
  }
  int64_t v1 = 0;
  bool n1 = bc_col_null(cx, row, COL_1_2);
  if (!n1) {
    v1 = bc_sub_bigint(cx, 0, bc_col_i64(cx, row, COL_1_2), 0);
  }
  int64_t v2 = 0;
  bool n2 = bc_col_null(cx, row, COL_1_2);
  if (!n2) {
    v2 = (bc_col_i64(cx, row, COL_1_2) < 0 ? bc_sub_bigint(cx, 0, bc_col_i64(cx, row, COL_1_2), 0) : bc_col_i64(cx, row, COL_1_2));
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_1_0);
  if (n0) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v0);
  }
  if (n1) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v1);
  }
  if (n2) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v2);
  }
  bc_emit_end(cx);
}
