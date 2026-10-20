// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_txn_test.cpp — Unit tests for MariaDBTxn, the plugin's adapter over
// the SQL layer's transaction, against a real bytecask::DB and the MariaDB
// stubs.
//
// The transaction itself — buffering, RYOW, OCC commit, statement rollback,
// savepoints, bulk copy, the merge iterators — is tested in the SQL layer
// (bytecaskdb-sql-layer/tests/transaction_test.cpp). What is tested here is
// what the adapter adds: registration with the server's coordinator, the
// HA_ERR_* codes and server errors a commit outcome maps to, the deferred
// insert's duplicate report, savepoint slots, the catalog row counter, and
// the plugin's table and index namespaces.

#include "mariadb_stubs.h"     // MariaDB type stubs
#include "bytecask.hpp"
#include "bytecaskdb_txn.h"
#include "catalog.h"
#include "key_encoding.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace bytecaskdb {
int64_t catalog_row_count(uint32_t table_id);
std::atomic<int64_t> *catalog_row_count_ptr(uint32_t table_id);
}  // namespace bytecaskdb

using namespace bytecaskdb;

namespace {

// RAII helper: opens a real bytecask::DB in a temp directory; removes it on dtor.
// `bytecask::DB` is non-moveable, so we wrap it in a holder struct (which can
// initialise the DB via member-init) and own that through unique_ptr.
struct DBHolder {
  bytecask::DB db;
  explicit DBHolder(const std::filesystem::path &dir)
      : db{bytecask::DB::open(dir)} {}
};

class TempDB {
public:
  TempDB() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("bcdb_txn_test_" + std::to_string(::getpid()) +
             "_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
    holder_ = std::make_unique<DBHolder>(path_);
  }
  ~TempDB() {
    holder_.reset();
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  bytecask::DB* db() { return &holder_->db; }
private:
  std::filesystem::path path_;
  std::unique_ptr<DBHolder> holder_;
};

}  // namespace

// =========================================================================
// Test Fixtures
// =========================================================================

class MariaDBTxnFixture {
public:
  MariaDBTxnFixture() = default;

  std::unique_ptr<MariaDBTxn> create_txn() {
    return std::make_unique<MariaDBTxn>(temp_db_.db());
  }

