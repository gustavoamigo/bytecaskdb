// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// SERIALIZABLE transactions and autocommit statements in the embedded API
// (docs/transaction_design.md, *Serializable isolation*, stage 1): every key
// and range a SERIALIZABLE statement reads is recorded by the runtime's read
// calls, and a commit that writes submits them as guards, so a commit whose
// reads changed is 40001 with nothing written, against every concurrent
// write. The interleavings are forced with two transactions on one thread,
// which never wait: a transaction's snapshot is taken at its first
// statement, and a commit is checked against everything committed since. The
// cases are those of the design's PostgreSQL 17 probes, named as it names
// them.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.api;
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;

namespace {

using Grid = std::vector<std::vector<Value>>;

constexpr std::string_view kReadWrite = "could not serialize access due to read/write dependencies among transactions";
constexpr std::string_view kUpdate = "could not serialize access due to concurrent update";

auto i(std::int64_t v) -> Value { return Value{v}; }

auto ser() -> TransactionOptions { return {.isolation = Isolation::Serializable}; }

auto ser_auto() -> ExecutionOptions { return {.isolation = Isolation::Serializable}; }

// The doctors on call of the write-skew probe (case 1): each transaction
// checks that two are on call, then takes one off.
auto open_doc(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE doc (id BIGINT PRIMARY KEY, on_call BOOLEAN NOT NULL)");
  (void)db->execute("INSERT INTO doc VALUES (1, TRUE), (2, TRUE)");
  (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
  return db;
}

auto on_call(Transaction& t) -> std::int64_t {
  return std::get<std::int64_t>(t.execute("SELECT count(*) FROM doc WHERE on_call = TRUE").rows.at(0).at(0));
}

auto on_call(Database& db) -> std::int64_t {
  return std::get<std::int64_t>(db.execute("SELECT count(*) FROM doc WHERE on_call = TRUE").rows.at(0).at(0));
}

void take_off(Transaction& t, std::int64_t id) {
  (void)t.execute("UPDATE doc SET on_call = FALSE WHERE id = $1", {i(id)});
}

// The commit is 40001 with `message`, and the read/write one has
// PostgreSQL's hint; the transaction has ended.
void commit_fails(Transaction& t, std::string_view message) {
  try {
    (void)t.commit();
    FAIL("committed");
  } catch (const SerializationFailure& e) {
    CHECK(sqlstate(e) == "40001");
    CHECK(std::string_view{e.what()} == message);
    CHECK(error_hint(e) == (message == kReadWrite ? "The transaction might succeed if retried." : ""));
  }
  CHECK_FALSE(t.is_open());
}

auto count_of(Database& db, std::string_view sql) -> std::int64_t {
  return std::get<std::int64_t>(db.execute(sql).rows.at(0).at(0));
}

}  // namespace

TEST_CASE("serializable: write skew is 40001 at the second commit with nothing written; at Snapshot both commit",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_skew"};
  auto db = open_doc(dir);
  SECTION("Serializable (case 1)") {
    auto a = db->begin(ser());
    auto b = db->begin(ser());
    CHECK(on_call(a) == 2);
    CHECK(on_call(b) == 2);
    take_off(a, 1);
    take_off(b, 2);
    CHECK(a.commit() == CommitOutcome::Committed);
    commit_fails(b, kReadWrite);
    CHECK(on_call(*db) == 1);  // b wrote nothing
  }
  SECTION("Snapshot (case 1b)") {
    auto a = db->begin();
    auto b = db->begin();
    CHECK(on_call(a) == 2);
    CHECK(on_call(b) == 2);
    take_off(a, 1);
    take_off(b, 2);
    CHECK(a.commit() == CommitOutcome::Committed);
    CHECK(b.commit() == CommitOutcome::Committed);
    CHECK(on_call(*db) == 0);
  }
  SECTION("against a Snapshot writer too (case C)") {
    auto a = db->begin();
    auto b = db->begin(ser());
    CHECK(on_call(a) == 2);
    CHECK(on_call(b) == 2);
    take_off(a, 1);
    take_off(b, 2);
    CHECK(a.commit() == CommitOutcome::Committed);
    commit_fails(b, kReadWrite);
  }
  SECTION("and against an autocommit statement") {
    auto b = db->begin(ser());
    CHECK(on_call(b) == 2);
    (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1");
    take_off(b, 2);
    commit_fails(b, kReadWrite);
  }
}

TEST_CASE("serializable: an empty range read is guarded against a phantom; disjoint ranges commit",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_phantom"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY, v BIGINT)");
  auto count = [](Transaction& t, std::int64_t lo, std::int64_t hi) {
    return std::get<std::int64_t>(t.execute("SELECT count(*) FROM r WHERE k BETWEEN $1 AND $2", {i(lo), i(hi)}).rows.at(0).at(0));
  };
  SECTION("each counts an empty range and inserts into the other's (case 6)") {
    auto a = db->begin(ser());
    auto b = db->begin(ser());
    CHECK(count(a, 10, 19) == 0);
    CHECK(count(b, 20, 29) == 0);
    (void)a.execute("INSERT INTO r VALUES (25, 0)");
    (void)b.execute("INSERT INTO r VALUES (15, 0)");
    CHECK(a.commit() == CommitOutcome::Committed);
    commit_fails(b, kReadWrite);
    CHECK(count_of(*db, "SELECT count(*) FROM r") == 1);
  }
  SECTION("each inserts into its own range (case 6b): both commit") {
    auto a = db->begin(ser());
    auto b = db->begin(ser());
    CHECK(count(a, 10, 19) == 0);
    CHECK(count(b, 20, 29) == 0);
    (void)a.execute("INSERT INTO r VALUES (11, 0)");
    (void)b.execute("INSERT INTO r VALUES (21, 0)");
    CHECK(a.commit() == CommitOutcome::Committed);
    CHECK(b.commit() == CommitOutcome::Committed);
  }
  SECTION("a full scan that found no row") {
    auto a = db->begin(ser());
    CHECK(a.execute("SELECT k FROM r WHERE v = 7").rows.empty());
    (void)db->execute("INSERT INTO r VALUES (1, 7)");
    (void)a.execute("INSERT INTO r VALUES (2, 7)");
    commit_fails(a, kReadWrite);
  }
}

TEST_CASE("serializable: reads of every shape are guarded: index equality, MIN and MAX, COUNT(*), EXISTS",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_shapes"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE m (id BIGINT PRIMARY KEY, color TEXT NOT NULL)");
  (void)db->execute("CREATE INDEX m_color ON m (color)");
  for (std::int64_t k = 1; k <= 200; ++k) {
    const auto color = k == 1 ? std::string{"black"} : k == 2 ? std::string{"white"} : "grey" + std::to_string(k);
    (void)db->execute("INSERT INTO m VALUES ($1, $2)", {i(k), Value{sql_test::bytes(color)}});
  }
  (void)db->execute("CREATE TABLE a (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE b (k BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO a VALUES (10), (20)");
  (void)db->execute("INSERT INTO b VALUES (10), (20)");
  SECTION("equality reads on an index, cross updates (case 15)") {
    auto t1 = db->begin(ser());
    auto t2 = db->begin(ser());
    (void)t1.execute("UPDATE m SET color = 'black' WHERE color = 'white'");
    (void)t2.execute("UPDATE m SET color = 'white' WHERE color = 'black'");
    CHECK(t1.commit() == CommitOutcome::Committed);
    commit_fails(t2, kReadWrite);
    CHECK(count_of(*db, "SELECT count(*) FROM m WHERE color = 'black'") == 2);
  }
  SECTION("MAX then cross inserts") {
    auto t1 = db->begin(ser());
    auto t2 = db->begin(ser());
    auto max_of = [](Transaction& t, std::string_view table) {
      return std::get<std::int64_t>(t.execute("SELECT max(k) FROM " + std::string{table}).rows.at(0).at(0));
    };
    (void)t1.execute("INSERT INTO b VALUES ($1)", {i(max_of(t1, "a") + 1)});
    (void)t2.execute("INSERT INTO a VALUES ($1)", {i(max_of(t2, "b") + 1)});
    CHECK(t1.commit() == CommitOutcome::Committed);
    commit_fails(t2, kReadWrite);
  }
  SECTION("MIN then cross inserts: one below the minimum is a conflict, one above it is not") {
    for (auto below : {true, false}) {
      INFO((below ? "below" : "above"));
      auto t1 = db->begin(ser());
      auto low = std::get<std::int64_t>(t1.execute("SELECT min(k) FROM a").rows.at(0).at(0));
      CHECK(low == 10);
      (void)db->execute("INSERT INTO a VALUES ($1)", {i(below ? 5 : 15)});
      (void)t1.execute("INSERT INTO b VALUES (99)");
      if (below) {
        commit_fails(t1, kReadWrite);
      } else {
        CHECK(t1.commit() == CommitOutcome::Committed);
      }
      (void)db->execute("DELETE FROM a WHERE k = $1", {i(below ? 5 : 15)});
      (void)db->execute("DELETE FROM b WHERE k = 99");
    }
  }
  SECTION("COUNT(*) of a point and of a range") {
    for (auto sql : {"SELECT count(*) FROM a WHERE k = 30", "SELECT count(*) FROM a WHERE k >= 25 AND k < 40"}) {
      INFO(sql);
      auto t1 = db->begin(ser());
      CHECK(std::get<std::int64_t>(t1.execute(sql).rows.at(0).at(0)) == 0);
      (void)db->execute("INSERT INTO a VALUES (30)");
      (void)t1.execute("INSERT INTO b VALUES (99)");
      commit_fails(t1, kReadWrite);
      (void)db->execute("DELETE FROM a WHERE k = 30");
    }
  }
  SECTION("EXISTS") {
    auto t1 = db->begin(ser());
    CHECK(t1.execute("SELECT k FROM b WHERE EXISTS (SELECT 1 FROM a WHERE a.k = 30)").rows.empty());
    (void)db->execute("INSERT INTO a VALUES (30)");
    (void)t1.execute("INSERT INTO b VALUES (99)");
    commit_fails(t1, kReadWrite);
  }
}

