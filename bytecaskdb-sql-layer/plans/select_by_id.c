/* SELECT id, qty FROM t WHERE id = $1
   isolation: snapshot   schema: catalog v2
   objects: TBL_1 t, COL_1_0 t.id, COL_1_2 t.qty */
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0_OFF = 20, COL_1_2_OFF = 28, COL_1_2_COL = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_key k0 = bc_key_begin(cx, TBL_1);
  bc_key_i64(cx, k0, bc_param_i64(cx, 0));
  bool found = false;
  bc_view row = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_i64(cx, bc_col_i64(cx, row, COL_1_0_OFF));
  if (bc_col_null(cx, row, COL_1_2_COL)) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, bc_col_i64(cx, row, COL_1_2_OFF));
  }
  bc_emit_end(cx);
}
