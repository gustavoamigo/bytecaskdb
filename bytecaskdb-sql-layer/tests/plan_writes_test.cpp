// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The table-level writes of plan API revision 5 (docs/plan_api_design.md,
// *Writes*): bc_insert, bc_update, bc_delete and bc_del_range keep every
// index exact, unique, DESC and nullable parts included; a duplicate is
// BC_E_CONSTRAINT and never retried; an index created after a plan was
// compiled is maintained by it; bc_scan_row finds a missing row as damage;
// a covering read reads no row; and the reads each call is charged.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::PhysType;

namespace {

// w (id BIGINT PRIMARY KEY, a BIGINT, b BYTEA, c BIGINT NOT NULL) with
//   w_a   (a)             nullable
//   w_b   UNIQUE (b DESC) nullable, inverted
//   w_ca  UNIQUE (c, a DESC)
// Ids follow t's (1-3): w is 4, its layout 5, the indexes 6, 7 and 8.
constexpr std::uint32_t kW = 4, kWA = 6, kWB = 7, kWCA = 8;
constexpr std::uint32_t kId = 0, kA = 1, kB = 2, kC = 3;

void create_w(Fixture& f) {
  f.ddl(bytecask::sql::CreateTableDdl{"w",
                                      {Fixture::col("id", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                                       Fixture::col("b", PhysType::Bytes, true), Fixture::col("c", PhysType::I64, false)},
                                      {"id"}});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_a", "w", {{"a"}}});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_b", "w", {{"b", true}}, true});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_ca", "w", {{"c"}, {"a", true}}, true});
  REQUIRE(f.cat().table("w")->id == kW);
  REQUIRE(f.cat().index("w_ca").second->id == kWCA);
}

int g_runs = 0;

// INSERT INTO w VALUES ($1, $2, $3, $4)
void insert_w(bc_ctx* cx) {
  ++g_runs;
  bc_row r = bc_row_begin(cx, kW);
  bc_row_set_param(cx, r, kId, 0);
  bc_row_set_param(cx, r, kA, 1);
  bc_row_set_param(cx, r, kB, 2);
  bc_row_set_param(cx, r, kC, 3);
  bc_insert(cx, kW, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}

// UPDATE w SET id = $2, a = $3, b = $4, c = $5 WHERE id = $1
void update_w(bc_ctx* cx) {
  ++g_runs;
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  if (!found) {
    bc_emit_count(cx, 0);
    return;
  }
  bc_row r = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r, kId, 1);
  bc_row_set_param(cx, r, kA, 2);
  bc_row_set_param(cx, r, kB, 3);
  bc_row_set_param(cx, r, kC, 4);
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

auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, bc_budget budget = {}, std::uint32_t retries = 0)
    -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{{}, {}, all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, budget, false, retries);
}

auto unique_on(std::uint32_t object) -> std::uint32_t { return (std::uint32_t{BC_C_UNIQUE} << 24) | object; }

// Every index of every table holds exactly the entries its rows derive, and
// no unique index holds two entries under one prefix without a null part:
// the physical invariant, read straight from the engine.
void check_indexes_exact(Fixture& f) {
  const auto& cat = f.cat();
  for (const auto& [tid, t] : cat.tables()) {
    std::map<std::uint32_t, std::set<Bytes>> expected;
    Bytes prefix;
    bytecask::sql::key_put_prefix(prefix, bytecask::sql::kNsRow, tid);
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
}

// The model: id -> (a, b, c), NULL as nullopt.
struct Row {
  std::optional<std::int64_t> a;
  std::optional<std::string> b;
  std::int64_t c;
};

auto opt_i(std::optional<std::int64_t> v) -> bc_value { return v ? bc_value{*v} : bc_value{std::monostate{}}; }
auto opt_b(const std::optional<std::string>& v) -> bc_value { return v ? bc_value{bytes(*v)} : bc_value{std::monostate{}}; }

// The constraint a row would break against the others (ignoring `self`),
// as bc_error_detail reports it, or 0.
auto violation(const std::map<std::int64_t, Row>& rows, std::int64_t id, const Row& r, std::optional<std::int64_t> self)
    -> std::uint32_t {
  if (id != self && rows.contains(id)) return unique_on(kW);
  for (const auto& [oid, o] : rows) {
    if (oid == self) continue;
    if (r.b && o.b == r.b) return unique_on(kWB);
  }
  for (const auto& [oid, o] : rows) {
    if (oid == self) continue;
    if (r.a && o.a == r.a && o.c == r.c) return unique_on(kWCA);
  }
  return 0;
}

}  // namespace