TEST_CASE("serializable: a scan that stops early guards only what it covered, forward and reverse",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_limit"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  for (std::int64_t k = 10; k <= 100; k += 10) (void)db->execute("INSERT INTO r VALUES ($1)", {i(k)});
  struct Case {
    const char* sql;
    std::int64_t inside, outside;
  };
  for (const auto& c : {Case{"SELECT k FROM r ORDER BY k LIMIT 2", 15, 55}, Case{"SELECT k FROM r ORDER BY k DESC LIMIT 2", 95, 45}}) {
    for (auto inside : {true, false}) {
      INFO(c.sql << (inside ? ": an insert before the stop" : ": an insert past the stop"));
      auto t = db->begin(ser());
      CHECK(t.execute(c.sql).rows.size() == 2);
      const auto k = inside ? c.inside : c.outside;
      (void)db->execute("INSERT INTO r VALUES ($1)", {i(k)});
      (void)t.execute("INSERT INTO w VALUES (1)");
      if (inside) {
        commit_fails(t, kReadWrite);
      } else {
        CHECK(t.commit() == CommitOutcome::Committed);
        (void)db->execute("DELETE FROM w WHERE k = 1");
      }
      (void)db->execute("DELETE FROM r WHERE k = $1", {i(k)});
    }
  }
}

TEST_CASE("serializable: an index scan that fetches rows guards each row; a covering read guards only its entries",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_fetch"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE ix (id BIGINT PRIMARY KEY, g BIGINT NOT NULL, note TEXT)");
  (void)db->execute("CREATE INDEX ix_g ON ix (g)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  for (std::int64_t k = 1; k <= 500; ++k) (void)db->execute("INSERT INTO ix VALUES ($1, $2, 'n')", {i(k), i(k % 100)});
  const auto& fetching = db->prepare("SELECT note FROM ix WHERE g = 3");
  const auto& covering = db->prepare("SELECT id FROM ix WHERE g = 3");
  REQUIRE(fetching.plan_source().find("bc_scan_row") != std::string::npos);
  REQUIRE(covering.plan_source().find("bc_scan_row") == std::string::npos);
  for (const auto* stmt : {&fetching, &covering}) {
    INFO(stmt->sql());
    auto t = db->begin(ser());
    CHECK(t.execute(*stmt, {}).rows.size() == 5);
    // A column the index does not hold: no entry of its range changes.
    (void)db->execute("UPDATE ix SET note = 'changed' WHERE id = 103");
    (void)t.execute("INSERT INTO w VALUES (1)");
    if (stmt == &fetching) {
      commit_fails(t, kReadWrite);
    } else {
      CHECK(t.commit() == CommitOutcome::Committed);
    }
  }
}

TEST_CASE("serializable: a join probe and a correlated subquery guard the key they probed; a repeated probe is one point",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_probe"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE o (id BIGINT PRIMARY KEY, ref BIGINT NOT NULL)");
  (void)db->execute("CREATE TABLE p (id BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  for (std::int64_t k = 1; k <= 1000; ++k) (void)db->execute("INSERT INTO o VALUES ($1, 5)", {i(k)});
  for (std::int64_t k = 100; k <= 3000; ++k) (void)db->execute("INSERT INTO p VALUES ($1)", {i(k)});
  for (auto sql : {"SELECT o.id FROM o JOIN p ON p.id = o.ref", "SELECT o.id FROM o WHERE EXISTS (SELECT 1 FROM p WHERE p.id = o.ref)"}) {
    INFO(sql);
    auto t = db->begin(ser());
    CHECK(t.execute(sql).rows.empty());
    const auto reads = t.read_set_for_testing();
    // A thousand probes of p's key 5 hold one point, or one range: the
    // read set grows with the distinct keys read, not with the reads.
    CHECK(reads.points.size() + reads.ranges.size() <= 3);
    (void)db->execute("INSERT INTO p VALUES (5)");
    (void)t.execute("INSERT INTO w VALUES (1)");
    commit_fails(t, kReadWrite);
    (void)db->execute("DELETE FROM p WHERE id = 5");
  }
}

TEST_CASE("serializable: a procedure's reads are its transaction's", "[sql][api][serializable][procedure]") {
  sql_test::TempDir dir{"ser_proc"};
  auto db = open_doc(dir);
  (void)db->execute(R"(CREATE PROCEDURE go_off(who BIGINT) LANGUAGE plpgsql AS $$
DECLARE n BIGINT;
BEGIN
  SELECT count(*) INTO n FROM doc WHERE on_call = TRUE;
  IF n >= 2 THEN
    UPDATE doc SET on_call = FALSE WHERE id = who;
  END IF;
END $$)");
  auto a = db->begin(ser());
  auto b = db->begin(ser());
  (void)a.call("go_off", {i(1)});
  (void)b.call("go_off", {i(2)});
  CHECK(a.commit() == CommitOutcome::Committed);
  commit_fails(b, kReadWrite);
  CHECK(on_call(*db) == 1);
  SECTION("and an autocommit CALL at Serializable is retried, then finds one on call") {
    (void)db->execute("UPDATE doc SET on_call = TRUE");
    (void)db->call("go_off", {i(1)}, ser_auto());
    CHECK(on_call(*db) == 1);
  }
}

TEST_CASE("serializable: the read-only anomaly fails the writer in both orders; readers whose reads changed commit",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_readonly"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE ctl (k BIGINT PRIMARY KEY, deposit_no BIGINT NOT NULL)");
  (void)db->execute("CREATE TABLE receipts (id BIGINT PRIMARY KEY, deposit_no BIGINT NOT NULL)");
  (void)db->execute("INSERT INTO ctl VALUES (1, 1)");
  auto batch = [](Transaction& t) {
    return std::get<std::int64_t>(t.execute("SELECT deposit_no FROM ctl WHERE k = 1").rows.at(0).at(0));
  };
  for (auto t3_reads_first : {true, false}) {
    INFO((t3_reads_first ? "T3 reads the receipts before T2 commits" : "T2 commits before T3 reads (probe s1)"));
    (void)db->execute("UPDATE ctl SET deposit_no = 1");
    (void)db->execute("DELETE FROM receipts");
    auto t2 = db->begin(ser());
    CHECK(batch(t2) == 1);
    (void)db->execute("UPDATE ctl SET deposit_no = deposit_no + 1 WHERE k = 1");  // T1
    (void)t2.execute("INSERT INTO receipts VALUES (1, 1)");
    auto t3 = db->begin(ser());
    if (!t3_reads_first) commit_fails(t2, kReadWrite);
    CHECK(batch(t3) == 2);
    CHECK(t3.execute("SELECT id FROM receipts WHERE deposit_no = 1").rows.empty());
    CHECK(t3.commit() == CommitOutcome::Committed);
    if (t3_reads_first) commit_fails(t2, kReadWrite);
    CHECK(count_of(*db, "SELECT count(*) FROM receipts") == 0);
  }
  SECTION("readers whose reads changed commit (cases 3 and 14)") {
    for (auto opts : {ser(), TransactionOptions{.isolation = Isolation::Serializable, .read_only = true}}) {
      auto t = db->begin(opts);
      CHECK(batch(t) > 0);
      (void)db->execute("UPDATE ctl SET deposit_no = deposit_no + 1 WHERE k = 1");
      (void)db->execute("INSERT INTO receipts VALUES (7, 7)");
      CHECK(t.execute("SELECT id FROM receipts").rows.empty());  // its snapshot's
      CHECK(t.commit() == CommitOutcome::Committed);
      (void)db->execute("DELETE FROM receipts");
    }
  }
}

