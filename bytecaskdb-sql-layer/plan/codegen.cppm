// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// codegen.cppm — the planner and the code generator: a bound DML statement to
// a plan file in the subset of docs/plan_api_design.md, plus what the runtime
// needs to run it (parameter types, result layout, schema reference).
//
// The planner has three rules:
//   1. every primary key column compared for equality  -> bc_get on the row
//   2. a range on the first column of a secondary index -> bc_scan_open on the
//      index, ASC or DESC, covering when the index key holds every result
//      column, bc_scan_row per entry otherwise (SELECT only)
//   3. anything else                                    -> rejected
//
// Writes are bc_insert, bc_update and bc_delete: the runtime maintains the
// indexes (docs/plan_api_design.md, *Decisions, not mechanics*).
//
// prototype: string templates, replaced by the designed planner and code
// generator (#492). Definitions are in codegen.cpp.

module;
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module bytecask.sql.codegen;

export import bytecask.sql.binder;

export namespace bytecask::sql {

// A column a plan reads or sets, as compiled: what bc_col_ref carries.
struct ColumnDep {
  std::uint32_t table, col;
  PhysType type;
};

struct GeneratedPlan {
  std::string source;  // the plan file: #include "bc_plan.h", constants, plan()
  std::vector<PhysType> param_types;
  bool returns_rows{false};
  std::vector<PhysType> result_types;  // the plan's result layout, for its bc_schema_ref
  // What its bc_schema_ref lists (docs/sql_layer_catalog_design.md,
  // *Compatibility*): the objects and columns it depends on.
  std::uint64_t catalog_version{0};
  std::vector<std::uint32_t> objects_read, tables_written;
  std::vector<ColumnDep> columns;
};

// Plans a DML statement. Throws ast::SqlError (position -1) when no rule
// applies.
[[nodiscard]] auto generate_plan(const bound::Bound& b, const Catalog& cat, std::string_view sql) -> GeneratedPlan;

}  // namespace bytecask::sql
