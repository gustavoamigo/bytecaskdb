// UPDATE t SET qty = $1 WHERE id = $2 RETURNING id, qty + 1
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 1)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_1, 1, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, TBL_1, old);
  bc_row_set_param(cx, r, COL_1_2, 0);
  bc_view nr = bc_row_end(cx, r);
  bc_update(cx, TBL_1, old, nr);
  int64_t v0 = 0;
  bool n0 = bc_col_null(cx, nr, COL_1_2) || bc_param_null(cx, 2);
  if (!n0) {
    v0 = bc_add_bigint(cx, bc_col_i64(cx, nr, COL_1_2), bc_param_i64(cx, 2), 0);
  }
  bc_emit_begin(cx);
  bc_emit_i64(cx, bc_col_i64(cx, nr, COL_1_0));
  if (n0) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v0);
  }
  bc_emit_end(cx);
  bc_emit_count(cx, 1);
}