TEST_CASE("serializable: one changed read fails a writer: a single anti-dependency, PostgreSQL commits it (cases A, B)",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_single"};
  auto db = open_doc(dir);
  auto t = db->begin(ser());
  CHECK(t.execute("SELECT on_call FROM doc WHERE id = 1").rows == Grid{{Value{true}}});
  (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1");
  (void)t.execute("INSERT INTO other VALUES (1)");
  commit_fails(t, kReadWrite);
}

TEST_CASE("serializable: ROLLBACK TO keeps the reads made since the savepoint, and so does a failed statement",
          "[sql][api][serializable][savepoint]") {
  sql_test::TempDir dir{"ser_savepoint"};
  auto db = open_doc(dir);
  SECTION("a read inside a savepoint, ROLLBACK TO, then skew (cases 5, D)") {
    auto a = db->begin(ser());
    a.savepoint("s");
    CHECK(on_call(a) == 2);
    const auto charged = a.read_set_for_testing().charged;
    a.rollback_to("s");
    CHECK(a.read_set_for_testing().charged == charged);
    take_off(a, 1);
    (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 2");
    commit_fails(a, kReadWrite);
  }
  SECTION("a statement that failed after it read") {
    auto a = db->begin(ser());
    a.savepoint("s");
    CHECK_THROWS_AS(a.execute("SELECT id / (id - id) FROM doc WHERE id = 1"), ExecError);
    CHECK(a.is_aborted());
    a.rollback_to("s");
    take_off(a, 2);
    (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1");
    commit_fails(a, kReadWrite);
  }
}

TEST_CASE("serializable: a read set past the transaction's bound is 54000 at the read, never at COMMIT",
          "[sql][api][serializable][memory]") {
  sql_test::TempDir dir{"ser_bound"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  auto t = db->begin({.max_bytes = 64u << 10, .isolation = Isolation::Serializable});
  // SAVEPOINT, read, ROLLBACK TO: a rollback frees writes, never reads.
  std::uint64_t last = 0;
  auto failed_at = 0;
  for (std::int64_t k = 1; k < 100000; ++k) {
    t.savepoint("s");
    try {
      (void)t.execute("SELECT k FROM r WHERE k = $1", {i(k)});
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "54000");
      CHECK(std::string_view{e.what()}.find("SERIALIZABLE") != std::string_view::npos);
      failed_at = static_cast<int>(k);
    }
    t.rollback_to("s");
    t.release("s");
    const auto now = t.read_set_for_testing().charged;
    CHECK(now >= last);
    last = now;
    if (failed_at != 0) break;
  }
  REQUIRE(failed_at > 100);
  // The reads stay: a commit with them, and no write, is a reader's.
  CHECK(t.commit() == CommitOutcome::Committed);
  SECTION("a writer with reads near its bound commits: the commit was charged with each read") {
    auto u = db->begin({.max_bytes = 64u << 10, .isolation = Isolation::Serializable});
    for (std::int64_t k = 1; k < failed_at / 2; ++k) (void)u.execute("SELECT k FROM r WHERE k = $1", {i(k)});
    (void)u.execute("INSERT INTO w VALUES (1)");
    CHECK(u.commit() == CommitOutcome::Committed);
  }
}

TEST_CASE("serializable: ON CONFLICT DO NOTHING's skip is a read of the arbiter key", "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_onconflict"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE u (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO u VALUES (1, 1)");
  for (auto level : {Isolation::Serializable, Isolation::Snapshot}) {
    INFO((level == Isolation::Serializable ? "Serializable" : "Snapshot"));
    auto t = db->begin({.isolation = level});
    CHECK(t.execute("INSERT INTO u VALUES (1, 0) ON CONFLICT DO NOTHING").affected == 0);
    (void)db->execute("DELETE FROM u WHERE k = 1");
    (void)t.execute("INSERT INTO w VALUES (1)");
    if (level == Isolation::Serializable) {
      commit_fails(t, kReadWrite);
    } else {
      CHECK(t.commit() == CommitOutcome::Committed);  // the owner's 2026-11-19 rule below SERIALIZABLE
    }
    (void)db->execute("INSERT INTO u VALUES (1, 1)");
  }
}

TEST_CASE("serializable: a unique value a transaction freed again is a read: 40001 when it writes anything else",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_unique"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE uq (id BIGINT PRIMARY KEY, email TEXT)");
  (void)db->execute("CREATE UNIQUE INDEX uq_email ON uq (email)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  struct Case {
    Isolation level;
    bool other_write;
    bool fails;
  };
  for (auto c : {Case{Isolation::Serializable, true, true}, Case{Isolation::Snapshot, true, false},
                 Case{Isolation::Serializable, false, false}}) {
    INFO((c.level == Isolation::Serializable ? "Serializable" : "Snapshot") << (c.other_write ? ", another write" : ", no other write"));
    (void)db->execute("DELETE FROM uq");
    (void)db->execute("DELETE FROM w");
    auto t = db->begin({.isolation = c.level});
    (void)t.execute("INSERT INTO uq VALUES (10, 'a@x')");
    (void)t.execute("DELETE FROM uq WHERE id = 10");
    if (c.other_write) (void)t.execute("INSERT INTO w VALUES (1)");
    (void)db->execute("INSERT INTO uq VALUES (20, 'a@x')");
    if (c.fails) {
      commit_fails(t, kReadWrite);
    } else {
      CHECK(t.commit() == CommitOutcome::Committed);
    }
  }
  SECTION("an insert deleted again and nothing else writes nothing and submits no read") {
    auto t = db->begin(ser());
    (void)t.execute("INSERT INTO w VALUES (5)");
    (void)t.execute("DELETE FROM w WHERE k = 5");
    (void)db->execute("INSERT INTO w VALUES (5)");
    CHECK(t.commit() == CommitOutcome::Committed);
  }
}

TEST_CASE("serializable: ON CONFLICT DO UPDATE's probe is a read of the arbiter key; a range DELETE's rows are read",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_upsert"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE uq (id BIGINT PRIMARY KEY, email TEXT, n BIGINT)");
  (void)db->execute("CREATE UNIQUE INDEX uq_email ON uq (email)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  auto fresh = [&] {
    (void)db->execute("DELETE FROM uq");
    (void)db->execute("DELETE FROM w");
  };
  SECTION("the unique arbiter found free, its row deleted again: a concurrent insert of the value is 40001") {
    // The arbiter's index range is the only read of the value: the row the
    // upsert inserted is gone, and the concurrent row has another key.
    for (auto level : {Isolation::Serializable, Isolation::Snapshot}) {
      INFO((level == Isolation::Serializable ? "Serializable" : "Snapshot"));
      fresh();
      auto t = db->begin({.isolation = level});
      (void)t.execute("INSERT INTO uq VALUES (10, 'a@x', 0) ON CONFLICT (email) DO UPDATE SET n = uq.n + 1");
      (void)t.execute("DELETE FROM uq WHERE id = 10");
      (void)t.execute("INSERT INTO w VALUES (1)");
      (void)db->execute("INSERT INTO uq VALUES (20, 'a@x', 0)");
      if (level == Isolation::Serializable) {
        commit_fails(t, kReadWrite);
      } else {
        CHECK(t.commit() == CommitOutcome::Committed);
      }
    }
  }
  SECTION("the primary-key arbiter: the key probed is the key written, checked as a write") {
    fresh();
    (void)db->execute("INSERT INTO uq VALUES (1, 'b@x', 0)");
    auto t = db->begin(ser());
    (void)t.execute("INSERT INTO uq VALUES (1, 'b@x', 0) ON CONFLICT (id) DO UPDATE SET n = uq.n + 1");
    (void)db->execute("UPDATE uq SET n = 5 WHERE id = 1");
    commit_fails(t, kUpdate);
  }
  SECTION("a range DELETE raced by an insert into its range") {
    fresh();
    for (std::int64_t k : {1, 2, 4, 5}) (void)db->execute("INSERT INTO uq VALUES ($1, NULL, 0)", {i(k)});
    auto t = db->begin(ser());
    CHECK(t.execute("DELETE FROM uq WHERE id BETWEEN 2 AND 4").affected == 2);
    (void)db->execute("INSERT INTO uq VALUES (3, NULL, 0)");
    try {
      (void)t.commit();
      FAIL("committed");
    } catch (const SerializationFailure& e) {
      CHECK(sqlstate(e) == "40001");
    }
  }
}

TEST_CASE("serializable: any DDL committed during a writer fails it; a reader commits", "[sql][api][serializable][ddl]") {
  sql_test::TempDir dir{"ser_ddl"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE a (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("CREATE TABLE c (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO a VALUES (1, 1)");
  (void)db->execute("CREATE VIEW va AS SELECT k FROM a");
  (void)db->execute(R"(CREATE PROCEDURE pa(INOUT n BIGINT) LANGUAGE plpgsql AS $$
BEGIN
  SELECT count(*) INTO n FROM a;
END $$)");
  struct Case {
    const char* what;
    std::function<void(Transaction&)> read;
    std::function<void()> ddl;
  };
  auto n = 0;
  const std::vector<Case> cases{
      {"ADD COLUMN on a table only read", [](Transaction& t) { (void)t.execute("SELECT v FROM a"); },
       [&] { (void)db->execute("ALTER TABLE a ADD COLUMN x" + std::to_string(n++) + " BIGINT"); }},
      {"a view read, dropped and created again", [](Transaction& t) { (void)t.execute("SELECT k FROM va"); },
       [&] {
         (void)db->execute("DROP VIEW va");
         (void)db->execute("CREATE VIEW va AS SELECT k FROM a WHERE k > 0");
       }},
      {"a procedure called, replaced", [](Transaction& t) { (void)t.call("pa", {Value{}}); },
       [&] {
         (void)db->execute(R"(CREATE OR REPLACE PROCEDURE pa(INOUT n BIGINT) LANGUAGE plpgsql AS $$
BEGIN
  SELECT count(*) INTO n FROM a WHERE k > 0;
END $$)");
       }},
      {"a table a WHERE false plan named, dropped", [](Transaction& t) { (void)t.execute("SELECT k FROM c WHERE false"); },
       [&] { (void)db->execute("DROP TABLE c"); }},
  };
  for (const auto& c : cases) {
    for (auto writer : {true, false}) {
      INFO(c.what << (writer ? ", then a write elsewhere" : ", read-only"));
      (void)db->execute("CREATE TABLE IF NOT EXISTS c (k BIGINT PRIMARY KEY)");
      (void)db->execute("DELETE FROM w");
      auto t = db->begin(ser());
      c.read(t);
      c.ddl();
      if (writer) {
        (void)t.execute("INSERT INTO w VALUES (1)");
        commit_fails(t, kReadWrite);
      } else {
        CHECK(t.commit() == CommitOutcome::Committed);
      }
    }
  }
  SECTION("a transaction with DDL and write skew fails through its DDL commit") {
    sql_test::TempDir skew_dir{"ser_ddl_skew"};
    auto doc = open_doc(skew_dir);
    auto a = doc->begin(ser());
    CHECK(on_call(a) == 2);
    (void)a.execute("CREATE TABLE made (k BIGINT PRIMARY KEY)");
    take_off(a, 1);
    (void)doc->execute("UPDATE doc SET on_call = FALSE WHERE id = 2");
    commit_fails(a, kReadWrite);
    CHECK(on_call(*doc) == 1);
  }
}

TEST_CASE("serializable: CREATE TABLE AS whose source range changed is 40001 in a transaction, retried in autocommit (case 10)",
          "[sql][api][serializable][ddl]") {
  sql_test::TempDir dir{"ser_ctas"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO r VALUES (1), (2)");
  SECTION("in a transaction") {
    auto t = db->begin(ser());
    CHECK(count_of(*db, "SELECT count(*) FROM r") == 2);
    CHECK(std::get<std::int64_t>(t.execute("SELECT count(*) FROM r").rows.at(0).at(0)) == 2);
    (void)db->execute("INSERT INTO r VALUES (3)");
    (void)t.execute("CREATE TABLE copy AS SELECT k FROM r");
    commit_fails(t, kReadWrite);
    CHECK_THROWS(db->execute("SELECT k FROM copy"));
  }
  SECTION("in autocommit: retried on a fresh snapshot, and the copy has the row") {
    auto raced = false;
    db->after_txn_pin_for_testing([&] {
      if (std::exchange(raced, true)) return;
      (void)db->execute("INSERT INTO r VALUES (3)");
    });
    auto out = db->execute("CREATE TABLE copy AS SELECT k FROM r", {}, ser_auto());
    db->after_txn_pin_for_testing({});
    CHECK(raced);
    CHECK(out.affected == 3);
    CHECK(count_of(*db, "SELECT count(*) FROM copy") == 3);
  }
}

TEST_CASE("serializable: an autocommit statement that always conflicts is 40001 after its last retry",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_auto"};
  auto db = open_doc(dir);
  db->conflict_on_every_commit_for_testing(true);
  try {
    (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1", {}, ser_auto());
    FAIL("committed");
  } catch (const ExecError& e) {
    CHECK(sqlstate(e) == "40001");
  }
  db->conflict_on_every_commit_for_testing(false);
  CHECK(on_call(*db) == 2);
  // A statement that writes no table records nothing and commits nothing.
  CHECK(db->execute("SELECT id FROM doc", {}, ser_auto()).rows.size() == 2);
}

TEST_CASE("serializable: a statement that writes nothing records nothing, and a read-only transaction refuses writes "
          "with 25006",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_ro"};
  // A statement budget smaller than the read set its join would record.
  auto db = Database::open(dir.path(), {.statement_memory_bytes = 256u << 10});
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY)");
  for (std::int64_t k = 1; k <= 3000; ++k) (void)db->execute("INSERT INTO r VALUES ($1, $1)", {i(k)});
  // More distinct point reads than the statement budget holds as a read set.
  const auto* many = "SELECT r.k FROM r JOIN r AS s ON s.k = r.v";
  REQUIRE(3000 * read_point_bytes(20) > (256u << 10));
  SECTION("an autocommit SELECT at Serializable") {
    CHECK(db->execute(many, {}, ser_auto()).rows.size() == 3000);
  }
  SECTION("a declared read-only transaction") {
    auto t = db->begin({.max_bytes = 256u << 10, .isolation = Isolation::Serializable, .read_only = true});
    CHECK(t.execute(many).rows.size() == 3000);
    CHECK(t.read_set_for_testing().points.empty());
    CHECK(t.read_set_for_testing().ranges.empty());
    for (auto sql : {"INSERT INTO w VALUES (1)", "UPDATE r SET v = 0 WHERE k = 1", "DELETE FROM r WHERE k = 1",
                     "SELECT k FROM r WHERE k = 1 FOR UPDATE", "CREATE TABLE z (k BIGINT PRIMARY KEY)"}) {
      INFO(sql);
      auto u = db->begin({.read_only = true});
      try {
        (void)u.execute(sql);
        FAIL("ran");
      } catch (const std::exception& e) {
        CHECK(sqlstate(e) == "25006");
        const std::string expected = std::string{"cannot execute "} +
                                     (std::string_view{sql}.starts_with("INSERT")   ? "INSERT"
                                      : std::string_view{sql}.starts_with("UPDATE") ? "UPDATE"
                                      : std::string_view{sql}.starts_with("DELETE") ? "DELETE"
                                      : std::string_view{sql}.starts_with("SELECT") ? "SELECT FOR UPDATE"
                                                                                    : "CREATE TABLE") +
                                     " in a read-only transaction";
        CHECK(e.what() == expected);
      }
      CHECK(u.is_aborted());
    }
    // A FOR UPDATE cursor is refused as it opens, over an empty table too,
    // as PostgreSQL refuses it at DECLARE: never at a FETCH, never not at all.
    for (auto sql : {"SELECT k FROM w FOR UPDATE", "SELECT k FROM r FOR UPDATE"}) {
      INFO(sql);
      auto u = db->begin({.read_only = true});
      try {
        (void)u.open_cursor(sql, ResultCheck::none(), {});
        FAIL("opened");
      } catch (const std::exception& e) {
        CHECK(sqlstate(e) == "25006");
        CHECK(std::string_view{e.what()} == "cannot execute SELECT FOR UPDATE in a read-only transaction");
      }
      CHECK(u.is_aborted());
    }
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  SECTION("the same transaction not read-only is 54000 at a read") {
    auto t = db->begin({.max_bytes = 256u << 10, .isolation = Isolation::Serializable});
    try {
      (void)t.execute(many);
      FAIL("ran");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "54000");
    }
  }
}

TEST_CASE("serializable: the level is set before the transaction's first statement, with PostgreSQL's 25001 after",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_set"};
  auto db = open_doc(dir);
  auto t = db->begin();
  t.set_isolation(Isolation::Serializable);
  t.set_read_only(true);
  t.set_read_only(false);
  CHECK(t.isolation() == Isolation::Serializable);
  CHECK(on_call(t) == 2);
  t.set_isolation(Isolation::Serializable);  // unchanged: taken
  auto refused = [&](auto&& call, std::string_view message) {
    try {
      call();
      FAIL("taken");
    } catch (const ast::SqlError& e) {
      CHECK(e.sqlstate() == "25001");
      CHECK(std::string_view{e.what()} == message);
    }
  };
  refused([&] { t.set_isolation(Isolation::Snapshot); }, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
  refused([&] { t.set_read_only(true); }, "transaction read-only mode must be set before any query");
  auto u = db->begin();
  u.savepoint("s");
  refused([&] { u.set_isolation(Isolation::Serializable); },
          "SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction");
  // PostgreSQL's orders: for the level a query first, then a subtransaction;
  // for read-write a subtransaction first, then a query.
  auto q = db->begin();
  CHECK(on_call(q) == 2);
  q.savepoint("s");
  refused([&] { q.set_isolation(Isolation::Serializable); },
          "SET TRANSACTION ISOLATION LEVEL must be called before any query");
  auto ro = db->begin({.read_only = true});
  ro.savepoint("s");
  refused([&] { ro.set_read_only(false); }, "cannot set transaction read-write mode inside a read-only transaction");
  auto ro_q = db->begin({.read_only = true});
  CHECK(on_call(ro_q) == 2);
  refused([&] { ro_q.set_read_only(false); }, "transaction read-write mode must be set before any query");
  ro_q.savepoint("s");
  refused([&] { ro_q.set_read_only(false); }, "cannot set transaction read-write mode inside a read-only transaction");
  // The level set took effect: the skew is caught.
  take_off(t, 1);
  (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 2");
  commit_fails(t, kReadWrite);
}

TEST_CASE("serializable: refused on a follower with 0A000, at begin, at the first statement, and in autocommit",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_follower"};
  auto db = open_doc(dir);
  auto refused = [](auto&& call) {
    try {
      call();
      FAIL("taken");
    } catch (const ast::SqlError& e) {
      CHECK(e.sqlstate() == "0A000");
      CHECK(std::string_view{e.what()} == "cannot use serializable mode in a hot standby");
      CHECK(e.hint() == "You can use REPEATABLE READ instead.");
    }
  };
  auto before = db->begin(ser());  // begun on the leader
  db->engine_for_testing().set_mode(bytecask::Mode::Follower);
  refused([&] { (void)db->begin(ser()); });
  refused([&] { (void)db->execute("SELECT id FROM doc", {}, ser_auto()); });
  refused([&] { (void)on_call(before); });
  auto fresh = db->begin();
  refused([&] { fresh.set_isolation(Isolation::Serializable); });
  // Snapshot isolation reads there as before.
  auto t = db->begin();
  CHECK(on_call(t) == 2);
  // After a query the change is 25001 first, as PostgreSQL checks it.
  try {
    t.set_isolation(Isolation::Serializable);
    FAIL("taken");
  } catch (const ast::SqlError& e) {
    CHECK(e.sqlstate() == "25001");
    CHECK(std::string_view{e.what()} == "SET TRANSACTION ISOLATION LEVEL must be called before any query");
  }
  CHECK(t.commit() == CommitOutcome::Committed);
}

TEST_CASE("serializable: WITH HOLD is refused in a Serializable writer, taken in a read-only one",
          "[sql][api][serializable][cursor]") {
  sql_test::TempDir dir{"ser_hold"};
  auto db = open_doc(dir);
  auto t = db->begin(ser());
  try {
    (void)t.open_cursor("SELECT id FROM doc", ResultCheck::none(), {}, Holdability::Hold);
    FAIL("opened");
  } catch (const std::exception& e) {
    CHECK(sqlstate(e) == "0A000");
  }
  auto r = db->begin({.isolation = Isolation::Serializable, .read_only = true});
  auto c = r.open_cursor("SELECT id FROM doc", ResultCheck::none(), {}, Holdability::Hold);
  CHECK(r.commit() == CommitOutcome::Committed);
  CHECK(c.fetch(10).rows.size() == 2);
}

TEST_CASE("serializable: a cursor's FETCH records its reads once, however often it replays", "[sql][api][serializable][cursor]") {
  sql_test::TempDir dir{"ser_fetch_replay"};
  auto db = open_doc(dir);
  auto t = db->begin(ser());
  auto c = t.open_cursor("SELECT id FROM doc WHERE id = 1", ResultCheck::none(), {});
  CHECK(c.fetch(1).rows.size() == 1);
  const auto after_one = t.read_set_for_testing();
  for (int k = 0; k < 20; ++k) {
    (void)c.fetch(CursorMove{CursorMove::Kind::Absolute, 1});
  }
  CHECK(t.read_set_for_testing().points == after_one.points);
  CHECK(t.read_set_for_testing().ranges == after_one.ranges);
  CHECK(t.read_set_for_testing().charged == after_one.charged);
  // The reads take part in the commit.
  (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1");
  (void)t.execute("INSERT INTO other VALUES (1)");
  c.close();
  commit_fails(t, kReadWrite);
}

TEST_CASE("serializable: a written key a concurrent commit changed is the concurrent-update 40001; a taken insert is 23505",
          "[sql][api][serializable]") {
  sql_test::TempDir dir{"ser_messages"};
  auto db = open_doc(dir);
  SECTION("a written key changed: PostgreSQL's case 11, no hint") {
    auto t = db->begin(ser());
    CHECK(on_call(t) == 2);
    (void)db->execute("UPDATE doc SET on_call = FALSE WHERE id = 1");
    take_off(t, 1);
    commit_fails(t, kUpdate);
  }
  SECTION("a key read absent, then inserted, raced by a commit that took it: 23505") {
    auto t = db->begin(ser());
    CHECK(t.execute("SELECT k FROM other WHERE k = 5").rows.empty());
    (void)t.execute("INSERT INTO other VALUES (5)");
    (void)db->execute("INSERT INTO other VALUES (5)");
    try {
      (void)t.commit();
      FAIL("committed");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "23505");
    }
  }
}

// ------------------------------------------------- the runtime's read set
//
// The fixture's table t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT),
// table 1, with the index t_by_name (name), 3, read and written by
// hand-written plans.

namespace {

using sql_test::Fixture;
using sql_test::SchemaRef;

constexpr std::uint32_t kT = 1;

// SELECT id FROM t WHERE id = $1
void get_t(bc_ctx* cx) {
  bool found = false;
  (void)bc_get(cx, bc_key_params(cx, kT, 0, 1), &found);
  bc_emit_count(cx, found ? 1 : 0);
}

// INSERT INTO t (id) VALUES ($1)
void insert_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kT);
  bc_row_set_param(cx, r, 0, 0);
  bc_insert(cx, kT, bc_row_end(cx, r));
}

// UPDATE t SET qty = qty + 1 WHERE id = $1
void bump_t(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kT, 0, 1), &found);
  if (!found) return;
  bc_row r = bc_row_from(cx, kT, old);
  bc_row_set_i64(cx, r, 2, bc_col_null(cx, old, 2) ? 1 : bc_col_i64(cx, old, 2) + 1);
  bc_update(cx, kT, old, bc_row_end(cx, r));
}

// Reads id $1 under a hand-written ensure_present, then inserts id $2.
void present_then_insert(bc_ctx* cx) {
  bc_view key = bc_key_params(cx, kT, 0, 1);
  bc_guard_present(cx, key);
  bool found = false;
  (void)bc_get(cx, key, &found);
  bc_row r = bc_row_begin(cx, kT);
  bc_row_set_param(cx, r, 0, 1);
  bc_insert(cx, kT, bc_row_end(cx, r));
}

// Every id in [$1, $2), forward, or in reverse when $3 is 1, stopping after
// $4 rows (0: all).
void scan_t(bc_ctx* cx) {
  auto flags = bc_param_i64(cx, 2) != 0 ? BC_SCAN_REVERSE : BC_SCAN_FORWARD;
  bc_cursor c = bc_scan_open(cx, kT, bc_key_params(cx, kT, 0, 1), bc_key_params(cx, kT, 1, 1),
                             static_cast<std::uint32_t>(flags));
  const auto stop = bc_param_i64(cx, 3);
  std::int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    if (++n == stop) break;
  }
  bc_scan_close(cx, c);
  bc_emit_count(cx, n);
}

// SELECT count(*) FROM t WHERE id >= $1 AND id < $2
void count_t(bc_ctx* cx) {
  bc_emit_count(cx, bc_count_range(cx, kT, bc_key_params(cx, kT, 0, 1), bc_key_params(cx, kT, 1, 1), 1000));
}

// INSERT INTO t (id) VALUES ($1), ($2) ON CONFLICT DO NOTHING
void insert_two_on_conflict(bc_ctx* cx) {
  for (std::uint32_t p = 0; p < 2; ++p) {
    bc_row r = bc_row_begin(cx, kT);
    bc_row_set_param(cx, r, 0, p);
    (void)bc_insert_on_conflict(cx, kT, bc_row_end(cx, r), BC_ARBITER_ANY);
  }
}

// Whether id $1 exists, as a count
void exists_t(bc_ctx* cx) { bc_emit_count(cx, bc_exists(cx, bc_key_params(cx, kT, 0, 1)) ? 1 : 0); }

// Every row of t through t_by_name: each entry's row fetched by bc_scan_row.
void rows_by_name_t(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, 3, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  std::int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    (void)bc_scan_row(cx, c);
    ++n;
  }
  bc_scan_close(cx, c);
  bc_emit_count(cx, n);
}

// DELETE FROM t WHERE id >= $1 AND id < $2, as one range delete
void del_range_t(bc_ctx* cx) { bc_del_range(cx, kT, bc_key_params(cx, kT, 0, 1), bc_key_params(cx, kT, 1, 1)); }

// INSERT INTO t (id, qty) VALUES ($1, $2)
void insert_qty_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kT);
  bc_row_set_param(cx, r, 0, 0);
  bc_row_set_param(cx, r, 2, 1);
  bc_insert(cx, kT, bc_row_end(cx, r));
}

