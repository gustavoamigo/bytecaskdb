// SELECT id FROM t WHERE name = $1
// isolation: snapshot   schema: catalog v7
// objects: IDX_3 t_by_name, PART_3_0 t_by_name.id
#include "bc_plan.h"
enum { IDX_3 = 3, PART_3_0 = 1 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0)) {
    return;
  }
  bc_view lo = bc_key_params(cx, IDX_3, 0, 1);
  bc_cursor c = bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, IDX_3, key, PART_3_0);
    bc_emit_end(cx);
  }
}
