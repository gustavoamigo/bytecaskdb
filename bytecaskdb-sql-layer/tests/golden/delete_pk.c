// DELETE FROM t WHERE id = $1
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t
#include "bc_plan.h"
enum { TBL_1 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    bc_emit_count(cx, 0);
    return;
  }
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, TBL_1, 0, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_delete(cx, TBL_1, old);
  bc_emit_count(cx, 1);
}
