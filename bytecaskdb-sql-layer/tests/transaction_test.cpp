// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The transaction of docs/transaction_design.md against a real DB: the write
// buffer and read-your-own-writes, the OCC snapshot an autocommit read pins,
// commit outcomes (conflict, deferred-insert duplicate), statement rollback
// and savepoints with their row counters, bulk-copy mode, and the merge
// iterators in both directions.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/bytecask.hpp"
#include "bytecaskdb-sql-layer/txn/transaction.h"

import bytecask.sql.tests.util;

using bytecask::sql::as_view;
using bytecask::sql::CommitOutcome;
using bytecask::sql::Transaction;
using Status = CommitOutcome::Status;

namespace {

constexpr bytecask::SizeLimits kLimits{65535, 16 * 1024 * 1024};

// Row counters by table id, as a caller's catalog keeps them; reset per test.
std::array<std::atomic<int64_t>, 16> g_rows{};

void add_rows(uint32_t table_id, int64_t delta) { g_rows.at(table_id).fetch_add(delta); }

struct DBHolder {
  bytecask::DB db;
  explicit DBHolder(const std::filesystem::path& dir) : db{bytecask::DB::open(dir)} {}
};

class TxnFixture {
public:
  TxnFixture() : dir_{"txn"}, holder_{std::make_unique<DBHolder>(dir_.path())} {
    for (auto& r : g_rows) {
      r.store(0);
    }
  }

  auto create_txn() -> std::unique_ptr<Transaction> {
    return std::make_unique<Transaction>(db(), kLimits, &add_rows);
  }
  auto db() -> bytecask::DB* { return &holder_->db; }

  static auto make_key(const std::string& s) -> std::vector<uint8_t> { return {s.begin(), s.end()}; }
  static auto make_value(const std::string& s) -> std::vector<uint8_t> { return {s.begin(), s.end()}; }

  static auto commit(Transaction& txn, bool all = true) -> Status { return txn.commit(all, true).status; }

private:
  sql_test::TempDir dir_;
  std::unique_ptr<DBHolder> holder_;
};

auto same(const bytecask::Bytes& got, const std::vector<uint8_t>& want) -> bool {
  return got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0;
}

}  // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction basic lifecycle", "[txn][lifecycle]") {
  auto txn = create_txn();

  SECTION("initial state") {
    REQUIRE_FALSE(txn->is_active());
  }

  SECTION("a multi-statement begin takes the snapshot and begins both") {
    auto begun = txn->begin_if_needed(true);
    REQUIRE(txn->is_active());
    REQUIRE(begun.transaction);
    REQUIRE(begun.statement);

    SECTION("a second begin inside the statement begins nothing") {
      begun = txn->begin_if_needed(true);
      REQUIRE_FALSE(begun.transaction);
      REQUIRE_FALSE(begun.statement);
    }

    SECTION("after a statement commit only a statement begins") {
      REQUIRE(commit(*txn, false) == Status::Committed);
      begun = txn->begin_if_needed(true);
      REQUIRE_FALSE(begun.transaction);
      REQUIRE(begun.statement);
    }
  }

  SECTION("an autocommit begin takes no snapshot and begins a statement") {
    auto begun = txn->begin_if_needed(false);
    REQUIRE_FALSE(txn->is_active());
    REQUIRE_FALSE(begun.transaction);
    REQUIRE(begun.statement);
  }
}

// =========================================================================
// Write buffering
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction write buffering", "[txn][buffer]") {
  auto txn = create_txn();

  auto key1 = make_key("user:1");
  auto val1 = make_value("alice");
  auto key2 = make_key("user:2");
  auto val2 = make_value("bob");

  SECTION("buffer_put stores operation") {
    txn->buffer_put(key1, val1);

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1, out_val) == 1);
    REQUIRE(same(out_val, val1));
  }

  SECTION("buffer_del creates tombstone") {
    txn->buffer_put(key1, val1);
    REQUIRE(txn->exists(key1));

    txn->buffer_del(key1);
    REQUIRE_FALSE(txn->exists(key1));

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1, out_val) == 0);
  }

  SECTION("multiple operations in order") {
    txn->buffer_put(key1, val1);
    txn->buffer_put(key2, val2);

    REQUIRE(txn->exists(key1));
    REQUIRE(txn->exists(key2));

    auto val1_new = make_value("alice_updated");
    txn->buffer_put(key1, val1_new);

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1, out_val) == 1);
    REQUIRE(same(out_val, val1_new));
  }

  SECTION("buffered_key_present sees the buffer only") {
    REQUIRE_FALSE(txn->buffered_key_present(key1));
    txn->buffer_put(key1, val1);
    REQUIRE(txn->buffered_key_present(key1));
    txn->buffer_del(key1);
    REQUIRE_FALSE(txn->buffered_key_present(key1));
  }
}

