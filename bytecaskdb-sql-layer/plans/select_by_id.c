/* SELECT id, qty FROM t WHERE id = $1
   isolation: snapshot   schema: catalog v2 */
#include "bc_plan.h"
enum { TBL_T = 1, T_ID_OFF = 16, T_QTY_OFF = 24, T_QTY_COL = 2, LAY_RESULT = 1073741824 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_key k0 = bc_key_begin(cx, TBL_T);
  bc_key_i64(cx, k0, bc_param_i64(cx, 0));
  bool found = false;
  bc_view row = bc_get(cx, bc_key_end(cx, k0), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx, LAY_RESULT);
  bc_emit_i64(cx, bc_col_i64(cx, row, T_ID_OFF));
  if (bc_col_null(cx, row, T_QTY_COL)) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, bc_col_i64(cx, row, T_QTY_OFF));
  }
  bc_emit_end(cx);
}