TEST_CASE("plan writes: insert, update and delete keep every index exact, unique, DESC and nullable included",
          "[sql][algebra][writes]") {
  Fixture f;
  create_w(f);
  std::map<std::int64_t, Row> model;
  std::mt19937_64 rng{20261019};
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };
  auto random_row = [&] {
    Row r;
    if (pick(4) != 0) r.a = pick(6);
    if (pick(3) != 0) r.b = std::string(1, static_cast<char>('a' + pick(12)));
    r.c = pick(3);
    return r;
  };
  auto refused = 0;
  for (int step = 0; step < 600; ++step) {
    auto id = std::int64_t{pick(20)};
    auto r = random_row();
    INFO("step " << step << " id " << id);
    switch (pick(4)) {
      case 0:
      case 1: {  // insert
        auto v = violation(model, id, r, std::nullopt);
        auto got = run(f, insert_w, {id, opt_i(r.a), opt_b(r.b), r.c});
        if (v == 0) {
          REQUIRE(got == BC_COMMITTED);
          model[id] = r;
        } else {
          REQUIRE(got == BC_FAILED);
          REQUIRE(bc_error(f.cx) == BC_E_CONSTRAINT);
          CHECK(bc_error_detail(f.cx) == v);
          ++refused;
        }
        break;
      }
      case 2: {  // update, the primary key moved one time in four
        auto to = pick(4) == 0 ? std::int64_t{pick(20)} : id;
        auto got = run(f, update_w, {id, to, opt_i(r.a), opt_b(r.b), r.c});
        if (!model.contains(id)) {
          REQUIRE(got == BC_COMMITTED);
          CHECK(bc_ctx_affected(f.cx) == 0);
          break;
        }
        auto v = violation(model, to, r, id);
        if (v == 0) {
          REQUIRE(got == BC_COMMITTED);
          model.erase(id);
          model[to] = r;
        } else {
          REQUIRE(got == BC_FAILED);
          CHECK(bc_error_detail(f.cx) == v);
          ++refused;
        }
        break;
      }
      default: {  // delete
        REQUIRE(run(f, delete_w, {id}) == BC_COMMITTED);
        CHECK(bc_ctx_affected(f.cx) == (model.erase(id) == 1 ? 1 : 0));
        break;
      }
    }
    if (step % 50 == 0) check_indexes_exact(f);
  }
  check_indexes_exact(f);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == static_cast<int>(model.size()));
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, kWA) == static_cast<int>(model.size()));
  CHECK(refused > 20);  // the workload reached every constraint more than once
  CHECK(static_cast<int>(model.size()) > 5);
}

TEST_CASE("plan writes: a duplicate is BC_E_CONSTRAINT and never retried", "[sql][algebra][writes]") {
  Fixture f;
  create_w(f);
  REQUIRE(run(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{0}}) == BC_COMMITTED);
  const std::vector<std::pair<std::vector<bc_value>, std::uint32_t>> dups{
      {{std::int64_t{1}, std::int64_t{2}, bytes("y"), std::int64_t{0}}, unique_on(kW)},    // the primary key
      {{std::int64_t{2}, std::int64_t{2}, bytes("x"), std::int64_t{0}}, unique_on(kWB)},   // b, a DESC part
      {{std::int64_t{3}, std::int64_t{1}, bytes("z"), std::int64_t{0}}, unique_on(kWCA)},  // (c, a)
  };
  for (const auto& [p, detail] : dups) {
    g_runs = 0;
    CHECK(run(f, insert_w, p, {}, 3) == BC_FAILED);
    CHECK(g_runs == 1);
    CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
    CHECK(bc_error_detail(f.cx) == detail);
  }
  // Nulls are distinct: two rows with a null in a unique part both insert.
  CHECK(run(f, insert_w, {std::int64_t{4}, bc_value{}, bc_value{}, std::int64_t{0}}) == BC_COMMITTED);
  CHECK(run(f, insert_w, {std::int64_t{5}, bc_value{}, bc_value{}, std::int64_t{0}}) == BC_COMMITTED);
  // An update into another row's unique value is refused; one that keeps its
  // own is not a collision with itself.
  g_runs = 0;
  CHECK(run(f, update_w, {std::int64_t{4}, std::int64_t{4}, std::int64_t{1}, bc_value{}, std::int64_t{0}}, {}, 3) ==
        BC_FAILED);
  CHECK(g_runs == 1);
  CHECK(bc_error_detail(f.cx) == unique_on(kWCA));
  CHECK(run(f, update_w, {std::int64_t{1}, std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{0}}) ==
        BC_COMMITTED);
  check_indexes_exact(f);
}

