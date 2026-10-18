/* DELETE FROM t WHERE id = $1
   isolation: snapshot   schema: catalog v2 */
#include "bc_plan.h"
enum { TBL_T = 1, IDX_T_BY_NAME = 3, T_NAME_SLOT = 0, T_NAME_COL = 1 };

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
  bc_del(cx, pk);
  bc_key k1 = bc_key_begin(cx, IDX_T_BY_NAME);
  if (bc_col_null(cx, old, T_NAME_COL)) {
    bc_key_null(cx, k1);
  } else {
    bc_key_bytes(cx, k1, bc_col_bytes(cx, old, T_NAME_SLOT));
  }
  bc_key_i64(cx, k1, bc_param_i64(cx, 0));
  bc_del(cx, bc_key_end(cx, k1));
  bc_emit_count(cx, 1);
}
