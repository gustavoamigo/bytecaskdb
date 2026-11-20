// INSERT INTO t VALUES ($1, $2) RETURNING *
// schema: catalog v7
// objects: TBL_1 t, COL_1_0 t.id, COL_1_1 t.name, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, COL_1_0 = 0, COL_1_1 = 1, COL_1_2 = 2 };

void plan(bc_ctx *cx) {
  bc_row r = bc_row_begin(cx, TBL_1);
  bc_row_set_param(cx, r, COL_1_0, 0);
  bc_row_set_param(cx, r, COL_1_1, 1);
  bc_view nr = bc_row_end(cx, r);
  bc_insert(cx, TBL_1, nr);
  bc_emit_begin(cx);
  bc_emit_i64(cx, bc_col_i64(cx, nr, COL_1_0));
  if (bc_col_null(cx, nr, COL_1_1)) {
    bc_emit_null(cx);
  } else {
    bc_emit_bytes(cx, bc_col_bytes(cx, nr, COL_1_1));
  }
  if (bc_col_null(cx, nr, COL_1_2)) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, bc_col_i64(cx, nr, COL_1_2));
  }
  bc_emit_end(cx);
  bc_emit_count(cx, 1);
}
