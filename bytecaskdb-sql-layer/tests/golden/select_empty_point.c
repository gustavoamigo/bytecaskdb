// SELECT FROM p WHERE a = $1 AND b = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p
#include "bc_plan.h"
enum { TBL_4 = 4 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_4, 0, 2), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  bc_emit_end(cx);
}