// =========================================================================
// Read-your-own-writes
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction RYOW semantics", "[txn][ryow]") {
  auto txn = create_txn();

  auto key = make_key("session:abc123");
  auto val = make_value("active");

  SECTION("exists() returns false for non-existent key") {
    REQUIRE_FALSE(txn->exists(key));
  }

  SECTION("exists() returns true after buffer_put") {
    // A false here is the duplicate-insert bug: a second insert in the
    // statement would not see the first.
    txn->buffer_put(key, val);
    REQUIRE(txn->exists(key));
  }

  SECTION("get() returns buffered value") {
    txn->buffer_put(key, val);

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key, out_val) == 1);
    REQUIRE(same(out_val, val));
  }

  SECTION("exists() returns false after buffer_del") {
    txn->buffer_put(key, val);
    REQUIRE(txn->exists(key));

    txn->buffer_del(key);
    REQUIRE_FALSE(txn->exists(key));
  }
}

// =========================================================================
// Isolation between transactions
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction isolation between transactions", "[txn][isolation]") {
  auto txn1 = create_txn();
  auto txn2 = create_txn();

  auto key = make_key("shared:key");
  auto val1 = make_value("txn1_value");
  auto val2 = make_value("txn2_value");

  SECTION("transactions don't see each other's buffered writes") {
    txn1->buffer_put(key, val1);
    REQUIRE(txn1->exists(key));

    REQUIRE_FALSE(txn2->exists(key));

    bytecask::Bytes out_val;
    REQUIRE(txn2->get(key, out_val) == 0);
  }

  SECTION("transactions can buffer same key independently") {
    txn1->buffer_put(key, val1);
    txn2->buffer_put(key, val2);

    bytecask::Bytes out_val1;
    bytecask::Bytes out_val2;
    REQUIRE(txn1->get(key, out_val1) == 1);
    REQUIRE(txn2->get(key, out_val2) == 1);
    REQUIRE(same(out_val1, val1));
    REQUIRE(same(out_val2, val2));
  }

  SECTION("both buffer the same key: the second commit conflicts") {
    txn1->buffer_put(key, val1);
    txn2->buffer_put(key, val2);
    REQUIRE(commit(*txn1) == Status::Committed);
    REQUIRE(commit(*txn2) == Status::Conflict);
  }
}

// =========================================================================
// Commit and rollback
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction commit rollback", "[txn][commit]") {
  auto txn = create_txn();

  auto key = make_key("counter:visits");
  auto val = make_value("42");

  SECTION("rollback clears buffer") {
    txn->buffer_put(key, val);
    REQUIRE(txn->exists(key));

    txn->rollback(true);

    REQUIRE_FALSE(txn->exists(key));
  }

  SECTION("commit makes the write visible to a new transaction") {
    txn->buffer_put(key, val);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE_FALSE(txn->is_active());
    auto probe = create_txn();
    REQUIRE(probe->exists(key));
  }
}

