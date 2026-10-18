// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The hand-written plans in plans/, compiled by clang into this binary, run
// through bc_execute against the prototype's catalog.

#include <catch2/catch_test_macros.hpp>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.tests.dml_scenario;

extern "C" {
void plan_insert(bc_ctx*);
void plan_select_by_id(bc_ctx*);
void plan_select_name_range(bc_ctx*);
void plan_update_qty(bc_ctx*);
void plan_delete_by_id(bc_ctx*);
}

TEST_CASE("hand-written plans: the five DML statements move rows", "[sql][plans]") {
  sql_test::Fixture f;
  sql_test::run_dml_scenario(
      f, {plan_insert, plan_select_by_id, plan_select_name_range, plan_update_qty, plan_delete_by_id});
}
