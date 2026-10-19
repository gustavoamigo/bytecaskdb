// INSERT INTO t VALUES ($1, $2, $3)
// isolation: snapshot   schema: catalog v2
// objects: TBL_1 t, LAY_2 t layout, IDX_3 t_by_name, COL_1_0 t.id, COL_1_1 t.name, COL_1_2 t.qty
#include "bc_plan.h"
enum { TBL_1 = 1, LAY_2 = 2, IDX_3 = 3, COL_1_0_COL = 0, COL_1_1_COL = 1, COL_1_2_COL = 2 };

void plan(bc_ctx *cx) {
  bc_key k0 = bc_key_begin(cx, TBL_1);
  bc_key_i64(cx, k0, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, k0);
  if (bc_exists(cx, pk)) {
    bc_raise(cx, BC_C_UNIQUE, TBL_1);
    return;
  }
  bc_row r0 = bc_row_begin(cx, LAY_2);
  bc_row_set_i64(cx, r0, COL_1_0_COL, bc_param_i64(cx, 0));
  if (bc_param_null(cx, 1)) {
    bc_row_set_null(cx, r0, COL_1_1_COL);
  } else {
    bc_row_set_bytes(cx, r0, COL_1_1_COL, bc_param_bytes(cx, 1));
  }
  if (bc_param_null(cx, 2)) {
    bc_row_set_null(cx, r0, COL_1_2_COL);
  } else {
    bc_row_set_i64(cx, r0, COL_1_2_COL, bc_param_i64(cx, 2));
  }
  bc_view row = bc_row_end(cx, r0);
  bc_guard_absent(cx, pk);
  bc_put(cx, pk, row);
  bc_key k1 = bc_key_begin(cx, IDX_3);
  if (bc_param_null(cx, 1)) {
    bc_key_null(cx, k1);
  } else {
    bc_key_bytes(cx, k1, bc_param_bytes(cx, 1));
  }
  bc_key_i64(cx, k1, bc_param_i64(cx, 0));
  bc_put(cx, bc_key_end(cx, k1), bc_empty(cx));
  bc_emit_count(cx, 1);
}
