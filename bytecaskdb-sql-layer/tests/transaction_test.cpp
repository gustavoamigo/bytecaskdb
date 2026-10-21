// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Transactions in the runtime (docs/transaction_design.md): statements read
// the snapshot pinned at the first statement through the overlay of their
// transaction's earlier statements, never their own writes; every read path
// goes through that view, forward and reverse, tables and indexes, range
// deletes included; a statement's writes join the transaction only if it
// succeeds, and a failure aborts it; the second-write rule is per
// statement; the commit is one batch, and a conflict writes nothing; the
// buffer has a bound; a read that fails is an error, never absent; and an
// allocation that fails anywhere is BC_E_MEMORY. The embedded API's handle
// is tested in api_test.cpp.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::PhysType;

namespace {

// w (id BIGINT PRIMARY KEY, a BIGINT, b BYTEA, c BIGINT NOT NULL) with
//   w_a  (a)      nullable
//   w_b  UNIQUE (b) nullable
// Ids follow t's (1-3): w is 4, its layout 5, the indexes 6 and 7.
constexpr std::uint32_t kW = 4, kWA = 6, kWB = 7;
constexpr std::uint32_t kId = 0, kA = 1, kB = 2, kC = 3;

void create_w(Fixture& f) {
  f.ddl(bytecask::sql::CreateTableDdl{"w",
                                      {Fixture::col("id", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                                       Fixture::col("b", PhysType::Bytes, true), Fixture::col("c", PhysType::I64, false)},
                                      {"id"}});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_a", "w", {{"a"}}});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_b", "w", {{"b"}}, true});
  REQUIRE(f.cat().table("w")->id == kW);
  REQUIRE(f.cat().index("w_b").second->id == kWB);
}

// INSERT INTO w VALUES ($1, $2, $3, $4)
void insert_w(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kW);
  bc_row_set_param(cx, r, kId, 0);
  bc_row_set_param(cx, r, kA, 1);
  bc_row_set_param(cx, r, kB, 2);
  bc_row_set_param(cx, r, kC, 3);
  bc_insert(cx, kW, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}

// UPDATE w SET a = $2, b = $3, c = $4 WHERE id = $1
void update_w(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r, kA, 1);
  bc_row_set_param(cx, r, kB, 2);
  bc_row_set_param(cx, r, kC, 3);
  bc_update(cx, kW, old, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}

// DELETE FROM w WHERE id = $1
void delete_w(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_delete(cx, kW, old);
  bc_emit_count(cx, 1);
}

// DELETE FROM w WHERE id >= $1 AND id < $2, as one range delete
void del_range_w(bc_ctx* cx) {
  bc_del_range(cx, kW, bc_key_params(cx, kW, 0, 1), bc_key_params(cx, kW, 1, 1));
}

// SELECT id, c FROM w WHERE id = $1
void get_w(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  if (!found) return;
  bc_emit_begin(cx);
  bc_emit_col(cx, row, kId);
  bc_emit_col(cx, row, kC);
  bc_emit_end(cx);
}

// Whether id $1 exists, as a count
void exists_w(bc_ctx* cx) { bc_emit_count(cx, bc_exists(cx, bc_key_params(cx, kW, 0, 1)) ? 1 : 0); }

void emit_ids(bc_ctx* cx, std::uint32_t object, std::uint32_t flags) {
  bc_cursor c = bc_scan_open(cx, object, bc_empty(cx), bc_empty(cx), flags);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, kId);
    bc_emit_col(cx, row, kC);
    bc_emit_end(cx);
  }
}
void scan_w(bc_ctx* cx) { emit_ids(cx, kW, BC_SCAN_FORWARD); }
void rscan_w(bc_ctx* cx) { emit_ids(cx, kW, BC_SCAN_REVERSE); }
void scan_wa(bc_ctx* cx) { emit_ids(cx, kWA, BC_SCAN_FORWARD); }
void rscan_wa(bc_ctx* cx) { emit_ids(cx, kWA, BC_SCAN_REVERSE); }

// The ids in [$1, $2), counted no further than $3
void count_w(bc_ctx* cx) {
  bc_emit_count(cx, bc_count_range(cx, kW, bc_key_params(cx, kW, 0, 1), bc_key_params(cx, kW, 1, 1),
                                   bc_param_i64(cx, 2)));
}

// INSERT, then read the row back in the same statement: never found.
void insert_then_read(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kW);
  bc_row_set_param(cx, r, kId, 0);
  bc_row_set_param(cx, r, kC, 3);
  bc_insert(cx, kW, bc_row_end(cx, r));
  bc_emit_count(cx, bc_exists(cx, bc_key_params(cx, kW, 0, 1)) ? 1 : 0);
}

