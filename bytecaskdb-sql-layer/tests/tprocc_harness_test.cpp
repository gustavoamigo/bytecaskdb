// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The TPROC-C harness's smoke test (docs/testing_design.md, *The TPROC-C
// harness*): a short mixed run at a tiny scale keeps the specification's
// consistency conditions. The full runs are manual.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tprocc;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

TEST_CASE("tprocc: a short mixed run keeps the specification's consistency conditions", "[sql][tprocc][harness]") {
  sql_test::TempDir dir{"tprocc_smoke"};
  auto db = Database::open(dir.path());
  tprocc::load(*db, {.warehouses = 2,
                   .scale = {.items = 100, .customers = 30, .orders = 30, .new_orders = 9},
                   .seed = 7,
                   .rows_per_txn = 500});
  REQUIRE(tprocc::consistency(*db).empty());
  // HammerDB's NOPM is the change in d_next_o_id: every committed New-Order's.
  auto next_order_ids = [&] {
    std::int64_t sum = 0;
    for (const auto& row : db->execute("SELECT d_next_o_id FROM district WHERE d_w_id >= $1", {std::int64_t{1}}).rows) {
      sum += std::get<std::int64_t>(row.at(0));
    }
    return sum;
  };
  const auto ids_before = next_order_ids();
  tprocc::RunOptions opts;
  opts.threads = {4};
  opts.seed = 11;
  opts.txns_per_thread = 100;
  std::vector<tprocc::PhaseReport> reports;
  tprocc::run(*db, opts, [&](const tprocc::PhaseReport& r) { reports.push_back(r); });
  REQUIRE(reports.size() == 1);
  const auto& r = reports[0];
  INFO(tprocc::format_report(r));
  CHECK(r.run == 1);  // the load's is run 0
  std::uint64_t done = 0;
  for (auto k : tprocc::mix()) {
    const auto& s = r.kinds[static_cast<std::size_t>(k)];
    CHECK(s.completed > 0);
    done += s.completed + s.abandoned;
  }
  CHECK(done == 4 * 100);
  REQUIRE(r.nopm());
  CHECK(*r.nopm() > 0);
  // Counting a number of transactions measures them all: the committed
  // ones are the ids New-Order took, its rollbacks none.
  const auto& n = r.kinds[static_cast<std::size_t>(tprocc::Kind::NewOrder)];
  CHECK(n.rolled_back <= n.completed);
  CHECK(static_cast<std::int64_t>(n.completed - n.rolled_back) == next_order_ids() - ids_before);
  for (const auto& bad : tprocc::consistency(*db)) FAIL_CHECK(bad);
  db->close();
}
