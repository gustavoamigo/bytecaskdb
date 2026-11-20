// SELECT o_id, EXTRACT(YEAR FROM o_entry_d), EXTRACT(SECOND FROM o_entry_d), EXTRACT(ISODOW FROM o_entry_d) FROM orders WHERE o_w_id = $1 AND o_d_id = $2 AND o_id = $3
// schema: catalog v13
// objects: TBL_15 orders, COL_15_0 orders.o_id, COL_15_7 orders.o_entry_d
#include "bc_plan.h"
enum { TBL_15 = 15, COL_15_0 = 0, COL_15_7 = 7 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    return;
  }
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, TBL_15, 0, 3), &found);
  if (!found) {
    return;
  }
  int64_t v0 = 0;
  bool n0 = bc_col_null(cx, row, COL_15_7);
  if (!n0) {
    v0 = bc_extract(cx, bc_col_i64(cx, row, COL_15_7), 10, BC_SOURCE_TIMESTAMP);
  }
  int64_t v1 = 0;
  bool n1 = bc_col_null(cx, row, COL_15_7);
  if (!n1) {
    v1 = bc_extract(cx, bc_col_i64(cx, row, COL_15_7), 3, BC_SOURCE_TIMESTAMP);
  }
  int64_t v2 = 0;
  bool n2 = bc_col_null(cx, row, COL_15_7);
  if (!n2) {
    v2 = bc_extract(cx, bc_col_i64(cx, row, COL_15_7), 16, BC_SOURCE_TIMESTAMP);
  }
  bc_emit_begin(cx);
  bc_emit_col(cx, row, COL_15_0);
  if (n0) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v0);
  }
  if (n1) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v1);
  }
  if (n2) {
    bc_emit_null(cx);
  } else {
    bc_emit_i64(cx, v2);
  }
  bc_emit_end(cx);
}
