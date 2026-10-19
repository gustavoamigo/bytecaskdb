// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.cppm — the C++ side of the plan API: the types bc_plan.h leaves
// opaque, built by the server (or a test) before it creates a context, and
// the table of plan-callable functions a run-time loader links a plan
// against. The API's definitions are in plan_api.cpp, the symbol table's in
// runtime.cpp.

module;
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

export module bytecask.sql.runtime;

export import bytecask.sql.catalog;

// One bound parameter: SQL NULL, an integer, a double or bytes.
export using bc_value = std::variant<std::monostate, std::int64_t, double, bytecask::sql::Bytes>;

// bc_plan.h declares these in the global module. A definition in this
// module's purview would attach them to the module, a second declaration of
// the same entity the compiler rejects; a linkage specification keeps the
// definitions in the global module ([basic.link]/10), where plan_api.cpp
// defines bc_ctx and bc_ctx_affected too. The definitions are not exported: a
// redeclaration of an entity the global module introduced cannot be
// ([module.interface]/6). The using-declarations below export the names, as
// the standard library's module exports its header-declared names.
extern "C++" {

// The open engine a context reads through and commits to. Not owned.
struct bc_db {
  bytecask::DB* db;
};

// The live catalog. Not owned; must outlive every context built on it.
struct bc_catalog {
  const bytecask::sql::Catalog* cat;
};

struct bc_params {
  std::vector<bc_value> values;
};

// prototype: the affected-row count bc_emit_count recorded, or -1. The plan
// API has no runtime call that reads it back; see the write-up on #497.
[[nodiscard]] auto bc_ctx_affected(const bc_ctx* cx) -> std::int64_t;

}  // extern "C++"

export using ::bc_db;
export using ::bc_catalog;
export using ::bc_params;
export using ::bc_ctx_affected;

export struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h, by name, for loaders that link a
// plan at run time (TinyCC's tcc_add_symbol). Runtime calls (bc_ctx_*,
// bc_execute, bc_error*) are left out: a plan never calls them.
export [[nodiscard]] auto plan_api_symbols() -> std::span<const ApiSymbol>;
