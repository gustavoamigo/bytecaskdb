// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_txn_test.cpp — Unit tests for MariaDBTxn transaction management.
//
// Tests the transaction buffer, RYOW semantics, commit/rollback behavior,
// and MergeIterator functionality without requiring a full MariaDB server.

#include "bytecask_c_stubs.h"  // Must be included first for C API stubs
#include "mariadb_stubs.h"     // MariaDB type stubs

#include "bytecaskdb_txn.h"
#include "catalog.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <vector>

using namespace bytecaskdb;

// =========================================================================
// Test Fixtures
// =========================================================================

class MariaDBTxnFixture {
public:
  MariaDBTxnFixture() {
    // Create a mock database handle
    db_ = reinterpret_cast<bytecask_db_t*>(0x12345678);
  }

  std::unique_ptr<MariaDBTxn> create_txn() {
    return std::make_unique<MariaDBTxn>(db_);
  }

  // Helper to create test key-value pairs
  std::vector<uint8_t> make_key(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

  std::vector<uint8_t> make_value(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

protected:
  bytecask_db_t* db_;
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
    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;

    int result = txn->get(key1.data(), key1.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val_len == val1.size());
    REQUIRE(std::memcmp(out_val, val1.data(), val1.size()) == 0);

    free(out_val);
  }

  SECTION("buffer_del creates tombstone") {
    // First put a value
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    REQUIRE(txn->exists(key1.data(), key1.size()));

    // Then delete it
    txn->buffer_del(key1.data(), key1.size());
    REQUIRE_FALSE(txn->exists(key1.data(), key1.size()));

    // get() should return not found
    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key1.data(), key1.size(), &out_val, &out_val_len);
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
    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key1.data(), key1.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);
    REQUIRE(out_val_len == val1_new.size());
    REQUIRE(std::memcmp(out_val, val1_new.data(), val1_new.size()) == 0);

    free(out_val);
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

    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key.data(), key.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val_len == val.size());
    REQUIRE(std::memcmp(out_val, val.data(), val.size()) == 0);

    free(out_val);
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

    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn2->get(key.data(), key.size(), &out_val, &out_val_len);
    REQUIRE(result == 0);  // Not found in txn2
  }

  SECTION("transactions can buffer same key independently") {
    // Both transactions buffer the same key with different values
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn2->buffer_put(key.data(), key.size(), val2.data(), val2.size());

    // Each should see their own value
    uint8_t* out_val1 = nullptr;
    size_t out_val1_len = 0;
    int result1 = txn1->get(key.data(), key.size(), &out_val1, &out_val1_len);

    uint8_t* out_val2 = nullptr;
    size_t out_val2_len = 0;
    int result2 = txn2->get(key.data(), key.size(), &out_val2, &out_val2_len);

    REQUIRE(result1 == 1);
    REQUIRE(result2 == 1);
    REQUIRE(std::memcmp(out_val1, val1.data(), val1.size()) == 0);
    REQUIRE(std::memcmp(out_val2, val2.data(), val2.size()) == 0);

    free(out_val1);
    free(out_val2);
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

    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key.data(), key.size(), &out_val, &out_val_len);

    REQUIRE(result == 0);  // Not found
    REQUIRE(out_val == nullptr);
    REQUIRE(out_val_len == 0);
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
    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key.data(), key.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);  // Found
    REQUIRE(out_val_len == 0);

    if (out_val) {
      free(out_val);
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
    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key.data(), key.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);
    REQUIRE(out_val_len == val3.size());
    REQUIRE(std::memcmp(out_val, val3.data(), val3.size()) == 0);

    free(out_val);
  }
}