// =========================================================================
// Autocommit read-modify-write: the snapshot must predate the read
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction autocommit read pins the OCC snapshot",
                 "[txn][occ][autocommit]") {
  auto key = make_key("counter:1");
  auto v0 = make_value("0");
  auto v1 = make_value("1");

  {
    auto seed = create_txn();
    seed->buffer_put(key, v0);
    REQUIRE(commit(*seed) == Status::Committed);
  }

  // No begin_if_needed(true): this is autocommit, where the statement's
  // first call is the point read.
  auto a = create_txn();
  auto b = create_txn();

  SECTION("get() then concurrent commit then write conflicts") {
    bytecask::Bytes out;
    REQUIRE(a->get(key, out) == 1);
    REQUIRE(a->is_active());  // snapshot taken by the read, not the write

    b->buffer_put(key, v1);
    REQUIRE(commit(*b) == Status::Committed);

    // A computes its new value from the stale read and writes it back.
    a->buffer_put(key, v1);
    REQUIRE(commit(*a) == Status::Conflict);
  }

  SECTION("exists() then concurrent commit then write conflicts") {
    REQUIRE(a->exists(key));
    REQUIRE(a->is_active());

    b->buffer_put(key, v1);
    REQUIRE(commit(*b) == Status::Committed);

    a->buffer_put(key, v1);
    REQUIRE(commit(*a) == Status::Conflict);
  }

  SECTION("read-only statement releases the snapshot at commit") {
    bytecask::Bytes out;
    REQUIRE(a->get(key, out) == 1);
    REQUIRE(commit(*a) == Status::Committed);
    REQUIRE_FALSE(a->is_active());
  }

  SECTION("a failed commit reverts the row counter") {
    auto& rows = g_rows.at(1);
    REQUIRE(a->exists(key));
    b->buffer_put(key, v1);
    REQUIRE(commit(*b) == Status::Committed);
    a->buffer_put(key, v1);
    a->track_row_count_delta(1, 1);
    REQUIRE(rows.load() == 1);
    REQUIRE(commit(*a) == Status::Conflict);
    REQUIRE(rows.load() == 0);
  }
}

// =========================================================================
// Deferred INSERT dup-check: commit-time ensure_absent conflict
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction deferred insert dup check", "[txn][commit][deferred]") {
  auto pk = make_key("user:1");
  auto val1 = make_value("alice");
  auto val2 = make_value("mallory");

  // Commit the row through an ordinary transaction, so it is in the DB
  // before the deferred-mode transaction under test inserts the same PK.
  {
    auto seed = create_txn();
    seed->buffer_put(pk, val1);
    REQUIRE(commit(*seed) == Status::Committed);
  }

  auto txn = create_txn();
  const int owner = 0;
  int reports = 0;
  std::vector<uint8_t> reported_key;
  std::vector<uint8_t> reported_row;
  bool reporter_result = true;
  auto reporter = [&](const std::vector<uint8_t>& k, const std::vector<uint8_t>& row) {
    ++reports;
    reported_key = k;
    reported_row = row;
    return reporter_result;
  };
  txn->begin_deferred_insert(&owner, reporter);
  REQUIRE(txn->in_deferred_insert());

  SECTION("the guard's conflict is a DuplicateKey, handed to the reporter") {
    auto other = make_key("user:2");
    txn->buffer_put(other, val2, /*guard_absent=*/true);
    txn->buffer_put(pk, val2, /*guard_absent=*/true);
    REQUIRE_FALSE(txn->is_active());  // deferred mode takes no snapshot

    auto out = txn->commit(true, true);

    REQUIRE(out.status == Status::DuplicateKey);
    REQUIRE(out.duplicate.has_value());
    REQUIRE(out.duplicate->key == pk);
    REQUIRE(out.duplicate->row == val2);
    REQUIRE(reports == 1);
    REQUIRE(reported_key == pk);
    REQUIRE(reported_row == val2);
    REQUIRE(out.duplicate_reported);
    REQUIRE_FALSE(txn->in_deferred_insert());
    auto probe = create_txn();
    REQUIRE_FALSE(probe->exists(other));  // nothing written
  }

  SECTION("a reporter that declines leaves it unreported") {
    reporter_result = false;
    txn->buffer_put(pk, val2, /*guard_absent=*/true);
    auto out = txn->commit(true, true);
    REQUIRE(out.status == Status::DuplicateKey);
    REQUIRE(reports == 1);
    REQUIRE_FALSE(out.duplicate_reported);
  }

  SECTION("a forgotten reporter is not called") {
    const int stranger = 0;
    txn->forget_deferred_reporter(&stranger);  // not its owner: kept
    txn->forget_deferred_reporter(&owner);
    txn->buffer_put(pk, val2, /*guard_absent=*/true);
    auto out = txn->commit(true, true);
    REQUIRE(out.status == Status::DuplicateKey);
    REQUIRE(reports == 0);
    REQUIRE_FALSE(out.duplicate_reported);
  }

  SECTION("an absent key commits") {
    auto other = make_key("user:3");
    txn->buffer_put(other, val2, /*guard_absent=*/true);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE_FALSE(txn->in_deferred_insert());
    REQUIRE(reports == 0);
  }
}

