// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The plan API calls revision 3 of docs/plan_api_design.md added or
// completed (#490): empty scan bounds, key-part decoders, bc_raise and the
// duplicate-key path, the f64 and bool calls, the arithmetic helpers, the
// remaining effects, bc_row_pk and bc_pin, and the fault path of each.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::PhysType;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kLay = 2;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1, kQtyCol = 2;
constexpr std::uint32_t kIdOff = 20, kNameSlot = 0;

// A second table with the other physical types: m (k BIGINT PRIMARY KEY,
// x DOUBLE PRECISION, b BOOLEAN) with an index on x. Ids follow t's.
constexpr std::uint32_t kM = 4, kMLay = 5, kMByX = 6;
constexpr std::uint32_t kKCol = 0, kXCol = 1, kBCol = 2;
constexpr std::uint32_t kXOff = 28, kBOff = 36;

constexpr auto kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr auto kInf = std::numeric_limits<double>::infinity();
const bc_value kNull = std::monostate{};
constexpr std::array<PhysType, 1> kI64Row{PhysType::I64};
constexpr std::array<PhysType, 2> kNameIdRow{PhysType::Bytes, PhysType::I64};
constexpr std::array<PhysType, 2> kF64BoolRow{PhysType::F64, PhysType::Bool};

auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, std::span<const PhysType> result = {},
         std::uint32_t retries = 0) -> int {
  auto ps = params(std::move(p));
  auto codes = type_codes(result);
  bc_schema_ref ref{f.cat.version(), nullptr, nullptr, 0, 0, codes.data(), static_cast<std::uint32_t>(codes.size())};
  return bc_execute(f.cx, plan, &ref, &ps, {}, false, retries);
}

auto bits(double d) -> std::uint64_t { return std::bit_cast<std::uint64_t>(d); }

void insert_t(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, kb);
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_i64(cx, rb, kIdCol, bc_param_i64(cx, 0));
  if (bc_param_null(cx, 1)) {
    bc_row_set_null(cx, rb, kNameCol);
  } else {
    bc_row_set_bytes(cx, rb, kNameCol, bc_param_bytes(cx, 1));
  }
  bc_row_set_i64(cx, rb, kQtyCol, bc_param_i64(cx, 2));
  bc_put(cx, pk, bc_row_end(cx, rb));
  bc_key ib = bc_key_begin(cx, kIdx);
  if (bc_param_null(cx, 1)) {
    bc_key_null(cx, ib);
  } else {
    bc_key_bytes(cx, ib, bc_param_bytes(cx, 1));
  }
  bc_key_i64(cx, ib, bc_param_i64(cx, 0));
  bc_put(cx, bc_key_end(cx, ib), bc_empty(cx));
}

void seed_t(Fixture& f) {
  REQUIRE(run(f, insert_t, {std::int64_t{3}, bytes("cherry"), std::int64_t{30}}) == BC_COMMITTED);
  REQUIRE(run(f, insert_t, {std::int64_t{1}, bytes("apple"), std::int64_t{10}}) == BC_COMMITTED);
  REQUIRE(run(f, insert_t, {std::int64_t{2}, kNull, std::int64_t{20}}) == BC_COMMITTED);
}

// Emits every id of t in scan order, the bounds empty: the whole table.
void scan_all_forward(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, bc_key_part_i64(cx, kTbl, bc_scan_key(cx, c), 0));
    bc_emit_end(cx);
  }
}

void scan_all_reverse(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_REVERSE);
  while (bc_scan_next(cx, c)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, bc_col_i64(cx, bc_scan_value(cx, c), kIdOff));
    bc_emit_end(cx);
  }
}

void scan_bad_flag(bc_ctx* cx) { bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), 1u << 5); }