  std::vector<uint8_t> make_key(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

  std::vector<uint8_t> make_value(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

protected:
  TempDB temp_db_;
};

// =========================================================================
// Registration with the server's transaction coordinator
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn registers with the coordinator",
                 "[txn][lifecycle]") {
  auto txn = create_txn();
  THD thd{};
  handlerton hton{};
  g_stub_trans_registered_all = 0;
  g_stub_trans_registered_stmt = 0;

  SECTION("initial state") {
    REQUIRE_FALSE(txn->is_active());
  }

  SECTION("multi-statement: the transaction and each statement, once") {
    // Simulate multi-statement mode (BEGIN or non-autocommit).
    g_stub_thd_options = OPTION_BEGIN;

    txn->begin_if_needed(&thd, &hton);
    REQUIRE(txn->is_active());
    txn->begin_if_needed(&thd, &hton);  // a second handler in the statement
    REQUIRE(g_stub_trans_registered_all == 1);
    REQUIRE(g_stub_trans_registered_stmt == 1);

    REQUIRE(txn->commit(&thd, false) == 0);
    txn->begin_if_needed(&thd, &hton);
    REQUIRE(g_stub_trans_registered_all == 1);
    REQUIRE(g_stub_trans_registered_stmt == 2);
    g_stub_thd_options = 0;
  }

  SECTION("autocommit: the statement only, and no snapshot") {
    txn->begin_if_needed(&thd, &hton);
    REQUIRE_FALSE(txn->is_active());
    REQUIRE(g_stub_trans_registered_all == 0);
    REQUIRE(g_stub_trans_registered_stmt == 1);
  }
}

// =========================================================================
// Commit outcomes as HA_ERR_* codes and server errors
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn commit conflict is a deadlock",
                 "[txn][occ][autocommit]") {
  auto key = make_key("counter:1");
  auto v0 = make_value("0");
  auto v1 = make_value("1");
  THD thd{};

  {
    auto seed = create_txn();
    seed->buffer_put(key.data(), key.size(), v0.data(), v0.size());
    REQUIRE(seed->commit(&thd, true) == 0);
  }

  auto a = create_txn();
  auto b = create_txn();
  g_stub_last_my_error_code = 0;

  bytecask::Bytes out;
  REQUIRE(a->get(key.data(), key.size(), out) == 1);

  b->buffer_put(key.data(), key.size(), v1.data(), v1.size());
  REQUIRE(b->commit(&thd, true) == 0);
  REQUIRE(g_stub_last_my_error_code == 0);

  a->buffer_put(key.data(), key.size(), v1.data(), v1.size());
  REQUIRE(a->commit(&thd, true) == HA_ERR_LOCK_DEADLOCK);
  REQUIRE(g_stub_last_my_error_code == ER_LOCK_DEADLOCK);
}

// =========================================================================
// Deferred INSERT dup-check (P3): commit-time ensure_absent conflict
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn deferred insert dup check",
                  "[txn][commit][deferred]") {
  auto pk = make_key("user:1");
  auto val1 = make_value("alice");
  auto val2 = make_value("mallory");

  // Commit the row via a first, ordinary (non-deferred) transaction so it's
  // durable before the deferred-mode transaction under test tries to insert
  // the same PK.
  {
    auto seed = create_txn();
    THD thd{};
    seed->buffer_put(pk.data(), pk.size(), val1.data(), val1.size());
    REQUIRE(seed->commit(&thd, true) == 0);
  }

  auto txn = create_txn();
  THD thd{};
  g_stub_last_my_error_code = 0;

  SECTION("commit-time conflict returns HA_ERR_FOUND_DUPP_KEY and raises "
          "ER_DUP_ENTRY (1062), the code clients check for") {
    txn->begin_deferred_insert(nullptr, nullptr);
    txn->buffer_put(pk.data(), pk.size(), val2.data(), val2.size(),
                    /*guard_absent=*/true);

    int rc = txn->commit(&thd, true);

    REQUIRE(rc == HA_ERR_FOUND_DUPP_KEY);
    // The server pairs code ER_DUP_ENTRY with the WITH_KEY_NAME message
    // format; the code must stay 1062 because drivers and ORMs key their
    // duplicate-key handling on it. (Using ER_DUP_ENTRY's *own* format with
    // a key-name string is UB: its second placeholder is an integer.)
    REQUIRE(g_stub_last_my_error_code == ER_DUP_ENTRY);
  }

  SECTION("the reporter is given the duplicate key and the row") {
    std::vector<uint8_t> got_pk;
    std::vector<uint8_t> got_row;
    txn->begin_deferred_insert(
        nullptr, [&](const std::vector<uint8_t> &k,
                     const std::vector<uint8_t> &row) {
          got_pk = k;
          got_row = row;
          return true;  // reported
        });
    txn->buffer_put(pk.data(), pk.size(), val2.data(), val2.size(),
                    /*guard_absent=*/true);

    REQUIRE(txn->commit(&thd, true) == HA_ERR_FOUND_DUPP_KEY);
    REQUIRE(got_pk == pk);
    REQUIRE(got_row == val2);
    REQUIRE(g_stub_last_my_error_code == 0);  // the reporter raised it
    REQUIRE_FALSE(txn->in_deferred_insert());
  }
}

// =========================================================================
// Savepoints in the server's slots
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture,
                 "MariaDBTxn savepoint rollback restores row counts",
                 "[txn][savepoint]") {
  auto k1 = make_key("r:1");
  auto k2 = make_key("r:2");
  auto v = make_value("x");
  THD thd{};
  // The table's catalog counter, as a handler passes it.
  constexpr uint32_t kTid = 9002;
  catalog_row_count_ptr(kTid)->store(0);
  std::atomic<int64_t> &rows = *catalog_row_count_ptr(kTid);
  uint32_t sp1 = 0, sp2 = 0;

  auto txn = create_txn();
  txn->savepoint_set(&sp1);
  txn->buffer_put(k1.data(), k1.size(), v.data(), v.size());
  txn->track_row_count_delta(kTid, &rows, 1);

  txn->savepoint_set(&sp2);
  REQUIRE(sp2 != sp1);
  txn->buffer_put(k2.data(), k2.size(), v.data(), v.size());
  txn->track_row_count_delta(kTid, &rows, 1);
  REQUIRE(rows.load() == 2);

  SECTION("rollback to the inner savepoint") {
    txn->savepoint_rollback(&sp2);
    REQUIRE(rows.load() == 1);
    REQUIRE(txn->exists(k1.data(), k1.size()));
    REQUIRE_FALSE(txn->exists(k2.data(), k2.size()));
  }

  SECTION("release keeps the work") {
    txn->savepoint_release(&sp1);
    REQUIRE(txn->commit(&thd, true) == 0);
    REQUIRE(rows.load() == 2);
  }
}