TEST_CASE_METHOD(TxnFixture, "Transaction releases the deferred reporter at reset",
                 "[txn][commit][deferred]") {
  auto txn = create_txn();
  auto held = std::make_shared<int>(0);
  auto pk = make_key("user:1");
  auto v = make_value("v");

  SECTION("a rollback") {
    txn->begin_deferred_insert(nullptr, [held](const auto&, const auto&) { return true; });
    REQUIRE(held.use_count() == 2);
    txn->rollback(true);
    REQUIRE(held.use_count() == 1);
  }

  SECTION("a commit") {
    txn->begin_deferred_insert(nullptr, [held](const auto&, const auto&) { return true; });
    txn->buffer_put(pk, v, /*guard_absent=*/true);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE(held.use_count() == 1);
  }

  SECTION("a statement rollback inside a transaction") {
    (void)txn->begin_if_needed(true);
    txn->begin_deferred_insert(nullptr, [held](const auto&, const auto&) { return true; });
    txn->rollback(false);
    REQUIRE_FALSE(txn->in_deferred_insert());
    REQUIRE(held.use_count() == 1);
  }
}

TEST_CASE_METHOD(TxnFixture, "Transaction reverts row counts by table id", "[txn][rollback]") {
  // The counter handed to track_row_count_delta applies the delta now; the
  // revert goes through the RowCountAddFn by table id, never through a
  // pointer kept from the call.
  std::atomic<int64_t> cached{0};
  auto txn = create_txn();
  auto k = make_key("r:1");
  auto v = make_value("v");
  txn->buffer_put(k, v);
  txn->track_row_count_delta(3, 1, &cached);
  REQUIRE(cached.load() == 1);
  REQUIRE(g_rows.at(3).load() == 0);
  txn->rollback(true);
  REQUIRE(cached.load() == 1);
  REQUIRE(g_rows.at(3).load() == -1);
}

// =========================================================================
// Edge cases
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction edge cases", "[txn][edge]") {
  auto txn = create_txn();

  SECTION("get on non-existent key returns 0") {
    auto key = make_key("nonexistent");
    bytecask::Bytes out_val;
    REQUIRE(txn->get(key, out_val) == 0);
    REQUIRE(out_val.empty());
  }

  SECTION("exists on empty key") {
    std::vector<uint8_t> empty_key;
    REQUIRE_FALSE(txn->exists(empty_key));
  }

  SECTION("buffer operations with empty values") {
    auto key = make_key("empty_value_key");
    txn->buffer_put(key, {});
    REQUIRE(txn->exists(key));

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key, out_val) == 1);
    REQUIRE(out_val.empty());
  }

  SECTION("overwrite same key multiple times") {
    auto key = make_key("overwrite_test");
    auto val1 = make_value("first");
    auto val2 = make_value("second");
    auto val3 = make_value("third");

    txn->buffer_put(key, val1);
    txn->buffer_put(key, val2);
    txn->buffer_put(key, val3);

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key, out_val) == 1);
    REQUIRE(same(out_val, val3));
  }

  SECTION("an empty commit writes nothing") {
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE(db()->keys_from({}).begin() == std::default_sentinel);
  }
}