// A covering read of the index: (name, id) from the key alone.
void covering_index(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view k = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    if (bc_key_part_null(cx, kIdx, k, 0)) {
      bc_emit_null(cx);
    } else {
      bc_emit_bytes(cx, bc_key_part_bytes(cx, kIdx, k, 0));
    }
    bc_emit_i64(cx, bc_key_part_i64(cx, kIdx, k, 1));
    bc_emit_end(cx);
  }
}

void key_part_out_of_range(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) bc_emit_count(cx, bc_key_part_i64(cx, kIdx, bc_scan_key(cx, c), 2));
}

void key_part_wrong_type(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) bc_emit_count(cx, bc_key_part_i64(cx, kIdx, bc_scan_key(cx, c), 0));
}

void key_part_wrong_table(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) bc_emit_count(cx, bc_key_part_i64(cx, kTbl, bc_scan_key(cx, c), 0));
}

}  // namespace

TEST_CASE("plan API: the symbol table names every plan-callable function of the header", "[sql][algebra]") {
  std::ifstream in{std::string{SQL_LAYER_ALGEBRA_DIR} + "/bc_plan.h"};
  REQUIRE(in);
  std::set<std::string> declared;
  const std::regex decl{R"(^[A-Za-z_][A-Za-z0-9_ \*]*?\b(bc_[a-z0-9_]+)\()"};
  for (std::string line; std::getline(in, line);) {
    std::smatch m;
    if (!std::regex_search(line, m, decl)) continue;
    auto name = m[1].str();
    if (name.starts_with("bc_ctx_") || name.starts_with("bc_error") || name == "bc_execute") continue;
    declared.insert(name);
  }
  std::set<std::string> table;
  for (const auto& sym : plan_api_symbols()) {
    CHECK(sym.address != nullptr);
    CHECK(table.insert(sym.name).second);  // no duplicates
  }
  CHECK(declared == table);
  CHECK(declared.size() > 80);
}