// SELECT id, c FROM w WHERE id >= $1 AND id < $2, forward or, with $3 = 1,
// in reverse
void scan_w_range(bc_ctx* cx) {
  auto flags = bc_param_i64(cx, 2) != 0 ? BC_SCAN_REVERSE : BC_SCAN_FORWARD;
  bc_cursor c = bc_scan_open(cx, kW, bc_key_params(cx, kW, 0, 1), bc_key_params(cx, kW, 1, 1),
                             static_cast<std::uint32_t>(flags));
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_emit_begin(cx);
    bc_emit_col(cx, row, kId);
    bc_emit_col(cx, row, kC);
    bc_emit_end(cx);
  }
}

auto id_c() -> std::vector<PhysType> { return {PhysType::I64, PhysType::I64}; }

struct Ran {
  int error;
  std::int64_t affected;
  std::vector<std::vector<bc_value>> rows;
};

// One statement of `txn`: begun, run and ended on the fixture's context.
auto stmt(Fixture& f, bc_txn* txn, bc_plan_fn plan, std::vector<bc_value> p,
          const std::vector<PhysType>& result = {}, bc_budget budget = {}) -> Ran {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(result), f.cat().version()};
  auto ref = sr.get();
  bc_ctx_begin_txn(f.cx, txn, &ps, budget);
  bc_ctx_run(f.cx, plan, &ref);
  auto added = bc_txn_add(f.cx);
  Ran out{static_cast<int>(bc_error(f.cx)), bc_ctx_affected(f.cx), {}};
  if (added) out.rows = f.results(result);
  CHECK(added == (out.error == BC_OK));
  bc_ctx_reset(f.cx);
  return out;
}

// One autocommit statement.
auto autocommit(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p) -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  auto r = bc_execute(f.cx, plan, &ref, &ps, {}, false, 0);
  bc_ctx_reset(f.cx);
  return r;
}

auto commit(Fixture& f, bc_txn* txn) -> int {
  auto r = bc_txn_commit(f.cx, txn, false);
  bc_ctx_reset(f.cx);
  return r;
}

struct TxnPtr {
  bc_txn* p;
  explicit TxnPtr(Fixture& f, std::uint64_t max_bytes = std::uint64_t{64} << 20)
      : p{bc_txn_new(&f.bdb, &f.bcat, max_bytes)} {}
  ~TxnPtr() { bc_txn_free(p); }
  TxnPtr(const TxnPtr&) = delete;
  TxnPtr& operator=(const TxnPtr&) = delete;
};

// The model: id -> (a, b, c), NULL as nullopt.
struct Row {
  std::optional<std::int64_t> a;
  std::optional<std::string> b;
  std::int64_t c;
};
using Model = std::map<std::int64_t, Row>;

auto opt_i(std::optional<std::int64_t> v) -> bc_value { return v ? bc_value{*v} : bc_value{std::monostate{}}; }
auto opt_b(const std::optional<std::string>& v) -> bc_value { return v ? bc_value{bytes(*v)} : bc_value{std::monostate{}}; }
auto unique_on(std::uint32_t object) -> std::uint32_t { return (std::uint32_t{BC_C_UNIQUE} << 24) | object; }

// The unique key a row would take that another (not `self`) holds, or 0.
auto violation(const Model& m, std::int64_t id, const Row& r, std::optional<std::int64_t> self) -> std::uint32_t {
  if (id != self && m.contains(id)) return unique_on(kW);
  for (const auto& [oid, o] : m) {
    if (oid != self && r.b && o.b == r.b) return unique_on(kWB);
  }
  return 0;
}

auto id_c_rows(const Model& m) -> std::vector<std::vector<bc_value>> {
  std::vector<std::vector<bc_value>> out;
  for (const auto& [id, r] : m) out.push_back({id, r.c});
  return out;
}

// Every index of w holds exactly the entries its rows derive, read straight
// from the engine.
void check_indexes_exact(Fixture& f) {
  const auto& t = *f.cat().table(kW);
  std::map<std::uint32_t, std::set<Bytes>> expected;
  Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, bytecask::sql::kNsRow, kW);
  for (const auto& e : f.db->iter_from({}, prefix)) {
    if (!std::ranges::equal(BytesView{e.key}.first(std::min(e.key.size(), prefix.size())), prefix)) break;
    for (const auto& ix : t.indexes) {
      Bytes key;
      REQUIRE(bytecask::sql::encode_index_key_from_row(t, ix, e.value, key));
      expected[ix.id].insert(key);
    }
  }
  for (const auto& ix : t.indexes) {
    Bytes ip;
    bytecask::sql::key_put_prefix(ip, bytecask::sql::kNsIndex, ix.id);
    std::set<Bytes> actual;
    for (const auto& k : f.db->keys_from({}, ip)) {
      if (!std::ranges::equal(BytesView{k}.first(std::min(k.size(), ip.size())), ip)) break;
      actual.insert(Bytes(k.begin(), k.end()));
    }
    INFO("index " << ix.name);
    CHECK(actual == expected[ix.id]);
  }
}

