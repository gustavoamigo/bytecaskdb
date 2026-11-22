// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The jsonb accessors and casts out of plan API revision 22
// (docs/jsonb_design.md, stage 3): bc_jsonb_field, _element, _path and their
// _text forms, bc_jsonb_path_in and bc_jsonb_path_add, bc_jsonb_typeof,
// bc_jsonb_array_length and bc_jsonb_to_*. Each call's result and its SQL
// NULL, its errors with their SQLSTATE, BC_E_CORRUPT on a damaged stored
// value, BC_E_HANDLE for a NULL out or a view that is no path, a slice that
// dies with its row's step, and the budget and allocation-failure sweeps.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.tests.fixture;

using bytecask::sql::Bytes;
using bytecask::sql::kPublicSchema;
using bytecask::sql::PhysType;
using bytecask::sql::SqlKind;

using namespace sql_test;

namespace {

// jt (k BIGINT PRIMARY KEY, j jsonb, s TEXT): its ids, set by make_jt.
std::uint32_t g_jt = 0;
constexpr std::uint32_t kKCol = 0, kJCol = 1, kSCol = 2;
const bc_value kNull = std::monostate{};
constexpr std::array<PhysType, 1> kJsonbRow{PhysType::Jsonb};
constexpr std::array<PhysType, 1> kBytesRow{PhysType::Bytes};
constexpr std::array<PhysType, 1> kI64Row{PhysType::I64};
constexpr std::array<PhysType, 1> kF64Row{PhysType::F64};
constexpr std::array<PhysType, 1> kBoolRow{PhysType::Bool};
constexpr std::array<PhysType, 1> kNumRow{PhysType::Num};

void make_jt(Fixture& f) {
  auto col = [](std::string name, SqlKind k, bool nullable) { return bytecask::sql::ColumnSpec{std::move(name), {k}, nullable, {}}; };
  f.ddl(bytecask::sql::CreateTableDdl{
      {kPublicSchema, "jt"}, {col("k", SqlKind::BigInt, false), col("j", SqlKind::Jsonb, true), col("s", SqlKind::Text, true)}, {"k"}});
  g_jt = f.cat().table({kPublicSchema, "jt"})->id;
}

auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, std::span<const PhysType> result = {}, bc_budget budget = {})
    -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(result), f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, budget, false, 0);
}

auto text(const bc_value& v) -> std::string {
  const auto& b = std::get<Bytes>(v);
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

auto is_null(const bc_value& v) -> bool { return std::holds_alternative<std::monostate>(v); }

void insert_jt(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, g_jt);
  bc_row_set_param(cx, r, kKCol, 0);
  bc_row_set_param(cx, r, kJCol, 1);
  bc_row_set_param(cx, r, kSCol, 2);
  bc_insert(cx, g_jt, bc_row_end(cx, r));
}

// Over every row of jt, in key order, `emit` of its j (NULL as NULL).
template <typename Emit>
void each_j(bc_ctx* cx, Emit emit) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    if (bc_col_null(cx, row, kJCol)) {
      bc_emit_null(cx);
    } else {
      emit(cx, bc_col_jsonb(cx, row, kJCol));
    }
    bc_emit_end(cx);
  }
}

void emit_node(bc_ctx* cx, bool found, bc_jsonb v) {
  if (found) {
    bc_emit_jsonb(cx, v);
  } else {
    bc_emit_null(cx);
  }
}
void emit_text(bc_ctx* cx, bool found, bc_view v) {
  if (found) {
    bc_emit_bytes(cx, v);
  } else {
    bc_emit_null(cx);
  }
}