TEST_CASE("plan API: empty bounds scan the whole table, forward and reverse", "[sql][algebra]") {
  Fixture f;
  seed_t(f);
  CHECK(run(f, scan_all_forward, {}, kI64Row) == BC_COMMITTED);
  auto rows = f.results(kI64Row);
  REQUIRE(rows.size() == 3);
  CHECK(std::get<std::int64_t>(rows[0][0]) == 1);
  CHECK(std::get<std::int64_t>(rows[2][0]) == 3);
  CHECK(run(f, scan_all_reverse, {}, kI64Row) == BC_COMMITTED);
  rows = f.results(kI64Row);
  REQUIRE(rows.size() == 3);
  CHECK(std::get<std::int64_t>(rows[0][0]) == 3);
  CHECK(std::get<std::int64_t>(rows[2][0]) == 1);
  CHECK(run(f, scan_bad_flag, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

TEST_CASE("plan API: key parts decode from an index scan without the row", "[sql][algebra]") {
  Fixture f;
  seed_t(f);
  CHECK(run(f, covering_index, {}, kNameIdRow) == BC_COMMITTED);
  auto rows = f.results(kNameIdRow);
  REQUIRE(rows.size() == 3);
  // Nulls first, then by name; the id is the primary-key part after the index column.
  CHECK(std::holds_alternative<std::monostate>(rows[0][0]));
  CHECK(std::get<std::int64_t>(rows[0][1]) == 2);
  CHECK(std::get<Bytes>(rows[1][0]) == bytes("apple"));
  CHECK(std::get<std::int64_t>(rows[1][1]) == 1);
  CHECK(std::get<Bytes>(rows[2][0]) == bytes("cherry"));
  CHECK(std::get<std::int64_t>(rows[2][1]) == 3);

  CHECK(run(f, key_part_out_of_range, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, key_part_wrong_type, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, key_part_wrong_table, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
int g_runs = 0;

// The unique-insert pattern: check the snapshot, raise, keep the guard.
void unique_insert(bc_ctx* cx) {
  ++g_runs;
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, kb);
  if (bc_exists(cx, pk)) {
    bc_raise(cx, BC_C_UNIQUE, kTbl);
    return;
  }
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_i64(cx, rb, kIdCol, bc_param_i64(cx, 0));
  bc_guard_absent(cx, pk);
  bc_put(cx, pk, bc_row_end(cx, rb));
  bc_emit_count(cx, 1);
}

void raise_then_write(bc_ctx* cx) {
  bc_raise(cx, BC_C_NOT_NULL, 7);
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, 99);
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_i64(cx, rb, kIdCol, 99);
  bc_put(cx, bc_key_end(cx, kb), bc_row_end(cx, rb));  // dropped: the error is sticky
  bc_emit_count(cx, 1);
}
}  // namespace

TEST_CASE("plan API: a raised constraint error is final: no retry, nothing committed", "[sql][algebra]") {
  Fixture f;
  g_runs = 0;
  CHECK(run(f, unique_insert, {std::int64_t{1}}, {}, 3) == BC_COMMITTED);
  CHECK(g_runs == 1);
  CHECK(bc_ctx_affected(f.cx) == 1);
  CHECK(run(f, unique_insert, {std::int64_t{1}}, {}, 3) == BC_FAILED);
  CHECK(g_runs == 2);  // not retried
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
  CHECK(bc_error_detail(f.cx) == ((std::uint32_t{BC_C_UNIQUE} << 24) | kTbl));
  CHECK(std::string{bc_error_message(f.cx)}.find("unique") != std::string::npos);
  CHECK(bc_ctx_affected(f.cx) == -1);

  CHECK(run(f, raise_then_write, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
  CHECK(bc_error_detail(f.cx) == ((std::uint32_t{BC_C_NOT_NULL} << 24) | 7u));
  Bytes key, out;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 99, {PhysType::I64});
  CHECK_FALSE(f.db->get({}, key, out));
  // The detail is cleared by the next execution.
  CHECK(run(f, scan_all_forward, {}, kI64Row) == BC_COMMITTED);
  CHECK(bc_error_detail(f.cx) == 0);
}

namespace {
void create_m(Fixture& f) {
  (void)f.cat.create_table(*f.db, "m", {{"k", PhysType::I64, false}, {"x", PhysType::F64, true}, {"b", PhysType::Bool, true}},
                           {"k"});
  (void)f.cat.create_index(*f.db, "m_by_x", "m", {"x"});
  REQUIRE(f.cat.table("m")->id == kM);
  REQUIRE(f.cat.table("m")->layout.find(kXCol)->pos == kXOff);
  REQUIRE(f.cat.table("m")->layout.find(kBCol)->pos == kBOff);
}

// INSERT INTO m VALUES ($1, $2, $3), maintaining m_by_x.
void insert_m(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kM);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, kb);
  bc_row rb = bc_row_begin(cx, kMLay);
  bc_row_set_i64(cx, rb, kKCol, bc_param_i64(cx, 0));
  if (bc_param_null(cx, 1)) {
    bc_row_set_null(cx, rb, kXCol);
  } else {
    bc_row_set_f64(cx, rb, kXCol, bc_param_f64(cx, 1));
  }
  if (bc_param_null(cx, 2)) {
    bc_row_set_null(cx, rb, kBCol);
  } else {
    bc_row_set_bool(cx, rb, kBCol, bc_param_bool(cx, 2));
  }
  bc_put(cx, pk, bc_row_end(cx, rb));
  bc_key ib = bc_key_begin(cx, kMByX);
  if (bc_param_null(cx, 1)) {
    bc_key_null(cx, ib);
  } else {
    bc_key_f64(cx, ib, bc_param_f64(cx, 1));
  }
  bc_key_i64(cx, ib, bc_param_i64(cx, 0));
  bc_put(cx, bc_key_end(cx, ib), bc_empty(cx));
}

// SELECT x, b FROM m WHERE k = $1
void select_m(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kM);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bool found = false;
  bc_view row = bc_get(cx, bc_key_end(cx, kb), &found);
  if (!found) {
    return;
  }
  bc_emit_begin(cx);
  if (bc_col_null(cx, row, kXCol)) {
    bc_emit_null(cx);
  } else {
    bc_emit_f64(cx, bc_col_f64(cx, row, kXOff));
  }
  if (bc_col_null(cx, row, kBCol)) {
    bc_emit_null(cx);
  } else {
    bc_emit_bool(cx, bc_col_bool(cx, row, kBOff));
  }
  bc_emit_end(cx);
}

// SELECT x FROM m ORDER BY x, read from the index key alone.
void x_in_index_order(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kMByX, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view k = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    if (bc_key_part_null(cx, kMByX, k, 0)) {
      bc_emit_null(cx);
    } else {
      bc_emit_f64(cx, bc_key_part_f64(cx, kMByX, k, 0));
    }
    bc_emit_bool(cx, bc_key_part_i64(cx, kMByX, k, 1) > 3);
    bc_emit_end(cx);
  }
}

void f64_part_on_i64_key(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kM);
  bc_key_f64(cx, kb, 1.0);
}

