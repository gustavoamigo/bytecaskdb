// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb's containment, existence, modification and construction, plan API
// revision 25 (docs/jsonb_design.md, stage 4): bc_jsonb_contains, _exists,
// _exists_any and _all, _concat, _delete_key, _index, _keys and _path,
// _set, _set_lax, _insert, _strip_nulls, _pretty, the bc_jsonb_of_* calls
// and the build calls. Each call's result, its errors with their SQLSTATE
// and HINT, a result that changes nothing being its input's own handle,
// BC_E_HANDLE for a view that is no path and a build without its arrays,
// BC_E_CORRUPT on a damaged stored value, and the budget and
// allocation-failure sweeps. And stage 2's comparison and sort key,
// bc_jsonb_cmp and bc_sort_key_jsonb (revision 26).

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
constexpr std::array<PhysType, 1> kBoolRow{PhysType::Bool};
constexpr std::array<PhysType, 1> kI64Row{PhysType::I64};

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

// SELECT f(j, …) over every row of jt, each a plan of its own. $1 is a
// jsonb, $2 a text, $3 a path's text.
void contains(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_contains(c, j, bc_param_jsonb(c, 0))); });
}
void contained(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_contains(c, bc_param_jsonb(c, 0), j)); });
}
void exists(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_exists(c, j, bc_param_bytes(c, 1))); });
}
void exists_any(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_exists_any(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)))); });
}
void exists_all(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bool(c, bc_jsonb_exists_all(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)))); });
}
void concat(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_concat(c, j, bc_param_jsonb(c, 0))); });
}
void delete_key(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_delete_key(c, j, bc_param_bytes(c, 1))); });
}
void delete_index(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_delete_index(c, j, -1)); });
}
void delete_keys(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_delete_keys(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)))); });
}
void delete_path(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_delete_path(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)))); });
}
void set(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_emit_jsonb(c, bc_jsonb_set(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)), bc_param_jsonb(c, 0), true));
  });
}
void insert_after(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_emit_jsonb(c, bc_jsonb_insert(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)), bc_param_jsonb(c, 0), true));
  });
}
// jsonb_set_lax(j, $3, NULL, true, $2): the value SQL NULL, $2 NULL as NULL.
void set_lax_null(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bool null = bc_param_null(c, 1);
    bc_jsonb none{};
    bc_emit_jsonb(c, bc_jsonb_set_lax(c, j, bc_jsonb_path_in(c, bc_param_bytes(c, 2)), none, true, true,
                                      null ? bc_empty(c) : bc_param_bytes(c, 1), null));
  });
}
void strip_nulls(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_strip_nulls(c, j)); });
}
void pretty(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_bytes(c, bc_jsonb_pretty(c, j)); });
}
// The unchanged input's own handle: j - 'zz' is j.
void same_handle(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) {
    bc_jsonb r = bc_jsonb_delete_key(c, j, bc_param_bytes(c, 1));
    bc_emit_bool(c, r.i == j.i && r.g == j.g);
  });
}
// to_jsonb of one value of each kind, and the builds over them.
void of_values(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_of_i64(cx, 150, BC_TYPE_DECIMAL, 2));
  bc_emit_jsonb(cx, bc_jsonb_of_i64(cx, -7, BC_TYPE_INTEGER, 0));
  bc_emit_jsonb(cx, bc_jsonb_of_i64(cx, 0, BC_TYPE_TIMESTAMPTZ, 0));
  bc_emit_jsonb(cx, bc_jsonb_of_i64(cx, 19724, BC_TYPE_DATE, 0));
  bc_emit_jsonb(cx, bc_jsonb_of_f64(cx, 1e100, false, false));
  bc_emit_jsonb(cx, bc_jsonb_of_f64(cx, 1e100, false, true));
  bc_emit_jsonb(cx, bc_jsonb_of_f64(cx, 0.1, true, false));
  bc_emit_jsonb(cx, bc_jsonb_of_bool(cx, true));
  bc_emit_jsonb(cx, bc_jsonb_of_text(cx, bc_param_bytes(cx, 1)));
  bc_emit_jsonb(cx, bc_jsonb_of_num(cx, bc_param_bytes(cx, 3)));
  bc_emit_end(cx);
}
void build(bc_ctx* cx) {
  bc_jsonb a[4] = {bc_jsonb_of_text(cx, bc_param_bytes(cx, 1)), bc_param_jsonb(cx, 0), bc_jsonb_of_i64(cx, 2, BC_TYPE_INTEGER, 0), {0, 0}};
  bool nulls[4] = {false, false, false, true};
  bool composite[4] = {false, false, false, false};
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_build_array(cx, 4, a, nulls));
  bc_emit_jsonb(cx, bc_jsonb_build_object(cx, 4, a, nulls, composite));
  bc_emit_jsonb(cx, bc_jsonb_build_array(cx, 0, nullptr, nullptr));
  bc_emit_end(cx);
}
// A jsonb argument as a key: 22023, and a NULL key 22023 with its position.
void build_composite_key(bc_ctx* cx) {
  bc_jsonb a[2] = {bc_param_jsonb(cx, 0), bc_param_jsonb(cx, 0)};
  bool nulls[2] = {false, false};
  bool composite[2] = {true, false};
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_build_object(cx, 2, a, nulls, composite));
  bc_emit_end(cx);
}
void build_without_arrays(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_build_array(cx, 2, nullptr, nullptr));
  bc_emit_end(cx);
}
void of_bad_type(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_of_i64(cx, 1, BC_TYPE_TEXT, 0));
  bc_emit_end(cx);
}
// Stage 2: j compared with $1 (-1, 0 or 1), and jt's keys in the order of
// a sort by j (its jsonb sort key, or bc_sort_key_col's part), NULLs last,
// ties by key.
void compare(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_i64(c, bc_jsonb_cmp(c, j, bc_param_jsonb(c, 0))); });
}
template <bool kCol, std::uint32_t kDir>
void sorted(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sort_key sk = bc_sort_key_begin(cx);
    if (kCol) {
      bc_sort_key_col(cx, sk, row, kJCol, kDir, BC_SORT_NULLS_LAST);
    } else if (bc_col_null(cx, row, kJCol)) {
      bc_sort_key_null(cx, sk, kDir, BC_SORT_NULLS_LAST);
    } else {
      bc_sort_key_jsonb(cx, sk, bc_col_jsonb(cx, row, kJCol), kDir, BC_SORT_NULLS_LAST);
    }
    bc_sort_key_i64(cx, sk, bc_col_i64(cx, row, kKCol), BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sorter_add(cx, s, bc_sort_key_end(cx, sk), row);
  }
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, bc_col_i64(cx, p, kKCol));
    bc_emit_end(cx);
  }
}