// =========================================================================
// Bulk-copy mode — batched writes for a copy into an unpublished keyspace
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction bulk copy mode", "[txn][bulk]") {
  auto txn = create_txn();

  auto db_has = [&](const std::vector<uint8_t>& k) {
    auto snap = db()->snapshot();
    return snap.contains_key({}, as_view(k));
  };
  auto db_key_count = [&]() {
    std::size_t n = 0;
    for (auto& k : db()->keys_from({})) {
      (void)k;
      ++n;
    }
    return n;
  };

  SECTION("flush persists to DB before any commit; buffer stays bounded") {
    txn->begin_bulk_copy(256);
    REQUIRE(txn->in_bulk_copy());

    // First key: below threshold, not flushed yet.
    auto k0 = make_key("row:0000");
    auto v0 = make_value(std::string(64, 'a'));
    txn->bulk_buffer_put(k0, v0);
    txn->bulk_note_key(k0);
    REQUIRE_FALSE(txn->bulk_should_flush());
    REQUIRE_FALSE(db_has(k0));

    // Pile on until the byte threshold trips, then flush.
    for (int i = 1; i < 8; ++i) {
      auto k = make_key("row:000" + std::to_string(i));
      txn->bulk_buffer_put(k, v0);
      txn->bulk_note_key(k);
    }
    REQUIRE(txn->bulk_should_flush());
    REQUIRE(txn->bulk_flush(false).status == Status::Committed);

    // Flushed rows are in the DB even though nothing committed.
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
    auto uprefix = make_key("uk:\x01" "alice");
    auto skey = make_key("uk:\x01" "alice\x01pk42");
    auto pk = make_key("pk:42");

    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(uprefix));
    txn->bulk_note_key(uprefix);
    // Same value again in the same unflushed batch -> caught.
    REQUIRE(txn->bulk_unique_prefix_exists(uprefix));

    txn->bulk_buffer_put(pk, {});
    txn->bulk_note_key(pk);
    txn->bulk_buffer_put(skey, {});
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);

    // After the flush, bulk_seen_ is cleared but the DB probe still catches it.
    REQUIRE(txn->bulk_pk_exists(pk));
    auto prefix = make_key("uk:\x01" "ali");
    REQUIRE(txn->bulk_unique_prefix_exists(prefix));
    auto absent = make_key("uk:\x01" "zzz");
    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(absent));
  }

  SECTION("empty flush is a no-op") {
    txn->begin_bulk_copy(256);
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);
    REQUIRE(db_key_count() == 0);
  }

  SECTION("rollback drops the pending batch and leaves bulk mode") {
    txn->begin_bulk_copy(1 << 20);
    auto k = make_key("row:pending");
    txn->bulk_buffer_put(k, {});
    txn->rollback(true);
    REQUIRE_FALSE(txn->in_bulk_copy());
    REQUIRE_FALSE(db_has(k));  // never flushed
  }

  SECTION("abort_bulk_copy discards batch; mode can be re-entered") {
    txn->begin_bulk_copy(1 << 20);
    auto k = make_key("row:aborted");
    txn->bulk_buffer_put(k, {});
    txn->abort_bulk_copy();
    REQUIRE_FALSE(txn->in_bulk_copy());

    txn->begin_bulk_copy(256);
    auto k2 = make_key("row:after");
    txn->bulk_buffer_put(k2, {});
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);
    REQUIRE(db_has(k2));
    REQUIRE_FALSE(db_has(k));
  }

  SECTION("commit flushes the tail of the batch") {
    txn->begin_bulk_copy(1 << 20);  // no auto-flush; all rows are "tail"
    for (int i = 0; i < 5; ++i) {
      auto k = make_key("tail:" + std::to_string(i));
      txn->bulk_buffer_put(k, {});
      txn->bulk_note_key(k);
    }
    REQUIRE(db_key_count() == 0);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE(db_key_count() == 5);
    REQUIRE_FALSE(txn->in_bulk_copy());
  }
}

