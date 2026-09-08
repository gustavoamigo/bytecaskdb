// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_bulk_index_build.cpp — invariants for the batched ALTER-copy path
// (MariaDBTxn bulk-copy mode). The copy loop targets a hidden #sql-xxx table
// whose keyspace is invisible until the final catalog rename, so write_row
// flushes its buffer to disk in fixed-size batches instead of accumulating a
// single giant transaction.
//
// The DB-reopen / crash-recovery of already-flushed batches is covered by the
// engine's model-based recovery tests; here we prove the plugin-side logic:
// batches actually land mid-copy, a flush failure aborts the ALTER, and an
// aborted ALTER's flushed rows are reclaimable by a range delete (what
// delete_table / ddl_log do).

#include "harness.h"
#include "key_encoding.h"
#include "bytecask_view.h"
#include "fault_injector.h"

#include <catch2/catch_test_macros.hpp>

using namespace bytecaskdb;
using namespace bytecaskdb::testing;
using namespace bytecask::testing;

namespace {

TableSpec index_build_spec() {
  TableSpec spec;
  spec.num_int_columns = 3;
  spec.has_pk = true;
  // Non-unique secondary index on column 1 — the index being "built".
  spec.secondary_indexes.push_back(IndexSpec{
      /*index_id=*/1, /*key_parts=*/1, /*is_unique=*/false,
      /*column_indexes=*/{1}});
  return spec;
}

size_t count_table_keys(bytecask::DB &db, uint32_t tid) {
  auto lo = table_id_prefix(tid);
  auto hi = table_id_upper_bound(tid);
  size_t n = 0;
  for (auto &k : db.keys_from({}, as_view(lo))) {
    if (k.size() < hi.size() ||
        std::memcmp(u8_data(k), hi.data(), hi.size()) >= 0) {
      break;
    }
    ++n;
  }
  return n;
}

size_t count_index_keys(bytecask::DB &db, uint32_t tid, uint16_t index_id) {
  auto lo = index_id_prefix(tid, index_id);
  auto hi = index_id_upper_bound(tid, index_id);
  size_t n = 0;
  for (auto &k : db.keys_from({}, as_view(lo))) {
    if (k.size() < hi.size() ||
        std::memcmp(u8_data(k), hi.data(), hi.size()) >= 0) {
      break;
    }
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("P-BULK-1: batches land mid-copy; commit persists every row",
          "[proof][bulk][inv1]") {
  PluginTestHarness h(index_build_spec());
  const uint32_t tid = h.table_id();

  // Tiny threshold so a handful of rows forces several flushes.
  h.txn().begin_bulk_copy(200);

  bool saw_early_flush = false;
  const int kRows = 40;
  for (int i = 0; i < kRows; ++i) {
    REQUIRE(h.insert_row({i, i * 10, i * 100}) == 0);
    if (i == 15 && count_table_keys(h.db(), tid) > 0) {
      saw_early_flush = true;  // rows durable before any commit
    }
  }
  REQUIRE(saw_early_flush);

  // Final barrier flush, then commit.
  REQUIRE(h.handler().end_bulk_insert() == 0);
  REQUIRE_FALSE(h.txn().in_bulk_copy());
  REQUIRE(h.commit() == 0);

  CHECK(count_table_keys(h.db(), tid) == static_cast<size_t>(kRows));
  CHECK(count_index_keys(h.db(), tid, 1) == static_cast<size_t>(kRows));
  CHECK(h.row_counter() == kRows);
}

TEST_CASE("P-BULK-2: a flush I/O failure aborts the copy",
          "[proof][bulk][inv2]") {
  PluginTestHarness h(index_build_spec());

  h.txn().begin_bulk_copy(200);

  ScopedFaultInjector guard("io_data_file_append");

  int rc = 0;
  for (int i = 0; i < 100 && rc == 0; ++i) {
    rc = h.insert_row({i, i * 10, i * 100});
  }
  REQUIRE(rc != 0);  // a batch flush failed and the failure propagated

  h.rollback();
  REQUIRE_FALSE(h.txn().in_bulk_copy());
}

TEST_CASE("P-BULK-3: an aborted copy's flushed rows are reclaimable",
          "[proof][bulk][inv3]") {
  PluginTestHarness h(index_build_spec());
  const uint32_t tid = h.table_id();

  h.txn().begin_bulk_copy(200);
  for (int i = 0; i < 30; ++i) {
    REQUIRE(h.insert_row({i, i * 10, i * 100}) == 0);
  }
  // Simulate ALTER failure before the rename.
  h.rollback();
  REQUIRE_FALSE(h.txn().in_bulk_copy());

  // Flushed rows are still on disk in the (invisible) #sql-xxx keyspace.
  REQUIRE(count_table_keys(h.db(), tid) > 0);

  // What delete_table (directly, or via ddl_log after a crash) does:
  {
    auto row_lo = table_id_prefix(tid);
    auto row_hi = table_id_upper_bound(tid);
    auto idx_lo = index_id_prefix(tid, 1);
    auto idx_hi = index_id_upper_bound(tid, 1);
    (void)h.db().del_range({}, as_view(row_lo), as_view(row_hi));
    (void)h.db().del_range({}, as_view(idx_lo), as_view(idx_hi));
  }

  CHECK(count_table_keys(h.db(), tid) == 0);
  CHECK(count_index_keys(h.db(), tid, 1) == 0);
}