// INSERT INTO t (id, qty) VALUES ($1, $2) ON CONFLICT DO NOTHING, under
// arbiter $3
void insert_qty_on_conflict_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kT);
  bc_row_set_param(cx, r, 0, 0);
  bc_row_set_param(cx, r, 2, 1);
  (void)bc_insert_on_conflict(cx, kT, bc_row_end(cx, r), static_cast<std::uint32_t>(bc_param_i64(cx, 2)));
}

// INSERT INTO t (id, qty) VALUES ($1, $2) ON CONFLICT DO UPDATE SET
// name = NULL, under arbiter $3
void upsert_qty_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kT);
  bc_row_set_param(cx, r, 0, 0);
  bc_row_set_param(cx, r, 2, 1);
  bc_view old = bc_empty(cx);
  if (bc_insert_or_existing(cx, kT, bc_row_end(cx, r), static_cast<std::uint32_t>(bc_param_i64(cx, 2)), &old)) return;
  if (bc_error(cx) != BC_OK) return;
  bc_row u = bc_row_from(cx, kT, old);
  bc_row_set_null(cx, u, 1);
  bc_update(cx, kT, old, bc_row_end(cx, u));
}

struct Txn {
  bc_txn* p;
  Txn(Fixture& f, bc_isolation level, std::uint64_t max_bytes = std::uint64_t{64} << 20)
      : p{bc_txn_new(&f.bdb, &f.bcat, max_bytes, level, false)} {}
  ~Txn() { bc_txn_free(p); }
  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;
};

