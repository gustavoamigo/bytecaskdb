// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.cpp — the plan-API symbol table; see runtime.cppm.

module;
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

module bytecask.sql.runtime;

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

auto schema_incompatibility(const Catalog& cat, const bc_schema_ref& ref) -> std::optional<std::string> {
  auto ids = [](const std::uint32_t* p, std::uint32_t n) { return std::span{p, p == nullptr ? 0 : n}; };
  for (auto id : ids(ref.objects_read, ref.n_read)) {
    if (cat.key_layout(id) == nullptr) return "table or index " + std::to_string(id) + " the plan reads no longer exists";
  }
  for (auto id : ids(ref.tables_written, ref.n_written)) {
    if (cat.table(id) == nullptr) return "table " + std::to_string(id) + " the plan writes no longer exists";
  }
  for (const auto& c : std::span{ref.cols, ref.cols == nullptr ? 0 : ref.n_cols}) {
    const auto* t = cat.table(c.table);
    if (t == nullptr) return "table " + std::to_string(c.table) + " no longer exists";
    const auto* slot = t->layout.find(c.col);
    if (slot == nullptr || static_cast<std::uint8_t>(slot->type) != c.type) {
      return "column " + std::to_string(c.col) + " of table " + t->name +
             " no longer exists with the type the plan was compiled with";
    }
  }
  return std::nullopt;
}

auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  out.reserve(types.size());
  for (auto t : types) out.push_back(static_cast<std::uint8_t>(t));
  return out;
}

}  // namespace bytecask::sql

auto constraint_message(std::uint32_t detail) -> std::string {
  auto object = std::to_string(detail & 0x00FF'FFFFu);
  switch (detail >> 24) {
    case BC_C_UNIQUE: return "duplicate key value violates unique constraint of object " + object;
    case BC_C_NOT_NULL: return "null value violates a not-null constraint of table " + object;
    case BC_C_CHECK: return "check constraint " + object + " violated";
    case BC_C_FOREIGN_KEY: return "foreign key constraint " + object + " violated";
    case BC_C_RANGE: return "value out of range for column " + object;
    default: return "constraint violated on object " + object;
  }
}

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
