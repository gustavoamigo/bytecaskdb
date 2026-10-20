// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The transaction of docs/transaction_design.md against a real DB: the write
// buffer and read-your-own-writes, the OCC snapshot an autocommit read pins,
// commit outcomes (conflict, deferred-insert duplicate), statement rollback
// and savepoints with their row counters, bulk-copy mode, and the merge
// iterators in both directions. Moved from the MariaDB plugin's
// mariadb_txn_test.cpp; what needs the server (registration, HA_ERR_* codes,
// the error raised) stays there.

#include <catch2/catch_test_macros.hpp>

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

struct DBHolder {
  bytecask::DB db;
  explicit DBHolder(const std::filesystem::path& dir) : db{bytecask::DB::open(dir)} {}
};

class TxnFixture {
public:
  TxnFixture() : dir_{"txn"}, holder_{std::make_unique<DBHolder>(dir_.path())} {}

  auto create_txn() -> std::unique_ptr<Transaction> {
    return std::make_unique<Transaction>(db(), kLimits);
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
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1.data(), key1.size(), out_val) == 1);
    REQUIRE(same(out_val, val1));
  }

  SECTION("buffer_del creates tombstone") {
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    REQUIRE(txn->exists(key1.data(), key1.size()));

    txn->buffer_del(key1.data(), key1.size());
    REQUIRE_FALSE(txn->exists(key1.data(), key1.size()));

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1.data(), key1.size(), out_val) == 0);
  }

  SECTION("multiple operations in order") {
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    txn->buffer_put(key2.data(), key2.size(), val2.data(), val2.size());

    REQUIRE(txn->exists(key1.data(), key1.size()));
    REQUIRE(txn->exists(key2.data(), key2.size()));

    auto val1_new = make_value("alice_updated");
    txn->buffer_put(key1.data(), key1.size(), val1_new.data(), val1_new.size());

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key1.data(), key1.size(), out_val) == 1);
    REQUIRE(same(out_val, val1_new));
  }

  SECTION("buffered_key_present sees the buffer only") {
    REQUIRE_FALSE(txn->buffered_key_present(key1.data(), key1.size()));
    txn->buffer_put(key1.data(), key1.size(), val1.data(), val1.size());
    REQUIRE(txn->buffered_key_present(key1.data(), key1.size()));
    txn->buffer_del(key1.data(), key1.size());
    REQUIRE_FALSE(txn->buffered_key_present(key1.data(), key1.size()));
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
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }

  SECTION("exists() returns true after buffer_put") {
    // A false here is the duplicate-insert bug: a second write_row in the
    // statement would not see the first.
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));
  }

  SECTION("get() returns buffered value") {
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key.data(), key.size(), out_val) == 1);
    REQUIRE(same(out_val, val));
  }

  SECTION("exists() returns false after buffer_del") {
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));

    txn->buffer_del(key.data(), key.size());
    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
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
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    REQUIRE(txn1->exists(key.data(), key.size()));

    REQUIRE_FALSE(txn2->exists(key.data(), key.size()));

    bytecask::Bytes out_val;
    REQUIRE(txn2->get(key.data(), key.size(), out_val) == 0);
  }

  SECTION("transactions can buffer same key independently") {
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn2->buffer_put(key.data(), key.size(), val2.data(), val2.size());

    bytecask::Bytes out_val1;
    bytecask::Bytes out_val2;
    REQUIRE(txn1->get(key.data(), key.size(), out_val1) == 1);
    REQUIRE(txn2->get(key.data(), key.size(), out_val2) == 1);
    REQUIRE(same(out_val1, val1));
    REQUIRE(same(out_val2, val2));
  }

  SECTION("both buffer the same key: the second commit conflicts") {
    txn1->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn2->buffer_put(key.data(), key.size(), val2.data(), val2.size());
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
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(txn->exists(key.data(), key.size()));

    txn->rollback(true);

    REQUIRE_FALSE(txn->exists(key.data(), key.size()));
  }

  SECTION("commit makes the write visible to a new transaction") {
    txn->buffer_put(key.data(), key.size(), val.data(), val.size());
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE_FALSE(txn->is_active());
    auto probe = create_txn();
    REQUIRE(probe->exists(key.data(), key.size()));
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
    seed->buffer_put(key.data(), key.size(), v0.data(), v0.size());
    REQUIRE(commit(*seed) == Status::Committed);
  }

  // No begin_if_needed(true): this is autocommit, where the handler's first
  // call on the statement is the point read (index_read_map -> get).
  auto a = create_txn();
  auto b = create_txn();

  SECTION("get() then concurrent commit then write conflicts") {
    bytecask::Bytes out;
    REQUIRE(a->get(key.data(), key.size(), out) == 1);
    REQUIRE(a->is_active());  // snapshot taken by the read, not the write

    b->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    REQUIRE(commit(*b) == Status::Committed);

    // A computes its new value from the stale read and writes it back.
    a->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    REQUIRE(commit(*a) == Status::Conflict);
  }

  SECTION("exists() then concurrent commit then write conflicts") {
    REQUIRE(a->exists(key.data(), key.size()));
    REQUIRE(a->is_active());

    b->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    REQUIRE(commit(*b) == Status::Committed);

    a->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    REQUIRE(commit(*a) == Status::Conflict);
  }

  SECTION("read-only statement releases the snapshot at commit") {
    bytecask::Bytes out;
    REQUIRE(a->get(key.data(), key.size(), out) == 1);
    REQUIRE(commit(*a) == Status::Committed);
    REQUIRE_FALSE(a->is_active());
  }

  SECTION("a failed commit reverts the row counter") {
    std::atomic<int64_t> rows{0};
    REQUIRE(a->exists(key.data(), key.size()));
    b->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    REQUIRE(commit(*b) == Status::Committed);
    a->buffer_put(key.data(), key.size(), v1.data(), v1.size());
    a->track_row_count_delta(1, rows, 1);
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
    seed->buffer_put(pk.data(), pk.size(), val1.data(), val1.size());
    REQUIRE(commit(*seed) == Status::Committed);
  }

  auto txn = create_txn();
  txn->begin_deferred_insert();
  REQUIRE(txn->in_deferred_insert());

  SECTION("the guard's conflict is a DuplicateKey naming the key and row") {
    auto other = make_key("user:2");
    txn->buffer_put(other.data(), other.size(), val2.data(), val2.size(), /*guard_absent=*/true);
    txn->buffer_put(pk.data(), pk.size(), val2.data(), val2.size(), /*guard_absent=*/true);
    REQUIRE_FALSE(txn->is_active());  // deferred mode takes no snapshot

    auto out = txn->commit(true, true);

    REQUIRE(out.status == Status::DuplicateKey);
    REQUIRE(out.duplicate.has_value());
    REQUIRE(out.duplicate->key == pk);
    REQUIRE(out.duplicate->row == val2);
    REQUIRE_FALSE(txn->in_deferred_insert());
    auto probe = create_txn();
    REQUIRE_FALSE(probe->exists(other.data(), other.size()));  // nothing written
  }

  SECTION("an absent key commits") {
    auto other = make_key("user:3");
    txn->buffer_put(other.data(), other.size(), val2.data(), val2.size(), /*guard_absent=*/true);
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE_FALSE(txn->in_deferred_insert());
  }
}