// SELECT j -> $1, j ->> $1, j -> $2 (an integer), j ->> $2, j #> $1 and
// j #>> $1 (a path's text), each a plan of its own.
void field(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_jsonb v{};
    emit_node(c, bc_jsonb_field(c, j, bc_param_bytes(c, 0), &v), v);
  });
}
void field_text(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_view v{};
    emit_text(c, bc_jsonb_field_text(c, j, bc_param_bytes(c, 0), &v), v);
  });
}
void element(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_jsonb v{};
    emit_node(c, bc_jsonb_element(c, j, bc_param_i64(c, 1), &v), v);
  });
}
void element_text(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_view v{};
    emit_text(c, bc_jsonb_element_text(c, j, bc_param_i64(c, 1), &v), v);
  });
}
void path(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_jsonb v{};
    emit_node(c, bc_jsonb_path(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 0)), &v), v);
  });
}
void path_text(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_view v{};
    emit_text(c, bc_jsonb_path_text(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 0)), &v), v);
  });
}
// j #> ARRAY['a', $1]: the path built element by element, $1 NULL as NULL.
void path_array(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_view p = bc_empty(c);
    p = bc_jsonb_path_add(c, p, bc_param_bytes(c, 0), false);
    bool null = bc_param_null(c, 1);
    p = bc_jsonb_path_add(c, p, null ? bc_empty(c) : bc_param_bytes(c, 1), null);
    bc_jsonb v{};
    emit_node(c, bc_jsonb_path(c, j, p, &v), v);
  });
}
void type_of(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bytes(c, bc_jsonb_typeof(c, j)); });
}
void array_length(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_i64(c, bc_jsonb_array_length(c, j)); });
}
void to_int(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_i64(c, bc_jsonb_to_i64(c, j, BC_INTEGER, 0)); });
}
void to_bigint(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_i64(c, bc_jsonb_to_i64(c, j, BC_BIGINT, 0)); });
}
void to_double(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_f64(c, bc_jsonb_to_f64(c, j, false)); });
}
void to_bool(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_to_bool(c, j)); });
}
void to_num(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_num(c, bc_jsonb_to_num(c, j)); });
}

// Misuse: a NULL out, a text view as a path, a slice kept past its step.
void null_out(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    (void)bc_jsonb_field(c, j, bc_param_bytes(c, 0), nullptr);
    bc_emit_null(c);
  });
}
void text_as_path(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_jsonb v{};
    emit_node(c, bc_jsonb_path(c, j, bc_param_bytes(c, 0), &v), v);
  });
}
void slice_past_its_step(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  bc_jsonb kept{};
  bool first = true;
  while (bc_scan_next(cx, c)) {
    if (!first) {
      bc_emit_begin(cx);
      bc_emit_jsonb(cx, kept);
      bc_emit_end(cx);
      return;
    }
    (void)bc_jsonb_element(cx, bc_col_jsonb(cx, bc_scan_row(cx, c), kJCol), 0, &kept);
    first = false;
  }
}

// A stored row of jt whose j holds `value` as it is, past the encoder.
void put_raw(Fixture& f, std::int64_t k, const Bytes& value) {
  const auto* t = f.cat().table(g_jt);
  bytecask::sql::RowWriter w{t->layout};
  REQUIRE(w.set_i64(kKCol, k));
  REQUIRE(w.set_bytes(kJCol, value));
  Bytes row;
  REQUIRE(w.finish(row));
  Bytes key;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, g_jt);
  REQUIRE(bytecask::sql::key_put_i64(key, k, {PhysType::I64}));
  f.db->put({}, key, row);
}

auto raw(std::initializer_list<int> b) -> Bytes {
  Bytes v;
  for (auto x : b) v.push_back(static_cast<std::byte>(x));
  return v;
}

// One column of strings and NULLs (nullptr), the form results compare in.
auto column(Fixture& f, std::span<const PhysType> types) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& row : f.results(types)) {
    if (is_null(row[0])) {
      out.emplace_back("NULL");
    } else if (const auto* i = std::get_if<std::int64_t>(&row[0])) {
      out.push_back(std::to_string(*i));
    } else if (const auto* d = std::get_if<double>(&row[0])) {
      out.push_back(std::to_string(*d));
    } else if (const auto* b = std::get_if<bool>(&row[0])) {
      out.emplace_back(*b ? "t" : "f");
    } else {
      out.push_back(text(row[0]));
    }
  }
  return out;
}

using Col = std::vector<std::string>;