// =========================================================================
// Statement rollback inside a multi-statement transaction (BC-249)
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction statement rollback keeps earlier statements",
                 "[txn][rollback][statement]") {
  auto k1 = make_key("row:1");
  auto k2 = make_key("row:2");
  auto k3 = make_key("row:3");
  auto v = make_value("x");
  auto& rows = g_rows.at(7);

  auto txn = create_txn();

  // Statement 1: insert row:1.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k1, v);
  txn->track_row_count_delta(7, 1);
  REQUIRE(commit(*txn, false) == Status::Committed);

  // Statement 2: insert row:2.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k2, v);
  txn->track_row_count_delta(7, 1);
  REQUIRE(commit(*txn, false) == Status::Committed);

  // Statement 3: buffers row:3 and a delete of row:1, then fails.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k3, v);
  txn->track_row_count_delta(7, 1);
  txn->buffer_del(k1);
  txn->track_row_count_delta(7, -1);
  REQUIRE(rows.load() == 2);
  txn->rollback(false);

  SECTION("only the failed statement's work is undone") {
    REQUIRE(txn->exists(k1));
    REQUIRE(txn->exists(k2));
    REQUIRE_FALSE(txn->exists(k3));
    REQUIRE(rows.load() == 2);
  }

  SECTION("session commit persists statements 1 and 2") {
    REQUIRE(commit(*txn) == Status::Committed);
    auto probe = create_txn();
    REQUIRE(probe->exists(k1));
    REQUIRE(probe->exists(k2));
    REQUIRE_FALSE(probe->exists(k3));
    REQUIRE(rows.load() == 2);
  }

  SECTION("session rollback after a statement rollback reverts everything") {
    txn->rollback(true);
    REQUIRE(rows.load() == 0);
    auto probe = create_txn();
    REQUIRE_FALSE(probe->exists(k1));
  }
}

// =========================================================================
// MergeIterator direction (BC-250): buffered writes merged in scan order
// =========================================================================

namespace {

// A key in namespace `ns` with a one-byte suffix.
auto ns_key(std::vector<uint8_t> ns, uint8_t c) -> std::vector<uint8_t> {
  ns.push_back(c);
  return ns;
}

// The namespace's exclusive upper bound: its last byte plus one.
auto ns_upper(std::vector<uint8_t> ns) -> std::vector<uint8_t> {
  ++ns.back();
  return ns;
}

auto collect_keys(Transaction::MergeIterator& it) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  for (; it.valid(); it.next()) {
    out.push_back(it.key().back());
  }
  return out;
}

}  // namespace

TEST_CASE_METHOD(TxnFixture, "Transaction MergeIterator honours scan direction",
                 "[txn][merge][reverse]") {
  // A table's row namespace and its next neighbour's: [0x02 | table_id BE4].
  const std::vector<uint8_t> ns{0x02, 0, 0, 0, 42};
  const std::vector<uint8_t> other_ns{0x02, 0, 0, 0, 43};
  auto v = make_value("v");

  // Committed rows: 1, 3, 5, 7. Another namespace's row must never appear.
  {
    auto seed = create_txn();
    for (uint8_t c : {uint8_t{1}, uint8_t{3}, uint8_t{5}, uint8_t{7}}) {
      auto k = ns_key(ns, c);
      seed->buffer_put(k, v);
    }
    auto other = ns_key(other_ns, 0);
    seed->buffer_put(other, v);
    REQUIRE(commit(*seed) == Status::Committed);
  }

  auto txn = create_txn();
  // Buffered: insert 2 and 8, delete 5, overwrite 3.
  for (uint8_t c : {uint8_t{2}, uint8_t{8}, uint8_t{3}}) {
    auto k = ns_key(ns, c);
    txn->buffer_put(k, v);
  }
  {
    auto k = ns_key(ns, 5);
    txn->buffer_del(k);
  }

  const auto& lo = ns;
  auto hi = ns_upper(ns);

  SECTION("forward scan is ascending with buffer merged") {
    auto it = txn->iter_entries(lo, hi, ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{1, 2, 3, 7, 8});
  }

  SECTION("reverse scan is descending with buffer merged") {
    auto it = txn->riter_entries(hi, lo, ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{8, 7, 3, 2, 1});
  }

  SECTION("reverse scan from a mid-range bound") {
    auto mid = ns_key(ns, 4);
    auto it = txn->riter_entries(mid, lo, ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{3, 2, 1});
  }

  SECTION("reverse scan when every buffered key is above the bound") {
    auto txn2 = create_txn();
    auto k = ns_key(ns, 9);
    txn2->buffer_put(k, v);
    auto bound = ns_key(ns, 4);
    auto it = txn2->riter_entries(bound, lo, ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{3, 1});
  }

  SECTION("key-only scans merge the same way in both directions") {
    auto fwd = txn->iter_keys(lo, hi, ns);
    REQUIRE(collect_keys(*fwd) == std::vector<uint8_t>{1, 2, 3, 7, 8});
    auto rev = txn->riter_keys(hi, lo, ns);
    REQUIRE(collect_keys(*rev) == std::vector<uint8_t>{8, 7, 3, 2, 1});
  }

  SECTION("a forward scan's values come from the buffer or the snapshot") {
    auto v2 = make_value("buffered");
    auto k3 = ns_key(ns, 3);
    txn->buffer_put(k3, v2);
    auto it = txn->iter_entries(lo, hi, ns);
    std::vector<std::vector<uint8_t>> values;
    for (; it->valid(); it->next()) {
      values.emplace_back(it->value().begin(), it->value().end());
    }
    REQUIRE(values == std::vector<std::vector<uint8_t>>{v, v, v2, v, v});
  }

  SECTION("the other namespace's scan sees only its own key") {
    auto other_hi = ns_upper(other_ns);
    auto it = txn->iter_entries(other_ns, other_hi,
                                other_ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{0});
  }
}

