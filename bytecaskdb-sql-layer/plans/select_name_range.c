/* SELECT id FROM t WHERE name >= $1 AND name < $2
   isolation: snapshot   schema: catalog v2 */
#include "bc_plan.h"
enum { TBL_T = 1, IDX_T_BY_NAME = 3, T_ID_OFF = 16, LAY_RESULT = 1073741825 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  bc_key k0 = bc_key_begin(cx, IDX_T_BY_NAME);
  bc_key_bytes(cx, k0, bc_param_bytes(cx, 0));
  bc_view lo = bc_key_end(cx, k0);
  bc_key k1 = bc_key_begin(cx, IDX_T_BY_NAME);
  bc_key_bytes(cx, k1, bc_param_bytes(cx, 1));
  bc_view hi = bc_key_end(cx, k1);
  bc_cursor c = bc_scan_open(cx, IDX_T_BY_NAME, lo, hi, BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bool found = false;
    bc_view row = bc_lookup(cx, TBL_T, bc_scan_pk(cx, c), &found);
    if (found) {
      bc_emit_begin(cx, LAY_RESULT);
      bc_emit_i64(cx, bc_col_i64(cx, row, T_ID_OFF));
      bc_emit_end(cx);
    }
  }
}