auto ref_of(Fixture& f) -> SchemaRef {
  return {sql_test::all_objects(f.cat()), sql_test::all_tables(f.cat()), sql_test::all_cols(f.cat()), {}, f.cat().version()};
}

// One statement of `txn` on `cx`, its parameters and reference built:
// allocates nothing of its own. Its error, BC_OK when it joined.
auto run_stmt(bc_ctx* cx, bc_txn* txn, bc_plan_fn plan, const bc_params& ps, const bc_schema_ref& ref) -> int {
  bc_ctx_begin_txn(cx, txn, &ps, {});
  bc_ctx_run(cx, plan, &ref);
  (void)bc_txn_add(cx);
  auto error = static_cast<int>(bc_error(cx));
  bc_ctx_reset(cx);
  return error;
}

auto stmt(Fixture& f, bc_ctx* cx, bc_txn* txn, bc_plan_fn plan, std::vector<bc_value> p) -> int {
  auto ps = sql_test::params(std::move(p));
  auto sr = ref_of(f);
  return run_stmt(cx, txn, plan, ps, sr.get());
}

// One autocommit statement on `cx` at `level`, retried `retries` times.
auto autocommit(Fixture& f, bc_ctx* cx, bc_plan_fn plan, std::vector<bc_value> p, bc_isolation level = BC_ISOLATION_SNAPSHOT,
                std::uint32_t retries = 0) -> int {
  auto ps = sql_test::params(std::move(p));
  auto sr = ref_of(f);
  auto ref = sr.get();
  bc_ctx_set_isolation(cx, level);
  return bc_execute(cx, plan, &ref, &ps, {}, false, retries);
}

