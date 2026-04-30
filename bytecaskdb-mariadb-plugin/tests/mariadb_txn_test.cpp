// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_txn_test.cpp — Unit tests for MariaDBTxn transaction management.
//
// Tests the transaction buffer, RYOW semantics, commit/rollback behavior,
// and MergeIterator functionality without requiring a full MariaDB server.
//
// NOTE: This file is not currently built by CMakeLists.txt. It exercises
// the real MariaDBTxn class and therefore requires a real bytecask::DB
// (opened in a temp directory) when wired in. The opaque pointer below
// is a placeholder that mirrors the constructor's signature; do not enable
// this target without supplying a real DB instance.

#include "mariadb_stubs.h"     // MariaDB type stubs
#include "bytecask.hpp"
#include "bytecaskdb_txn.h"
#include "catalog.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

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

  // Helper to create test key-value pairs
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
// Basic Transaction Lifecycle
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn basic lifecycle", "[txn][lifecycle]") {
  auto txn = create_txn();

  SECTION("initial state") {
    REQUIRE_FALSE(txn->is_active());
  }

  SECTION("becomes active after begin_if_needed") {
    // Mock THD and handlerton for begin_if_needed
    THD thd{};
    handlerton hton{};

    txn->begin_if_needed(&thd, &hton);
    REQUIRE(txn->is_active());
  }
}

// =========================================================================
// Write Buffering
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn write buffering", "[txn][buffer]") {
  auto txn = create_txn();

  auto key1 = make_key("user:1");
  auto val1 = make_value("alice");
  auto key2 = make_key("user:2");
  auto val2 = make_value("bob");

  SECTION("buffer_put stores operation") {
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());

    // Should be able to read back the value
    bytecask::Bytes out_val;
    int result = txn->get(key1.data(), key1.size(), out_val);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val.size() == val1.size());
    REQUIRE(std::memcmp(out_val.data(), val1.data(), val1.size()) == 0);
  }

  SECTION("buffer_del creates tombstone") {
    // First put a value
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    REQUIRE(txn->exists(key1.data(), key1.size()));

    // Then delete it
    txn->buffer_del(key1.data(), key1.size());
    REQUIRE_FALSE(txn->exists(key1.data(), key1.size()));

    // get() should return not found
    bytecask::Bytes out_val;
    int result = txn->get(key1.data(), key1.size(), out_val);
    REQUIRE(result == 0);  // Not found
  }

  SECTION("multiple operations in order") {
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    txn->buffer_put(key2.data(), key2.size(), val2.data(), val2.size());

    REQUIRE(txn->exists(key1.data(), key1.size()));
    REQUIRE(txn->exists(key2.data(), key2.size()));

    // Overwrite key1 with new value
    auto val1_new = make_value("alice_updated");
    txn->buffer_put(key1.data(), key1.size(), val1_new.data(), val1_new.size());

    // Should see the new value
    bytecask::Bytes out_val;
    int result = txn->get(key1.data(), key1.size(), out_val);

    REQUIRE(result == 1);
    REQUIRE(out_val.size() == val1_new.size());
    REQUIRE(std::memcmp(out_val.data(), val1_new.data(), val1_new.size()) == 0);
  }
}

// =========================================================================
// Read-Your-Own-Writes (RYOW) Semantics - THE CRITICAL TEST
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn RYOW semantics", "[txn][ryow]") {
  auto txn = create_txn();

  auto key = make_key("session:abc123");
  auto val = make_value("active");

  SECTION("exists() returns false for non-existent key") {
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }

  SECTION("exists() returns true after buffer_put - CRITICAL TEST") {
    // This is the exact scenario that's failing in the actual bug
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());

    // This MUST return true - if it returns false, it's the duplicate insert bug
    bool key_exists = txn->exists(key.data(), key.size());
    REQUIRE(key_exists);
  }

  SECTION("get() returns buffered value") {
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());

    bytecask::Bytes out_val;
    int result = txn->get(key.data(), key.size(), out_val);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val.size() == val.size());
    REQUIRE(std::memcmp(out_val.data(), val.data(), val.size()) == 0);
  }

  SECTION("exists() returns false after buffer_del") {
    // Put then delete
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));

    txn->buffer_del(key.data(), key.size());
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }
}

