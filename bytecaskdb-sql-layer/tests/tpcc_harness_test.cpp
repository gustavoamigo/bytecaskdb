// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The MVP gate harness's smoke test (docs/testing_design.md, *The MVP gate
// harness*): a short mixed run at a tiny scale keeps TPC-C's consistency
// conditions. The full runs are manual.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tpcc;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

TEST_CASE("tpcc: a short mixed run keeps TPC-C's consistency conditions", "[sql][tpcc][harness]") {
  sql_test::TempDir dir{"tpcc_smoke"};
  auto db = Database::open(dir.path());
  tpcc::load(*db, {.warehouses = 2,
                   .scale = {.items = 100, .customers = 30, .orders = 30, .new_orders = 9},
                   .seed = 7,
                   .rows_per_txn = 500});
  REQUIRE(tpcc::consistency(*db).empty());
  tpcc::RunOptions opts;
  opts.threads = {4};
  opts.seed = 11;
  opts.txns_per_thread = 100;
  std::vector<tpcc::PhaseReport> reports;
  tpcc::run(*db, opts, [&](const tpcc::PhaseReport& r) { reports.push_back(r); });
  REQUIRE(reports.size() == 1);
  const auto& r = reports[0];
  INFO(tpcc::format_report(r));
  CHECK(r.run == 1);  // the load's is run 0
  std::uint64_t done = 0;
  for (auto k : tpcc::mix()) {
    const auto& s = r.kinds[static_cast<std::size_t>(k)];
    CHECK(s.completed > 0);
    done += s.completed + s.abandoned;
  }
  CHECK(done == 4 * 100);
  CHECK_FALSE(r.nopm());  // New-Order is not in the mix yet
  for (const auto& bad : tpcc::consistency(*db)) FAIL_CHECK(bad);
  db->close();
}
