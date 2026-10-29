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
#include <expected>
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
// Test only: the bytes the context's statement is charged now.
std::uint64_t bc_ctx_held_for_testing(const bc_ctx* cx);
// Test only: the capacity, in elements, of the write set's vectors (effects,
// claims, locks, the tables written).
std::size_t bc_ctx_write_set_slots_for_testing(const bc_ctx* cx);
#endif

}  // extern "C++"

export using ::bc_db;
export using ::bc_catalog;
export using ::bc_params;
export using ::bc_ctx_affected;
#ifdef BYTECASK_SQL_TESTING
export using ::bc_ctx_before_commit_for_testing;
export using ::bc_ctx_held_for_testing;
export using ::bc_ctx_write_set_slots_for_testing;
#endif

export namespace bytecask::sql {

// What the plan API charges a statement's memory budget for an entry of its
// own, beside the bytes the entry holds (docs/plan_api_design.md, *Memory*):
// each handle a plan is given (a view, a pin, a key or row builder, a
// cursor, a hash table probe), each sorter, hash table and aggregate the
// plan opens, each aggregate group's node and slot, and each sorter and
// hash table entry's node. Fixed numbers, so a statement
// is charged the same on every run and every build; plan_api.cpp checks
// that each is no less than what it stands for.
inline constexpr std::uint64_t kViewEntryBytes = 40;
inline constexpr std::uint64_t kKeyEntryBytes = 64;
inline constexpr std::uint64_t kRowEntryBytes = 152;
inline constexpr std::uint64_t kCursorEntryBytes = 408;
inline constexpr std::uint64_t kGroupEntryBytes = 136;
inline constexpr std::uint64_t kAggSlotBytes = 48;
inline constexpr std::uint64_t kSortEntryBytes = 128;
inline constexpr std::uint64_t kHashEntryBytes = 240;
inline constexpr std::uint64_t kHashIterEntryBytes = 32;
inline constexpr std::uint64_t kSorterBytes = 192;
inline constexpr std::uint64_t kHashBytes = 192;
inline constexpr std::uint64_t kAggBytes = 320;

// What the write set is charged for what it holds (docs/plan_api_design.md,
// *The write set is statement memory*): a conservative bound, never less
// than the memory used, and the one size both of its bounds count, the
// statement budget as a call adds an item and a transaction's max_bytes as
// bc_txn_add takes it in. Each heap block costs its capacity and
// kAllocBytes, the allocator's header and rounding; each item a slot in its
// vector, which grows to at most twice its size; and each row, unique prefix
// or range the statement wrote, to refuse a second write, the node it takes
// in its set. Each effect and table also costs its entry in the commit's
// WritePlan, which copies its buffers, in autocommit and in a transaction
// alike, so no commit needs memory its statements were not charged; and a
// write in a transaction the node it takes in the overlay.
inline constexpr std::uint64_t kAllocBytes = 32;
inline constexpr std::uint64_t kEffectSlotBytes = 112;
inline constexpr std::uint64_t kClaimSlotBytes = 112;
inline constexpr std::uint64_t kLockSlotBytes = 64;
inline constexpr std::uint64_t kTableSlotBytes = 8;  // a table's, the first time it is written
// The most slots each of the write set's vectors keeps across a reset,
// cleared and uncharged: bounded scratch, so small statements on a pooled
// context do not regrow them. A vector grown past it is freed.
inline constexpr std::size_t kKeptWriteSlots = 32;
inline constexpr std::uint64_t kOverlayNodeBytes = 120;
inline constexpr std::uint64_t kWrittenRowNodeBytes = 96;
inline constexpr std::uint64_t kUniqueNodeBytes = 96;
inline constexpr std::uint64_t kRangeNodeBytes = 112;
// A WritePlan's: a put, delete or range delete's slot in its writes; a point
// guard's node in its map of guards; a range guard's slot in its list.
inline constexpr std::uint64_t kPlanWriteSlotBytes = 112;
inline constexpr std::uint64_t kPlanKeyGuardNodeBytes = 96;
inline constexpr std::uint64_t kPlanRangeGuardSlotBytes = 96;

[[nodiscard]] constexpr auto buffer_bytes(std::size_t capacity) -> std::uint64_t {
  return capacity == 0 ? 0 : capacity + kAllocBytes;
}
// The one size of a buffer the write set holds: its capacity, whatever its
// size. A copy made from a view has its size as its capacity.
[[nodiscard]] constexpr auto held_bytes(const Bytes& b) -> std::uint64_t { return buffer_bytes(b.capacity()); }

// A WritePlan entry's slot or node: the engine copies its key, and its value
// or range end, into buffers of their size.
enum class PlanEntry : std::uint8_t { Write, KeyGuard, RangeGuard };
[[nodiscard]] constexpr auto plan_entry_bytes(PlanEntry e, std::size_t a, std::size_t b) -> std::uint64_t {
  std::uint64_t slot = 0;
  switch (e) {
    case PlanEntry::Write: slot = kPlanWriteSlotBytes; break;
    case PlanEntry::KeyGuard: slot = kPlanKeyGuardNodeBytes; break;
    case PlanEntry::RangeGuard: slot = kPlanRangeGuardSlotBytes; break;
  }
  return buffer_bytes(a) + buffer_bytes(b) + slot;
}
// An effect with buffers of these capacities, whose plan entry is `e`, and
// in a transaction a write's overlay node.
[[nodiscard]] constexpr auto effect_bytes(bool in_txn, PlanEntry e, std::size_t a, std::size_t b) -> std::uint64_t {
  auto own = buffer_bytes(a) + buffer_bytes(b) + kEffectSlotBytes + plan_entry_bytes(e, a, b);
  return own + (in_txn && e == PlanEntry::Write ? kOverlayNodeBytes : 0);
}
// A table the writes are in: its slot and the commit's guard on its record,
// a key of `record_key` bytes.
[[nodiscard]] constexpr auto table_bytes(std::size_t record_key) -> std::uint64_t {
  return kTableSlotBytes + plan_entry_bytes(PlanEntry::KeyGuard, record_key, 0);
}
[[nodiscard]] constexpr auto claim_bytes(std::size_t key) -> std::uint64_t {
  return buffer_bytes(key) + kClaimSlotBytes;
}
[[nodiscard]] constexpr auto lock_bytes(std::size_t key) -> std::uint64_t { return buffer_bytes(key) + kLockSlotBytes; }

// Every column of a row, by its layout: NULL, or the typed value; the id of
// the first malformed column (row_column), never a plausible value. The one
// decoder of whole rows: result rows for the embedded API and the tests,
// stored rows for Database::table_rows.
[[nodiscard]] auto decode_row(const RowLayout& layout, BytesView row)
    -> std::expected<std::vector<bc_value>, std::uint32_t>;


// A plan's result_types as the bc_type codes a bc_schema_ref carries.
[[nodiscard]] auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t>;

}  // namespace bytecask::sql

export struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h, by name, for loaders that link a
// plan at run time (TinyCC's tcc_add_symbol). Runtime calls (bc_ctx_*,
// bc_txn_*, bc_execute, bc_error*) are left out: a plan never calls them.
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

auto decode_row(const RowLayout& layout, BytesView row) -> std::expected<std::vector<bc_value>, std::uint32_t> {
  std::vector<bc_value> out;
  out.reserve(layout.columns.size());
  for (const auto& c : layout.columns) {
    auto v = row_column(row, c);
    switch (v.state) {
      case ColumnState::Malformed: return std::unexpected(c.col_id);
      case ColumnState::Absent: out.emplace_back(std::monostate{}); continue;
      case ColumnState::Value: break;
    }
    switch (c.type) {
      case PhysType::I64: out.emplace_back(v.i64()); break;
      case PhysType::F64: out.emplace_back(v.f64()); break;
      case PhysType::Bool: out.emplace_back(v.boolean()); break;
      case PhysType::Bytes: out.emplace_back(Bytes(v.bytes.begin(), v.bytes.end())); break;
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
      {"bc_add_bigint", fn_addr(bc_add_bigint)},
      {"bc_sub_bigint", fn_addr(bc_sub_bigint)},
      {"bc_mul", fn_addr(bc_mul)},
      {"bc_div", fn_addr(bc_div)},
      {"bc_mod", fn_addr(bc_mod)},
      {"bc_neg", fn_addr(bc_neg)},
      {"bc_f64_to_i64", fn_addr(bc_f64_to_i64)},
      {"bc_check_int", fn_addr(bc_check_int)},
      {"bc_i64_to_f64", fn_addr(bc_i64_to_f64)},
      {"bc_bytes_eq", fn_addr(bc_bytes_eq)},
      {"bc_bytes_cmp", fn_addr(bc_bytes_cmp)},
      {"bc_bytes_prefix", fn_addr(bc_bytes_prefix)},
      {"bc_like", fn_addr(bc_like)},
      {"bc_sub_begin", fn_addr(bc_sub_begin)},
      {"bc_sub_end", fn_addr(bc_sub_end)},
      {"bc_sub_has_i64", fn_addr(bc_sub_has_i64)},
      {"bc_sub_has_f64", fn_addr(bc_sub_has_f64)},
      {"bc_sub_has_bool", fn_addr(bc_sub_has_bool)},
      {"bc_sub_has_bytes", fn_addr(bc_sub_has_bytes)},
      {"bc_null_row", fn_addr(bc_null_row)},
      {"bc_key_begin", fn_addr(bc_key_begin)},
      {"bc_key_i64", fn_addr(bc_key_i64)},
      {"bc_key_f64", fn_addr(bc_key_f64)},
      {"bc_key_bool", fn_addr(bc_key_bool)},
      {"bc_key_bytes", fn_addr(bc_key_bytes)},
      {"bc_key_null", fn_addr(bc_key_null)},
      {"bc_key_param", fn_addr(bc_key_param)},
      {"bc_key_end", fn_addr(bc_key_end)},
      {"bc_key_fits", fn_addr(bc_key_fits)},
      {"bc_key_params", fn_addr(bc_key_params)},
      {"bc_key_successor", fn_addr(bc_key_successor)},
      {"bc_sort_key_begin", fn_addr(bc_sort_key_begin)},
      {"bc_sort_key_i64", fn_addr(bc_sort_key_i64)},
      {"bc_sort_key_f64", fn_addr(bc_sort_key_f64)},
      {"bc_sort_key_bool", fn_addr(bc_sort_key_bool)},
      {"bc_sort_key_bytes", fn_addr(bc_sort_key_bytes)},
      {"bc_sort_key_null", fn_addr(bc_sort_key_null)},
      {"bc_sort_key_col", fn_addr(bc_sort_key_col)},
      {"bc_sort_key_end", fn_addr(bc_sort_key_end)},
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
      {"bc_sort_key_part_i64", fn_addr(bc_sort_key_part_i64)},
      {"bc_sort_key_part_f64", fn_addr(bc_sort_key_part_f64)},
      {"bc_sort_key_part_bool", fn_addr(bc_sort_key_part_bool)},
      {"bc_sort_key_part_bytes", fn_addr(bc_sort_key_part_bytes)},
      {"bc_sort_key_part_null", fn_addr(bc_sort_key_part_null)},
      {"bc_count_range", fn_addr(bc_count_range)},
      {"bc_col_null", fn_addr(bc_col_null)},
      {"bc_col_i64", fn_addr(bc_col_i64)},
      {"bc_col_f64", fn_addr(bc_col_f64)},
      {"bc_col_bool", fn_addr(bc_col_bool)},
      {"bc_col_bytes", fn_addr(bc_col_bytes)},
      {"bc_col_key", fn_addr(bc_col_key)},
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
      {"bc_agg_clear", fn_addr(bc_agg_clear)},
      {"bc_agg_i64", fn_addr(bc_agg_i64)},
      {"bc_agg_f64", fn_addr(bc_agg_f64)},
      {"bc_agg_null", fn_addr(bc_agg_null)},
      {"bc_insert", fn_addr(bc_insert)},
      {"bc_update", fn_addr(bc_update)},
      {"bc_delete", fn_addr(bc_delete)},
      {"bc_del_range", fn_addr(bc_del_range)},
      {"bc_guard_unchanged", fn_addr(bc_guard_unchanged)},
      {"bc_guard_row", fn_addr(bc_guard_row)},
      {"bc_guard_range", fn_addr(bc_guard_range)},
      {"bc_guard_present", fn_addr(bc_guard_present)},
      {"bc_guard_absent", fn_addr(bc_guard_absent)},
      {"bc_raise", fn_addr(bc_raise)},
  };
  return kSymbols;
}
