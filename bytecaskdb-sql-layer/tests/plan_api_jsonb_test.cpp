// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The jsonb calls of plan API revision 21 (docs/jsonb_design.md, *Plan API
// for jsonb*): bc_col_jsonb, bc_row_set_jsonb, bc_param_jsonb, bc_jsonb_in,
// bc_jsonb_text and bc_emit_jsonb, and bc_row_set_param and bc_emit_col of a
// jsonb column. Each call's result and its NULL, its user errors with their
// SQLSTATE, DETAIL and CONTEXT, BC_E_CORRUPT on a damaged stored value,
// BC_E_HANDLE for a handle of the other form, a node that dies with its
// row's step, and a budget sweep that gives 54000 or the whole result.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <cstdint>
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

// INSERT INTO jt VALUES ($1, $2, $3), $2 read from its text.
void insert_jt(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, g_jt);
  bc_row_set_param(cx, r, kKCol, 0);
  bc_row_set_param(cx, r, kJCol, 1);
  bc_row_set_param(cx, r, kSCol, 2);
  bc_insert(cx, g_jt, bc_row_end(cx, r));
}

// SELECT j FROM jt ORDER BY k, as bc_emit_jsonb of bc_col_jsonb.
void select_j(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    if (bc_col_null(cx, row, kJCol)) {
      bc_emit_null(cx);
    } else {
      bc_emit_jsonb(cx, bc_col_jsonb(cx, row, kJCol));
    }
    bc_emit_end(cx);
  }
}

// The same, as bc_emit_col.
void select_j_col(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, bc_scan_row(cx, c), kJCol);
    bc_emit_end(cx);
  }
}

// SELECT j::text FROM jt ORDER BY k.
void select_j_text(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    if (bc_col_null(cx, row, kJCol)) {
      bc_emit_null(cx);
    } else {
      bc_emit_bytes(cx, bc_jsonb_text(cx, bc_col_jsonb(cx, row, kJCol)));
    }
    bc_emit_end(cx);
  }
}

// SELECT $1::text::jsonb: bc_jsonb_in of a bytes view.
void param_in(bc_ctx* cx) {
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, bc_jsonb_in(cx, bc_param_bytes(cx, 0)));
  bc_emit_end(cx);
}

// SELECT $1::jsonb, twice: one parse, one handle.
void param_jsonb_twice(bc_ctx* cx) {
  bc_jsonb a = bc_param_jsonb(cx, 0);
  bc_jsonb b = bc_param_jsonb(cx, 0);
  if (a.i != b.i || a.g != b.g) {
    bc_raise(cx, BC_C_CHECK, 0);
    return;
  }
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, b);
  bc_emit_end(cx);
}

// UPDATE jt SET j = (j of row $2) WHERE k = $1: bc_row_set_jsonb of another row's node.
void copy_j(bc_ctx* cx) {
  bc_key ks = bc_key_begin(cx, g_jt);
  bc_key_i64(cx, ks, bc_param_i64(cx, 1));
  bool found = false;
  bc_view src = bc_pin(cx, bc_get(cx, bc_key_end(cx, ks), &found));
  bc_key kd = bc_key_begin(cx, g_jt);
  bc_key_i64(cx, kd, bc_param_i64(cx, 0));
  bc_view old = bc_get(cx, bc_key_end(cx, kd), &found);
  bc_row r = bc_row_from(cx, g_jt, old);
  bc_row_set_jsonb(cx, r, kJCol, bc_col_jsonb(cx, src, kJCol));
  bc_update(cx, g_jt, old, bc_row_end(cx, r));
}