// The rows of w, read straight from the engine.
auto stored(Fixture& f) -> Model {
  Model out;
  Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, bytecask::sql::kNsRow, kW);
  const auto& t = *f.cat().table(kW);
  for (const auto& e : f.db->iter_from({}, prefix)) {
    if (!std::ranges::equal(BytesView{e.key}.first(std::min(e.key.size(), prefix.size())), prefix)) break;
    // The layout's slots in column order, as Database::table_rows reads them.
    auto layout = t.layout;
    layout.columns.clear();
    for (const auto& col : t.columns) layout.columns.push_back(*t.layout.find(col.id));
    auto v = bytecask::sql::decode_row(layout, e.value);
    REQUIRE(v);
    Row r;
    auto id = std::get<std::int64_t>((*v)[0]);
    if (const auto* a = std::get_if<std::int64_t>(&(*v)[1])) r.a = *a;
    if (const auto* b = std::get_if<Bytes>(&(*v)[2])) {
      r.b = std::string{};
      for (auto x : *b) r.b->push_back(static_cast<char>(std::to_integer<int>(x)));
    }
    r.c = std::get<std::int64_t>((*v)[3]);
    out[id] = r;
  }
  return out;
}

}  // namespace

TEST_CASE("transactions: statements read their transaction's earlier writes through every read path, and every index "
          "stays exact",
          "[sql][txn][model]") {
  Fixture f;
  create_w(f);
  Model base;
  std::mt19937_64 rng{20261020};
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };
  auto random_row = [&] {
    Row r;
    if (pick(4) != 0) r.a = pick(5);
    if (pick(3) != 0) r.b = std::string(1, static_cast<char>('a' + pick(14)));
    r.c = pick(100);
    return r;
  };
  // Some rows committed before any transaction, so reads merge both sides.
  for (std::int64_t id = 0; id < 12; id += 2) {
    auto r = random_row();
    if (violation(base, id, r, std::nullopt) != 0) continue;
    REQUIRE(autocommit(f, insert_w, {id, opt_i(r.a), opt_b(r.b), r.c}) == BC_COMMITTED);
    base[id] = r;
  }
  auto committed = 0, rolled_back = 0, aborted = 0, ranges = 0;
  for (int t = 0; t < 120; ++t) {
    INFO("transaction " << t);
    TxnPtr txn{f};
    auto view = base;  // what the transaction's next statement must see
    auto ok = true;
    auto statements = 1 + pick(12);
    for (int s = 0; s < statements && ok; ++s) {
      INFO("statement " << s);
      auto id = std::int64_t{pick(24)};
      auto r = random_row();
      switch (pick(10)) {
        case 0:
        case 1: {
          auto v = violation(view, id, r, std::nullopt);
          auto got = stmt(f, txn.p, insert_w, {id, opt_i(r.a), opt_b(r.b), r.c});
          if (v == 0) {
            REQUIRE(got.error == BC_OK);
            view[id] = r;
          } else {
            REQUIRE(got.error == BC_E_CONSTRAINT);
            ok = false;
          }
          break;
        }
        case 2: {
          if (auto it = view.find(id); it != view.end() && pick(2) == 0) r.b = it->second.b;
          auto got = stmt(f, txn.p, update_w, {id, opt_i(r.a), opt_b(r.b), r.c});
          if (!view.contains(id)) {
            REQUIRE(got.error == BC_OK);
            CHECK(got.affected == 0);
          } else if (violation(view, id, r, id) == 0) {
            REQUIRE(got.error == BC_OK);
            view[id] = r;
          } else {
            REQUIRE(got.error == BC_E_CONSTRAINT);
            ok = false;
          }
          break;
        }
        case 3: {
          auto got = stmt(f, txn.p, delete_w, {id});
          REQUIRE(got.error == BC_OK);
          CHECK(got.affected == (view.erase(id) == 1 ? 1 : 0));
          break;
        }
        case 4: {
          auto hi = id + 1 + pick(6);
          REQUIRE(stmt(f, txn.p, del_range_w, {id, hi}).error == BC_OK);
          view.erase(view.lower_bound(id), view.lower_bound(hi));
          ++ranges;
          break;
        }
        case 5: {
          auto got = stmt(f, txn.p, get_w, {id}, id_c());
          REQUIRE(got.error == BC_OK);
          std::vector<std::vector<bc_value>> want;
          if (auto it = view.find(id); it != view.end()) want.push_back({id, it->second.c});
          CHECK(got.rows == want);
          CHECK(stmt(f, txn.p, exists_w, {id}).affected == (view.contains(id) ? 1 : 0));
          break;
        }
        case 6: {
          auto fwd = stmt(f, txn.p, scan_w, {}, id_c());
          auto rev = stmt(f, txn.p, rscan_w, {}, id_c());
          REQUIRE(fwd.error == BC_OK);
          REQUIRE(rev.error == BC_OK);
          auto want = id_c_rows(view);
          CHECK(fwd.rows == want);
          std::ranges::reverse(want);
          CHECK(rev.rows == want);
          break;
        }
        case 7: {
          // The index's order is its own; both directions hold every row,
          // one the other's reverse.
          auto fwd = stmt(f, txn.p, scan_wa, {}, id_c());
          auto rev = stmt(f, txn.p, rscan_wa, {}, id_c());
          REQUIRE(fwd.error == BC_OK);
          REQUIRE(rev.error == BC_OK);
          auto sorted = fwd.rows;
          std::ranges::sort(sorted, [](const auto& x, const auto& y) {
            return std::get<std::int64_t>(x[0]) < std::get<std::int64_t>(y[0]);
          });
          CHECK(sorted == id_c_rows(view));
          std::ranges::reverse(rev.rows);
          CHECK(rev.rows == fwd.rows);
          break;
        }
        default: {
          auto hi = id + pick(12);
          auto limit = std::int64_t{pick(8)};
          auto got = stmt(f, txn.p, count_w, {id, hi, limit});
          REQUIRE(got.error == BC_OK);
          auto n = std::ranges::count_if(view, [&](const auto& e) { return e.first >= id && e.first < hi; });
          CHECK(got.affected == std::min<std::int64_t>(n, limit));
          break;
        }
      }
    }
    if (!ok) {
      // A failed statement aborted it: only freed, and nothing was written.
      CHECK(commit(f, txn.p) == BC_FAILED);
      ++aborted;
    } else if (pick(4) == 0) {
      ++rolled_back;  // freed uncommitted: nothing written
    } else {
      REQUIRE(commit(f, txn.p) == BC_COMMITTED);
      base = view;
      ++committed;
    }
    REQUIRE(stored(f).size() == base.size());
    if (t % 10 == 0) check_indexes_exact(f);
  }
  auto got = stored(f);
  CHECK(got.size() == base.size());
  for (const auto& [id, r] : base) {
    INFO("id " << id);
    REQUIRE(got.contains(id));
    CHECK(got[id].a == r.a);
    CHECK(got[id].b == r.b);
    CHECK(got[id].c == r.c);
  }
  check_indexes_exact(f);
  CHECK(committed > 40);
  CHECK(rolled_back > 5);
  CHECK(aborted > 5);
  CHECK(ranges > 20);
}