// =========================================================================
// Row counters: a null counter is the table's catalog counter
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture,
                 "MariaDBTxn null row counter is the catalog's",
                 "[txn][rollback]") {
  constexpr uint32_t kTid = 9001;
  auto k = make_key("r:1");
  auto v = make_value("x");
  THD thd{};
  const auto before = catalog_row_count(kTid);

  auto txn = create_txn();
  txn->buffer_put(k.data(), k.size(), v.data(), v.size());
  txn->track_row_count_delta(kTid, nullptr, 1);
  REQUIRE(catalog_row_count(kTid) == before + 1);
  txn->rollback(&thd, true);
  REQUIRE(catalog_row_count(kTid) == before);
}

// =========================================================================
// Bulk flush as an HA_ERR_* code
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn bulk flush", "[txn][bulk]") {
  auto txn = create_txn();
  txn->begin_bulk_copy(1 << 20);
  auto k = make_key("row:1");
  txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
  REQUIRE(txn->bulk_flush(true) == 0);
  REQUIRE(temp_db_.db()->contains_key({}, as_view(k)));
}

// =========================================================================
// MergeIterator over the plugin's table and index namespaces
// =========================================================================

namespace {

// Row key for table `tid` with a one-byte suffix: [0x02 | tid BE4 | c].
std::vector<uint8_t> row_key(uint32_t tid, uint8_t c) {
  auto k = table_id_prefix(tid);
  k.push_back(c);
  return k;
}

// Index key for (tid, iid) with a one-byte suffix.
std::vector<uint8_t> index_key(uint32_t tid, uint16_t iid, uint8_t c) {
  auto k = index_id_prefix(tid, iid);
  k.push_back(c);
  return k;
}

std::vector<uint8_t> collect_keys(MariaDBTxn::MergeIterator &it) {
  std::vector<uint8_t> out;
  for (; it.valid(); it.next()) {
    out.push_back(it.key().data()[it.key().size() - 1]);
  }
  return out;
}

}  // namespace

TEST_CASE_METHOD(MariaDBTxnFixture,
                 "MariaDBTxn MergeIterator scans one table or index",
                 "[txn][merge][reverse]") {
  constexpr uint32_t kTid = 42;
  constexpr uint16_t kIid = 1;
  auto v = make_value("v");
  THD thd{};

  // Committed: rows 1 and 3, index entries 1 and 3. Rows of the next table
  // and entries of the next index must never appear.
  {
    auto seed = create_txn();
    for (uint8_t c : {1, 3}) {
      auto r = row_key(kTid, c);
      seed->buffer_put(r.data(), r.size(), v.data(), v.size());
      auto i = index_key(kTid, kIid, c);
      seed->buffer_put(i.data(), i.size(), nullptr, 0);
    }
    auto other_row = row_key(kTid + 1, 0);
    seed->buffer_put(other_row.data(), other_row.size(), v.data(), v.size());
    auto other_idx = index_key(kTid, kIid + 1, 0);
    seed->buffer_put(other_idx.data(), other_idx.size(), nullptr, 0);
    REQUIRE(seed->commit(&thd, true) == 0);
  }

  auto txn = create_txn();
  auto r2 = row_key(kTid, 2);
  txn->buffer_put(r2.data(), r2.size(), v.data(), v.size());
  auto i2 = index_key(kTid, kIid, 2);
  txn->buffer_put(i2.data(), i2.size(), nullptr, 0);

  SECTION("table, both directions") {
    auto lo = table_id_prefix(kTid);
    auto hi = table_id_upper_bound(kTid);
    auto fwd = txn->iter_prefix(lo.data(), lo.size(), hi.data(), hi.size(), kTid);
    REQUIRE(collect_keys(*fwd) == std::vector<uint8_t>{1, 2, 3});
    auto rev = txn->riter_prefix(hi.data(), hi.size(), lo.data(), lo.size(), kTid);
    REQUIRE(collect_keys(*rev) == std::vector<uint8_t>{3, 2, 1});
  }

  SECTION("index, both directions") {
    auto lo = index_id_prefix(kTid, kIid);
    auto hi = index_id_upper_bound(kTid, kIid);
    auto fwd = txn->iter_index_prefix(lo.data(), lo.size(), hi.data(), hi.size(),
                                      kTid, kIid);
    REQUIRE(collect_keys(*fwd) == std::vector<uint8_t>{1, 2, 3});
    auto rev = txn->riter_index_prefix(hi.data(), hi.size(), lo.data(), lo.size(),
                                       kTid, kIid);
    REQUIRE(collect_keys(*rev) == std::vector<uint8_t>{3, 2, 1});
  }
}