// =========================================================================
// Transaction Isolation - CRITICAL FOR BUG PREVENTION
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn isolation between transactions", "[txn][isolation]") {
  auto txn1 = create_txn();
  auto txn2 = create_txn();

  auto key = make_key("shared:key");
  auto val1 = make_value("txn1_value");
  auto val2 = make_value("txn2_value");

  SECTION("transactions don't see each other's buffered writes") {
    // txn1 buffers a write
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    REQUIRE(txn1->exists(key.data(), key.size()));

    // txn2 should not see it
    REQUIRE_FALSE(txn2->exists(key.data(), key.size()));

    bytecask::Bytes out_val;
    int result = txn2->get(key.data(), key.size(), out_val);
    REQUIRE(result == 0);  // Not found in txn2
  }

  SECTION("transactions can buffer same key independently") {
    // Both transactions buffer the same key with different values
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn2->buffer_put(key.data(), key.size(), val2.data(), val2.size());

    // Each should see their own value
    bytecask::Bytes out_val1;
    int result1 = txn1->get(key.data(), key.size(), out_val1);

    bytecask::Bytes out_val2;
    int result2 = txn2->get(key.data(), key.size(), out_val2);

    REQUIRE(result1 == 1);
    REQUIRE(result2 == 1);
    REQUIRE(std::memcmp(out_val1.data(), val1.data(), val1.size()) == 0);
    REQUIRE(std::memcmp(out_val2.data(), val2.data(), val2.size()) == 0);
  }

  SECTION("REGRESSION TEST: simulate multiple write_row calls") {
    // This simulates the exact bug scenario:
    // Two separate MariaDBTxn objects trying to write the same key

    auto duplicate_key = make_key("table:1:pk:42");  // Same primary key
    auto row_value = make_value("user_data");

    // First write_row call - uses txn1
    txn1->buffer_put(duplicate_key.data(), duplicate_key.size(),
                    row_value.data(), row_value.size());
    REQUIRE(txn1->exists(duplicate_key.data(), duplicate_key.size()));

    // Second write_row call - uses txn2 (different transaction object)
    // The exists() check should NOT see txn1's buffered write
    REQUIRE_FALSE(txn2->exists(duplicate_key.data(), duplicate_key.size()));

    // This would allow the duplicate to be buffered (which is the bug)
    txn2->buffer_put(duplicate_key.data(), duplicate_key.size(),
                    row_value.data(), row_value.size());
    REQUIRE(txn2->exists(duplicate_key.data(), duplicate_key.size()));

    // Both transactions now have the same key buffered independently
    // When both commit, we get duplicate rows in the database
  }
}

// =========================================================================
// Commit/Rollback Behavior
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn commit rollback", "[txn][commit]") {
  auto txn = create_txn();

  auto key = make_key("counter:visits");
  auto val = make_value("42");

  SECTION("rollback clears buffer") {
    THD thd{};

    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));

    txn->rollback(&thd, true);  // all=true for full rollback

    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }
}

// =========================================================================
// Edge Cases and Error Handling
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn edge cases", "[txn][edge]") {
  auto txn = create_txn();

  SECTION("get on non-existent key returns 0") {
    auto key = make_key("nonexistent");

    bytecask::Bytes out_val;
    int result = txn->get(key.data(), key.size(), out_val);

    REQUIRE(result == 0);  // Not found
    REQUIRE(out_val.size() == 0);
  }

  SECTION("exists on empty key") {
    std::vector<uint8_t> empty_key;
    REQUIRE_FALSE(txn->exists(empty_key.data(), empty_key.size()));
  }

  SECTION("buffer operations with empty values") {
    auto key = make_key("empty_value_key");

    // Put empty value
    txn->buffer_put(key.data(), key.size(), nullptr, 0);
    REQUIRE(txn->exists(key.data(), key.size()));

    // Should be able to retrieve empty value
    bytecask::Bytes out_val;
    int result = txn->get(key.data(), key.size(), out_val);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val.size() == 0);

    if (out_val) {
    }
  }

  SECTION("overwrite same key multiple times") {
    auto key = make_key("overwrite_test");
    auto val1 = make_value("first");
    auto val2 = make_value("second");
    auto val3 = make_value("third");

    txn->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn->buffer_put(key.data(), key.size(), val2.data(), val2.size());
    txn->buffer_put(key.data(), key.size(), val3.data(), val3.size());

    // Should see the last value
    bytecask::Bytes out_val;
    int result = txn->get(key.data(), key.size(), out_val);

    REQUIRE(result == 1);
    REQUIRE(out_val.size() == val3.size());
    REQUIRE(std::memcmp(out_val.data(), val3.data(), val3.size()) == 0);
  }
}