void bool_param_as_i64(bc_ctx* cx) { bc_emit_count(cx, bc_param_i64(cx, 0)); }
}  // namespace

TEST_CASE("plan API: f64 and bool flow through parameters, rows, keys and results", "[sql][algebra]") {
  Fixture f;
  create_m(f);
  const std::vector<bc_value> xs{-kInf, -1.5, -0.0, 0.0, 2.5, kNaN, kNull};
  for (std::int64_t k = 1; k <= std::ssize(xs); ++k) {
    REQUIRE(run(f, insert_m, {k, xs[static_cast<std::size_t>(k - 1)], k % 2 == 0 ? bc_value{true} : kNull}) ==
            BC_COMMITTED);
  }
  REQUIRE(run(f, select_m, {std::int64_t{2}}, kF64BoolRow) == BC_COMMITTED);
  auto rows = f.results(kF64BoolRow);
  REQUIRE(rows.size() == 1);
  CHECK(bits(std::get<double>(rows[0][0])) == bits(-1.5));
  CHECK(std::get<bool>(rows[0][1]) == true);
  REQUIRE(run(f, select_m, {std::int64_t{7}}, kF64BoolRow) == BC_COMMITTED);
  rows = f.results(kF64BoolRow);
  REQUIRE(rows.size() == 1);
  CHECK(std::holds_alternative<std::monostate>(rows[0][0]));
  CHECK(std::holds_alternative<std::monostate>(rows[0][1]));
  REQUIRE(run(f, select_m, {std::int64_t{6}}, kF64BoolRow) == BC_COMMITTED);
  rows = f.results(kF64BoolRow);
  REQUIRE(rows.size() == 1);
  auto nan = std::get<double>(rows[0][0]);
  CHECK(nan != nan);

  // Index order: null, -inf, -1.5, 0 (twice: -0.0 encodes as 0.0), 2.5, NaN.
  REQUIRE(run(f, x_in_index_order, {}, kF64BoolRow) == BC_COMMITTED);
  rows = f.results(kF64BoolRow);
  REQUIRE(rows.size() == 7);
  CHECK(std::holds_alternative<std::monostate>(rows[0][0]));
  CHECK(std::get<bool>(rows[0][1]) == true);  // k = 7
  CHECK(bits(std::get<double>(rows[1][0])) == bits(-kInf));
  CHECK(bits(std::get<double>(rows[2][0])) == bits(-1.5));
  CHECK(bits(std::get<double>(rows[3][0])) == bits(0.0));
  CHECK(bits(std::get<double>(rows[4][0])) == bits(0.0));
  CHECK(std::get<bool>(rows[3][1]) == false);  // k = 3 before k = 4 on the primary-key suffix
  CHECK(std::get<bool>(rows[4][1]) == true);
  CHECK(bits(std::get<double>(rows[5][0])) == bits(2.5));
  auto top = std::get<double>(rows[6][0]);
  CHECK(top != top);

  CHECK(run(f, f64_part_on_i64_key, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, bool_param_as_i64, {bc_value{true}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
void add_p(bc_ctx* cx) { bc_emit_count(cx, bc_add(cx, bc_param_i64(cx, 0), bc_param_i64(cx, 1))); }
void mul_p(bc_ctx* cx) { bc_emit_count(cx, bc_mul(cx, bc_param_i64(cx, 0), bc_param_i64(cx, 1))); }
void div_p(bc_ctx* cx) { bc_emit_count(cx, bc_div(cx, bc_param_i64(cx, 0), bc_param_i64(cx, 1))); }
void mod_p(bc_ctx* cx) { bc_emit_count(cx, bc_mod(cx, bc_param_i64(cx, 0), bc_param_i64(cx, 1))); }
void neg_p(bc_ctx* cx) { bc_emit_count(cx, bc_neg(cx, bc_param_i64(cx, 0))); }
void trunc_p(bc_ctx* cx) { bc_emit_count(cx, bc_f64_to_i64(cx, bc_param_f64(cx, 0))); }
void bytes_p(bc_ctx* cx) {
  bc_view a = bc_param_bytes(cx, 0);
  bc_view b = bc_param_bytes(cx, 1);
  int64_t r = bc_bytes_cmp(cx, a, b);
  if (bc_bytes_eq(cx, a, b)) {
    r = bc_add(cx, r, 100);
  }
  if (bc_bytes_prefix(cx, a, b)) {
    r = bc_add(cx, r, 10);
  }
  bc_emit_count(cx, r);
}

constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

auto value(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p) -> std::int64_t {
  REQUIRE(run(f, plan, std::move(p)) == BC_COMMITTED);
  return bc_ctx_affected(f.cx);
}

void arith_error(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p) {
  CHECK(run(f, plan, std::move(p)) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
  CHECK(bc_ctx_affected(f.cx) == -1);
}
}  // namespace

TEST_CASE("plan API: the arithmetic helpers define what C leaves undefined", "[sql][algebra]") {
  Fixture f;
  CHECK(value(f, add_p, {std::int64_t{2}, std::int64_t{3}}) == 5);
  arith_error(f, add_p, {kMax, std::int64_t{1}});
  arith_error(f, add_p, {kMin, std::int64_t{-1}});
  CHECK(value(f, mul_p, {std::int64_t{-4}, std::int64_t{3}}) == -12);
  arith_error(f, mul_p, {kMax, std::int64_t{2}});
  arith_error(f, mul_p, {kMin, std::int64_t{-1}});
  CHECK(value(f, div_p, {std::int64_t{-7}, std::int64_t{2}}) == -3);  // truncates toward zero
  arith_error(f, div_p, {std::int64_t{1}, std::int64_t{0}});
  arith_error(f, div_p, {kMin, std::int64_t{-1}});
  CHECK(value(f, div_p, {kMin, std::int64_t{1}}) == kMin);
  CHECK(value(f, mod_p, {std::int64_t{-7}, std::int64_t{2}}) == -1);  // the dividend's sign
  CHECK(value(f, mod_p, {std::int64_t{7}, std::int64_t{-2}}) == 1);
  CHECK(value(f, mod_p, {kMin, std::int64_t{-1}}) == 0);
  arith_error(f, mod_p, {std::int64_t{1}, std::int64_t{0}});
  CHECK(value(f, neg_p, {std::int64_t{5}}) == -5);
  arith_error(f, neg_p, {kMin});
  CHECK(value(f, trunc_p, {-2.7}) == -2);
  CHECK(value(f, trunc_p, {2.7}) == 2);
  CHECK(value(f, trunc_p, {-9223372036854775808.0}) == kMin);
  arith_error(f, trunc_p, {9223372036854775808.0});
  arith_error(f, trunc_p, {kNaN});
  arith_error(f, trunc_p, {kInf});
  CHECK(bits(bc_i64_to_f64(kMax)) == bits(9223372036854775808.0));
  CHECK(value(f, bytes_p, {bytes("ab"), bytes("ab")}) == 110);  // equal, a prefix, cmp 0
  CHECK(value(f, bytes_p, {bytes("ab"), bytes("abc")}) == 9);   // less, a prefix
  CHECK(value(f, bytes_p, {bytes("b"), bytes("abc")}) == 1);
  CHECK(value(f, bytes_p, {Bytes{}, bytes("abc")}) == 9);
}

namespace {
void count_pk_range(bc_ctx* cx) {
  bc_key lo = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, lo, bc_param_i64(cx, 0));
  bc_key hi = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, hi, bc_param_i64(cx, 1));
  bc_emit_count(cx, bc_count_range(cx, kTbl, bc_key_end(cx, lo), bc_key_end(cx, hi), 100));
}
void count_all(bc_ctx* cx) { bc_emit_count(cx, bc_count_range(cx, kTbl, bc_empty(cx), bc_empty(cx), bc_param_i64(cx, 0))); }
void del_pk_range(bc_ctx* cx) {
  bc_key lo = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, lo, bc_param_i64(cx, 0));
  bc_key hi = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, hi, bc_param_i64(cx, 1));
  bc_del_range(cx, bc_key_end(cx, lo), bc_key_end(cx, hi));
}
void guard_present_p(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_guard_present(cx, bc_key_end(cx, kb));
  bc_key ib = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, ib, bc_param_bytes(cx, 1));
  bc_key_i64(cx, ib, 77);
  bc_put(cx, bc_key_end(cx, ib), bc_empty(cx));
}
void guard_unchanged_p(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_guard_unchanged(cx, bc_key_end(cx, kb));
  bc_key ib = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, ib, bc_param_bytes(cx, 1));
  bc_key_i64(cx, ib, 78);
  bc_put(cx, bc_key_end(cx, ib), bc_empty(cx));
}
void guard_whole_table(bc_ctx* cx) {
  bc_guard_range(cx, kTbl, bc_empty(cx), bc_empty(cx));
  bc_key ib = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, ib, bc_param_bytes(cx, 0));
  bc_key_i64(cx, ib, 79);
  bc_put(cx, bc_key_end(cx, ib), bc_empty(cx));
}