// =========================================================================
// count_range: the snapshot's count adjusted by the buffer
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction count_range counts the buffer", "[txn][count]") {
  auto v = make_value("v");
  {
    auto seed = create_txn();
    for (char c : std::string{"aceg"}) {
      auto k = make_key(std::string{"k"} + c);
      seed->buffer_put(k, v);
    }
    REQUIRE(commit(*seed) == Status::Committed);
  }
  auto txn = create_txn();
  auto added = make_key("kb");
  auto deleted = make_key("kc");
  txn->buffer_put(added, v);
  txn->buffer_del(deleted);

  auto lo = make_key("k");
  auto hi = make_key("l");
  REQUIRE(txn->count_range(lo, hi, 100) == 4);
  REQUIRE(txn->count_range(lo, hi, 2) == 2);
  // lo >= hi holds no rows.
  REQUIRE(txn->count_range(hi, lo, 100) == 0);
}

// =========================================================================
// Savepoints restore the row counters, not only the op log
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction savepoint rollback restores row counts", "[txn][savepoint]") {
  auto k1 = make_key("r:1");
  auto k2 = make_key("r:2");
  auto k3 = make_key("r:3");
  auto v = make_value("x");
  auto& rows = g_rows.at(1);

  auto txn = create_txn();
  txn->buffer_put(k1, v);
  txn->track_row_count_delta(1, 1);

  const auto sp1 = txn->savepoint_set();
  txn->buffer_put(k2, v);
  txn->track_row_count_delta(1, 1);

  const auto sp2 = txn->savepoint_set();
  txn->buffer_put(k3, v);
  txn->track_row_count_delta(1, 1);
  REQUIRE(rows.load() == 3);

  SECTION("rollback to the inner savepoint") {
    txn->savepoint_rollback(sp2);
    REQUIRE(rows.load() == 2);
    REQUIRE(txn->exists(k2));
    REQUIRE_FALSE(txn->exists(k3));
  }

  SECTION("rollback to the outer savepoint, then keep working") {
    txn->savepoint_rollback(sp1);
    REQUIRE(rows.load() == 1);
    REQUIRE_FALSE(txn->exists(k2));

    txn->buffer_put(k3, v);
    txn->track_row_count_delta(1, 1);
    REQUIRE(rows.load() == 2);

    // The savepoint survives ROLLBACK TO and can be rolled back to again.
    txn->savepoint_rollback(sp1);
    REQUIRE(rows.load() == 1);
    REQUIRE_FALSE(txn->exists(k3));
  }

  SECTION("release keeps the work and the counters") {
    txn->savepoint_release(sp1);
    REQUIRE(rows.load() == 3);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE(rows.load() == 3);
  }

  SECTION("a released savepoint can no longer be rolled back to") {
    txn->savepoint_release(sp1);
    txn->savepoint_rollback(sp2);
    REQUIRE(rows.load() == 3);
  }

  SECTION("full rollback after a savepoint rollback reverts everything") {
    txn->savepoint_rollback(sp2);
    txn->rollback(true);
    REQUIRE(rows.load() == 0);
  }
}
