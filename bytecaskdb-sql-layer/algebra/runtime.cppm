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
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

export module bytecask.sql.runtime;

export import bytecask.sql.catalog;

// One bound parameter: SQL NULL, an integer, a double, a boolean or bytes;
// the catalog's Datum, which defaults are stored as.
export using bc_value = bytecask::sql::Datum;

// bc_plan.h declares these in the global module. A definition in this
// module's purview would attach them to the module, a second declaration of
// the same entity the compiler rejects; a linkage specification keeps the
// definitions in the global module ([basic.link]/10), where plan_api.cpp
// defines bc_ctx and the extern "C" functions. The definitions are not exported: a
// redeclaration of an entity the global module introduced cannot be
// ([module.interface]/6). The using-declarations below export the names, as
// the standard library's module exports its header-declared names.
extern "C++" {

// The open engine a context reads through and commits to. Not owned.
struct bc_db {
  bytecask::DB* db;
};

// The live catalog. Not owned; must outlive every context built on it. A
// context pins one version of it per execution.
struct bc_catalog {
  bytecask::sql::LiveCatalog* live;
};

struct bc_params {
  std::vector<bc_value> values;
};

}  // extern "C++"

export using ::bc_db;
export using ::bc_catalog;
export using ::bc_params;
export using ::bc_ctx_affected;

// What a BC_E_CONSTRAINT detail (bc_error_detail) means, in words: the one
// mapping from constraint codes to text, for the plan API's error message
// and the embedded API's exception.
export [[nodiscard]] auto constraint_message(std::uint32_t detail) -> std::string;

export namespace bytecask::sql {

// Every column of a result row, by its layout: NULL, or the typed value. The
// one decoder of result rows, for the embedded API and the tests.
[[nodiscard]] auto decode_row(const RowLayout& layout, BytesView row) -> std::vector<bc_value>;

// Why a plan compiled against `ref` cannot run on `cat`, or nullopt if it
// can: docs/sql_layer_catalog_design.md, *Compatibility*. What bc_ctx_run
// checks, and what the embedded API asks before it reuses a cached plan.
[[nodiscard]] auto schema_incompatibility(const Catalog& cat, const bc_schema_ref& ref) -> std::optional<std::string>;

// A plan's result_types as the bc_type codes a bc_schema_ref carries.
[[nodiscard]] auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t>;

}  // namespace bytecask::sql

export struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h, by name, for loaders that link a
// plan at run time (TinyCC's tcc_add_symbol). Runtime calls (bc_ctx_*,
// bc_execute, bc_error*) are left out: a plan never calls them.
// tests/plan_api_test.cpp checks the table against the header.
export [[nodiscard]] auto plan_api_symbols() -> std::span<const ApiSymbol>;
