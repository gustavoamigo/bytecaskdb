// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The hand-written plans in plans/, compiled by clang into this binary, run
// through bc_execute against the prototype's catalog. Expected rows are
// computed by hand from the fixture below.

#include <catch2/catch_test_macros.hpp>

#include "fixture.hpp"

extern "C" {
void plan_insert(bc_ctx*);
void plan_select_by_id(bc_ctx*);
void plan_select_name_range(bc_ctx*);
void plan_update_qty(bc_ctx*);
void plan_delete_by_id(bc_ctx*);
}

using namespace sql_test;
using bytecask::sql::PhysType;

namespace {

using Row = std::vector<bc_value>;

constexpr std::uint32_t kTbl = 1;

struct PlanRunner {
  Fixture& f;
  std::uint32_t lay_pk_result;
  std::uint32_t lay_range_result;

  explicit PlanRunner(Fixture& fx)
      : f{fx},
        // Interned in the order the code generator meets them.
        lay_pk_result{fx.cat.result_layout({PhysType::I64, PhysType::I64})},
        lay_range_result{fx.cat.result_layout({PhysType::I64})} {}

  auto exec(bc_plan_fn plan, std::vector<bc_value> p) -> int {
    auto ps = params(std::move(p));
    std::uint32_t tables[] = {kTbl};
    bc_schema_ref ref{f.cat.version(), tables, tables, 1, 1};
    return bc_execute(f.cx, plan, &ref, &ps, {}, false, 3);
  }

  auto insert(std::int64_t id, bc_value name, bc_value qty) -> int {
    return exec(plan_insert, {id, std::move(name), std::move(qty)});
  }
  auto by_id(bc_value id) -> std::vector<Row> {
    REQUIRE(exec(plan_select_by_id, {std::move(id)}) == BC_COMMITTED);
    return f.results(lay_pk_result);
  }
  auto ids_in(std::string_view lo, std::string_view hi) -> std::vector<std::int64_t> {
    REQUIRE(exec(plan_select_name_range, {bytes(lo), bytes(hi)}) == BC_COMMITTED);
    std::vector<std::int64_t> ids;
    for (const auto& r : f.results(lay_range_result)) ids.push_back(std::get<std::int64_t>(r[0]));
    return ids;
  }
  auto affected(bc_plan_fn plan, std::vector<bc_value> p) -> std::int64_t {
    REQUIRE(exec(plan, std::move(p)) == BC_COMMITTED);
    return bc_ctx_affected(f.cx);
  }
};

const bc_value kNull = std::monostate{};

// Counts the live entries of a table or an index, straight from the engine.
auto count_entries(bytecask::DB& db, std::uint8_t ns, std::uint32_t id) -> int {
  bytecask::sql::Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, ns, id);
  auto n = 0;
  for (const auto& k : db.keys_from({}, prefix)) {
    if (k.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), k.begin())) break;
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("hand-written plans: the five DML statements move rows", "[sql][plans]") {
  Fixture f;
  PlanRunner p{f};

  CHECK(p.insert(1, bytes("apple"), std::int64_t{10}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);
  CHECK(p.insert(2, bytes("banana"), std::int64_t{20}) == BC_COMMITTED);
  CHECK(p.insert(3, kNull, std::int64_t{30}) == BC_COMMITTED);
  CHECK(p.insert(4, bytes("cherry"), kNull) == BC_COMMITTED);
  CHECK(p.insert(5, bytes("banana"), std::int64_t{50}) == BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, 1) == 5);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, 3) == 5);

  // A duplicate primary key fails its guard on every attempt.
  CHECK(p.insert(1, bytes("again"), std::int64_t{0}) == BC_CONFLICT);

  CHECK(p.by_id(std::int64_t{1}) == std::vector<Row>{{std::int64_t{1}, std::int64_t{10}}});
  CHECK(p.by_id(std::int64_t{4}) == std::vector<Row>{{std::int64_t{4}, kNull}});
  CHECK(p.by_id(std::int64_t{9}).empty());
  CHECK(p.by_id(kNull).empty());

  // Index order is name, then primary key; a null name is never in a range.
  CHECK(p.ids_in("b", "c") == std::vector<std::int64_t>{2, 5});
  CHECK(p.ids_in("a", "z") == std::vector<std::int64_t>{1, 2, 5, 4});
  CHECK(p.ids_in("banana", "banana").empty());
  CHECK(p.ids_in("banana", "banana\x01") == std::vector<std::int64_t>{2, 5});
  CHECK(p.ids_in("c", "b").empty());

  CHECK(p.affected(plan_update_qty, {std::int64_t{1}, std::int64_t{3}}) == 1);
  CHECK(p.by_id(std::int64_t{1}) == std::vector<Row>{{std::int64_t{1}, std::int64_t{7}}});
  CHECK(p.affected(plan_update_qty, {std::int64_t{4}, std::int64_t{3}}) == 1);  // NULL - 3 is NULL
  CHECK(p.by_id(std::int64_t{4}) == std::vector<Row>{{std::int64_t{4}, kNull}});
  CHECK(p.affected(plan_update_qty, {std::int64_t{9}, std::int64_t{3}}) == 0);
  CHECK(p.exec(plan_update_qty, {std::int64_t{1}, std::numeric_limits<std::int64_t>::min()}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
  CHECK(p.by_id(std::int64_t{1}) == std::vector<Row>{{std::int64_t{1}, std::int64_t{7}}});

  CHECK(p.affected(plan_delete_by_id, {std::int64_t{2}}) == 1);
  CHECK(p.ids_in("b", "c") == std::vector<std::int64_t>{5});
  CHECK(p.by_id(std::int64_t{2}).empty());
  CHECK(p.affected(plan_delete_by_id, {std::int64_t{2}}) == 0);
  CHECK(p.affected(plan_delete_by_id, {std::int64_t{3}}) == 1);  // a null name's index entry goes too
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, 1) == 3);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, 3) == 3);
}
