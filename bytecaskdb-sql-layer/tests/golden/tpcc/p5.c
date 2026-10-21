// SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3
// isolation: snapshot   schema: catalog v13
// objects: IDX_10 customer_i2
#include "bc_plan.h"
enum { IDX_10 = 10 };

void plan(bc_ctx *cx) {
  if (bc_param_null(cx, 0) || bc_param_null(cx, 1) || bc_param_null(cx, 2)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, 0);
    bc_emit_end(cx);
    return;
  }
  bc_key k0 = bc_key_begin(cx, IDX_10);
  bc_key_param(cx, k0, 2);
  bc_key_param(cx, k0, 1);
  bc_key_param(cx, k0, 0);
  bc_view lo = bc_key_end(cx, k0);
  bc_view hi = bc_key_successor(cx, lo);
  int64_t n = bc_count_range(cx, IDX_10, lo, hi, INT64_MAX);
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