// =========================================================================
// Edge cases
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction edge cases", "[txn][edge]") {
  auto txn = create_txn();

  SECTION("get on non-existent key returns 0") {
    auto key = make_key("nonexistent");
    bytecask::Bytes out_val;
    REQUIRE(txn->get(key.data(), key.size(), out_val) == 0);
    REQUIRE(out_val.empty());
  }

  SECTION("exists on empty key") {
    std::vector<uint8_t> empty_key;
    REQUIRE_FALSE(txn->exists(empty_key.data(), empty_key.size()));
  }

  SECTION("buffer operations with empty values") {
    auto key = make_key("empty_value_key");
    txn->buffer_put(key.data(), key.size(), nullptr, 0);
    REQUIRE(txn->exists(key.data(), key.size()));

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key.data(), key.size(), out_val) == 1);
    REQUIRE(out_val.empty());
  }

  SECTION("overwrite same key multiple times") {
    auto key = make_key("overwrite_test");
    auto val1 = make_value("first");
    auto val2 = make_value("second");
    auto val3 = make_value("third");

    txn->buffer_put(key.data(), key.size(), val1.data(), val1.size());
    txn->buffer_put(key.data(), key.size(), val2.data(), val2.size());
    txn->buffer_put(key.data(), key.size(), val3.data(), val3.size());

    bytecask::Bytes out_val;
    REQUIRE(txn->get(key.data(), key.size(), out_val) == 1);
    REQUIRE(same(out_val, val3));
  }

  SECTION("an empty commit writes nothing") {
    REQUIRE(commit(*txn) == Status::Committed);
    REQUIRE(db()->keys_from({}).begin() == std::default_sentinel);
  }
}

