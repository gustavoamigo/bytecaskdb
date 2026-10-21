// SELECT v FROM p WHERE a = $1 AND b = $2
// isolation: snapshot   schema: catalog v7
// objects: TBL_4 p, COL_4_2 p.v
#include "bc_plan.h"
enum { TBL_4 = 4, COL_4_2 = 2 };

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
  bc_emit_col(cx, row, COL_4_2);
  bc_emit_end(cx);
}
