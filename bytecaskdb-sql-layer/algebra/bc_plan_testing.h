/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Gustavo Amigo

   bc_plan_testing.h — what the tests and sql_layer_bench observe of the
   runtime beyond the plan contract (bc_plan.h). Not plan-callable: no plan
   names these, and the loader's symbol table does not hold them. */

#ifndef BC_PLAN_TESTING_H
#define BC_PLAN_TESTING_H

#include <stdint.h>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many bc_schema_refs this context has verified: one per run its cache
   of verified references did not answer. */
uint64_t bc_ctx_verifications(const bc_ctx *);

#ifdef __cplusplus
}
#endif

#endif