TEST_CASE("transactions: a read stays in its bounds and passes a deleted range in one seek, every read charged",
          "[sql][txn]") {
  Fixture f;
  create_w(f);
  constexpr std::int64_t kRows = 400;
  for (std::int64_t id = 0; id < kRows; ++id) {
    REQUIRE(autocommit(f, insert_w, {id, bc_value{}, bc_value{}, id}) == BC_COMMITTED);
  }
  TxnPtr txn{f};
  // Rows 10..389 deleted as one range, and 390..399 one by one: tombstones
  // past the end of a scan of [0, 10).
  REQUIRE(stmt(f, txn.p, del_range_w, {std::int64_t{10}, std::int64_t{390}}).error == BC_OK);
  for (std::int64_t id = 390; id < kRows; ++id) REQUIRE(stmt(f, txn.p, delete_w, {id}).error == BC_OK);
  auto ids = [](const Ran& r) {
    std::vector<std::int64_t> out;
    for (const auto& row : r.rows) out.push_back(std::get<std::int64_t>(row[0]));
    return out;
  };
  // A budget far below the 390 entries the range and the tombstones hold:
  // the scans read their ten rows, a seek past the range and the record
  // that ends them, and none of the tombstones beyond.
  bc_budget tight{40, 0};
  for (std::int64_t reverse : {0, 1}) {
    auto in_range = stmt(f, txn.p, scan_w_range, {std::int64_t{0}, std::int64_t{10}, reverse}, id_c(), tight);
    REQUIRE(in_range.error == BC_OK);
    CHECK(in_range.rows.size() == 10);
    // Across the deleted range: the rows on both sides of it, nothing inside.
    auto across = stmt(f, txn.p, scan_w_range, {std::int64_t{5}, kRows, reverse}, id_c(), tight);
    REQUIRE(across.error == BC_OK);
    auto expected = std::vector<std::int64_t>{5, 6, 7, 8, 9};
    if (reverse != 0) std::ranges::reverse(expected);
    CHECK(ids(across) == expected);
  }
  // A budget too small for the rows themselves still ends the scan: the
  // reads it makes are charged.
  CHECK(stmt(f, txn.p, scan_w_range, {std::int64_t{0}, std::int64_t{10}, std::int64_t{0}}, id_c(), {5, 0}).error ==
        BC_E_BUDGET);
}