void fill(Fixture& f, std::initializer_list<const char*> docs) {
  std::int64_t k = 0;
  for (const auto* d : docs) {
    ++k;
    REQUIRE(run(f, insert_jt, {k, d == nullptr ? kNull : bytes(d), kNull}) == BC_COMMITTED);
  }
}

}  // namespace

TEST_CASE("plan API jsonb accessors: -> and ->> by key and by index, NULL where PostgreSQL's is", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {R"({"a": {"b": [1, "x"]}, "k": "v\n", "n": null})", "[10, 20, 30]", "\"str\"", "5", nullptr});
  REQUIRE(run(f, field, {bytes("a"), std::int64_t{0}}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"{\"b\": [1, \"x\"]}", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, field_text, {bytes("k"), std::int64_t{0}}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"v\n", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, field_text, {bytes("n"), std::int64_t{0}}, kBytesRow) == BC_COMMITTED);  // a JSON null: SQL NULL
  CHECK(column(f, kBytesRow) == Col{"NULL", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, field_text, {bytes("a"), std::int64_t{0}}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"{\"b\": [1, \"x\"]}", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, element, {bytes(""), std::int64_t{-1}}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "30", "\"str\"", "5", "NULL"});
  REQUIRE(run(f, element_text, {bytes(""), std::int64_t{0}}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"NULL", "10", "str", "5", "NULL"});
  REQUIRE(run(f, element, {bytes(""), std::int64_t{3}}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, element, {bytes(""), std::int64_t{-4}}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "NULL", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, element, {bytes(""), INT64_MIN}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "NULL", "NULL", "NULL", "NULL"});
}

TEST_CASE("plan API jsonb accessors: #> and #>> over a path's text, or one built element by element", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {R"({"a": {"b": [1, "x"]}})", "[10, 20]", "\"s\"", nullptr});
  REQUIRE(run(f, path, {bytes("{a,b,1}")}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"\"x\"", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, path_text, {bytes("{a,b}")}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"[1, \"x\"]", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, path_text, {bytes("{}")}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"{\"a\": {\"b\": [1, \"x\"]}}", "[10, 20]", "s", "NULL"});
  REQUIRE(run(f, path, {bytes("{ -1 }")}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "20", "NULL", "NULL"});
  REQUIRE(run(f, path_array, {bytes("a"), bytes("b")}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"[1, \"x\"]", "NULL", "NULL", "NULL"});
  REQUIRE(run(f, path_array, {bytes("a"), kNull}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"NULL", "NULL", "NULL", "NULL"});
  // array_in's error, the plan's BC_E_DATA with its DETAIL.
  CHECK(run(f, path, {bytes("{a")}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_DATA);
  CHECK(bc_error_detail(f.cx) == BC_SQLSTATE('2', '2', 'P', '0', '2'));
  CHECK(std::string_view{bc_error_message(f.cx)} == "malformed array literal: \"{a\"");
  CHECK(std::string_view{bc_error_note(f.cx)} == "Unexpected end of input.");
}

