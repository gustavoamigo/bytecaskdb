// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The TPROC-C schema through SQL DDL: created, persisted, reopened and
// checked table by table, then written and read through generated plans.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.api;
import bytecask.sql.tests.tprocc;
import bytecask.sql.tests.util;

using namespace bytecask::sql;
using sql_test::bytes;

namespace {

struct Expect {
  std::size_t columns;
  std::vector<std::string> pk;
  std::vector<std::string> unique_index;  // empty: none
};

// What each table must hold: HammerDB's keys and indexes.
auto expected() -> std::map<std::string, Expect> {
  return {
      {"warehouse", {9, {"w_id"}, {}}},
      {"district", {11, {"d_w_id", "d_id"}, {}}},
      {"customer", {21, {"c_w_id", "c_d_id", "c_id"}, {"c_w_id", "c_d_id", "c_last", "c_first", "c_id"}}},
      {"history", {9, {"h_id"}, {}}},
      {"new_order", {3, {"no_w_id", "no_d_id", "no_o_id"}, {}}},
      {"orders", {8, {"o_w_id", "o_d_id", "o_id"}, {"o_w_id", "o_d_id", "o_c_id", "o_id"}}},
      {"order_line", {10, {"ol_w_id", "ol_d_id", "ol_o_id", "ol_number"}, {}}},
      {"item", {5, {"i_id"}, {}}},
      {"stock", {17, {"s_i_id", "s_w_id"}, {}}},
  };
}

auto names(const Table& t, const std::vector<std::uint32_t>& ids) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto id : ids) out.push_back(t.column(id)->name);
  return out;
}

void check_schema(const Catalog& cat) {
  CHECK(cat.tables().size() == 9);
  for (const auto& [name, e] : expected()) {
    INFO(name);
    const auto* t = cat.table(name);
    REQUIRE(t != nullptr);
    CHECK(t->columns.size() == e.columns);
    CHECK(names(*t, t->pk_col_ids) == e.pk);
    if (e.unique_index.empty()) {
      CHECK(t->indexes.empty());
      continue;
    }
    REQUIRE(t->indexes.size() == 1);
    CHECK(t->indexes[0].unique);
    std::vector<std::uint32_t> parts;
    for (const auto& p : t->indexes[0].parts) parts.push_back(p.col_id);
    CHECK(names(*t, parts) == e.unique_index);
  }
  // A sample of the types, as declared.
  const auto& c = *cat.table("customer");
  CHECK(c.column("c_balance")->sql == SqlType{SqlKind::Decimal, 0, 12, 2});
  CHECK(c.column("c_discount")->sql == SqlType{SqlKind::Decimal, 0, 4, 4});
  CHECK(c.column("c_data")->sql == SqlType{SqlKind::Varchar, 500});
  CHECK(c.column("c_since")->sql == SqlType{SqlKind::Timestamp});
  CHECK(c.column("c_d_id")->sql == SqlType{SqlKind::SmallInt});
  CHECK(c.column("c_id")->type == PhysType::I64);
  CHECK(c.column("c_last")->type == PhysType::Bytes);
  CHECK(c.column("c_last")->nullable);
  CHECK_FALSE(c.column("c_w_id")->nullable);
  // Every table fits its layout with room to add columns.
  for (const auto& [id, t] : cat.tables()) CHECK(t.layout.id_ceiling() >= t.next_col_id + 16);
  // The constants a hand-written plan on customer names.
  auto cs = Catalog::constants(c);
  auto has = [&](std::string_view object) {
    return std::ranges::any_of(cs, [&](const auto& k) { return k.object == object; });
  };
  CHECK(has("customer"));
  CHECK(has("customer_i2"));
  CHECK(has("customer.c_balance"));
  CHECK(has("customer.c_last"));
}

auto i(std::int64_t v) -> Value { return v; }

}  // namespace

TEST_CASE("TPROC-C: the schema is created by SQL DDL, persists and reopens", "[sql][catalog][tprocc]") {
  sql_test::TempDir dir{"tprocc"};
  {
    auto db = Database::open(dir.path());
    for (auto ddl : sql_test::kTproccSchema) {
      INFO(ddl);
      CHECK(db->execute(ddl).affected == 0);
    }
    check_schema(*db->catalog());
    CHECK(db->catalog()->version() == sql_test::kTproccSchema.size());
    db->close();
  }
  auto db = Database::open(dir.path());
  auto cat = db->catalog();
  check_schema(*cat);
  CHECK(cat->version() == sql_test::kTproccSchema.size());

  // Writes and reads through generated plans, on the reopened catalog.
  constexpr auto kCustIns = "INSERT INTO customer (c_w_id, c_d_id, c_id, c_first, c_last, c_balance) "
                            "VALUES ($1, $2, $3, $4, $5, $6)";
  CHECK(db->execute(kCustIns, {i(1), i(1), i(1), bytes("ann"), bytes("BARBAR"), i(-1000)}).affected == 1);
  CHECK(db->execute(kCustIns, {i(1), i(1), i(2), bytes("bob"), bytes("BARBAR"), i(0)}).affected == 1);
  // customer_i2 is unique over (w, d, last, first, id): the same name with
  // another id is not a duplicate, and its rows are found by name.
  CHECK(db->execute("SELECT c_id FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3", {i(1), i(1), i(2)})
            .rows.size() == 1);
  auto by_w = db->execute("SELECT c_id FROM customer WHERE c_w_id >= $1 AND c_w_id < $2", {i(1), i(2)});
  CHECK(by_w.rows.size() == 2);
  CHECK(db->execute("UPDATE customer SET c_balance = c_balance - $4 WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                    {i(1), i(1), i(1), i(250)})
            .affected == 1);
  CHECK(db->execute("SELECT c_balance FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3", {i(1), i(1), i(1)})
            .rows == std::vector<std::vector<Value>>{{i(-1250)}});
  CHECK(db->execute("INSERT INTO new_order VALUES ($1, $2, $3)", {i(1), i(1), i(3001)}).affected == 1);
  CHECK(db->execute("DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3", {i(1), i(1), i(3001)})
            .affected == 1);
}