void text_as_path(bc_ctx* cx) {
  each_j(cx, [](bc_ctx* c, bc_jsonb j) { bc_emit_jsonb(c, bc_jsonb_delete_path(c, j, bc_param_bytes(c, 2))); });
}

constexpr std::array<PhysType, 10> kOfRow{PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb,
                                          PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb};
constexpr std::array<PhysType, 3> kBuildRow{PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb};

auto row_text(Fixture& f, std::span<const PhysType> types) -> std::vector<std::string> {
  auto rows = f.results(types);
  REQUIRE(rows.size() == 1);
  std::vector<std::string> out;
  for (const auto& v : rows[0]) out.push_back(is_null(v) ? "NULL" : text(v));
  return out;
}

auto state(bc_ctx* cx) -> std::uint32_t { return bc_error_detail(cx); }

// The parameters of each plan: $1 jsonb, $2 text, $3 a path, $4 a NUMERIC's text.
auto ps(const char* doc, const char* key, const char* p, const char* num = "1.50") -> std::vector<bc_value> {
  return {bytes(doc), key == nullptr ? kNull : bytes(key), bytes(p), bytes(num)};
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

TEST_CASE("plan API jsonb ops: @>, <@, ?, ?| and ?& over each row", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {R"({"a": [1, 2], "b": {"c": null}})", R"(["a", 1, [2]])", "\"a\"", "1"});
  REQUIRE(run(f, contains, ps(R"({"a": [2]})", "a", "{a}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"t", "f", "f", "f"});
  REQUIRE(run(f, contains, ps("[[2], 1]", "a", "{a}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"f", "t", "f", "f"});
  REQUIRE(run(f, contained, ps("[1, \"a\", [2, 3]]", "a", "{a}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"f", "t", "t", "t"});
  REQUIRE(run(f, exists, ps("{}", "a", "{a}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"t", "t", "t", "f"});
  REQUIRE(run(f, exists_any, ps("{}", "a", "{zz,b,NULL}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"t", "f", "f", "f"});
  REQUIRE(run(f, exists_all, ps("{}", "a", "{a,b}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"t", "f", "f", "f"});
}

TEST_CASE("plan API jsonb ops: ||, -, #-, jsonb_set, jsonb_insert, jsonb_strip_nulls and jsonb_pretty", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {R"({"a": [1, 2], "b": {"c": null}})", R"({"x": ["a", 1], "a": "s"})"});
  REQUIRE(run(f, concat, ps(R"({"a": 0})", "a", "{a}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"a": 0, "b": {"c": null}})", R"({"a": 0, "x": ["a", 1]})"});
  REQUIRE(run(f, delete_key, ps("{}", "a", "{a}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"b": {"c": null}})", R"({"x": ["a", 1]})"});
  REQUIRE(run(f, delete_keys, ps("{}", "a", "{a,b}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{"{}", R"({"x": ["a", 1]})"});
  REQUIRE(run(f, delete_path, ps("{}", "a", "{b,c}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"a": [1, 2], "b": {}})", R"({"a": "s", "x": ["a", 1]})"});
  REQUIRE(run(f, set, ps("true", "a", "{a,-1}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"a": [1, true], "b": {"c": null}})", R"({"a": "s", "x": ["a", 1]})"});
  REQUIRE(run(f, insert_after, ps("9", "a", "{x,0}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"a": [1, 2], "b": {"c": null}})", R"({"a": "s", "x": ["a", 9, 1]})"});
  REQUIRE(run(f, set_lax_null, ps("{}", "delete_key", "{a}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"b": {"c": null}})", R"({"x": ["a", 1]})"});
  REQUIRE(run(f, set_lax_null, ps("{}", "use_json_null", "{0}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"0": null, "a": [1, 2], "b": {"c": null}})", R"({"0": null, "a": "s", "x": ["a", 1]})"});
  REQUIRE(run(f, strip_nulls, {}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(f, kJsonbRow) == Col{R"({"a": [1, 2], "b": {}})", R"({"a": "s", "x": ["a", 1]})"});
  REQUIRE(run(f, pretty, {}, kBytesRow) == BC_COMMITTED);
  CHECK(column(f, kBytesRow) ==
        Col{"{\n    \"a\": [\n        1,\n        2\n    ],\n    \"b\": {\n        \"c\": null\n    }\n}",
            "{\n    \"a\": \"s\",\n    \"x\": [\n        \"a\",\n        1\n    ]\n}"});
  REQUIRE(run(f, delete_index, {}, kJsonbRow) == BC_FAILED);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '2', '3'));
  CHECK(std::string_view{bc_error_message(f.cx)} == "cannot delete from object using integer index");
  REQUIRE(run(f, same_handle, ps("{}", "zz", "{a}"), kBoolRow) == BC_COMMITTED);
  CHECK(column(f, kBoolRow) == Col{"t", "t"});  // an object's missing key changes nothing: no copy
  Fixture g;
  make_jt(g);
  fill(g, {R"(["a", 1, [2]])"});
  REQUIRE(run(g, delete_index, {}, kJsonbRow) == BC_COMMITTED);
  CHECK(column(g, kJsonbRow) == Col{R"(["a", 1])"});
  REQUIRE(run(g, delete_key, ps("{}", "a", "{}"), kJsonbRow) == BC_COMMITTED);
  CHECK(column(g, kJsonbRow) == Col{"[1, [2]]"});
}

TEST_CASE("plan API jsonb ops: bc_jsonb_cmp and bc_sort_key_jsonb order as PostgreSQL 17's jsonb_ops (stage 2)",
          "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  // Keys 1..12; PostgreSQL 17's ORDER BY j, k: 2 6 4 9 5 12 3 10 7 8 1, then the NULL.
  fill(f, {R"({"b": 1, "c": 1})", "[]", "true", "\"a\"", "1.00", "null", "[1, 2]", "{}", "-1", "[null]", nullptr, "1"});
  const Col asc{"2", "6", "4", "9", "5", "12", "3", "10", "7", "8", "1", "11"};
  const Col desc{"1", "8", "7", "10", "3", "5", "12", "9", "4", "6", "2", "11"};
  REQUIRE(run(f, sorted<false, BC_SORT_ASC>, {}, kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row) == asc);
  REQUIRE(run(f, sorted<true, BC_SORT_ASC>, {}, kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row) == asc);
  REQUIRE(run(f, sorted<false, BC_SORT_DESC>, {}, kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row) == desc);
  REQUIRE(run(f, sorted<true, BC_SORT_DESC>, {}, kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row) == desc);
  // By value: 1.0 equals 1.00 and 1; key order is no order.
  REQUIRE(run(f, compare, ps("1.0", "a", "{}"), kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row) == Col{"1", "-1", "1", "-1", "0", "-1", "1", "1", "-1", "1", "NULL", "0"});
  REQUIRE(run(f, compare, ps(R"({"c": 1, "b": 1.0})", "a", "{}"), kI64Row) == BC_COMMITTED);
  CHECK(column(f, kI64Row).front() == "0");
  // A damaged stored value is BC_E_CORRUPT, never an order.
  put_raw(f, 13, raw({0xF8, 0x05, 'a', 0x00}));
  for (auto* plan : {compare, sorted<false, BC_SORT_ASC>, sorted<true, BC_SORT_DESC>}) {
    CHECK(run(f, plan, ps("1", "a", "{}"), kI64Row) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  }
}

TEST_CASE("plan API jsonb ops: errors carry PostgreSQL's SQLSTATE, message and HINT", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {R"({"a": 1})"});
  REQUIRE(run(f, insert_after, ps("9", "a", "{a}"), kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_DATA);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '2', '3'));
  CHECK(std::string_view{bc_error_message(f.cx)} == "cannot replace existing key");
  CHECK(std::string_view{bc_error_hint(f.cx)} == "Try using the function jsonb_set to replace key value.");
  REQUIRE(run(f, set, ps("9", "a", "{a,NULL}"), kJsonbRow) == BC_FAILED);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '0', '4'));
  CHECK(std::string_view{bc_error_message(f.cx)} == "path element at position 2 is null");
  REQUIRE(run(f, set, ps("9", "a", "{{a},{b}}"), kJsonbRow) == BC_FAILED);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '2', 'E'));
  REQUIRE(run(f, set_lax_null, ps("{}", "raise_exception", "{a}"), kJsonbRow) == BC_FAILED);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '0', '4'));
  CHECK(std::string_view{bc_error_message(f.cx)} == "JSON value must not be null");
  CHECK(std::string_view{bc_error_note(f.cx)} == R"(Exception was raised because null_value_treatment is "raise_exception".)");
  REQUIRE(run(f, set_lax_null, ps("{}", nullptr, "{a}"), kJsonbRow) == BC_FAILED);
  CHECK(state(f.cx) == BC_SQLSTATE('2', '2', '0', '2', '3'));
  REQUIRE(run(f, build_composite_key, ps("1", "a", "{a}"), kJsonbRow) == BC_FAILED);
  CHECK(std::string_view{bc_error_message(f.cx)} == "key value must be scalar, not array, composite, or json");
}

TEST_CASE("plan API jsonb ops: to_jsonb of each kind of value and the build calls", "[sql][algebra][jsonb]") {
  Fixture f;
  REQUIRE(run(f, of_values, ps("{}", "a\"b", "{}", "NaN"), kOfRow) == BC_COMMITTED);
  CHECK(row_text(f, kOfRow) == std::vector<std::string>{"1.50", "-7", "\"1970-01-01T00:00:00+00:00\"", "\"2024-01-02\"",
                                                        "1" + std::string(100, '0'), "\"1e+100\"", "0.1", "true", "\"a\\\"b\"",
                                                        "\"NaN\""});
  REQUIRE(run(f, build, ps(R"({"x": [1]})", "k", "{}"), kBuildRow) == BC_COMMITTED);
  CHECK(row_text(f, kBuildRow) == std::vector<std::string>{R"(["k", {"x": [1]}, 2, null])", R"({"2": null, "k": {"x": [1]}})", "[]"});
}

TEST_CASE("plan API jsonb ops: misuse is BC_E_HANDLE, damage BC_E_CORRUPT", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  fill(f, {"[1, 2]"});
  CHECK(run(f, text_as_path, ps("{}", "a", "{0}"), kJsonbRow) == BC_FAILED);  // a text, not a path's form
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, build_without_arrays, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, of_bad_type, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  put_raw(f, 1, raw({0xF8, 0x06, 1, 0, 0, 0, 9, 0, 0, 0, 0x01}));
  for (auto* plan : {contains, exists, concat, delete_key, strip_nulls}) {
    CHECK(run(f, plan, ps("[1]", "a", "{0}"), plan == contains || plan == exists ? std::span<const PhysType>{kBoolRow}
                                                                                  : std::span<const PhysType>{kJsonbRow}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  }
}

TEST_CASE("plan API jsonb ops: a budget sweep gives BC_E_LIMIT or the whole result", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  std::string doc = "{\"k\": [";
  for (int i = 0; i < 40; ++i) doc += (i == 0 ? "" : ", ") + std::string{"{\"v\": 1e30, \"s\": \"\\n\\u0001x\", \"n\": null}"};
  doc += "], \"n\": -123.456e2}";
  fill(f, {doc.c_str()});
  struct Case {
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::span<const PhysType> row;
  };
  for (const auto& c : {Case{contains, ps(R"({"k": [{"n": null}]})", "a", "{}"), kBoolRow},
                        Case{concat, ps(R"({"z": [1]})", "a", "{}"), kJsonbRow}, Case{set, ps("[1]", "a", "{k,39,v}"), kJsonbRow},
                        Case{strip_nulls, {}, kJsonbRow}, Case{pretty, {}, kBytesRow}, Case{delete_keys, ps("{}", "a", "{n,k}"), kJsonbRow},
                        Case{compare, ps(R"({"n": 1})", "a", "{}"), kI64Row}, Case{sorted<false, BC_SORT_DESC>, {}, kI64Row}}) {
    REQUIRE(run(f, c.plan, c.params, c.row) == BC_COMMITTED);
    auto want = column(f, c.row);
    auto limits = 0;
    for (std::uint64_t max = 1;; max += 97) {  // 0 is no budget
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

TEST_CASE("plan API jsonb ops: every allocation fails in turn as BC_E_MEMORY", "[sql][algebra][jsonb][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Fixture f;
  make_jt(f);
  fill(f, {R"({"b": [1, 2.50, {"z": null, "a": "é\n"}], "a": -0.001})", "[true, 3, \"a\"]"});
  struct Case {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
  };
  const std::vector<Case> cases{
      {"bc_jsonb_contains", contains, ps(R"({"b": [{"z": null}]})", "a", "{}"), {PhysType::Bool}},
      {"bc_jsonb_concat", concat, ps("[9]", "a", "{}"), {PhysType::Jsonb}},
      {"bc_jsonb_delete_keys", delete_keys, ps("{}", "a", "{a,b}"), {PhysType::Jsonb}},
      {"bc_jsonb_set", set, ps(R"({"y": 1})", "a", "{1}"), {PhysType::Jsonb}},
      {"bc_jsonb_strip_nulls", strip_nulls, {}, {PhysType::Jsonb}},
      {"bc_jsonb_pretty", pretty, {}, {PhysType::Bytes}},
      {"bc_jsonb_of_* and the build calls", build, ps(R"({"x": [1]})", "k", "{}"), {PhysType::Jsonb, PhysType::Jsonb, PhysType::Jsonb}},
  };
  for (const auto& c : cases) {
    INFO(c.name);
    auto failed = 0;
    auto memory_errors = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 100000);
      auto p = params(c.params);
      SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(c.result), f.cat().version()};
      auto ref = sr.get();
      bc_ctx_begin(f.cx, &p, {});
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
    // Each allocation was failed once and reached the calls as
    // BC_E_MEMORY; which of sql/jsonb.cppm's buffers a sweep reaches is
    // asserted by "jsonb ops: every allocation of the calls that allocate
    // fails in turn as bad_alloc".
    CHECK(failed > 0);
    CHECK(memory_errors > 0);
  }
#endif
}