TEST_CASE("plan API jsonb accessors: jsonb_typeof, jsonb_array_length and the casts out", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {"[1, [2], {}]", "{}", "2.5", "true", "null", "\"x\""});
  REQUIRE(run(f, type_of, {}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) == Col{"array", "object", "number", "boolean", "null", "string"});
  struct Failing {
    bc_plan_fn plan;
    std::span<const PhysType> row;
    std::uint32_t state;
    std::string_view message;
  };
  for (const auto& c : {Failing{array_length, kI64Row, BC_SQLSTATE('2', '2', '0', '2', '3'), "cannot get array length of a non-array"},
                        Failing{to_int, kI64Row, BC_SQLSTATE('2', '2', '0', '2', '3'), "cannot cast jsonb array to type integer"},
                        Failing{to_double, kF64Row, BC_SQLSTATE('2', '2', '0', '2', '3'), "cannot cast jsonb array to type double precision"},
                        Failing{to_bool, kBoolRow, BC_SQLSTATE('2', '2', '0', '2', '3'), "cannot cast jsonb array to type boolean"},
                        Failing{to_num, kNumRow, BC_SQLSTATE('2', '2', '0', '2', '3'), "cannot cast jsonb array to type numeric"}}) {
    CHECK(run(f, c.plan, {}, c.row) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_DATA);
    CHECK(bc_error_detail(f.cx) == c.state);
    CHECK(std::string_view{bc_error_message(f.cx)} == c.message);
  }
  Fixture g;
  make_jt(g);
  fill(g, {"1.5", "-2.5", "1e3", "0.49"});
  REQUIRE(run(g, to_int, {}, kI64Row) == BC_COMMITTED);
  CHECK(column(g, kI64Row) == Col{"2", "-3", "1000", "0"});
  REQUIRE(run(g, to_double, {}, kF64Row) == BC_COMMITTED);
  CHECK(column(g, kF64Row) == Col{"1.500000", "-2.500000", "1000.000000", "0.490000"});
  REQUIRE(run(g, to_num, {}, kNumRow) == BC_COMMITTED);
  CHECK(column(g, kNumRow) == Col{"1.5", "-2.5", "1000", "0.49"});
  REQUIRE(run(g, array_length, {}, kI64Row) == BC_FAILED);
  CHECK(std::string_view{bc_error_message(g.cx)} == "cannot get array length of a scalar");
  // Past the target's range: BC_C_RANGE, as bc_check_int's.
  REQUIRE(run(g, insert_jt, {std::int64_t{9}, bytes("3000000000"), kNull}) == BC_COMMITTED);
  CHECK(run(g, to_int, {}, kI64Row) == BC_FAILED);
  CHECK(bc_error(g.cx) == BC_E_CONSTRAINT);
  CHECK(std::string_view{bc_error_message(g.cx)} == "integer out of range");
  REQUIRE(run(g, to_bigint, {}, kI64Row) == BC_COMMITTED);
  REQUIRE(run(g, insert_jt, {std::int64_t{10}, bytes("1e400"), kNull}) == BC_COMMITTED);
  CHECK(run(g, to_double, {}, kF64Row) == BC_FAILED);
  CHECK(bc_error(g.cx) == BC_E_DATA);
  CHECK(bc_error_detail(g.cx) == BC_SQLSTATE('2', '2', '0', '0', '3'));
  CHECK(run(g, to_bigint, {}, kI64Row) == BC_FAILED);
  CHECK(std::string_view{bc_error_message(g.cx)} == "bigint out of range");
  Fixture h;
  make_jt(h);
  fill(h, {"true", "false"});
  REQUIRE(run(h, to_bool, {}, kBoolRow) == BC_COMMITTED);
  CHECK(column(h, kBoolRow) == Col{"t", "f"});
}