TEST_CASE("transactions: a reverse scan charges the record at its bound it steps past", "[sql][txn]") {
  Fixture f;
  create_w(f);
  for (std::int64_t id = 0; id <= 10; ++id) {
    REQUIRE(autocommit(f, insert_w, {id, bc_value{}, bc_value{}, id}) == BC_COMMITTED);
  }
  // The least budget a reverse scan of id < 10 succeeds with.
  auto cost = [&] {
    for (std::uint64_t budget = 1;; ++budget) {
      REQUIRE(budget < 1000);
      TxnPtr txn{f};
      auto r = stmt(f, txn.p, scan_w_range, {std::int64_t{0}, std::int64_t{10}, std::int64_t{1}}, id_c(), {budget, 0});
      if (r.error == BC_OK) {
        CHECK(r.rows.size() == 10);
        return budget;
      }
      REQUIRE(r.error == BC_E_BUDGET);
    }
  };
  auto with_row_10 = cost();
  REQUIRE(autocommit(f, delete_w, {std::int64_t{10}}) == BC_COMMITTED);
  // The seek lands on row 10, the scan's bound, reads it and steps past:
  // one read more than when it is absent.
  CHECK(with_row_10 == cost() + 1);
}

TEST_CASE("transactions: a transaction no statement was added to can be pinned again", "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f};
  auto none = params({});
  bc_ctx_begin_txn(f.cx, txn.p, &none, {});
  auto v = bc_txn_catalog_version(txn.p);
  CHECK(v == f.cat().version());
  CHECK_FALSE(bc_txn_repin(txn.p));  // the context still reads its snapshot
  bc_ctx_reset(f.cx);  // the statement did not run: nothing was added
  f.ddl(bytecask::sql::CreateIndexDdl{"w_c", "w", {{"c"}}});
  CHECK(bc_txn_repin(txn.p));
  REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{1}}).error == BC_OK);
  CHECK(bc_txn_catalog_version(txn.p) == v + 1);  // the live catalog, with w_c
  CHECK_FALSE(bc_txn_repin(txn.p));                // a statement is in
  CHECK(commit(f, txn.p) == BC_COMMITTED);
  check_indexes_exact(f);
}

TEST_CASE("transactions: an aborted transaction lets go of its snapshot; a freed one detaches its context",
          "[sql][txn]") {
  Fixture f;
  create_w(f);
  {
    TxnPtr txn{f};
    REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{1}}).error == BC_OK);
    CHECK(bc_txn_catalog_version(txn.p) != 0);
    CHECK(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("b"), std::int64_t{1}}).error ==
          BC_E_CONSTRAINT);
    CHECK(bc_txn_catalog_version(txn.p) == 0);  // the pin and the snapshot went with the abort
    CHECK(commit(f, txn.p) == BC_FAILED);
  }
  // A transaction freed while a context still reads it: the context is
  // detached, and resetting it later touches nothing freed.
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, std::uint64_t{1} << 20);
  REQUIRE(txn != nullptr);
  auto none = params({});
  bc_ctx_begin_txn(f.cx, txn, &none, {});
  bc_txn_free(txn);
  bc_ctx_reset(f.cx);
  CHECK(autocommit(f, insert_w, {std::int64_t{2}, std::int64_t{2}, bytes("c"), std::int64_t{2}}) == BC_COMMITTED);
}

TEST_CASE("transactions: one context at a time; a commit needs every statement reset", "[sql][txn]") {
  Fixture f;
  create_w(f);
  auto* other = bc_ctx_new(&f.bdb, &f.bcat, 1u << 20);
  REQUIRE(other != nullptr);
  auto none = params({});
  {
    TxnPtr txn{f};
    bc_ctx_begin_txn(f.cx, txn.p, &none, {});
    REQUIRE(bc_error(f.cx) == BC_OK);
    // A second context is refused while the first is attached.
    bc_ctx_begin_txn(other, txn.p, &none, {});
    CHECK(bc_error(other) == BC_E_HANDLE);
    bc_ctx_reset(other);
    bc_ctx_reset(f.cx);
    REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{1}}).error == BC_OK);
    // A statement begun on `other` and not reset: a commit is refused,
    // through `other` or through f.cx, and the transaction is as it was.
    bc_ctx_begin_txn(other, txn.p, &none, {});
    REQUIRE(bc_error(other) == BC_OK);
    CHECK(bc_txn_commit(f.cx, txn.p, false) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_HANDLE);
    bc_ctx_reset(f.cx);
    CHECK(stored(f).empty());
    // Through the context running the statement, the refusal fails that
    // statement, and it aborts the transaction at bc_txn_add: wrong, but safe.
    CHECK(bc_txn_commit(other, txn.p, false) == BC_FAILED);
    CHECK(bc_error(other) == BC_E_HANDLE);
    CHECK_FALSE(bc_txn_add(other));
    bc_ctx_reset(other);
    CHECK(commit(f, txn.p) == BC_FAILED);  // aborted
    CHECK(stored(f).empty());
  }
  {
    // Committing txn1 through a context attached to txn2: refused, txn1 as it
    // was; txn2's statement then fails, and txn2 aborts. txn1 commits once
    // committed through a reset context.
    TxnPtr txn1{f}, txn2{f};
    REQUIRE(stmt(f, txn1.p, insert_w, {std::int64_t{2}, std::int64_t{2}, bytes("b"), std::int64_t{2}}).error == BC_OK);
    bc_ctx_begin_txn(other, txn2.p, &none, {});
    REQUIRE(bc_error(other) == BC_OK);
    CHECK(bc_txn_commit(other, txn1.p, false) == BC_FAILED);
    CHECK(bc_error(other) == BC_E_HANDLE);
    CHECK_FALSE(bc_txn_add(other));
    bc_ctx_reset(other);
    CHECK(bc_txn_catalog_version(txn2.p) == 0);  // aborted: its pin let go
    CHECK(commit(f, txn1.p) == BC_COMMITTED);
  }
  {
    // An aborted or ended transaction says so first, whatever the contexts.
    TxnPtr txn{f};
    CHECK(commit(f, txn.p) == BC_COMMITTED);
    bc_ctx_begin_txn(other, txn.p, &none, {});
    CHECK(std::string{bc_error_message(other)} == "the transaction has ended");
    bc_ctx_reset(other);
    // Committed through a context running another transaction's statement:
    // the ended transaction is reported, not the context.
    TxnPtr busy{f};
    bc_ctx_begin_txn(f.cx, busy.p, &none, {});
    REQUIRE(bc_error(f.cx) == BC_OK);
    CHECK(bc_txn_commit(f.cx, txn.p, false) == BC_FAILED);
    CHECK(std::string{bc_error_message(f.cx)} == "the transaction has ended");
    bc_ctx_reset(f.cx);
  }
  bc_ctx_free(other);
  CHECK(stored(f).size() == 1);
}