// =========================================================================
// Bulk-copy mode — batched writes for ALTER TABLE ... ALGORITHM=COPY
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction bulk copy mode", "[txn][bulk]") {
  auto txn = create_txn();

  auto db_has = [&](const std::vector<uint8_t>& k) {
    auto snap = db()->snapshot();
    return snap.contains_key({}, as_view(k.data(), k.size()));
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
    txn->bulk_buffer_put(k0.data(), k0.size(), v0.data(), v0.size());
    txn->bulk_note_key(k0.data(), k0.size());
    REQUIRE_FALSE(txn->bulk_should_flush());
    REQUIRE_FALSE(db_has(k0));

    // Pile on until the byte threshold trips, then flush.
    for (int i = 1; i < 8; ++i) {
      auto k = make_key("row:000" + std::to_string(i));
      txn->bulk_buffer_put(k.data(), k.size(), v0.data(), v0.size());
      txn->bulk_note_key(k.data(), k.size());
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

    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(uprefix.data(), uprefix.size()));
    txn->bulk_note_key(uprefix.data(), uprefix.size());
    // Same value again in the same unflushed batch -> caught.
    REQUIRE(txn->bulk_unique_prefix_exists(uprefix.data(), uprefix.size()));

    txn->bulk_buffer_put(pk.data(), pk.size(), nullptr, 0);
    txn->bulk_note_key(pk.data(), pk.size());
    txn->bulk_buffer_put(skey.data(), skey.size(), nullptr, 0);
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);

    // After the flush, bulk_seen_ is cleared but the DB probe still catches it.
    REQUIRE(txn->bulk_pk_exists(pk.data(), pk.size()));
    auto prefix = make_key("uk:\x01" "ali");
    REQUIRE(txn->bulk_unique_prefix_exists(prefix.data(), prefix.size()));
    auto absent = make_key("uk:\x01" "zzz");
    REQUIRE_FALSE(txn->bulk_unique_prefix_exists(absent.data(), absent.size()));
  }

  SECTION("empty flush is a no-op") {
    txn->begin_bulk_copy(256);
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);
    REQUIRE(db_key_count() == 0);
  }

  SECTION("rollback drops the pending batch and leaves bulk mode") {
    txn->begin_bulk_copy(1 << 20);
    auto k = make_key("row:pending");
    txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
    txn->rollback(true);
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
    REQUIRE(txn->bulk_flush(true).status == Status::Committed);
    REQUIRE(db_has(k2));
    REQUIRE_FALSE(db_has(k));
  }

  SECTION("commit flushes the tail of the batch") {
    txn->begin_bulk_copy(1 << 20);  // no auto-flush; all rows are "tail"
    for (int i = 0; i < 5; ++i) {
      auto k = make_key("tail:" + std::to_string(i));
      txn->bulk_buffer_put(k.data(), k.size(), nullptr, 0);
      txn->bulk_note_key(k.data(), k.size());
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
  std::atomic<int64_t> rows{0};

  auto txn = create_txn();

  // Statement 1: insert row:1.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k1.data(), k1.size(), v.data(), v.size());
  txn->track_row_count_delta(7, rows, 1);
  REQUIRE(commit(*txn, false) == Status::Committed);

  // Statement 2: insert row:2.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k2.data(), k2.size(), v.data(), v.size());
  txn->track_row_count_delta(7, rows, 1);
  REQUIRE(commit(*txn, false) == Status::Committed);

  // Statement 3: buffers row:3 and a delete of row:1, then fails.
  (void)txn->begin_if_needed(true);
  txn->buffer_put(k3.data(), k3.size(), v.data(), v.size());
  txn->track_row_count_delta(7, rows, 1);
  txn->buffer_del(k1.data(), k1.size());
  txn->track_row_count_delta(7, rows, -1);
  REQUIRE(rows.load() == 2);
  txn->rollback(false);

  SECTION("only the failed statement's work is undone") {
    REQUIRE(txn->exists(k1.data(), k1.size()));
    REQUIRE(txn->exists(k2.data(), k2.size()));
    REQUIRE_FALSE(txn->exists(k3.data(), k3.size()));
    REQUIRE(rows.load() == 2);
  }

  SECTION("session commit persists statements 1 and 2") {
    REQUIRE(commit(*txn) == Status::Committed);
    auto probe = create_txn();
    REQUIRE(probe->exists(k1.data(), k1.size()));
    REQUIRE(probe->exists(k2.data(), k2.size()));
    REQUIRE_FALSE(probe->exists(k3.data(), k3.size()));
    REQUIRE(rows.load() == 2);
  }

  SECTION("session rollback after a statement rollback reverts everything") {
    txn->rollback(true);
    REQUIRE(rows.load() == 0);
    auto probe = create_txn();
    REQUIRE_FALSE(probe->exists(k1.data(), k1.size()));
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
    out.push_back(it.key_data()[it.key_len() - 1]);
  }
  return out;
}

}  // namespace

