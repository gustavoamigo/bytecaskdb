// SELECT t.id FROM t, p WHERE t.name = $1 AND p.w = t.qty LIMIT 0
// isolation: snapshot   schema: catalog v7
// objects: 
#include "bc_plan.h"


void plan(bc_ctx *cx) {
}
