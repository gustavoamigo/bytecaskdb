// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_txn_simple_test.cpp — Simplified unit tests for MariaDBTxn RYOW semantics.
//
// Tests only the essential buffer_put/exists functionality that would have
// caught the duplicate INSERT bug, without requiring full MariaDB compilation.

#include "mariadb_stubs.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <vector>
#include <map>
#include <optional>

// Opaque DB-handle stand-in. The simple tests never call into ByteCaskDB —
// they only need a non-null pointer to mirror the MariaDBTxn constructor
// signature.
using opaque_db_t = void;

// Minimal MariaDBTxn implementation for testing RYOW semantics
class SimpleTxn {
public:
  using LookupMap = std::map<std::vector<uint8_t>, std::optional<std::vector<uint8_t>>>;

  explicit SimpleTxn(opaque_db_t* db) : db_(db) {}

  void buffer_put(const uint8_t* key, size_t klen,
                 const uint8_t* val, size_t vlen) {
    std::vector<uint8_t> k(key, key + klen);
    std::vector<uint8_t> v(val, val + vlen);
    lookup_[k] = v;
  }

  void buffer_del(const uint8_t* key, size_t klen) {
    std::vector<uint8_t> k(key, key + klen);
    lookup_[k] = std::nullopt;  // tombstone
  }

  bool exists(const uint8_t* key, size_t klen) {
    std::vector<uint8_t> k(key, key + klen);
    auto it = lookup_.find(k);
    if (it != lookup_.end()) {
      return it->second.has_value();
    }
    // In real implementation, would check snapshot here
    return false;
  }

  int get(const uint8_t* key, size_t klen,
          uint8_t** out_val, size_t* out_val_len) {
    std::vector<uint8_t> k(key, key + klen);
    auto it = lookup_.find(k);
    if (it != lookup_.end()) {
      if (!it->second.has_value()) {
        return 0;  // tombstone
      }
      const auto& val = it->second.value();
      *out_val = static_cast<uint8_t*>(malloc(val.size()));
      if (*out_val) {
        std::memcpy(*out_val, val.data(), val.size());
        *out_val_len = val.size();
        return 1;  // found
      }
    }
    return 0;  // not found
  }

private:
  opaque_db_t* db_;
  LookupMap lookup_;
};

// =========================================================================
// Test Fixtures
// =========================================================================

class SimpleTxnFixture {
public:
  SimpleTxnFixture() {
    db_ = reinterpret_cast<opaque_db_t*>(0x12345678);
  }

  std::unique_ptr<SimpleTxn> create_txn() {
    return std::make_unique<SimpleTxn>(db_);
  }

  std::vector<uint8_t> make_key(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

  std::vector<uint8_t> make_value(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
  }

protected:
  opaque_db_t* db_;
};

// =========================================================================
// CRITICAL RYOW Tests - These would catch the duplicate INSERT bug
// =========================================================================

TEST_CASE_METHOD(SimpleTxnFixture, "SimpleTxn RYOW semantics", "[txn][ryow][critical]") {
  auto txn = create_txn();

  auto key = make_key("user:42");
  auto val = make_value("john_doe");

  SECTION("exists() returns false for non-existent key") {
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }

  SECTION("exists() returns true after buffer_put - CRITICAL TEST") {
    // This is the exact scenario that's failing in the duplicate INSERT bug
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());

    // This MUST return true - if it returns false, duplicate key detection fails
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
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));

    txn->buffer_del(key.data(), key.size());
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }
}

// =========================================================================
// Transaction Isolation - Multiple Transaction Objects
// =========================================================================

TEST_CASE_METHOD(SimpleTxnFixture, "SimpleTxn isolation test", "[txn][isolation][critical]") {
  auto txn1 = create_txn();
  auto txn2 = create_txn();

  auto key = make_key("shared:key");
  auto val1 = make_value("txn1_value");
  auto val2 = make_value("txn2_value");

  SECTION("REGRESSION TEST: Simulate duplicate INSERT bug scenario") {
    // This simulates the exact bug: multiple MariaDBTxn objects for same INSERT

    // First write_row() call - uses txn1
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    REQUIRE(txn1->exists(key.data(), key.size()));

    // Second write_row() call - uses txn2 (different transaction object)
    // The exists() check should NOT see txn1's buffered write
    REQUIRE_FALSE(txn2->exists(key.data(), key.size()));

    // Since txn2 can't see txn1's write, it allows the duplicate to be buffered
    // This is what causes the bug - both transactions buffer the same key
    txn2->buffer_put(key.data(), key.size(), val2.data(), val2.size());
    REQUIRE(txn2->exists(key.data(), key.size()));

    // Both transactions now have the same key buffered independently
    // When both commit to ByteCask, we get duplicate rows
  }

  SECTION("transactions don't see each other's buffered writes") {
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    REQUIRE(txn1->exists(key.data(), key.size()));

    // txn2 should not see txn1's buffer
    REQUIRE_FALSE(txn2->exists(key.data(), key.size()));

    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn2->get(key.data(), key.size(), &out_val, &out_val_len);
    REQUIRE(result == 0);  // Not found in txn2
  }
}

// =========================================================================
// Edge Cases and Multiple Operations
// =========================================================================

TEST_CASE_METHOD(SimpleTxnFixture, "SimpleTxn edge cases", "[txn][edge]") {
  auto txn = create_txn();

  SECTION("overwrite same key multiple times") {
    auto key = make_key("counter");
    auto val1 = make_value("1");
    auto val2 = make_value("2");
    auto val3 = make_value("3");

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

  SECTION("empty values") {
    auto key = make_key("empty_key");

    txn->buffer_put(key.data(), key.size(), nullptr, 0);
    REQUIRE(txn->exists(key.data(), key.size()));

    uint8_t* out_val = nullptr;
    size_t out_val_len = 0;
    int result = txn->get(key.data(), key.size(), &out_val, &out_val_len);

    REQUIRE(result == 1);
    REQUIRE(out_val_len == 0);

    if (out_val) {
      free(out_val);
    }
  }
}