auto key_of(std::int64_t id) -> Bytes {
  Bytes k;
  bytecask::sql::key_put_prefix(k, bytecask::sql::kNsRow, kT);
  (void)bytecask::sql::key_put_i64(k, id, bytecask::sql::KeyPart{PhysType::I64, false, false, 8});
  return k;
}

}  // namespace

TEST_CASE("serializable: the read set keeps what a naive list of keys and ranges covers, merged", "[sql][serializable][model]") {
  std::mt19937 rng{20261120};
  for (int round = 0; round < 300; ++round) {
    std::vector<ReadSetOp> ops;
    const auto n = std::uniform_int_distribution<int>{1, 30}(rng);
    for (int k = 0; k < n; ++k) {
      auto a = std::uniform_int_distribution<int>{0, 40}(rng);
      if (std::uniform_int_distribution<int>{0, 1}(rng) == 0) {
        ops.push_back({Bytes{static_cast<std::byte>(a)}, {}});
      } else {
        auto b = a + std::uniform_int_distribution<int>{1, 8}(rng);
        ops.push_back({Bytes{static_cast<std::byte>(a)}, Bytes{static_cast<std::byte>(b)}});
      }
    }
    // The naive model: the one-byte keys covered, and the points kept.
    std::set<int> covered, points;
    for (const auto& op : ops) {
      const auto a = std::to_integer<int>(op.lo.at(0));
      if (op.hi.empty()) {
        points.insert(a);
      } else {
        for (int k = a; k < std::to_integer<int>(op.hi.at(0)); ++k) covered.insert(k);
      }
    }
    auto got = read_set_apply_for_testing(ops);
    INFO("round " << round);
    std::set<int> got_covered, got_points;
    std::uint64_t bytes = 0;
    for (std::size_t r = 0; r < got.ranges.size(); ++r) {
      const auto& [lo, hi] = got.ranges[r];
      REQUIRE(std::ranges::lexicographical_compare(lo, hi));
      // Disjoint and not touching: a merge covers exactly the union.
      if (r > 0) CHECK(std::ranges::lexicographical_compare(got.ranges[r - 1].second, lo));
      for (int k = std::to_integer<int>(lo.at(0)); k < std::to_integer<int>(hi.at(0)); ++k) got_covered.insert(k);
      bytes += read_range_bytes(lo.size(), hi.size());
    }
    for (const auto& p : got.points) {
      got_points.insert(std::to_integer<int>(p.at(0)));
      bytes += read_point_bytes(p.size());
    }
    CHECK(got_covered == covered);
    std::set<int> expected_points;
    for (auto p : points) {
      if (!covered.contains(p)) expected_points.insert(p);
    }
    CHECK(got_points == expected_points);  // a point inside a range is not kept
    CHECK(got.bytes == bytes);
  }
}