TEST_CASE("plan writes: a concurrent insert of the same key conflicts with no absent guard",
          "[sql][algebra][writes]") {
  // bc_insert writes no ensure_absent on the primary key: the engine's check
  // of the plan's own write set already sees a key absent in the snapshot
  // and present at commit. A runs on a snapshot without key 1; B inserts key
  // 1 and commits first; A's commit is a conflict, and its retry raises.
  Fixture f;
  create_w(f);
  auto pa = params({std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{0}});
  auto pb = params({std::int64_t{1}, std::int64_t{2}, bytes("b"), std::int64_t{1}});
  SchemaRef sr{{}, {}, all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  auto* other = bc_ctx_new(&f.bdb, &f.bcat, 1u << 20);
  REQUIRE(other != nullptr);
  bc_ctx_begin(f.cx, &pa, {});
  bc_ctx_run(f.cx, insert_w, &ref);
  REQUIRE(bc_error(f.cx) == BC_OK);
  CHECK(bc_execute(other, insert_w, &ref, &pb, {}, false, 0) == BC_COMMITTED);
  CHECK(bc_ctx_commit(f.cx, false) == BC_CONFLICT);
  bc_ctx_reset(f.cx);
  CHECK(bc_execute(f.cx, insert_w, &ref, &pa, {}, false, 3) == BC_FAILED);
  CHECK(bc_error_detail(f.cx) == unique_on(kW));
  bc_ctx_free(other);
  check_indexes_exact(f);
}

TEST_CASE("plan writes: an index created after the plan was compiled is maintained by it", "[sql][algebra][writes]") {
  Fixture f;
  create_w(f);
  // The reference is built before the index exists, and names no index.
  SchemaRef sr{{}, {kW}, all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  auto exec = [&](bc_plan_fn plan, std::vector<bc_value> p) {
    auto ps = params(std::move(p));
    return bc_execute(f.cx, plan, &ref, &ps, {}, false, 3);
  };
  REQUIRE(exec(insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("a"), std::int64_t{7}}) == BC_COMMITTED);
  f.ddl(bytecask::sql::CreateIndexDdl{"w_c", "w", {{"c", true}}});
  f.ddl(bytecask::sql::CreateIndexDdl{"w_bu", "w", {{"b"}, {"c"}}, true});
  const auto w_c = f.cat().index("w_c").second->id;
  const auto w_bu = f.cat().index("w_bu").second->id;
  CHECK(exec(insert_w, {std::int64_t{2}, std::int64_t{2}, bytes("b"), std::int64_t{7}}) == BC_COMMITTED);
  CHECK(exec(update_w, {std::int64_t{1}, std::int64_t{3}, std::int64_t{1}, bytes("c"), std::int64_t{8}}) ==
        BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, w_c) == 2);
  check_indexes_exact(f);
  // Its unique check too: the new unique index refuses what the old plan
  // writes.
  CHECK(exec(insert_w, {std::int64_t{9}, std::int64_t{9}, bytes("b"), std::int64_t{9}}) == BC_FAILED);
  CHECK(bc_error_detail(f.cx) == unique_on(kWB));
  f.ddl(bytecask::sql::DropIndexDdl{"w_b"});
  CHECK(exec(insert_w, {std::int64_t{9}, std::int64_t{9}, bytes("b"), std::int64_t{7}}) == BC_FAILED);
  CHECK(bc_error_detail(f.cx) == unique_on(w_bu));
  CHECK(exec(delete_w, {std::int64_t{2}}) == BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, w_c) == 1);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, w_bu) == 1);
  check_indexes_exact(f);
}

namespace {
// SELECT id FROM t ORDER BY name: covering, from the index key alone.
void covering(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, 3, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) n = bc_add(cx, n, bc_key_part_i64(cx, 3, bc_scan_key(cx, c), 1));
  bc_emit_count(cx, n);
}
// SELECT qty FROM t ORDER BY name: each entry's row fetched.
void fetching(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, 3, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) n = bc_add(cx, n, bc_col_i64(cx, bc_scan_row(cx, c), 2));
  bc_emit_count(cx, n);
}
void insert_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, 1);
  bc_row_set_param(cx, r, 0, 0);
  bc_row_set_param(cx, r, 1, 1);
  bc_row_set_param(cx, r, 2, 2);
  bc_insert(cx, 1, bc_row_end(cx, r));
}
void del_all_t(bc_ctx* cx) { bc_del_range(cx, 1, bc_empty(cx), bc_empty(cx)); }
std::uint32_t g_plain = 0;  // a table with no secondary index
void insert_plain(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, g_plain);
  bc_row_set_param(cx, r, 0, 0);
  bc_insert(cx, g_plain, bc_row_end(cx, r));
}
void del_all_plain(bc_ctx* cx) { bc_del_range(cx, g_plain, bc_empty(cx), bc_empty(cx)); }
// Rewrites row $1 of t with the same name: no index entry moves.
void touch_qty(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, 1, 0, 1), &found);
  bc_row r = bc_row_from(cx, 1, old);
  bc_row_set_param(cx, r, 2, 1);
  bc_update(cx, 1, old, bc_row_end(cx, r));
}
}  // namespace