// Forged handles: a node read as bytes, bytes read as a node.
void jsonb_as_bytes(bc_ctx* cx) {
  bc_jsonb j = bc_param_jsonb(cx, 0);
  bc_view v;
  v.i = j.i;
  v.g = j.g;
  bc_emit_begin(cx);
  bc_emit_bytes(cx, v);
  bc_emit_end(cx);
}
void bytes_as_jsonb(bc_ctx* cx) {
  bc_view v = bc_param_bytes(cx, 0);
  bc_jsonb j;
  j.i = v.i;
  j.g = v.g;
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, j);
  bc_emit_end(cx);
}
void empty_jsonb(bc_ctx* cx) {
  bc_jsonb j;
  j.i = 0;
  j.g = 0;
  bc_emit_begin(cx);
  bc_emit_jsonb(cx, j);
  bc_emit_end(cx);
}
void jsonb_col_as_bytes(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) bc_col_bytes(cx, bc_scan_row(cx, c), kJCol);
}
void bytes_into_jsonb_col(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, g_jt);
  bc_row_set_i64(cx, r, kKCol, 9);
  bc_row_set_bytes(cx, r, kJCol, bc_param_bytes(cx, 0));
  bc_insert(cx, g_jt, bc_row_end(cx, r));
}
// A node kept past its row's step: stale.
void node_past_its_step(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, g_jt, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  bc_jsonb kept;
  kept.i = 0;
  kept.g = 0;
  bool first = true;
  while (bc_scan_next(cx, c)) {
    if (!first) {
      bc_emit_begin(cx);
      bc_emit_jsonb(cx, kept);
      bc_emit_end(cx);
      return;
    }
    kept = bc_col_jsonb(cx, bc_scan_row(cx, c), kJCol);
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

auto bytes_of(std::initializer_list<int> b) -> Bytes {
  Bytes v;
  for (auto x : b) v.push_back(static_cast<std::byte>(x));
  return v;
}

}  // namespace

TEST_CASE("plan API jsonb: a parameter's text in, jsonb_out's text out, NULL as NULL", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  REQUIRE(run(f, insert_jt, {std::int64_t{1}, bytes("{\"b\": 1, \"a\": [1, 2.50, null], \"a\": \"x\"}"), bytes("s")}) ==
          BC_COMMITTED);
  REQUIRE(run(f, insert_jt, {std::int64_t{2}, kNull, kNull}) == BC_COMMITTED);
  REQUIRE(run(f, insert_jt, {std::int64_t{3}, bytes("  -0.0e-3 "), kNull}) == BC_COMMITTED);
  for (auto* plan : {select_j, select_j_col}) {
    REQUIRE(run(f, plan, {}, kJsonbRow) == BC_COMMITTED);
    auto rows = f.results(kJsonbRow);
    REQUIRE(rows.size() == 3);
    CHECK(text(rows[0][0]) == "{\"a\": \"x\", \"b\": 1}");
    CHECK(std::holds_alternative<std::monostate>(rows[1][0]));
    CHECK(text(rows[2][0]) == "0.0000");
  }
  REQUIRE(run(f, select_j_text, {}, kBytesRow) == BC_COMMITTED);
  auto rows = f.results(kBytesRow);
  REQUIRE(rows.size() == 3);
  CHECK(text(rows[0][0]) == "{\"a\": \"x\", \"b\": 1}");
  // Across a reopen, the same text.
  f.reopen();
  REQUIRE(run(f, select_j, {}, kJsonbRow) == BC_COMMITTED);
  CHECK(text(f.results(kJsonbRow)[0][0]) == "{\"a\": \"x\", \"b\": 1}");
  // bc_row_set_jsonb of another row's node.
  REQUIRE(run(f, copy_j, {std::int64_t{3}, std::int64_t{1}}) == BC_COMMITTED);
  REQUIRE(run(f, select_j, {}, kJsonbRow) == BC_COMMITTED);
  CHECK(text(f.results(kJsonbRow)[2][0]) == "{\"a\": \"x\", \"b\": 1}");
}

TEST_CASE("plan API jsonb: bc_jsonb_in and bc_param_jsonb parse; text that is no JSON is BC_E_DATA", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  REQUIRE(run(f, param_in, {bytes("[1 , {\"z\":true,\"a\":null}]")}, kJsonbRow) == BC_COMMITTED);
  CHECK(text(f.results(kJsonbRow)[0][0]) == "[1, {\"a\": null, \"z\": true}]");
  REQUIRE(run(f, param_jsonb_twice, {bytes("\"\\u00e9\"")}, kJsonbRow) == BC_COMMITTED);
  CHECK(text(f.results(kJsonbRow)[0][0]) == "\"\xc3\xa9\"");
  struct Bad {
    std::string_view input;
    std::uint32_t state;
    std::string_view message, detail, context;
  };
  for (const auto& b : {Bad{"[1,]", BC_SQLSTATE('2', '2', 'P', '0', '2'), "invalid input syntax for type json",
                            "Expected JSON value, but found \"]\".", "JSON data, line 1: [1,]"},
                        Bad{"\"\\u0000\"", BC_SQLSTATE('2', '2', 'P', '0', '5'), "unsupported Unicode escape sequence",
                            "\\u0000 cannot be converted to text.", "JSON data, line 1: \"\\u0000..."},
                        Bad{"1e131072", BC_SQLSTATE('2', '2', '0', '0', '3'), "value overflows numeric format", "", ""},
                        Bad{"\xff", BC_SQLSTATE('2', '2', '0', '2', '1'), "invalid byte sequence for encoding \"UTF8\"", "", ""}}) {
    INFO(b.input);
    for (auto* plan : {param_in, param_jsonb_twice}) {
      CHECK(run(f, plan, {bytes(std::string{b.input})}, kJsonbRow) == BC_FAILED);
      CHECK(bc_error(f.cx) == BC_E_DATA);
      CHECK(bc_error_detail(f.cx) == b.state);
      CHECK(std::string_view{bc_error_message(f.cx)} == b.message);
      CHECK(std::string_view{bc_error_note(f.cx)} == b.detail);
      CHECK(std::string_view{bc_error_context(f.cx)} == b.context);
    }
    // Into a row: the same error, nothing written.
    CHECK(run(f, insert_jt, {std::int64_t{1}, bytes(std::string{b.input}), kNull}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_DATA);
    CHECK(bc_error_detail(f.cx) == b.state);
  }
  std::string deep(10001, '[');
  deep += std::string(10001, ']');
  CHECK(run(f, param_in, {bytes(deep)}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_DATA);
  CHECK(bc_error_detail(f.cx) == BC_SQLSTATE('5', '4', '0', '0', '1'));
  REQUIRE(run(f, select_j, {}, kJsonbRow) == BC_COMMITTED);
  CHECK(f.results(kJsonbRow).empty());
}

TEST_CASE("plan API jsonb: a handle of the other form, or a bytes call on a jsonb column, is BC_E_HANDLE",
          "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  REQUIRE(run(f, insert_jt, {std::int64_t{1}, bytes("[1]"), kNull}) == BC_COMMITTED);
  REQUIRE(run(f, insert_jt, {std::int64_t{2}, bytes("[2]"), kNull}) == BC_COMMITTED);
  CHECK(run(f, jsonb_as_bytes, {bytes("[1]")}, kBytesRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, bytes_as_jsonb, {bytes("[1]")}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, empty_jsonb, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, jsonb_col_as_bytes, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, bytes_into_jsonb_col, {bytes("[1]")}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A node read from a row dies when the row's loop steps.
  CHECK(run(f, node_past_its_step, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A jsonb result column takes only a jsonb.
  CHECK(run(f, select_j_text, {}, kJsonbRow) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
}

TEST_CASE("plan API jsonb: a damaged stored value is BC_E_CORRUPT on every read, never a value", "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  REQUIRE(run(f, insert_jt, {std::int64_t{1}, bytes("[1]"), kNull}) == BC_COMMITTED);
  // The wire's form (version 1, then the text), canonical text, a format byte
  // alone, and a node whose count runs past its extent.
  for (const auto& damaged : {bytes_of({0x01, 0x01}), bytes_of({'[', ']'}), bytes_of({0xF8}),
                              bytes_of({0xF8, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}), bytes_of({0xF8, 0x05, 'a', 0x00})}) {
    put_raw(f, 2, damaged);
    for (auto* plan : {select_j, select_j_col, select_j_text}) {
      CHECK(run(f, plan, {}, plan == select_j_text ? std::span<const PhysType>{kBytesRow} : std::span<const PhysType>{kJsonbRow}) ==
            BC_FAILED);
      CHECK(bc_error(f.cx) == BC_E_CORRUPT);
    }
  }
  // bc_row_set_jsonb runs the full check: a damaged node is never written.
  put_raw(f, 2, bytes_of({0xF8, 0x07, 0x02, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 'b', 'a', 0x01, 0x01}));
  CHECK(run(f, copy_j, {std::int64_t{1}, std::int64_t{2}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CORRUPT);
}

TEST_CASE("plan API jsonb: a budget sweep gives 54000 or the whole result, the same bytes on every run",
          "[sql][algebra][jsonb]") {
  Fixture f;
  make_jt(f);
  std::string doc = "{\"k\": [";
  for (int i = 0; i < 50; ++i) doc += (i == 0 ? "" : ", ") + std::string{"{\"v\": 1e30, \"s\": \"\\n\\u0001x\"}"};
  doc += "]}";
  std::string want;
  auto limits = 0;
  for (std::uint64_t max = 1;; max += 61) {  // 0 is no budget
    bc_budget budget{};
    budget.max_bytes = max;
    auto r = run(f, insert_jt, {std::int64_t{1}, bytes(doc), kNull}, {}, budget);
    if (r == BC_COMMITTED) break;
    REQUIRE(r == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_LIMIT);
    ++limits;
    // Nothing written.
    REQUIRE(run(f, select_j, {}, kJsonbRow) == BC_COMMITTED);
    REQUIRE(f.results(kJsonbRow).empty());
  }
  CHECK(limits > 0);
  limits = 0;
  for (std::uint64_t max = 1;; max += 61) {  // 0 is no budget
    bc_budget budget{};
    budget.max_bytes = max;
    auto both = true;
    for (auto* plan : {select_j, select_j_text}) {
      auto r = run(f, plan, {}, plan == select_j ? std::span<const PhysType>{kJsonbRow} : std::span<const PhysType>{kBytesRow}, budget);
      if (r != BC_COMMITTED) {
        CHECK(bc_error(f.cx) == BC_E_LIMIT);
        ++limits;
        both = false;
        continue;
      }
      auto rows = f.results(plan == select_j ? std::span<const PhysType>{kJsonbRow} : std::span<const PhysType>{kBytesRow});
      REQUIRE(rows.size() == 1);
      if (want.empty()) want = text(rows[0][0]);
      CHECK(text(rows[0][0]) == want);
    }
    if (both) break;
  }
  CHECK(limits > 0);
  CHECK(want.starts_with("{\"k\": [{\"s\": \"\\n\\u0001x\", \"v\": 1000000000000000000000000000000}"));
}

TEST_CASE("plan API jsonb: every allocation of the parser, the encoder and the output writer fails in turn as BC_E_MEMORY",
          "[sql][algebra][jsonb][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Fixture f;
  make_jt(f);
  const std::string doc = "{\"b\": [1, 2.50, {\"z\": null, \"a\": \"\\u00e9\\n\"}], \"a\": -0.001}";
  REQUIRE(run(f, insert_jt, {std::int64_t{1}, bytes(doc), kNull}) == BC_COMMITTED);
  REQUIRE(run(f, insert_jt, {std::int64_t{2}, bytes("[true]"), kNull}) == BC_COMMITTED);
  struct Case {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
    int clean;
  };
  const std::vector<Case> cases{
      {"bc_row_set_param of a jsonb column", insert_jt, {std::int64_t{3}, bytes(doc), kNull}, {}, BC_OK},
      {"bc_row_set_param of text that is no JSON", insert_jt, {std::int64_t{3}, bytes("[1, 2,"), kNull}, {}, BC_E_DATA},
      {"bc_emit_jsonb", select_j, {}, {PhysType::Jsonb}, BC_OK},
      {"bc_emit_col of a jsonb column", select_j_col, {}, {PhysType::Jsonb}, BC_OK},
      {"bc_jsonb_text", select_j_text, {}, {PhysType::Bytes}, BC_OK},
      {"bc_jsonb_in", param_in, {bytes(doc)}, {PhysType::Jsonb}, BC_OK},
      {"bc_param_jsonb", param_jsonb_twice, {bytes(doc)}, {PhysType::Jsonb}, BC_OK},
      {"bc_row_set_jsonb", copy_j, {std::int64_t{2}, std::int64_t{1}}, {}, BC_OK},
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
        CHECK(error == c.clean);
        bc_ctx_reset(f.cx);
        break;
      }
      bc_ctx_reset(f.cx);  // nothing committed: the run is never committed
      CHECK((error == BC_E_MEMORY || error == c.clean));
      CHECK(bc_ctx_held_for_testing(f.cx) == 0);
      ++failed;
      if (error == BC_E_MEMORY) ++memory_errors;
    }
    // The plan allocates, each allocation was failed once, and some failure
    // reached the call as BC_E_MEMORY. Which of the parser's, the encoder's
    // and the output writer's allocations a sweep reaches is asserted by
    // "jsonb parse and output: every allocation fails in turn as bad_alloc,
    // leaving nothing", over the same functions.
    CHECK(failed > 0);
    CHECK(memory_errors > 0);
  }
#endif
}
