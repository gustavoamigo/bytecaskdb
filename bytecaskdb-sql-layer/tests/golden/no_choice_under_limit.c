// SELECT a, b FROM p WHERE a = $1 AND w = $2 ORDER BY b LIMIT 3
// isolation: snapshot   schema: catalog v7
// objects: IDX_6 p_w, PART_6_0 p_w.a, PART_6_1 p_w.b
#include "bc_plan.h"
enum { IDX_6 = 6, PART_6_0 = 1, PART_6_1 = 2 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1)) {
    return;
  }
  int64_t n = 0;
  bc_key k0 = bc_key_begin(cx, IDX_6);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view lo = bc_key_end(cx, k0);
  bc_cursor c = bc_scan_open(cx, IDX_6, lo, lo, BC_SCAN_PREFIX);
  while (n < 3 && bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, IDX_6, key, PART_6_0);
    bc_emit_key_part(cx, IDX_6, key, PART_6_1);
    bc_emit_end(cx);
    ++n;
  }
  bc_scan_close(cx, c);
}
