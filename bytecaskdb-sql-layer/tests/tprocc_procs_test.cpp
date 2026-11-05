// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The TPROC-C procedures of hammerdb/tprocc_procs.sql (docs/procedures_design.md,
// *Phases*, 4c), run through the embedded API at a tiny scale, as
// hammerdb/bcdboltp.tcl calls them: the specification's consistency
// conditions hold after them.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tprocc;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

auto procedures() -> std::vector<std::string> {
  std::ifstream in{std::string{SQL_LAYER_HAMMERDB_DIR} + "/tprocc_procs.sql"};
  REQUIRE(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return tprocc::procedure_statements(ss.str());
}

auto i(std::int64_t v) -> Value { return Value{v}; }

}  // namespace

TEST_CASE("tprocc procedures: the five transactions as procedures keep the consistency conditions",
          "[sql][tprocc][procedure]") {
  sql_test::TempDir dir{"tprocc_procs"};
  auto db = Database::open(dir.path());
  constexpr std::int64_t kWarehouses = 2, kItems = 100, kCustomers = 30;
  tprocc::load(*db, {.warehouses = kWarehouses,
                     .scale = {.items = kItems, .customers = kCustomers, .orders = 30, .new_orders = 9},
                     .seed = 7,
                     .rows_per_txn = 500});
  REQUIRE(tprocc::consistency(*db).empty());
  auto procs = procedures();
  REQUIRE(procs.size() == 5);
  for (const auto& p : procs) {
    INFO(p.substr(0, 40));
    (void)db->execute(p);
  }
  std::mt19937_64 rng{42};
  auto draw = [&](std::int64_t lo, std::int64_t hi) { return std::uniform_int_distribution<std::int64_t>{lo, hi}(rng); };
  auto last_name = [&](std::int64_t w, std::int64_t d) {
    return db->execute("SELECT c_last FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                       {i(w), i(d), i(draw(1, kCustomers))})
        .rows.at(0)
        .at(0);
  };
  const Value now{std::int64_t{1'793'707'530} * 1'000'000};
  std::int64_t h_id = 1'000'000;
  int new_orders = 0, rolled_back = 0, payments_by_name = 0;
  for (int n = 0; n < 400; ++n) {
    auto w = draw(1, kWarehouses);
    auto d = draw(1, 10);
    auto kind = draw(1, 23);  // HammerDB's deck: 10 New-Order, 10 Payment, 1 each of the rest
    INFO("call " << n << " kind " << kind);
    if (kind <= 10) {
      try {
        auto r = db->call("neword", {i(w), i(kWarehouses), i(d), i(draw(1, kCustomers)), i(draw(5, 15)), i(kItems),
                                     Value{}, Value{}, Value{}, Value{}, Value{}, Value{}, now});
        CHECK(r.rows.size() == 1);
        ++new_orders;
      } catch (const std::exception& e) {
        // The 1% unused item: its SELECT INTO STRICT fails the call, which rolls it back.
        INFO(e.what());
        CHECK(sqlstate(e) == "P0002");
        ++rolled_back;
      }
    } else if (kind <= 20) {
      auto byname = draw(1, 100) <= 60;
      payments_by_name += byname ? 1 : 0;
      auto cw = draw(1, 100) <= 85 ? w : draw(1, kWarehouses);
      auto cd = draw(1, 10);
      auto r = db->call("payment", {i(w), i(d), i(cw), i(cd), i(byname ? 1 : 0), Value{draw(100, 500000)},
                                    byname ? Value{} : i(draw(1, kCustomers)), byname ? last_name(cw, cd) : Value{},
                                    Value{}, Value{}, i(h_id++), now});
      REQUIRE(r.rows.size() == 1);
      CHECK(std::holds_alternative<std::int64_t>(r.rows[0].at(0)));  // the customer it picked
    } else if (kind == 21) {
      auto byname = draw(1, 100) <= 60;
      auto r = db->call("ostat", {i(w), i(d), byname ? Value{} : i(draw(1, kCustomers)), i(byname ? 1 : 0),
                                  byname ? last_name(w, d) : Value{}, Value{}, Value{}, Value{}, Value{}, Value{}});
      REQUIRE(r.rows.size() == 1);
    } else if (kind == 22) {
      (void)db->call("delivery", {i(w), i(draw(1, 10)), now});
    } else {
      auto r = db->call("slev", {i(w), i(d), i(draw(10, 20)), Value{}});
      REQUIRE(r.rows.size() == 1);
    }
  }
  CHECK(new_orders > 0);
  CHECK(payments_by_name > 0);
  CHECK(rolled_back < new_orders);
  auto broken = tprocc::consistency(*db);
  INFO((broken.empty() ? std::string{} : broken.front()));
  CHECK(broken.empty());
}
