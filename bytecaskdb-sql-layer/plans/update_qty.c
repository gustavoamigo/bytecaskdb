/* UPDATE t SET qty = qty - $2 WHERE id = $1
   isolation: snapshot   schema: catalog v2 */
#include "bc_plan.h"
enum { TBL_T = 1, LAY_T = 2, T_QTY_OFF = 28, T_QTY_COL = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_key k0 = bc_key_begin(cx, TBL_T);
  bc_key_i64(cx, k0, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, k0);
  bool found = false;
  bc_view old = bc_get(cx, pk, &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r0 = bc_row_from(cx, LAY_T, old);
  if (bc_col_null(cx, old, T_QTY_COL) || bc_param_null(cx, 1)) {
    bc_row_set_null(cx, r0, T_QTY_COL);
  } else {
    bc_row_set_i64(cx, r0, T_QTY_COL, bc_sub(cx, bc_col_i64(cx, old, T_QTY_OFF), bc_param_i64(cx, 1)));
  }
  bc_put(cx, pk, bc_row_end(cx, r0));
  bc_emit_count(cx, 1);
}