TEST_CASE("transactions: a statement's results stay readable after the free that detaches it", "[sql][txn]") {
  Fixture f;
  create_w(f);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{10}}) == BC_COMMITTED);
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, std::uint64_t{1} << 20);
  REQUIRE(txn != nullptr);
  auto none = params({});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(id_c()), f.cat().version()};
  auto ref = sr.get();
  // A SELECT, its rows not yet read when the transaction is freed.
  bc_ctx_begin_txn(f.cx, txn, &none, {});
  bc_ctx_run(f.cx, scan_w, &ref);
  REQUIRE(bc_txn_add(f.cx));
  bc_txn_free(txn);
  auto rows = f.results(id_c());
  REQUIRE(rows.size() == 1);
  CHECK(std::get<std::int64_t>(rows[0][1]) == 10);
  bc_ctx_reset(f.cx);
}

TEST_CASE("transactions: every allocation of a statement's begin failed in turn leaves nothing dangling",
          "[sql][txn][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("operator new is the sanitizer's in this build");
  Fixture f;
  create_w(f);
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 1000);
    auto* txn = bc_txn_new(&f.bdb, &f.bcat, std::uint64_t{1} << 20);
    REQUIRE(txn != nullptr);
    auto none = params({});
    auto fired = false;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      bc_ctx_begin_txn(f.cx, txn, &none, {});
      fired = faults.report().fired;
    }
    INFO("allocation " << nth);
    CHECK((bc_error(f.cx) == BC_OK || bc_error(f.cx) == BC_E_MEMORY));
    if (bc_error(f.cx) == BC_E_MEMORY) CHECK_FALSE(bc_txn_add(f.cx));  // the failed begin aborts it
    bc_ctx_reset(f.cx);
    bc_txn_free(txn);
    if (!fired) break;
  }
}

TEST_CASE("transactions: a statement does not see its own writes; the next one does", "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f};
  auto first = stmt(f, txn.p, insert_then_read, {std::int64_t{5}, bc_value{}, bc_value{}, std::int64_t{1}});
  REQUIRE(first.error == BC_OK);
  CHECK(first.affected == 0);
  CHECK(stmt(f, txn.p, exists_w, {std::int64_t{5}}).affected == 1);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == 0);  // nothing reaches the engine before the commit
  REQUIRE(commit(f, txn.p) == BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == 1);
}

TEST_CASE("transactions: the second-write rule is per statement", "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f};
  REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{1}}).error == BC_OK);
  // Later statements update, delete and insert the row again: each its own
  // statement's first write.
  REQUIRE(stmt(f, txn.p, update_w, {std::int64_t{1}, std::int64_t{2}, bytes("y"), std::int64_t{2}}).error == BC_OK);
  REQUIRE(stmt(f, txn.p, delete_w, {std::int64_t{1}}).error == BC_OK);
  REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, std::int64_t{3}, bytes("x"), std::int64_t{3}}).error == BC_OK);
  REQUIRE(commit(f, txn.p) == BC_COMMITTED);
  auto got = stored(f);
  REQUIRE(got.size() == 1);
  CHECK(got[1].a == 3);
  CHECK(got[1].b == "x");
  check_indexes_exact(f);
}