TEST_CASE_METHOD(TxnFixture, "Transaction MergeIterator honours scan direction",
                 "[txn][merge][reverse]") {
  // A table's row namespace and its next neighbour's, as the plugin lays
  // them out: [0x02 | table_id BE4].
  const std::vector<uint8_t> ns{0x02, 0, 0, 0, 42};
  const std::vector<uint8_t> other_ns{0x02, 0, 0, 0, 43};
  auto v = make_value("v");

  // Committed rows: 1, 3, 5, 7. Another namespace's row must never appear.
  {
    auto seed = create_txn();
    for (uint8_t c : {uint8_t{1}, uint8_t{3}, uint8_t{5}, uint8_t{7}}) {
      auto k = ns_key(ns, c);
      seed->buffer_put(k.data(), k.size(), v.data(), v.size());
    }
    auto other = ns_key(other_ns, 0);
    seed->buffer_put(other.data(), other.size(), v.data(), v.size());
    REQUIRE(commit(*seed) == Status::Committed);
  }

  auto txn = create_txn();
  // Buffered: insert 2 and 8, delete 5, overwrite 3.
  for (uint8_t c : {uint8_t{2}, uint8_t{8}, uint8_t{3}}) {
    auto k = ns_key(ns, c);
    txn->buffer_put(k.data(), k.size(), v.data(), v.size());
  }
  {
    auto k = ns_key(ns, 5);
    txn->buffer_del(k.data(), k.size());
  }

  const auto& lo = ns;
  auto hi = ns_upper(ns);

  SECTION("forward scan is ascending with buffer merged") {
    auto it = txn->iter_entries(lo.data(), lo.size(), hi.data(), hi.size(), ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{1, 2, 3, 7, 8});
  }

  SECTION("reverse scan is descending with buffer merged") {
    auto it = txn->riter_entries(hi.data(), hi.size(), lo.data(), lo.size(), ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{8, 7, 3, 2, 1});
  }

  SECTION("reverse scan from a mid-range bound") {
    auto mid = ns_key(ns, 4);
    auto it = txn->riter_entries(mid.data(), mid.size(), lo.data(), lo.size(), ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{3, 2, 1});
  }

  SECTION("reverse scan when every buffered key is above the bound") {
    auto txn2 = create_txn();
    auto k = ns_key(ns, 9);
    txn2->buffer_put(k.data(), k.size(), v.data(), v.size());
    auto bound = ns_key(ns, 4);
    auto it = txn2->riter_entries(bound.data(), bound.size(), lo.data(), lo.size(), ns);
    REQUIRE(collect_keys(*it) == std::vector<uint8_t>{3, 1});
  }

  SECTION("key-only scans merge the same way in both directions") {
    auto fwd = txn->iter_keys(lo.data(), lo.size(), hi.data(), hi.size(), ns);
    REQUIRE(collect_keys(*fwd) == std::vector<uint8_t>{1, 2, 3, 7, 8});
    auto rev = txn->riter_keys(hi.data(), hi.size(), lo.data(), lo.size(), ns);
    REQUIRE(collect_keys(*rev) == std::vector<uint8_t>{8, 7, 3, 2, 1});
  }

  SECTION("a forward scan's values come from the buffer or the snapshot") {
    auto v2 = make_value("buffered");
    auto k3 = ns_key(ns, 3);
    txn->buffer_put(k3.data(), k3.size(), v2.data(), v2.size());
    auto it = txn->iter_entries(lo.data(), lo.size(), hi.data(), hi.size(), ns);
    std::vector<std::vector<uint8_t>> values;
    for (; it->valid(); it->next()) {
      values.emplace_back(it->value_data(), it->value_data() + it->value_len());
    }
    REQUIRE(values == std::vector<std::vector<uint8_t>>{v, v, v2, v, v});
  }

  SECTION("the other namespace's scan sees only its own key") {
    auto other_hi = ns_upper(other_ns);
    auto it = txn->iter_entries(other_ns.data(), other_ns.size(), other_hi.data(), other_hi.size(),
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
      seed->buffer_put(k.data(), k.size(), v.data(), v.size());
    }
    REQUIRE(commit(*seed) == Status::Committed);
  }
  auto txn = create_txn();
  auto added = make_key("kb");
  auto deleted = make_key("kc");
  txn->buffer_put(added.data(), added.size(), v.data(), v.size());
  txn->buffer_del(deleted.data(), deleted.size());

  auto lo = make_key("k");
  auto hi = make_key("l");
  REQUIRE(txn->count_range(lo.data(), lo.size(), hi.data(), hi.size(), 100) == 4);
  REQUIRE(txn->count_range(lo.data(), lo.size(), hi.data(), hi.size(), 2) == 2);
  // lo >= hi holds no rows.
  REQUIRE(txn->count_range(hi.data(), hi.size(), lo.data(), lo.size(), 100) == 0);
}

