// INSERT INTO t VALUES ($1, $2, $3)
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_1 t.name, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_1 = 1, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_1);
  bc_row_set_param(cx, r, COL_1_0, 0);
  bc_row_set_param(cx, r, COL_1_1, 1);
  bc_row_set_param(cx, r, COL_1_2, 2);
  bc_insert(cx, TBL_1, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}