TEST_CASE("plan writes: the reads each read and write is charged", "[sql][algebra][writes]") {
  Fixture f;
  constexpr std::int64_t kN = 6;
  for (std::int64_t i = 1; i <= kN; ++i) {
    REQUIRE(run(f, insert_t, {i, bytes(std::string(1, static_cast<char>('a' + i))), i}) == BC_COMMITTED);
  }
  // A covering read is one unit per entry: it reads no row.
  CHECK(run(f, covering, {}, {.max_reads = kN, .deadline_ns = 0}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kN * (kN + 1) / 2);
  CHECK(run(f, covering, {}, {.max_reads = kN - 1, .deadline_ns = 0}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_BUDGET);
  // A fetch is one more per entry.
  CHECK(run(f, fetching, {}, {.max_reads = 2 * kN - 1, .deadline_ns = 0}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_BUDGET);
  CHECK(run(f, fetching, {}, {.max_reads = 2 * kN, .deadline_ns = 0}) == BC_COMMITTED);
  // An update that moves no entry of a unique index checks nothing: the
  // bc_get is its only read.
  CHECK(run(f, touch_qty, {std::int64_t{1}, std::int64_t{99}}, {.max_reads = 1, .deadline_ns = 0}) == BC_COMMITTED);
  // An insert checks its primary key; one per unique index on top (w has two).
  create_w(f);
  CHECK(run(f, insert_t, {std::int64_t{50}, bytes("zz"), std::int64_t{1}}, {.max_reads = 1, .deadline_ns = 0}) ==
        BC_COMMITTED);
  CHECK(run(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{0}},
            {.max_reads = 2, .deadline_ns = 0}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_BUDGET);
  CHECK(run(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{0}},
            {.max_reads = 3, .deadline_ns = 0}) == BC_COMMITTED);
  // A range delete of a table with indexes reads each row (t holds kN + 1),
  // of one without, nothing.
  CHECK(run(f, del_all_t, {}, {.max_reads = kN, .deadline_ns = 0}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_BUDGET);
  CHECK(run(f, del_all_t, {}, {.max_reads = kN + 1, .deadline_ns = 0}) == BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, 1) == 0);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, 3) == 0);
  f.ddl(bytecask::sql::CreateTableDdl{"plain", {Fixture::col("k", PhysType::I64, false)}, {"k"}});
  g_plain = f.cat().table("plain")->id;
  for (std::int64_t i = 1; i <= kN; ++i) REQUIRE(run(f, insert_plain, {i}) == BC_COMMITTED);
  CHECK(run(f, del_all_plain, {}, {.max_reads = 1, .deadline_ns = 0}) == BC_COMMITTED);  // 0 reads; 0 means no budget
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, g_plain) == 0);
  check_indexes_exact(f);
}

namespace {
void scan_rows_t(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, 3, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) n = bc_add(cx, n, bc_col_i64(cx, bc_scan_row(cx, c), 0));
  bc_emit_count(cx, n);
}
// bc_update and bc_delete with a row the plan built, not one it read.
void update_with_built_old(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, 1);
  bc_row_set_param(cx, r, 0, 0);
  bc_view row = bc_row_end(cx, r);
  bc_update(cx, 1, row, row);
}
void delete_built(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, 1);
  bc_row_set_param(cx, r, 0, 0);
  bc_delete(cx, 1, bc_row_end(cx, r));
}
// A row of t inserted into w.
void insert_into_other(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, 1);
  bc_row_set_param(cx, r, 0, 0);
  bc_insert(cx, kW, bc_row_end(cx, r));
}
// A parameter is not a row.
void insert_param(bc_ctx* cx) { bc_insert(cx, 1, bc_param_bytes(cx, 0)); }
// Reads row $1 of t through bc_get and decodes its id.
void read_id(bc_ctx* cx) {
  bool found = false;
  bc_emit_count(cx, bc_col_i64(cx, bc_get(cx, bc_key_params(cx, 1, 0, 1), &found), 0));
}
}  // namespace

