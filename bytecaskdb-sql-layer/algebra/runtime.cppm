// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.cppm — the C++ side of the plan API: the types bc_plan.h leaves
// opaque, built by the server (or a test) before it creates a context, and
// the table of plan-callable functions a run-time loader links a plan
// against. The API's definitions are in plan_api.cpp.

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

// bc_plan.h declares these in the global module. A definition in the purview
// would attach them to this module, a second declaration of the same name
// that the compiler rejects; a linkage specification keeps them in the
// global module ([module.unit]/7), where plan_api.cpp defines bc_ctx and
// bc_ctx_affected as well.
export extern "C++" {

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

export struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h, by name, for loaders that link a
// plan at run time (TinyCC's tcc_add_symbol). Runtime calls (bc_ctx_*,
// bc_execute, bc_error*) are left out: a plan never calls them.
export [[nodiscard]] auto plan_api_symbols() -> std::span<const ApiSymbol>;

namespace {

// A function's address as the void* a C loader takes. Converting a function
// pointer to void* is conditionally supported; POSIX requires it (dlsym).
template <typename F>
auto fn_addr(F* f) -> const void* {
  return reinterpret_cast<const void*>(f);
}

}  // namespace

auto plan_api_symbols() -> std::span<const ApiSymbol> {
  // Function-local: initialised on first use, not by a global constructor.
  static const ApiSymbol kSymbols[] = {
      {"bc_param_i64", fn_addr(bc_param_i64)},
      {"bc_param_f64", fn_addr(bc_param_f64)},
      {"bc_param_bytes", fn_addr(bc_param_bytes)},
      {"bc_param_null", fn_addr(bc_param_null)},
      {"bc_emit_begin", fn_addr(bc_emit_begin)},
      {"bc_emit_i64", fn_addr(bc_emit_i64)},
      {"bc_emit_f64", fn_addr(bc_emit_f64)},
      {"bc_emit_bytes", fn_addr(bc_emit_bytes)},
      {"bc_emit_null", fn_addr(bc_emit_null)},
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
      {"bc_key_bytes", fn_addr(bc_key_bytes)},
      {"bc_key_null", fn_addr(bc_key_null)},
      {"bc_key_pk", fn_addr(bc_key_pk)},
      {"bc_key_end", fn_addr(bc_key_end)},
      {"bc_key_successor", fn_addr(bc_key_successor)},
      {"bc_get", fn_addr(bc_get)},
      {"bc_lookup", fn_addr(bc_lookup)},
      {"bc_exists", fn_addr(bc_exists)},
      {"bc_scan_open", fn_addr(bc_scan_open)},
      {"bc_scan_next", fn_addr(bc_scan_next)},
      {"bc_scan_key", fn_addr(bc_scan_key)},
      {"bc_scan_value", fn_addr(bc_scan_value)},
      {"bc_scan_pk", fn_addr(bc_scan_pk)},
      {"bc_scan_close", fn_addr(bc_scan_close)},
      {"bc_count_range", fn_addr(bc_count_range)},
      {"bc_col_null", fn_addr(bc_col_null)},
      {"bc_col_i64", fn_addr(bc_col_i64)},
      {"bc_col_i32", fn_addr(bc_col_i32)},
      {"bc_col_f64", fn_addr(bc_col_f64)},
      {"bc_col_bytes", fn_addr(bc_col_bytes)},
      {"bc_row_pk", fn_addr(bc_row_pk)},
      {"bc_row_begin", fn_addr(bc_row_begin)},
      {"bc_row_from", fn_addr(bc_row_from)},
      {"bc_row_set_i64", fn_addr(bc_row_set_i64)},
      {"bc_row_set_f64", fn_addr(bc_row_set_f64)},
      {"bc_row_set_bytes", fn_addr(bc_row_set_bytes)},
      {"bc_row_set_null", fn_addr(bc_row_set_null)},
      {"bc_row_end", fn_addr(bc_row_end)},
      {"bc_pin", fn_addr(bc_pin)},
      {"bc_empty", fn_addr(bc_empty)},
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
      {"bc_put", fn_addr(bc_put)},
      {"bc_del", fn_addr(bc_del)},
      {"bc_del_range", fn_addr(bc_del_range)},
      {"bc_guard_unchanged", fn_addr(bc_guard_unchanged)},
      {"bc_guard_range", fn_addr(bc_guard_range)},
      {"bc_guard_present", fn_addr(bc_guard_present)},
      {"bc_guard_absent", fn_addr(bc_guard_absent)},
  };
  return kSymbols;
}
