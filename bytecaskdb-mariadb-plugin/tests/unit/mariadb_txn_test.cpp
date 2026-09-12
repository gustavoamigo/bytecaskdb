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
    // Simulate multi-statement mode (BEGIN or non-autocommit).
    g_stub_thd_options = OPTION_BEGIN;
    THD thd{};
    handlerton hton{};

    txn->begin_if_needed(&thd, &hton);
    REQUIRE(txn->is_active());
    g_stub_thd_options = 0;
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

  SECTION("commit-time conflict returns HA_ERR_FOUND_DUPP_KEY and raises "
          "ER_DUP_ENTRY_WITH_KEY_NAME") {
    auto txn = create_txn();
    THD thd{};

    g_stub_last_my_error_code = 0;

    txn->begin_deferred_insert();
    txn->buffer_put(pk.data(), pk.size(), val2.data(), val2.size(),
                    /*guard_absent=*/true);

    int rc = txn->commit(&thd, true);

    REQUIRE(rc == HA_ERR_FOUND_DUPP_KEY);
    // ER_DUP_ENTRY's second format placeholder is an integer key index, not
    // a string — passing a key name there is undefined behavior on the
    // varargs call and produces a garbled message at runtime. Regression
    // coverage for that bug: assert the code actually raised is the
    // two-string variant the (value, key-name) args match.
    REQUIRE(g_stub_last_my_error_code == ER_DUP_ENTRY_WITH_KEY_NAME);
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

    (void)out_val;
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
// =========================================================================
// Bulk-copy mode — batched writes for ALTER TABLE ... ALGORITHM=COPY
// =========================================================================

TEST_CASE_METHOD(MariaDBTxnFixture, "MariaDBTxn bulk copy mode", "[txn][bulk]") {
  auto txn = create_txn();
  THD thd{};

  auto db_has = [&](const std::vector<uint8_t> &k) {
    auto snap = temp_db_.db()->snapshot();
    return snap.contains_key({}, as_view(k.data(), k.size()));
  };
  auto db_key_count = [&]() {
    size_t n = 0;
    for (auto &k : temp_db_.db()->keys_from({})) { (void)k; ++n; }
    return n;
  };

  SECTION("flush persists to DB before any commit; buffer stays bounded") {
    txn->begin_bulk_copy(256);
    REQUIRE(txn->in_bulk_copy());

    // First key: below threshold, not flushed yet.
    auto k0 = make_key("row:0000");
    auto v0 = make_value(std::string(64, 'a').c_str());
    txn->bulk_buffer_put(k0.data(), k0.size(), v0.data(), v0.size());
    txn->bulk_note_key(k0.data(), k0.size());
    REQUIRE_FALSE(txn->bulk_should_flush());
    REQUIRE_FALSE(db_has(k0));

    // Pile on until the byte threshold trips, then flush.
    for (int i = 1; i < 8; ++i) {
      auto k = make_key(("row:000" + std::to_string(i)).c_str());
      txn->bulk_buffer_put(k.data(), k.size(), v0.data(), v0.size());
      txn->bulk_note_key(k.data(), k.size());
    }
    REQUIRE(txn->bulk_should_flush());
    REQUIRE(txn->bulk_flush(false) == 0);

    // Flushed rows are durable in the DB even though nothing committed.
    REQUIRE(db_has(k0));
    REQUIRE(db_key_count() == 8);
    // Threshold counter reset; snapshot/ops untouched (txn still "inactive").
    REQUIRE_FALSE(txn->bulk_should_flush());
    REQUIRE_FALSE(txn->is_active());
    REQUIRE(txn->in_bulk_copy());
  }

  SECTION("duplicate detection within a batch and across a flush") {
    txn->begin_bulk_copy(1 << 20);  // large — no auto-flush

    // Unique-index probe key is the value prefix; the stored secondary key
    // has the PK appended. Mirror that: note the prefix, store prefix+pk.
    auto uprefix = make_key("uk:\x01""alice");
    auto skey    = make_key("uk:\x01""alice\x01pk42");
    auto pk      = make_key("pk:42");

    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(uprefix.data(), uprefix.size()));
    txn->bulk_note_key(uprefix.data(), uprefix.size());
    // Same value again in the same unflushed batch -> caught.
    REQUIRE(txn->bulk_unique_prefix_exists(uprefix.data(), uprefix.size()));

    txn->bulk_buffer_put(pk.data(), pk.size(), nullptr, 0);
    txn->bulk_note_key(pk.data(), pk.size());
    txn->bulk_buffer_put(skey.data(), skey.size(), nullptr, 0);
    REQUIRE(txn->bulk_flush(true) == 0);

    // After the flush, bulk_seen_ is cleared but the DB probe still catches it.
    REQUIRE(txn->bulk_pk_exists(pk.data(), pk.size()));
    auto prefix = make_key("uk:\x01""ali");
    REQUIRE(txn->bulk_unique_prefix_exists(prefix.data(), prefix.size()));
    auto absent = make_key("uk:\x01""zzz");
    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(absent.data(), absent.size()));
  }

  SECTION("empty flush is a no-op") {
    txn->begin_bulk_copy(256);
    REQUIRE(txn->bulk_flush(true) == 0);
    REQUIRE(db_key_count() == 0);
  }

  SECTION("rollback drops the pending batch and leaves bulk mode") {
    txn->begin_bulk_copy(1 << 20);
    auto k = make_key("row:pending");
    txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
    txn->rollback(&thd, true);
    REQUIRE_FALSE(txn->in_bulk_copy());
    REQUIRE_FALSE(db_has(k));  // never flushed
  }

  SECTION("abort_bulk_copy discards batch; mode can be re-entered") {
    txn->begin_bulk_copy(1 << 20);
    auto k = make_key("row:aborted");
    txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
    txn->abort_bulk_copy();
    REQUIRE_FALSE(txn->in_bulk_copy());

    txn->begin_bulk_copy(256);
    auto k2 = make_key("row:after");
    txn->bulk_buffer_put(k2.data(), k2.size(), nullptr, 0);
    REQUIRE(txn->bulk_flush(true) == 0);
    REQUIRE(db_has(k2));
    REQUIRE_FALSE(db_has(k));
  }

  SECTION("commit flushes the tail of the batch") {
    txn->begin_bulk_copy(1 << 20);  // no auto-flush; all rows are "tail"
    for (int i = 0; i < 5; ++i) {
      auto k = make_key(("tail:" + std::to_string(i)).c_str());
      txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
      txn->bulk_note_key(k.data(), k.size());
    }
    REQUIRE(db_key_count() == 0);
    REQUIRE(txn->commit(&thd, true) == 0);
    REQUIRE(db_key_count() == 5);
    REQUIRE_FALSE(txn->in_bulk_copy());
  }
}