TEST_CASE("plan API jsonb accessors: misuse is BC_E_HANDLE, damage BC_E_CORRUPT", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {"[1, 2]", "[3]"});
  CHECK(run(f, null_out, {bytes("a")}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, text_as_path, {bytes("{a}")}, kJsonbRow) == BC_FAILED);  // text, not a path's form
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A slice shares its row's lifetime: stale once the loop steps.
  CHECK(run(f, slice_past_its_step, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A stored array whose element end runs past its area, and an object
  // whose keys are out of order: met by the lookup itself.
  put_raw(f, 1, raw({0xF8, 0x06, 1, 0, 0, 0, 9, 0, 0, 0, 0x01}));
  put_raw(f, 2, raw({0xF8, 0x07, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 'a', 'b', 0x01, 0x03}));
  for (auto* plan : {element, element_text}) {
    CHECK(run(f, plan, {bytes("a"), std::int64_t{0}}, plan == element ? std::span<const PhysType>{kJsonbRow} : std::span<const PhysType>{kBytesRow}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  }
  put_raw(f, 1, raw({0xF8, 0x06, 0, 0, 0, 0}));
  CHECK(run(f, field, {bytes("a"), std::int64_t{0}}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  CHECK(run(f, type_of, {}, kBytesRow) == BC_COMMITTED);  // reads only the tags
}

TEST_CASE("plan API jsonb accessors: a budget sweep gives BC_E_LIMIT or the whole result", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  std::string doc = "{\"k\": [";
  for (int i = 0; i < 40; ++i) doc += (i == 0 ? "" : ", ") + std::string{"{\"v\": 1e30, \"s\": \"\\n\\u0001x\"}"};
  doc += "], \"n\": -123.456e2}";
  fill(f, {doc.c_str()});
  struct Case {
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::span<const PhysType> row;
  };
  for (const auto& c : {Case{field_text, {bytes("k"), std::int64_t{0}}, kBytesRow}, Case{path, {bytes("{k,39,s}")}, kJsonbRow},
                        Case{path_text, {bytes("{k,0}")}, kBytesRow}, Case{path_array, {bytes("n"), kNull}, kJsonbRow}}) {
    REQUIRE(run(f, c.plan, c.params, c.row) == BC_COMMITTED);
    auto want = column(f, c.row);
    auto limits = 0;
    for (std::uint64_t max = 1;; max += 37) {  // 0 is no budget
      bc_budget budget{};
      budget.max_bytes = max;
      auto r = run(f, c.plan, c.params, c.row, budget);
      if (r != BC_COMMITTED) {
        CHECK(bc_error(f.cx) == BC_E_LIMIT);
        ++limits;
        continue;
      }
      CHECK(column(f, c.row) == want);
      break;
    }
    CHECK(limits > 0);
  }
}

TEST_CASE("plan API jsonb accessors: every allocation fails in turn as BC_E_MEMORY", "[sql][algebra][jsonb][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Fixture f;
  make_jt(f);
  fill(f, {R"({"b": [1, 2.50, {"z": null, "a": "é\n"}], "a": -0.001})", "[true, 3]"});
  struct Case {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
  };
  const std::vector<Case> cases{
      {"bc_jsonb_field", field, {bytes("b"), std::int64_t{0}}, {PhysType::Jsonb}},
      {"bc_jsonb_field_text", field_text, {bytes("b"), std::int64_t{0}}, {PhysType::Bytes}},
      {"bc_jsonb_element_text", element_text, {bytes(""), std::int64_t{1}}, {PhysType::Bytes}},
      {"bc_jsonb_path_in and bc_jsonb_path", path, {bytes("{b,2,a}")}, {PhysType::Jsonb}},
      {"bc_jsonb_path_text", path_text, {bytes("{b}")}, {PhysType::Bytes}},
      {"bc_jsonb_path_add", path_array, {bytes("b"), bytes("1")}, {PhysType::Jsonb}},
      {"bc_jsonb_typeof", type_of, {}, {PhysType::Bytes}},
  };
  for (const auto& c : cases) {
    INFO(c.name);
    auto failed = 0;
    auto memory_errors = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 100000);
      auto ps = params(c.params);
      SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(c.result), f.cat().version()};
      auto ref = sr.get();
      bc_ctx_begin(f.cx, &ps, {});
      auto threw = false;
      bytecask::testing::AllocFaultReport report;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          bc_ctx_run(f.cx, c.plan, &ref);
        } catch (...) {
          threw = true;
        }
        report = faults.report();
      }
      INFO("allocation " << nth);
      CHECK_FALSE(threw);
      auto error = static_cast<int>(bc_error(f.cx));
      if (!report.fired) {
        CHECK(error == BC_OK);
        bc_ctx_reset(f.cx);
        break;
      }
      bc_ctx_reset(f.cx);
      CHECK((error == BC_E_MEMORY || error == BC_OK));
      CHECK(bc_ctx_held_for_testing(f.cx) == 0);
      ++failed;
      if (error == BC_E_MEMORY) ++memory_errors;
    }
    // Each allocation of the run was failed once, and failures reached the
    // calls as BC_E_MEMORY; which of sql/jsonb.cppm's buffers a sweep
    // reaches is asserted by "jsonb accessors: every allocation of the calls
    // that allocate fails in turn as bad_alloc".
    CHECK(failed > 0);
    CHECK(memory_errors > 0);
  }
#endif
}