auto pk_of(std::int64_t id) -> Bytes {
  Bytes key;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, id, {PhysType::I64});
  return key;
}

// Runs a plan, changes the engine underneath it, then commits: the guard's
// test.
auto commit_after(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, auto&& between) -> int {
  auto ps = params(std::move(p));
  bc_ctx_begin(f.cx, &ps, {});
  bc_ctx_run(f.cx, plan, nullptr);
  REQUIRE(bc_error(f.cx) == BC_OK);
  between();
  return bc_ctx_commit(f.cx, false);
}
}  // namespace

TEST_CASE("plan API: count_range, del_range and the guards reach the engine", "[sql][algebra]") {
  Fixture f;
  for (std::int64_t i = 1; i <= 5; ++i) {
    REQUIRE(run(f, insert_t, {i, bytes("n"), i}) == BC_COMMITTED);
  }
  CHECK(value(f, count_all, {std::int64_t{100}}) == 5);
  CHECK(value(f, count_all, {std::int64_t{2}}) == 2);
  CHECK(value(f, count_pk_range, {std::int64_t{2}, std::int64_t{4}}) == 2);
  CHECK(value(f, count_pk_range, {std::int64_t{4}, std::int64_t{2}}) == 0);
  CHECK(run(f, count_all, {std::int64_t{-1}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);

  CHECK(run(f, del_pk_range, {std::int64_t{2}, std::int64_t{4}}) == BC_COMMITTED);
  CHECK(value(f, count_all, {std::int64_t{100}}) == 3);
  Bytes out;
  CHECK_FALSE(f.db->get({}, pk_of(2), out));
  CHECK(f.db->get({}, pk_of(4), out));

  CHECK(run(f, guard_present_p, {std::int64_t{1}, bytes("p")}) == BC_COMMITTED);
  CHECK(run(f, guard_present_p, {std::int64_t{2}, bytes("p")}) == BC_CONFLICT);

  CHECK(commit_after(f, guard_unchanged_p, {std::int64_t{1}, bytes("u")}, [] {}) == BC_COMMITTED);
  CHECK(commit_after(f, guard_unchanged_p, {std::int64_t{1}, bytes("u")},
                     [&] { f.db->put({}, pk_of(1), bytes("changed")); }) == BC_CONFLICT);
  CHECK(commit_after(f, guard_whole_table, {bytes("r")}, [] {}) == BC_COMMITTED);
  CHECK(commit_after(f, guard_whole_table, {bytes("r")}, [&] { f.db->put({}, pk_of(9), bytes("new")); }) ==
        BC_CONFLICT);
  bc_ctx_reset(f.cx);
}

namespace {
// Each row's primary key, rebuilt from its columns, equals its scan key; a
// pinned view survives the cursor's step.
void row_pk_matches_key(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  bool first = true;
  bc_view previous = bc_empty(cx);
  int64_t ok = 0;
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_value(cx, c);
    if (bc_bytes_eq(cx, bc_row_pk(cx, kTbl, row), bc_scan_key(cx, c))) {
      ok = bc_add(cx, ok, 1);
    }
    if (!first) {
      ok = bc_add(cx, ok, bc_col_i64(cx, previous, kIdOff));  // pinned: still readable
    }
    previous = bc_pin(cx, row);
    first = false;
  }
  bc_emit_count(cx, ok);
}
void row_pk_wrong_table(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) bc_row_pk(cx, kIdx, bc_scan_value(cx, c));
}
void emit_without_types(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_i64(cx, 1);
  bc_emit_end(cx);
}
void emit_wrong_type(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_bytes(cx, bc_empty(cx));
  bc_emit_end(cx);
}
void emit_too_few(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_end(cx);
}
void key_too_long(bc_ctx* cx) {
  bc_key ib = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, ib, bc_param_bytes(cx, 0));
  bc_key_end(cx, ib);
}
void set_wrong_type(bc_ctx* cx) {
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_f64(cx, rb, kIdCol, 1.0);
}
}  // namespace

TEST_CASE("plan API: bc_row_pk rebuilds the scan key and bc_pin outlives the step", "[sql][algebra]") {
  Fixture f;
  seed_t(f);
  CHECK(value(f, row_pk_matches_key, {}) == 3 + 1 + 2);  // three matches, then ids 1 and 2 read after the step
  CHECK(run(f, row_pk_wrong_table, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

TEST_CASE("plan API: result rows follow the schema ref's types; limits and types are checked", "[sql][algebra]") {
  Fixture f;
  auto ps = params({});
  bc_ctx_begin(f.cx, &ps, {});
  bc_ctx_run(f.cx, emit_without_types, nullptr);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(run(f, emit_wrong_type, {}, kI64Row) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(run(f, emit_too_few, {}, kI64Row) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(run(f, key_too_long, {Bytes(70000, std::byte{'k'})}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(run(f, key_too_long, {Bytes(1000, std::byte{'k'})}) == BC_COMMITTED);
  CHECK(run(f, set_wrong_type, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  const std::uint8_t bad_code[] = {9};
  bc_schema_ref ref{f.cat.version(), nullptr, nullptr, 0, 0, bad_code, 1};
  CHECK(bc_execute(f.cx, emit_without_types, &ref, &ps, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
}