TEST_CASE("serializable: k⁺ is the key and a zero byte, or past the key ceiling the successor of its prefix",
          "[sql][serializable]") {
  CHECK(read_successor_for_testing(Bytes{std::byte{1}, std::byte{0xFF}}) == Bytes{std::byte{1}, std::byte{0xFF}, std::byte{0}});
  Bytes at_ceiling(kMaxKeyBytes, std::byte{7});
  at_ceiling.back() = std::byte{0xFF};
  at_ceiling[at_ceiling.size() - 2] = std::byte{0xFF};
  auto next = read_successor_for_testing(at_ceiling);
  CHECK(next.size() == kMaxKeyBytes - 2);
  CHECK(next.back() == std::byte{8});
  CHECK(std::ranges::lexicographical_compare(at_ceiling, next));
}

TEST_CASE("serializable: a hand-written bc_guard_present on a read key commits", "[sql][serializable]") {
  Fixture f;
  REQUIRE(autocommit(f, f.cx, insert_t, {std::int64_t{1}}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  SECTION("in a transaction") {
    Txn t{f, BC_ISOLATION_SERIALIZABLE};
    REQUIRE(stmt(f, f.cx, t.p, present_then_insert, {std::int64_t{1}, std::int64_t{2}}) == BC_OK);
    CHECK(bc_txn_commit(t.p, false) == BC_COMMITTED);
  }
  SECTION("in autocommit") {
    CHECK(autocommit(f, f.cx, present_then_insert, {std::int64_t{1}, std::int64_t{3}}, BC_ISOLATION_SERIALIZABLE) ==
          BC_COMMITTED);
  }
}

TEST_CASE("serializable: bc_txn_repin refuses a transaction whose read set holds a read", "[sql][serializable]") {
  Fixture f;
  Txn t{f, BC_ISOLATION_SERIALIZABLE};
  CHECK(bc_txn_repin(t.p));  // nothing read
  // A first statement that failed after reading: no statement added, but a read made.
  CHECK(bc_txn_savepoint(t.p) == 1);
  auto ps = sql_test::params({std::int64_t{1}});
  auto sr = ref_of(f);
  auto ref = sr.get();
  bc_ctx_begin_txn(f.cx, t.p, &ps, {});
  bc_ctx_run(f.cx, get_t, &ref);
  REQUIRE(bc_error(f.cx) == BC_OK);
  bc_ctx_reset(f.cx);  // not added: the transaction aborts back to its mark
  CHECK(bc_txn_rollback_to(t.p, 1));
  CHECK_FALSE(bc_txn_reads_for_testing(t.p).points.empty());
  CHECK_FALSE(bc_txn_repin(t.p));
  CHECK_FALSE(bc_txn_set_mode(t.p, BC_ISOLATION_SNAPSHOT, false));
}

TEST_CASE("serializable: an autocommit retry submits only its own attempt's reads", "[sql][serializable]") {
  Fixture f;
  for (std::int64_t id : {1, 2, 10}) REQUIRE(autocommit(f, f.cx, insert_t, {id}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  auto* other = bc_ctx_new(&f.bdb, &f.bcat);
  SECTION("a conflict once: the retry commits, and holds its own read only") {
    // Attempt one reads id 1, then a concurrent commit changes it, and the
    // hook points the retry's read at id 2.
    auto read_then_write = [](bc_ctx* cx) {
      bool found = false;
      (void)bc_get(cx, bc_key_params(cx, kT, 0, 1), &found);
      bc_row r = bc_row_begin(cx, kT);
      bc_row_set_param(cx, r, 0, 1);
      bc_insert(cx, kT, bc_row_end(cx, r));
    };
    auto ps = sql_test::params({std::int64_t{1}, std::int64_t{50}});
    auto sr = ref_of(f);
    auto ref = sr.get();
    auto attempt = 0;
    bc_ctx_set_isolation(f.cx, BC_ISOLATION_SERIALIZABLE);
    bc_ctx_before_commit_for_testing(f.cx, [&] {
      if (attempt++ != 0) return;
      REQUIRE(autocommit(f, other, bump_t, {std::int64_t{1}}) == BC_COMMITTED);
      bc_ctx_reset(other);
      ps.values[0] = std::int64_t{2};
    });
    CHECK(bc_execute(f.cx, +read_then_write, &ref, &ps, {}, false, 3) == BC_COMMITTED);
    CHECK(attempt == 2);
    const auto reads = bc_ctx_reads_for_testing(f.cx);
    CHECK(std::ranges::find(reads.points, key_of(2)) != reads.points.end());
    CHECK(std::ranges::find(reads.points, key_of(1)) == reads.points.end());
    bc_ctx_before_commit_for_testing(f.cx, {});
    bc_ctx_reset(f.cx);
  }
  SECTION("ON CONFLICT DO NOTHING: a concurrently deleted arbiter row is inserted by the retry") {
    // id 10 is skipped, id 11 inserted; before the first commit id 10 goes.
    auto deleted = false;
    bc_ctx_before_commit_for_testing(f.cx, [&] {
      if (std::exchange(deleted, true)) return;
      auto del = [](bc_ctx* cx) {
        bool found = false;
        bc_view old = bc_get(cx, bc_key_params(cx, kT, 0, 1), &found);
        if (found) bc_delete(cx, kT, old);
      };
      REQUIRE(autocommit(f, other, +del, {std::int64_t{10}}) == BC_COMMITTED);
      bc_ctx_reset(other);
    });
    CHECK(autocommit(f, f.cx, insert_two_on_conflict, {std::int64_t{10}, std::int64_t{11}}, BC_ISOLATION_SERIALIZABLE, 3) ==
          BC_COMMITTED);
    bc_ctx_before_commit_for_testing(f.cx, {});
    bc_ctx_reset(f.cx);
    CHECK(autocommit(f, f.cx, get_t, {std::int64_t{10}}) == BC_COMMITTED);
    CHECK(bc_ctx_affected(f.cx) == 1);
    bc_ctx_reset(f.cx);
  }
  bc_ctx_free(other);
}

TEST_CASE("serializable: an allocation failure at any read leaves a read set that still guards what was read",
          "[sql][serializable][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("operator new is the sanitizer's in this build");
  // Under a savepoint: point reads, a range that swallows them, a count that
  // merges with it, and a scan that stops early. Each allocation of the
  // whole is failed in turn; ROLLBACK TO then reopens the transaction, a
  // concurrent commit changes id 1, which the first statement read, and the
  // transaction writes: once that statement completed, it must never
  // commit.
  std::set<ReadSetTransition> reached;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 100000);
    INFO("allocation " << nth);
    Fixture f;
    for (std::int64_t id = 1; id <= 6; ++id) REQUIRE(autocommit(f, f.cx, insert_t, {id}) == BC_COMMITTED);
    bc_ctx_reset(f.cx);
    Txn t{f, BC_ISOLATION_SERIALIZABLE};
    REQUIRE(bc_txn_savepoint(t.p) == 1);
    struct Step {
      bc_plan_fn plan;
      std::vector<bc_value> params;
    };
    const std::vector<Step> steps{
        {get_t, {std::int64_t{1}}},
        {get_t, {std::int64_t{2}}},
        {scan_t, {std::int64_t{1}, std::int64_t{3}, std::int64_t{0}, std::int64_t{0}}},
        {count_t, {std::int64_t{3}, std::int64_t{4}}},
        {scan_t, {std::int64_t{4}, std::int64_t{100}, std::int64_t{0}, std::int64_t{1}}},
        {scan_t, {std::int64_t{0}, std::int64_t{100}, std::int64_t{1}, std::int64_t{2}}},
    };
    std::vector<bc_params> ps;
    for (const auto& s : steps) ps.push_back(sql_test::params(s.params));
    const auto sr = ref_of(f);
    const auto ref = sr.get();
    auto completed = 0;
    bool fired = false;
    (void)read_set_transitions_for_testing(true);
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      for (std::size_t k = 0; k < steps.size(); ++k) {
        auto error = run_stmt(f.cx, t.p, steps[k].plan, ps[k], ref);
        if (error != BC_OK) {
          CHECK((error == BC_E_MEMORY || error == BC_E_LIMIT));
          break;
        }
        ++completed;
      }
      fired = faults.report().fired;
    }
    const auto seen = read_set_transitions_for_testing(true);
    if (fired) {
      for (std::size_t k = 0; k < kReadSetTransitions; ++k) {
        if (seen.at(k) != 0) reached.insert(static_cast<ReadSetTransition>(k));
      }
    }
    // What the read set is charged is what it holds: no reservation leaked.
    const auto reads = bc_txn_reads_for_testing(t.p);
    CHECK(reads.charged == reads.bytes);
    REQUIRE(bc_txn_rollback_to(t.p, 1));
    auto* other = bc_ctx_new(&f.bdb, &f.bcat);
    REQUIRE(autocommit(f, other, bump_t, {std::int64_t{1}}) == BC_COMMITTED);
    bc_ctx_free(other);
    REQUIRE(stmt(f, f.cx, t.p, insert_t, {std::int64_t{100}}) == BC_OK);
    const auto r = bc_txn_commit(t.p, false);
    if (completed > 0) CHECK(r == BC_CONFLICT);
    if (!fired) {
      CHECK(completed == static_cast<int>(steps.size()));
      break;
    }
  }
  CHECK(reached.size() == kReadSetTransitions);
}

TEST_CASE("serializable: 54000 at a recording site fails the read before it is made, and the reads before it stay",
          "[sql][serializable][memory]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 50; ++id) REQUIRE(autocommit(f, f.cx, insert_t, {id}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  // A bound that takes a few points: the transaction's own holdings are small.
  Txn t{f, BC_ISOLATION_SERIALIZABLE, 4 * read_point_bytes(key_of(1).size())};
  REQUIRE(bc_txn_savepoint(t.p) == 1);
  std::int64_t id = 1;
  int error = BC_OK;
  for (; id <= 50 && error == BC_OK; ++id) error = stmt(f, f.cx, t.p, get_t, {id});
  CHECK(error == BC_E_LIMIT);
  const auto reads = bc_txn_reads_for_testing(t.p);
  CHECK(reads.points.size() == static_cast<std::size_t>(id - 2));
  REQUIRE(bc_txn_rollback_to(t.p, 1));
  CHECK(bc_txn_reads_for_testing(t.p).points == reads.points);
  // A scan's reservation does not fit either: refused at its open.
  CHECK(stmt(f, f.cx, t.p, scan_t, {std::int64_t{1}, std::int64_t{100}, std::int64_t{0}, std::int64_t{0}}) == BC_E_LIMIT);
  REQUIRE(bc_txn_rollback_to(t.p, 1));
  auto* other = bc_ctx_new(&f.bdb, &f.bcat);
  REQUIRE(autocommit(f, other, bump_t, {std::int64_t{1}}) == BC_COMMITTED);
  bc_ctx_free(other);
  CHECK(bc_txn_commit(t.p, false) == BC_COMMITTED);  // a reader: it wrote nothing
}

TEST_CASE("serializable: 54000 at every recording site: each bound below a statement's need fails it at a read or "
          "write, with no read past the bound",
          "[sql][serializable][memory]") {
  Fixture f;
  f.ddl(bytecask::sql::CreateIndexDdl{"t_by_qty", "t", {{"qty"}}, true});
  const auto by_qty = f.cat().index("t_by_qty").second->id;
  for (std::int64_t id = 1; id <= 6; ++id) {
    REQUIRE(autocommit(f, f.cx, insert_qty_t, {id, id * 10}) == BC_COMMITTED);
  }
  bc_ctx_reset(f.cx);
  struct Site {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
  };
  const std::vector<Site> sites{
      {"bc_get", get_t, {std::int64_t{3}}},
      {"bc_exists", exists_t, {std::int64_t{3}}},
      {"bc_scan_open and bc_scan_row", rows_by_name_t, {}},
      {"bc_count_range", count_t, {std::int64_t{2}, std::int64_t{5}}},
      {"bc_del_range", del_range_t, {std::int64_t{2}, std::int64_t{5}}},
      {"the primary-key check of an insert", insert_qty_t, {std::int64_t{40}, std::int64_t{400}}},
      {"the unique check of an insert", insert_qty_t, {std::int64_t{41}, std::int64_t{35}}},
      {"ON CONFLICT DO NOTHING, the primary key", insert_qty_on_conflict_t,
       {std::int64_t{3}, std::int64_t{401}, std::int64_t{kT}}},
      {"ON CONFLICT DO NOTHING, the unique index", insert_qty_on_conflict_t,
       {std::int64_t{42}, std::int64_t{30}, std::int64_t{by_qty}}},
      {"ON CONFLICT DO UPDATE, the primary key", upsert_qty_t, {std::int64_t{3}, std::int64_t{30}, std::int64_t{kT}}},
      {"ON CONFLICT DO UPDATE, the unique index", upsert_qty_t,
       {std::int64_t{43}, std::int64_t{30}, std::int64_t{by_qty}}},
  };
  for (const auto& site : sites) {
    INFO(site.name);
    auto ps = sql_test::params(site.params);
    const auto sr = ref_of(f);
    const auto ref = sr.get();
    // What the statement records with room to spare.
    ReadSetContents full;
    {
      Txn t{f, BC_ISOLATION_SERIALIZABLE};
      REQUIRE(run_stmt(f.cx, t.p, site.plan, ps, ref) == BC_OK);
      full = bc_txn_reads_for_testing(t.p);
      REQUIRE_FALSE((full.points.empty() && full.ranges.empty()));
    }
    // Every bound from nothing up to that need, in steps finer than any
    // charge: each is 54000 at the read or write that passes it.
    auto limited = 0;
    for (std::uint64_t bound = 0;; bound += 16) {
      INFO("bound " << bound);
      Txn t{f, BC_ISOLATION_SERIALIZABLE, bound};
      REQUIRE(bc_txn_savepoint(t.p) == 1);
      const auto error = run_stmt(f.cx, t.p, site.plan, ps, ref);
      if (error == BC_OK) break;
      CHECK(error == BC_E_LIMIT);
      ++limited;
      REQUIRE(bc_txn_rollback_to(t.p, 1));
      const auto reads = bc_txn_reads_for_testing(t.p);
      CHECK(reads.charged == reads.bytes);
      CHECK(reads.charged <= bound);
      // No read past the bound: what is kept is what the statement reads.
      for (const auto& k : reads.points) {
        const auto covered = std::ranges::find(full.points, k) != full.points.end() ||
                             std::ranges::any_of(full.ranges, [&](const auto& r) {
                               return !(k < r.first) && k < r.second;  // std::byte: memcmp order
                             });
        CHECK(covered);
      }
      REQUIRE(bound < (std::uint64_t{1} << 20));
    }
    CHECK(limited > 0);
  }
}

TEST_CASE("serializable: bc_del_range's range is checked: an insert into it is a conflict and one outside it commits",
          "[sql][serializable]") {
  Fixture f;
  for (std::int64_t id : {1, 2, 4, 5}) REQUIRE(autocommit(f, f.cx, insert_t, {id}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  for (auto inside : {true, false}) {
    INFO((inside ? "an insert inside the deleted range" : "an insert outside it"));
    Txn t{f, BC_ISOLATION_SERIALIZABLE};
    // t has an index, so the delete reads the range's rows to drop their entries.
    REQUIRE(stmt(f, f.cx, t.p, del_range_t, {std::int64_t{2}, std::int64_t{5}}) == BC_OK);
    auto* other = bc_ctx_new(&f.bdb, &f.bcat);
    REQUIRE(autocommit(f, other, insert_t, {std::int64_t{inside ? 3 : 30}}) == BC_COMMITTED);
    bc_ctx_free(other);
    CHECK(bc_txn_commit(t.p, false) == (inside ? BC_CONFLICT : BC_COMMITTED));
  }
}

TEST_CASE("serializable: bc_count_range guards its whole range, an empty one included", "[sql][serializable]") {
  Fixture f;
  for (std::int64_t id : {1, 30}) REQUIRE(autocommit(f, f.cx, insert_t, {id}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  for (auto inside : {true, false}) {
    INFO((inside ? "an insert inside the counted range" : "an insert outside it"));
    Txn t{f, BC_ISOLATION_SERIALIZABLE};
    REQUIRE(stmt(f, f.cx, t.p, count_t, {std::int64_t{10}, std::int64_t{20}}) == BC_OK);
    auto* other = bc_ctx_new(&f.bdb, &f.bcat);
    REQUIRE(autocommit(f, other, insert_t, {std::int64_t{inside ? 15 : 25}}) == BC_COMMITTED);
    bc_ctx_free(other);
    REQUIRE(stmt(f, f.cx, t.p, insert_t, {std::int64_t{inside ? 100 : 101}}) == BC_OK);
    CHECK(bc_txn_commit(t.p, false) == (inside ? BC_CONFLICT : BC_COMMITTED));
  }
}