TEST_CASE("transactions: the snapshot is taken at the first statement, not when the transaction is made",
          "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f};
  // Committed after the transaction was made, before its first statement:
  // seen.
  REQUIRE(autocommit(f, insert_w, {std::int64_t{1}, bc_value{}, bc_value{}, std::int64_t{1}}) == BC_COMMITTED);
  CHECK(stmt(f, txn.p, exists_w, {std::int64_t{1}}).affected == 1);
  // Committed after its first statement: not seen.
  REQUIRE(autocommit(f, insert_w, {std::int64_t{2}, bc_value{}, bc_value{}, std::int64_t{2}}) == BC_COMMITTED);
  CHECK(stmt(f, txn.p, exists_w, {std::int64_t{2}}).affected == 0);
  CHECK(stmt(f, txn.p, count_w, {std::int64_t{0}, std::int64_t{9}, std::int64_t{9}}).affected == 1);
}

TEST_CASE("transactions: a failed statement writes nothing and aborts the transaction", "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f};
  REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{1}, bc_value{}, bytes("x"), std::int64_t{1}}).error == BC_OK);
  // A duplicate of the unique b, raised after the statement planned the row.
  auto dup = stmt(f, txn.p, insert_w, {std::int64_t{2}, bc_value{}, bytes("x"), std::int64_t{2}});
  CHECK(dup.error == BC_E_CONSTRAINT);
  // Every later statement, and the commit, is refused.
  CHECK(stmt(f, txn.p, exists_w, {std::int64_t{1}}).error == BC_E_HANDLE);
  CHECK(commit(f, txn.p) == BC_FAILED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == 0);
}

TEST_CASE("transactions: a conflict at commit writes nothing", "[sql][txn]") {
  Fixture f;
  create_w(f);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{1}, bc_value{}, bc_value{}, std::int64_t{1}}) == BC_COMMITTED);
  TxnPtr a{f};
  TxnPtr b{f};
  // Both read the row, both update it; the first to commit wins.
  REQUIRE(stmt(f, a.p, update_w, {std::int64_t{1}, bc_value{}, bc_value{}, std::int64_t{10}}).error == BC_OK);
  REQUIRE(stmt(f, a.p, insert_w, {std::int64_t{2}, bc_value{}, bc_value{}, std::int64_t{2}}).error == BC_OK);
  REQUIRE(stmt(f, b.p, update_w, {std::int64_t{1}, bc_value{}, bc_value{}, std::int64_t{20}}).error == BC_OK);
  REQUIRE(commit(f, b.p) == BC_COMMITTED);
  CHECK(commit(f, a.p) == BC_CONFLICT);
  auto got = stored(f);
  CHECK(got.size() == 1);  // a's insert of row 2 went with its update
  CHECK(got[1].c == 20);
}

TEST_CASE("transactions: a statement past the buffer bound is BC_E_LIMIT and aborts the transaction",
          "[sql][txn]") {
  Fixture f;
  create_w(f);
  TxnPtr txn{f, 8192};
  auto n = 0;
  Ran r{};
  do {
    r = stmt(f, txn.p, insert_w, {std::int64_t{n}, std::int64_t{n}, bytes(std::string(40, 'x') + std::to_string(n)),
                                  std::int64_t{n}});
    ++n;
  } while (r.error == BC_OK && n < 100);
  CHECK(r.error == BC_E_LIMIT);
  CHECK(n > 1);  // the first statements fit
  CHECK(commit(f, txn.p) == BC_FAILED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == 0);
}

TEST_CASE("transactions: a read that fails is the statement's error, never absent", "[sql][txn][faults]") {
  Fixture f;
  create_w(f);
  // Row 2's value holds a marker; its stored bytes are then damaged, so
  // every read of the record fails its CRC.
  REQUIRE(autocommit(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("one"), std::int64_t{1}}) == BC_COMMITTED);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{2}, std::int64_t{2}, bytes("ZZMARKERZZ"), std::int64_t{2}}) ==
          BC_COMMITTED);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{3}, std::int64_t{3}, bytes("three"), std::int64_t{3}}) == BC_COMMITTED);
  auto damaged = false;
  for (const auto& e : std::filesystem::directory_iterator{f.dir.path()}) {
    if (e.path().extension() != ".data") continue;
    std::fstream file{e.path(), std::ios::in | std::ios::out | std::ios::binary};
    std::string data{std::istreambuf_iterator<char>{file}, {}};
    auto at = data.find("ZZMARKERZZ");
    if (at == std::string::npos) continue;
    file.seekp(static_cast<std::streamoff>(at + 4));
    file.put('Q');
    damaged = true;
  }
  REQUIRE(damaged);
  struct Case {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
  };
  const std::vector<Case> cases{
      {"bc_get", get_w, {std::int64_t{2}}, id_c()},
      {"bc_exists", exists_w, {std::int64_t{2}}, {}},
      {"forward table scan", scan_w, {}, id_c()},
      {"reverse table scan", rscan_w, {}, id_c()},
      {"forward index scan and bc_scan_row", scan_wa, {}, id_c()},
      {"reverse index scan and bc_scan_row", rscan_wa, {}, id_c()},
      {"bc_update's read", update_w, {std::int64_t{2}, bc_value{}, bc_value{}, std::int64_t{9}}, {}},
      {"bc_del_range's rows", del_range_w, {std::int64_t{0}, std::int64_t{9}}, {}},
  };
  for (const auto& c : cases) {
    INFO(c.name);
    TxnPtr txn{f};
    // An earlier statement's write, so the read goes through the overlay.
    REQUIRE(stmt(f, txn.p, insert_w, {std::int64_t{7}, std::int64_t{7}, bytes("seven"), std::int64_t{7}}).error ==
            BC_OK);
    auto got = stmt(f, txn.p, c.plan, c.params, c.result);
    CHECK(got.error == BC_E_CORRUPT);
    CHECK(commit(f, txn.p) == BC_FAILED);  // aborted
  }
  // The unique check of an insert reads the damaged row's entry: it is not
  // taken as free.
  TxnPtr txn{f};
  REQUIRE(stmt(f, txn.p, delete_w, {std::int64_t{1}}).error == BC_OK);
  CHECK(stmt(f, txn.p, insert_w, {std::int64_t{2}, bc_value{}, bc_value{}, std::int64_t{0}}).error == BC_E_CORRUPT);
  // Closing writes the hint files, which reads the damaged record too.
  try {
    f.close();
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the damage is the test's own
  }
}

