// SELECT count(*) FROM t
// isolation: snapshot   schema: catalog v7
// objects: TBL_1 t
#include "bc_plan.h"
enum { TBL_1 = 1 };

void plan(bc_ctx *cx) {
  bc_view lo = bc_empty(cx);
  bc_view hi = bc_empty(cx);
  int64_t n = bc_count_range(cx, TBL_1, lo, hi, INT64_MAX);
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