// =========================================================================
// Savepoints restore the row counters, not only the op log
// =========================================================================

TEST_CASE_METHOD(TxnFixture, "Transaction savepoint rollback restores row counts", "[txn][savepoint]") {
  auto k1 = make_key("r:1");
  auto k2 = make_key("r:2");
  auto k3 = make_key("r:3");
  auto v = make_value("x");
  std::atomic<int64_t> rows{0};

  auto txn = create_txn();
  txn->buffer_put(k1.data(), k1.size(), v.data(), v.size());
  txn->track_row_count_delta(1, rows, 1);

  const auto sp1 = txn->savepoint_set();
  txn->buffer_put(k2.data(), k2.size(), v.data(), v.size());
  txn->track_row_count_delta(1, rows, 1);

  const auto sp2 = txn->savepoint_set();
  txn->buffer_put(k3.data(), k3.size(), v.data(), v.size());
  txn->track_row_count_delta(1, rows, 1);
  REQUIRE(rows.load() == 3);

  SECTION("rollback to the inner savepoint") {
    txn->savepoint_rollback(sp2);
    REQUIRE(rows.load() == 2);
    REQUIRE(txn->exists(k2.data(), k2.size()));
    REQUIRE_FALSE(txn->exists(k3.data(), k3.size()));
  }

  SECTION("rollback to the outer savepoint, then keep working") {
    txn->savepoint_rollback(sp1);
    REQUIRE(rows.load() == 1);
    REQUIRE_FALSE(txn->exists(k2.data(), k2.size()));

    txn->buffer_put(k3.data(), k3.size(), v.data(), v.size());
    txn->track_row_count_delta(1, rows, 1);
    REQUIRE(rows.load() == 2);

    // The savepoint survives ROLLBACK TO and can be rolled back to again.
    txn->savepoint_rollback(sp1);
    REQUIRE(rows.load() == 1);
    REQUIRE_FALSE(txn->exists(k3.data(), k3.size()));
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