TEST_CASE("transactions: an allocation failure in a statement or the commit is BC_E_MEMORY, and nothing half-done "
          "commits",
          "[sql][txn][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("operator new is the sanitizer's in this build");
  Fixture f;
  create_w(f);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{1}}) == BC_COMMITTED);
  REQUIRE(autocommit(f, insert_w, {std::int64_t{3}, std::int64_t{3}, bytes("c"), std::int64_t{3}}) == BC_COMMITTED);
  // The transaction: an insert, an update of a committed row, a range
  // delete, then reads through the overlay, then the commit. Each
  // allocation of the whole is failed in turn. Each attempt inserts a row of
  // its own: a commit whose allocation failed after its batch reached the
  // data file may keep it, as the engine's contract allows.
  struct Step {
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
  };
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 100000);
    INFO("allocation " << nth);
    const std::vector<Step> steps{
        {insert_w, {std::int64_t{100} + nth, bc_value{}, bytes("b" + std::to_string(nth)), std::int64_t{2}}, {}},
        {update_w, {std::int64_t{1}, std::int64_t{10}, bytes("a"), std::int64_t{10}}, {}},
        {del_range_w, {std::int64_t{3}, std::int64_t{4}}, {}},
        {scan_w, {}, id_c()},
        {rscan_wa, {}, id_c()},
        {count_w, {std::int64_t{0}, std::int64_t{9}, std::int64_t{9}}, {}},
    };
    auto before = stored(f);
    TxnPtr txn{f};
    std::vector<bc_params> ps;
    std::vector<SchemaRef> refs;
    for (const auto& s : steps) {
      ps.push_back(params(s.params));
      refs.push_back({all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(s.result),
                      f.cat().version()});
    }
    auto threw = false;
    auto error = BC_OK;
    auto r = BC_FAILED;
    bytecask::testing::AllocFaultReport report;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      try {
        for (std::size_t i = 0; i < steps.size() && error == BC_OK; ++i) {
          auto ref = refs[i].get();
          bc_ctx_begin_txn(f.cx, txn.p, &ps[i], {});
          bc_ctx_run(f.cx, steps[i].plan, &ref);
          (void)bc_txn_add(f.cx);
          error = bc_error(f.cx);
          bc_ctx_reset(f.cx);  // a commit needs every statement reset
        }
        r = bc_txn_commit(f.cx, txn.p, false);
        if (error == BC_OK) error = bc_error(f.cx);
      } catch (...) {
        threw = true;
      }
      report = faults.report();
    }
    bc_ctx_reset(f.cx);
    CHECK_FALSE(threw);
    if (!report.fired) {
      CHECK(error == BC_OK);
      CHECK(r == BC_COMMITTED);
      break;
    }
    ++failed;
    CHECK((error == BC_E_MEMORY || error == BC_E_UNKNOWN || error == BC_OK));
    CHECK((r == BC_COMMITTED) == (error == BC_OK));
    // A batch that reached the data file before an allocation failed leaves
    // the engine refusing writes until resume(): exactly then the commit's
    // outcome is unknown, never "nothing written". Any other failure wrote
    // nothing. A commit can also survive a failure the engine absorbed; each
    // attempt's row is its own, so the sweep goes on either way.
    auto degraded = f.db->is_degraded();
    CHECK(degraded == (error == BC_E_UNKNOWN));
    if (degraded) f.db->resume();
    if (error == BC_E_MEMORY) CHECK(stored(f).size() == before.size());
    if (error == BC_E_UNKNOWN) {
      // Whichever way it went, it went whole: the row and the update together.
      auto after = stored(f);
      if (after.contains(100 + nth)) CHECK(after.at(1).c == 10);
      check_indexes_exact(f);
    }
  }
  CHECK(failed > 0);
  auto got = stored(f);
  CHECK(got[1].c == 10);
  CHECK(!got.contains(3));
  check_indexes_exact(f);
}
