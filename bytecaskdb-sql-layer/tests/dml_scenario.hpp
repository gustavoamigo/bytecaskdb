// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The five DML plans run in order against the fixture, with expected rows
// computed by hand. Shared by every tier that runs the same plan files:
// clang-linked (plans_test.cpp) and TinyCC (tcc_test.cpp).

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "fixture.hpp"

namespace sql_test {

struct PlanSet {
  bc_plan_fn insert, select_by_id, select_name_range, update_qty, delete_by_id;
};

using bytecask::sql::PhysType;

using Row = std::vector<bc_value>;

constexpr std::uint32_t kTbl = 1;

struct PlanRunner {
  Fixture& f;
  PlanSet plans;
  std::uint32_t lay_pk_result;
  std::uint32_t lay_range_result;

  PlanRunner(Fixture& fx, PlanSet ps)
      : f{fx},
        plans{ps},
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
    return exec(plans.insert, {id, std::move(name), std::move(qty)});
  }
  auto by_id(bc_value id) -> std::vector<Row> {
    REQUIRE(exec(plans.select_by_id, {std::move(id)}) == BC_COMMITTED);
    return f.results(lay_pk_result);
  }
  auto ids_in(std::string_view lo, std::string_view hi) -> std::vector<std::int64_t> {
    REQUIRE(exec(plans.select_name_range, {bytes(lo), bytes(hi)}) == BC_COMMITTED);
    std::vector<std::int64_t> ids;
    for (const auto& r : f.results(lay_range_result)) ids.push_back(std::get<std::int64_t>(r[0]));
    return ids;
  }
  auto affected(bc_plan_fn plan, std::vector<bc_value> p) -> std::int64_t {
    REQUIRE(exec(plan, std::move(p)) == BC_COMMITTED);
    return bc_ctx_affected(f.cx);
  }
};

inline const bc_value kNull = std::monostate{};

// Counts the live entries of a table or an index, straight from the engine.
inline auto count_entries(bytecask::DB& db, std::uint8_t ns, std::uint32_t id) -> int {
  bytecask::sql::Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, ns, id);
  auto n = 0;
  for (const auto& k : db.keys_from({}, prefix)) {
    if (k.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), k.begin())) break;
    ++n;
  }
  return n;
}


// Every row and count the plans produce, from an empty table t.
inline void run_dml_scenario(Fixture& f, PlanSet plans) {
  PlanRunner p{f, plans};

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

  CHECK(p.affected(p.plans.update_qty, {std::int64_t{1}, std::int64_t{3}}) == 1);
  CHECK(p.by_id(std::int64_t{1}) == std::vector<Row>{{std::int64_t{1}, std::int64_t{7}}});
  CHECK(p.affected(p.plans.update_qty, {std::int64_t{4}, std::int64_t{3}}) == 1);  // NULL - 3 is NULL
  CHECK(p.by_id(std::int64_t{4}) == std::vector<Row>{{std::int64_t{4}, kNull}});
  CHECK(p.affected(p.plans.update_qty, {std::int64_t{9}, std::int64_t{3}}) == 0);
  CHECK(p.exec(p.plans.update_qty, {std::int64_t{1}, std::numeric_limits<std::int64_t>::min()}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
  CHECK(p.by_id(std::int64_t{1}) == std::vector<Row>{{std::int64_t{1}, std::int64_t{7}}});

  CHECK(p.affected(p.plans.delete_by_id, {std::int64_t{2}}) == 1);
  CHECK(p.ids_in("b", "c") == std::vector<std::int64_t>{5});
  CHECK(p.by_id(std::int64_t{2}).empty());
  CHECK(p.affected(p.plans.delete_by_id, {std::int64_t{2}}) == 0);
  CHECK(p.affected(p.plans.delete_by_id, {std::int64_t{3}}) == 1);  // a null name's index entry goes too
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, 1) == 3);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, 3) == 3);
}

}  // namespace sql_test
