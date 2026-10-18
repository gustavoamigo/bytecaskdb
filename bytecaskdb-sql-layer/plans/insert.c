/* INSERT INTO t VALUES ($1, $2, $3)
   isolation: snapshot   schema: catalog v2 */
#include "bc_plan.h"
enum { TBL_T = 1, LAY_T = 2, IDX_T_BY_NAME = 3, T_ID_COL = 0, T_NAME_COL = 1, T_QTY_COL = 2 };

void plan(bc_ctx *cx) {
  bc_key k0 = bc_key_begin(cx, TBL_T);
  bc_key_i64(cx, k0, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, k0);
  bc_row r0 = bc_row_begin(cx, LAY_T);
  bc_row_set_i64(cx, r0, T_ID_COL, bc_param_i64(cx, 0));
  if (bc_param_null(cx, 1)) {
    bc_row_set_null(cx, r0, T_NAME_COL);
  } else {
    bc_row_set_bytes(cx, r0, T_NAME_COL, bc_param_bytes(cx, 1));
  }
  if (bc_param_null(cx, 2)) {
    bc_row_set_null(cx, r0, T_QTY_COL);
  } else {
    bc_row_set_i64(cx, r0, T_QTY_COL, bc_param_i64(cx, 2));
  }
  bc_view row = bc_row_end(cx, r0);
  bc_guard_absent(cx, pk);
  bc_put(cx, pk, row);
  bc_key k1 = bc_key_begin(cx, IDX_T_BY_NAME);
  if (bc_param_null(cx, 1)) {
    bc_key_null(cx, k1);
  } else {
    bc_key_bytes(cx, k1, bc_param_bytes(cx, 1));
  }
  bc_key_i64(cx, k1, bc_param_i64(cx, 0));
  bc_put(cx, bc_key_end(cx, k1), bc_empty(cx));
  bc_emit_count(cx, 1);
}
