// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.cppm — the C++ side of the plan API: the types bc_plan.h leaves
// opaque, built by the server (or a test) before it creates a context, and
// the table of plan-callable functions a run-time loader links a plan
// against. The API's definitions are in plan_api.cpp, a plain translation unit
// behind the C header; the symbol table's are here.

module;
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

#ifdef BYTECASK_SQL_TESTING
// Test only (sql_layer_testing): runs `hook` in every bc_ctx_commit of the
// context, after the plan ran and before its batch reaches the engine, so a
// test can make the commit conflict. An empty function removes it.
void bc_ctx_before_commit_for_testing(bc_ctx* cx, std::function<void()> hook);
#endif

}  // extern "C++"

export using ::bc_db;
export using ::bc_catalog;
export using ::bc_params;
export using ::bc_ctx_affected;
#ifdef BYTECASK_SQL_TESTING
export using ::bc_ctx_before_commit_for_testing;
#endif

export namespace bytecask::sql {

// Every column of a result row, by its layout: NULL, or the typed value. The
// one decoder of result rows, for the embedded API and the tests.
[[nodiscard]] auto decode_row(const RowLayout& layout, BytesView row) -> std::vector<bc_value>;


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

namespace {

// A function's address as the void* a C loader takes. Converting a function
// pointer to void* is conditionally supported; POSIX requires it (dlsym).
template <typename F>
auto fn_addr(F* f) -> const void* {
  return reinterpret_cast<const void*>(f);
}

}  // namespace

namespace bytecask::sql {

auto decode_row(const RowLayout& layout, BytesView row) -> std::vector<bc_value> {
  std::vector<bc_value> out;
  out.reserve(layout.columns.size());
  for (const auto& c : layout.columns) {
    if (row_is_null(row, c)) {
      out.emplace_back(std::monostate{});
      continue;
    }
    switch (c.type) {
      case PhysType::I64: out.emplace_back(row_i64(row, c.pos).value_or(0)); break;
      case PhysType::F64: out.emplace_back(row_f64(row, c.pos).value_or(0.0)); break;
      case PhysType::Bool: out.emplace_back(row_bool(row, c.pos).value_or(false)); break;
      case PhysType::Bytes: {
        auto v = row_bytes(row, c.pos).value_or(BytesView{});
        out.emplace_back(Bytes(v.begin(), v.end()));
        break;
      }
    }
  }
  return out;
}

auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  out.reserve(types.size());
  for (auto t : types) out.push_back(static_cast<std::uint8_t>(t));
  return out;
}

}  // namespace bytecask::sql