namespace {
// Two writes of row $1 of w in one plan.
void update_twice(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  bc_row r1 = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r1, kA, 1);
  bc_update(cx, kW, old, bc_row_end(cx, r1));
  bc_row r2 = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r2, kA, 2);
  bc_update(cx, kW, old, bc_row_end(cx, r2));
}
void update_then_delete(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  bc_row r = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r, kA, 1);
  bc_update(cx, kW, old, bc_row_end(cx, r));
  bc_delete(cx, kW, old);
}
void insert_twice(bc_ctx* cx) {
  insert_w(cx);
  insert_w(cx);
}
// A row's key moved away, then inserted again in the same plan.
void move_then_insert(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kW, 0, 1), &found);
  bc_row r = bc_row_from(cx, kW, old);
  bc_row_set_param(cx, r, kId, 1);
  bc_update(cx, kW, old, bc_row_end(cx, r));
  bc_row n = bc_row_begin(cx, kW);
  bc_row_set_param(cx, n, kId, 0);
  bc_row_set_param(cx, n, kC, 2);
  bc_insert(cx, kW, bc_row_end(cx, n));
}
// Reads each t_by_name entry's row twice.
void fetch_twice(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, 3, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    n = bc_add(cx, n, bc_col_i64(cx, bc_scan_row(cx, c), 0));
    n = bc_add(cx, n, bc_col_i64(cx, bc_scan_row(cx, c), 0));
  }
  bc_emit_count(cx, n);
}
}  // namespace

TEST_CASE("plan writes: a second write of one row in one plan is refused", "[sql][algebra][writes]") {
  // Each write derives the index entries it removes from the snapshot's row,
  // so a second write of that row would remove them again and leave the
  // first write's entries without a row.
  Fixture f;
  create_w(f);
  REQUIRE(run(f, insert_w, {std::int64_t{1}, std::int64_t{1}, bytes("x"), std::int64_t{0}}) == BC_COMMITTED);
  for (auto* plan : {update_twice, update_then_delete}) {
    CHECK(run(f, plan, {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_HANDLE);
    CHECK(std::string{bc_error_message(f.cx)}.find("written twice") != std::string::npos);
  }
  CHECK(run(f, insert_twice, {std::int64_t{2}, std::int64_t{2}, bytes("y"), std::int64_t{0}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, move_then_insert, {std::int64_t{1}, std::int64_t{5}, std::int64_t{0}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // Nothing of any of them committed.
  CHECK(count_entries(*f.db, bytecask::sql::kNsRow, kW) == 1);
  check_indexes_exact(f);
}

TEST_CASE("plan writes: bc_scan_row reads an entry's row once per step", "[sql][algebra][writes]") {
  Fixture f;
  constexpr std::int64_t kN = 4;
  for (std::int64_t i = 1; i <= kN; ++i) {
    REQUIRE(run(f, insert_t, {i, bytes(std::string(1, static_cast<char>('a' + i))), i}) == BC_COMMITTED);
  }
  CHECK(run(f, fetch_twice, {}, {.max_reads = 2 * kN, .deadline_ns = 0}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 2 * kN * (kN + 1) / 2);
}

TEST_CASE("plan writes: a missing row and a wrong row are errors, never skipped", "[sql][algebra][writes]") {
  Fixture f;
  for (std::int64_t i = 1; i <= 3; ++i) REQUIRE(run(f, insert_t, {i, bytes("n"), i}) == BC_COMMITTED);
  CHECK(run(f, scan_rows_t, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 6);
  // An index entry whose row is gone is damage: the runtime keeps them paired.
  Bytes key;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, 1);
  bytecask::sql::key_put_i64(key, 2, {PhysType::I64});
  Bytes row2;
  REQUIRE(f.db->get({}, key, row2));
  REQUIRE(f.db->del({}, key));
  CHECK(run(f, scan_rows_t, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  CHECK(std::string{bc_error_message(f.cx)}.find("row is missing") != std::string::npos);
  f.db->put({}, key, row2);

  CHECK(run(f, update_with_built_old, {std::int64_t{1}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run(f, delete_built, {std::int64_t{1}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  create_w(f);
  CHECK(run(f, insert_into_other, {std::int64_t{1}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(run(f, insert_param, {row2}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);

  // A stored row of a layout generation its table never had is damage.
  auto bad = row2;
  bad[0] = std::byte{7};
  f.db->put({}, key, bad);
  CHECK(run(f, read_id, {std::int64_t{2}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CORRUPT);
  CHECK(run(f, read_id, {std::int64_t{1}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);
}