auto plan_api_symbols() -> std::span<const ApiSymbol> {
  // Function-local: initialised on first use, not by a global constructor.
  static const ApiSymbol kSymbols[] = {
      {"bc_param_i64", fn_addr(bc_param_i64)},
      {"bc_param_f64", fn_addr(bc_param_f64)},
      {"bc_param_bool", fn_addr(bc_param_bool)},
      {"bc_param_bytes", fn_addr(bc_param_bytes)},
      {"bc_param_null", fn_addr(bc_param_null)},
      {"bc_emit_begin", fn_addr(bc_emit_begin)},
      {"bc_emit_i64", fn_addr(bc_emit_i64)},
      {"bc_emit_f64", fn_addr(bc_emit_f64)},
      {"bc_emit_bool", fn_addr(bc_emit_bool)},
      {"bc_emit_bytes", fn_addr(bc_emit_bytes)},
      {"bc_emit_null", fn_addr(bc_emit_null)},
      {"bc_emit_col", fn_addr(bc_emit_col)},
      {"bc_emit_key_part", fn_addr(bc_emit_key_part)},
      {"bc_emit_end", fn_addr(bc_emit_end)},
      {"bc_emit_count", fn_addr(bc_emit_count)},
      {"bc_add", fn_addr(bc_add)},
      {"bc_sub", fn_addr(bc_sub)},
      {"bc_mul", fn_addr(bc_mul)},
      {"bc_div", fn_addr(bc_div)},
      {"bc_mod", fn_addr(bc_mod)},
      {"bc_neg", fn_addr(bc_neg)},
      {"bc_f64_to_i64", fn_addr(bc_f64_to_i64)},
      {"bc_i64_to_f64", fn_addr(bc_i64_to_f64)},
      {"bc_bytes_eq", fn_addr(bc_bytes_eq)},
      {"bc_bytes_cmp", fn_addr(bc_bytes_cmp)},
      {"bc_bytes_prefix", fn_addr(bc_bytes_prefix)},
      {"bc_key_begin", fn_addr(bc_key_begin)},
      {"bc_key_i64", fn_addr(bc_key_i64)},
      {"bc_key_f64", fn_addr(bc_key_f64)},
      {"bc_key_bool", fn_addr(bc_key_bool)},
      {"bc_key_bytes", fn_addr(bc_key_bytes)},
      {"bc_key_null", fn_addr(bc_key_null)},
      {"bc_key_param", fn_addr(bc_key_param)},
      {"bc_key_end", fn_addr(bc_key_end)},
      {"bc_key_params", fn_addr(bc_key_params)},
      {"bc_key_successor", fn_addr(bc_key_successor)},
      {"bc_get", fn_addr(bc_get)},
      {"bc_exists", fn_addr(bc_exists)},
      {"bc_scan_open", fn_addr(bc_scan_open)},
      {"bc_scan_next", fn_addr(bc_scan_next)},
      {"bc_scan_key", fn_addr(bc_scan_key)},
      {"bc_scan_row", fn_addr(bc_scan_row)},
      {"bc_scan_close", fn_addr(bc_scan_close)},
      {"bc_empty", fn_addr(bc_empty)},
      {"bc_key_part_i64", fn_addr(bc_key_part_i64)},
      {"bc_key_part_f64", fn_addr(bc_key_part_f64)},
      {"bc_key_part_bool", fn_addr(bc_key_part_bool)},
      {"bc_key_part_bytes", fn_addr(bc_key_part_bytes)},
      {"bc_key_part_null", fn_addr(bc_key_part_null)},
      {"bc_count_range", fn_addr(bc_count_range)},
      {"bc_col_null", fn_addr(bc_col_null)},
      {"bc_col_i64", fn_addr(bc_col_i64)},
      {"bc_col_f64", fn_addr(bc_col_f64)},
      {"bc_col_bool", fn_addr(bc_col_bool)},
      {"bc_col_bytes", fn_addr(bc_col_bytes)},
      {"bc_row_key", fn_addr(bc_row_key)},
      {"bc_row_begin", fn_addr(bc_row_begin)},
      {"bc_row_from", fn_addr(bc_row_from)},
      {"bc_row_set_i64", fn_addr(bc_row_set_i64)},
      {"bc_row_set_f64", fn_addr(bc_row_set_f64)},
      {"bc_row_set_bool", fn_addr(bc_row_set_bool)},
      {"bc_row_set_bytes", fn_addr(bc_row_set_bytes)},
      {"bc_row_set_null", fn_addr(bc_row_set_null)},
      {"bc_row_set_param", fn_addr(bc_row_set_param)},
      {"bc_row_end", fn_addr(bc_row_end)},
      {"bc_pin", fn_addr(bc_pin)},
      {"bc_sorter_open", fn_addr(bc_sorter_open)},
      {"bc_sorter_add", fn_addr(bc_sorter_add)},
      {"bc_sorter_count", fn_addr(bc_sorter_count)},
      {"bc_sorter_nth", fn_addr(bc_sorter_nth)},
      {"bc_sorter_next", fn_addr(bc_sorter_next)},
      {"bc_hash_open", fn_addr(bc_hash_open)},
      {"bc_hash_put", fn_addr(bc_hash_put)},
      {"bc_hash_find", fn_addr(bc_hash_find)},
      {"bc_hash_next", fn_addr(bc_hash_next)},
      {"bc_agg_open", fn_addr(bc_agg_open)},
      {"bc_agg_group", fn_addr(bc_agg_group)},
      {"bc_agg_add_i64", fn_addr(bc_agg_add_i64)},
      {"bc_agg_add_f64", fn_addr(bc_agg_add_f64)},
      {"bc_agg_min_i64", fn_addr(bc_agg_min_i64)},
      {"bc_agg_max_i64", fn_addr(bc_agg_max_i64)},
      {"bc_agg_count", fn_addr(bc_agg_count)},
      {"bc_agg_next", fn_addr(bc_agg_next)},
      {"bc_agg_i64", fn_addr(bc_agg_i64)},
      {"bc_agg_f64", fn_addr(bc_agg_f64)},
      {"bc_insert", fn_addr(bc_insert)},
      {"bc_update", fn_addr(bc_update)},
      {"bc_delete", fn_addr(bc_delete)},
      {"bc_del_range", fn_addr(bc_del_range)},
      {"bc_guard_unchanged", fn_addr(bc_guard_unchanged)},
      {"bc_guard_range", fn_addr(bc_guard_range)},
      {"bc_guard_present", fn_addr(bc_guard_present)},
      {"bc_guard_absent", fn_addr(bc_guard_absent)},
      {"bc_raise", fn_addr(bc_raise)},
  };
  return kSymbols;
}
