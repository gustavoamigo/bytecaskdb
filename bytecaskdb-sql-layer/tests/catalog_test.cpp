// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The catalog of docs/sql_layer_catalog_design.md: the record round-trips
// and a damaged one is refused, every DDL is one batch that survives a
// reopen or leaves nothing, drops leave no key behind, and DDL races with
// writes and with other DDL are conflicts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "include/bytecask.hpp"

import bytecask.sql.catalog;
import bytecask.sql.tests.util;

using namespace bytecask::sql;
using bytecask::DB;
using bytecask::DbFollowerMode;
using bytecask::Mode;
using sql_test::bytes;

namespace {

auto col(std::string name, SqlKind k, bool nullable = true, Datum def = {}) -> ColumnSpec {
  return {std::move(name), {k}, nullable, std::move(def)};
}

auto create_t() -> CreateTableDdl {
  return {"t", {col("id", SqlKind::BigInt), col("name", SqlKind::Bytea), col("qty", SqlKind::BigInt)}, {"id"}};
}

auto constant(const std::vector<Catalog::PlanConstant>& cs, std::string_view n) -> std::uint32_t {
  auto it = std::ranges::find(cs, n, &Catalog::PlanConstant::name);
  REQUIRE(it != cs.end());
  return it->value;
}

// A row of t, written straight to the engine.
void put_t_row(DB& db, const Catalog& cat, std::int64_t id, std::string_view name, std::int64_t qty) {
  const auto& t = *cat.table("t");
  RowWriter w{t.layout};
  REQUIRE(w.set_i64(t.column("id")->id, id));
  REQUIRE(w.set_bytes(t.column("name")->id, bytes(name)));
  REQUIRE(w.set_i64(t.column("qty")->id, qty));
  Bytes row, pk;
  REQUIRE(w.finish(row));
  REQUIRE(encode_pk_from_row(t, row, pk));
  db.put({}, pk, row);
}

// Every key in the engine starting with `prefix`.
auto count_prefix(DB& db, const Bytes& prefix) -> std::size_t {
  std::size_t n = 0;
  for (const auto& e : db.iter_from({}, prefix)) {
    if (e.key.size() < prefix.size() || !std::ranges::equal(e.key.first(prefix.size()), prefix)) break;
    ++n;
  }
  return n;
}

auto prefix_of(std::uint8_t ns, std::uint32_t id) -> Bytes {
  Bytes p;
  key_put_prefix(p, ns, id);
  return p;
}

// A table with a column of every kind, every default kind, a unique DESC
// index and a retired layout.
auto rich_table() -> Table {
  Table t;
  t.id = 10;
  t.name = "rich";
  t.version = 4;
  std::vector<ColumnDef> defs;
  auto add = [&](std::string name, SqlType sql, bool nullable, Datum def) {
    Column c{t.next_col_id++, std::move(name), sql, phys_type(sql), nullable, std::move(def)};
    defs.push_back({c.id, c.type, c.nullable});
    t.columns.push_back(std::move(c));
  };
  add("a", {SqlKind::Integer}, false, std::int64_t{-3});
  add("b", {SqlKind::Decimal, 0, 12, 2}, true, std::int64_t{150});
  add("c", {SqlKind::Varchar, 16}, true, bytes(std::string_view{"x\0y", 3}));
  add("d", {SqlKind::Double}, true, -0.5);
  add("e", {SqlKind::Boolean}, true, true);
  add("f", {SqlKind::Timestamp}, true, {});
  t.pk_col_ids = {0};
  t.indexes.push_back({11, "rich_u", true, {{2, true}, {1, false}}});
  t.layout_id = 12;
  t.layout = make_layout(defs);
  t.retired_layouts.emplace_back(9, make_layout(std::vector<ColumnDef>{defs[0]}));
  return t;
}

}  // namespace

TEST_CASE("catalog: a table record round-trips every field", "[sql][catalog]") {
  auto t = rich_table();
  auto rec = serialize_table(t);
  auto u = parse_table(rec);
  CHECK(u.id == t.id);
  CHECK(u.name == t.name);
  CHECK(u.version == t.version);
  CHECK(u.next_col_id == t.next_col_id);
  REQUIRE(u.columns.size() == t.columns.size());
  for (std::size_t i = 0; i < t.columns.size(); ++i) {
    CHECK(u.columns[i].id == t.columns[i].id);
    CHECK(u.columns[i].name == t.columns[i].name);
    CHECK(u.columns[i].sql == t.columns[i].sql);
    CHECK(u.columns[i].type == t.columns[i].type);
    CHECK(u.columns[i].nullable == t.columns[i].nullable);
    CHECK(u.columns[i].default_value == t.columns[i].default_value);
  }
  CHECK(u.pk_col_ids == t.pk_col_ids);
  REQUIRE(u.indexes.size() == 1);
  CHECK(u.indexes[0].unique);
  CHECK(u.indexes[0].parts[0].desc);
  CHECK_FALSE(u.indexes[0].parts[1].desc);
  CHECK(u.layout_id == 12);
  CHECK(u.layout.columns.size() == t.layout.columns.size());
  for (const auto& s : t.layout.columns) CHECK(u.layout.find(s.col_id)->pos == s.pos);
  CHECK(u.layout.fixed_end == t.layout.fixed_end);
  CHECK(u.layout.nvar == t.layout.nvar);
  REQUIRE(u.retired_layouts.size() == 1);
  CHECK(u.retired_layouts[0].first == 9);
  CHECK(serialize_table(u) == rec);
}

TEST_CASE("catalog: a damaged or foreign-format record is refused", "[sql][catalog]") {
  auto rec = serialize_table(rich_table());
  // Cut at every byte: always refused, never over-read.
  for (std::size_t n = 0; n < rec.size(); ++n) {
    INFO(n);
    CHECK_THROWS_AS(parse_table(BytesView{rec}.first(n)), std::runtime_error);
  }
  auto trailing = rec;
  trailing.push_back(std::byte{0});
  CHECK_THROWS_AS(parse_table(trailing), std::runtime_error);

  auto format = [&](std::uint8_t f) {
    auto r = rec;
    r[0] = std::byte{f};
    try {
      (void)parse_table(r);
      FAIL("format " << int{f} << " must be refused");
    } catch (const std::runtime_error& e) {
      return std::string{e.what()};
    }
    return std::string{};
  };
  CHECK(format(1).find("earlier SQL-layer build") != std::string::npos);
  CHECK(format(2).find("earlier SQL-layer build") != std::string::npos);
  CHECK(format(4).find("later SQL-layer build") != std::string::npos);

  // Records that parse but break an invariant.
  auto broken = [](auto&& mutate) {
    auto t = rich_table();
    mutate(t);
    return serialize_table(t);
  };
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.pk_col_ids = {42}; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.indexes[0].parts[0].col_id = 42; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.columns[1].sql.precision = 19; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.columns[1].sql.kind = static_cast<SqlKind>(99); })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.columns[0].default_value = 1.5; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.columns[2].id = 0; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.columns[0].nullable = true; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.layout.bitmap_bytes = 7; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.layout.columns[1].pos = t.layout.columns[0].pos; })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.layout.columns.pop_back(); })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.pk_col_ids = {0, 0}; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([](Table& t) { t.indexes[0].parts = {{2, false}, {2, true}}; })),
                  std::runtime_error);
}

TEST_CASE("catalog: counters of the wrong width or past the id ceiling are refused at load", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_counters"};
  auto counter = Bytes{std::byte{kNsCatalog}, std::byte{0x01}, std::byte{0x01}};
  {
    auto db = DB::open(dir.path());
    db.put({}, counter, Bytes(9, std::byte{0}));  // nine bytes: would overflow be_value
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    Bytes past(8, std::byte{0});
    past[4] = std::byte{0x01};  // 2^24 + 1, past kMaxObjectId + 1
    past[7] = std::byte{0x01};
    db.put({}, counter, past);
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    Bytes last(8, std::byte{0});
    last[4] = std::byte{0x01};  // 2^24: every id used, none reused
    db.put({}, counter, last);
    LiveCatalog full{db};
    CHECK_THROWS_AS(full.apply(create_t()), std::length_error);
  }
}

TEST_CASE("catalog: any name is accepted; plan constants come from ids and never collide", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_idents"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(CreateTableDdl{"a-b c", {col("my col", SqlKind::BigInt, false), col("x*/y", SqlKind::BigInt)},
                                  {"my col"}});
  // Table idx with column foo, and an index foo_col: by name both would be
  // IDX_FOO_COL; by id they are COL_<t>_<c> and IDX_<i>.
  (void)live.apply(CreateTableDdl{"idx", {col("foo", SqlKind::BigInt, false), col("v", SqlKind::BigInt)}, {"foo"}});
  (void)live.apply(CreateIndexDdl{"foo_col", "idx", {{"v"}}});
  auto cat = live.current();
  for (const auto& [id, t] : cat->tables()) {
    auto cs = Catalog::constants(t);
    std::vector<std::string> names;
    for (const auto& c : cs) {
      CHECK(std::ranges::all_of(c.name, [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) != 0 ||
                                                              std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
                                                              ch == '_'; }));
      names.push_back(c.name);
    }
    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
  }
  // apply returns the version its own DDL published.
  auto v = live.current()->version();
  CHECK(live.apply(CreateIndexDdl{"idx_v", "idx", {{"v"}}})->version() == v + 1);
}

TEST_CASE("catalog: concurrent DDL each returns the version it published", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_concurrent"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  constexpr int kEach = 15;
  auto run = [&](char prefix, std::vector<std::shared_ptr<const Catalog>>& seen) {
    for (int i = 0; i < kEach; ++i) {
      seen.push_back(live.apply(CreateTableDdl{std::string{prefix} + std::to_string(i), {col("k", SqlKind::BigInt)}, {"k"}}));
    }
  };
  std::vector<std::shared_ptr<const Catalog>> a, b;
  {
    std::jthread ta{[&] { run('a', a); }};
    std::jthread tb{[&] { run('b', b); }};
  }
  auto check = [](char prefix, const std::vector<std::shared_ptr<const Catalog>>& seen) {
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < seen.size(); ++i) {
      // Its own table is in it, and the versions it saw only grow.
      CHECK(seen[i]->table(std::string{prefix} + std::to_string(i)) != nullptr);
      CHECK(seen[i]->version() > last);
      last = seen[i]->version();
    }
  };
  check('a', a);
  check('b', b);
  CHECK(live.current()->version() == 2 * kEach);
}

TEST_CASE("catalog: a reader pinned during a DDL's commit is not blocked and sees the old version", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_pin"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  auto before = live.current();
  // Hold the next DDL after the generation counter turns odd and before its
  // batch reaches the engine: the window a backfill's write and sync span.
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  live.set_commit_hook_for_testing([&, released] {
    entered.set_value();
    released.wait();
  });
  std::jthread ddl{[&] { (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}}); }};
  entered.get_future().wait();
  // The pin returns without waiting for the commit, on the old version and a
  // snapshot that agrees with it.
  auto pinned = std::async(std::launch::async, [&] { return live.pin(); });
  REQUIRE(pinned.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
  auto p = pinned.get();
  CHECK(p.cat == before);
  Bytes v;
  REQUIRE(p.snap.get({}, Bytes{std::byte{kNsCatalog}, std::byte{0x01}, std::byte{0x02}}, v));
  CHECK(v.back() == std::byte{1});  // catalog version 1 in the snapshot too
  release.set_value();
  ddl.join();
  live.set_commit_hook_for_testing({});
  CHECK(live.current()->version() == 2);
  CHECK(live.pin().cat == live.current());
}

TEST_CASE("catalog: an empty database bootstraps without writing", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_boot"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  auto cat = live.current();
  CHECK(cat->version() == 0);
  CHECK(cat->next_object_id() == 1);
  CHECK(cat->tables().empty());
  CHECK(count_prefix(db, Bytes{std::byte{kNsCatalog}}) == 0);
  (void)live.apply(create_t());
  CHECK(live.current()->version() == 1);
  CHECK(count_prefix(db, Bytes{std::byte{kNsCatalog}, std::byte{0x01}}) == 2);  // both counters
}

TEST_CASE("catalog: every DDL survives a reopen", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_ddl"};
  auto reopen = [&](auto&& check) {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    check(db, live);
    db.close();
  };
  reopen([](DB& db, LiveCatalog& live) {
    (void)live.apply(create_t());
    put_t_row(db, *live.current(), 1, "bob", 10);
    put_t_row(db, *live.current(), 2, "amy", 20);
  });
  reopen([](DB& db, LiveCatalog& live) {
    const auto& t = *live.current()->table("t");
    CHECK(t.id == 1);
    CHECK(t.layout_id == 2);
    CHECK_FALSE(t.column("id")->nullable);  // a primary key column is never null
    CHECK(t.column("name")->nullable);
    CHECK(t.column("qty")->sql == SqlType{SqlKind::BigInt});
    // CREATE UNIQUE INDEX with a DESC part, backfilled from the two rows.
    (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name", true}}, true});
    CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 2);
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();
    CHECK(cat->version() == 2);
    const auto& t = *cat->table("t");
    CHECK(t.version == 2);
    const auto* ix = t.index("t_by_name");
    REQUIRE(ix != nullptr);
    CHECK(ix->unique);
    REQUIRE(ix->parts.size() == 1);
    CHECK(ix->parts[0].desc);
    const auto* kl = cat->key_layout(3);
    REQUIRE(kl != nullptr);
    CHECK(kl->ns == kNsIndex);
    CHECK(kl->index_parts == 1);
    REQUIRE(kl->parts.size() == 2);
    CHECK((kl->parts[0].type == PhysType::Bytes && kl->parts[0].nullable && kl->parts[0].desc));
    CHECK_FALSE(kl->parts[1].desc);
    CHECK(cat->index("t_by_name").first == &t);
    // The DESC entry for "bob": the part inverted, then the primary key.
    Bytes key;
    key_put_prefix(key, kNsIndex, 3);
    key_put_bytes(key, bytes("bob"), kl->parts[0]);
    key_put_i64(key, 1, kl->parts[1]);
    Bytes out;
    CHECK(db.get({}, key, out));
    // ADD COLUMN with a default rewrites the rows; positions do not move.
    auto qty_pos = t.layout.find(t.column("qty")->id)->pos;
    (void)live.apply(AddColumnDdl{"t", col("tag", SqlKind::Integer, false, std::int64_t{7})});
    const auto& t2 = *live.current()->table("t");
    CHECK(t2.layout.find(t2.column("qty")->id)->pos == qty_pos);
    CHECK(t2.layout_id == t.layout_id);
  });
  reopen([](DB& db, LiveCatalog& live) {
    const auto& t = *live.current()->table("t");
    REQUIRE(t.column("tag") != nullptr);
    CHECK(t.column("tag")->id == 3);
    CHECK(t.next_col_id == 4);
    CHECK(t.version == 3);
    CHECK_FALSE(t.column("tag")->nullable);
    CHECK(t.column("tag")->default_value == Datum{std::int64_t{7}});
    Bytes pk, row;
    key_put_prefix(pk, kNsRow, 1);
    key_put_i64(pk, 2, {PhysType::I64});
    REQUIRE(db.get({}, pk, row));
    CHECK(row_i64(row, t.layout.find(3)->pos) == 7);
    CHECK(row_i64(row, t.layout.find(2)->pos) == 20);
    (void)live.apply(DropIndexDdl{"t_by_name"});
    CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 0);
    CHECK_FALSE(db.contains_key({}, name_entry_key("t_by_name")));
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();
    CHECK(cat->table("t")->indexes.empty());
    CHECK(cat->key_layout(3) == nullptr);
    CHECK(cat->version() == 4);
    (void)live.apply(CreateIndexDdl{"t_by_qty", "t", {{"qty"}}});
    (void)live.apply(DropTableDdl{"t"});
    // Rows, index entries, the record and both name entries are gone.
    CHECK(count_prefix(db, prefix_of(kNsRow, 1)) == 0);
    CHECK(count_prefix(db, prefix_of(kNsIndex, 4)) == 0);
    CHECK_FALSE(db.contains_key({}, table_record_key(1)));
    CHECK_FALSE(db.contains_key({}, name_entry_key("t")));
    CHECK_FALSE(db.contains_key({}, name_entry_key("t_by_qty")));
  });
  reopen([](DB&, LiveCatalog& live) {
    auto cat = live.current();
    CHECK(cat->tables().empty());
    CHECK(cat->version() == 6);
    // Ids are not reused: the next table takes 5 and 6.
    (void)live.apply(create_t());
    CHECK(live.current()->table("t")->id == 5);
  });
}

TEST_CASE("catalog: refused DDL writes nothing", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_refused"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  put_t_row(db, *live.current(), 1, "bob", 10);
  put_t_row(db, *live.current(), 2, "bob", 20);
  auto before = live.current();

  CHECK_THROWS_AS(live.apply(create_t()), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t", "t", {{"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", "t", {{"nope"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", "t", {{"qty"}, {"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", "nope", {{"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(DropTableDdl{"nope"}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(DropIndexDdl{"t"}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{"t", col("qty", SqlKind::BigInt)}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{"t", col("x", SqlKind::BigInt, false)}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateTableDdl{"u", {col("k", SqlKind::BigInt)}, {"k", "k"}}), std::invalid_argument);
  // A unique index over rows that already collide.
  try {
    (void)live.apply(CreateIndexDdl{"t_u", "t", {{"name"}}, true});
    FAIL("a unique index over duplicates must be refused");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string{e.what()}.find("duplicate key") != std::string::npos);
  }
  // IF EXISTS on nothing is a no-op.
  CHECK(live.apply(DropTableDdl{"nope", true}) == before);
  CHECK(live.apply(DropIndexDdl{"nope", true}) == before);

  CHECK(live.current() == before);
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  CHECK(count_prefix(db, Bytes{std::byte{kNsCatalog}, std::byte{0x03}}) == 1);  // t's name entry only
  db.close();
  auto db2 = DB::open(dir.path());
  LiveCatalog again{db2};
  CHECK(again.current()->version() == before->version());
  CHECK(again.current()->next_object_id() == before->next_object_id());
}

TEST_CASE("catalog: a DDL the engine refuses leaves nothing on disk or in memory", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_inject"};
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    put_t_row(db, *live.current(), 1, "bob", 10);
    auto before = live.current();
    // The injected failure: a follower refuses the batch at commit, after
    // the DDL was built (the backfill included).
    db.set_mode(Mode::Follower);
    CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}}), DbFollowerMode);
    CHECK_THROWS_AS(live.apply(AddColumnDdl{"t", col("x", SqlKind::BigInt, true, std::int64_t{1})}), DbFollowerMode);
    CHECK_THROWS_AS(live.apply(DropTableDdl{"t"}), DbFollowerMode);
    CHECK(live.current() == before);
    // Pins do not wait on a DDL that failed.
    auto p = live.pin();
    CHECK(p.cat == before);
    db.set_mode(Mode::Leader);
    db.close();
  }
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  auto cat = live.current();
  CHECK(cat->version() == 1);
  CHECK(cat->table("t")->indexes.empty());
  CHECK(cat->table("t")->columns.size() == 3);
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  CHECK(count_prefix(db, prefix_of(kNsRow, 1)) == 1);
  // The ids the failed DDL would have taken are free.
  (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}});
  CHECK(live.current()->table("t")->indexes[0].id == 3);
}

TEST_CASE("catalog: a backfill raced by a write, and two catalogs on one database, conflict", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_race"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  put_t_row(db, *live.current(), 1, "bob", 10);

  // A row written between the backfill's snapshot and its commit: the range
  // guard refuses the commit, and nothing is written.
  auto p = live.prepare(CreateIndexDdl{"t_by_name", "t", {{"name"}}});
  put_t_row(db, *live.current(), 2, "amy", 20);
  CHECK_FALSE(live.commit(std::move(p)));
  CHECK(live.current()->table("t")->indexes.empty());
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  // apply retries on a fresh snapshot and indexes both rows.
  (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}});
  CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 2);

  // A DDL without a backfill does not guard the rows.
  auto q = live.prepare(AddColumnDdl{"t", col("note", SqlKind::Text)});
  put_t_row(db, *live.current(), 3, "cy", 30);
  CHECK(live.commit(std::move(q)));

  // Two catalogs on one database: the second one's DDL commits first, and
  // the first one's, prepared on the version before, is a conflict.
  LiveCatalog other{db};
  auto r = live.prepare(CreateTableDdl{"u", {col("k", SqlKind::BigInt)}, {"k"}});
  (void)other.apply(CreateTableDdl{"v", {col("k", SqlKind::BigInt)}, {"k"}});
  CHECK_FALSE(live.commit(std::move(r)));
  CHECK(live.current()->table("u") == nullptr);
  // The next DDL of the first catalog builds on what the database holds, not
  // on its stale version: it takes fresh ids and leaves v's record alone.
  auto after = live.apply(CreateTableDdl{"u", {col("k", SqlKind::BigInt)}, {"k"}});
  REQUIRE(after->table("v") != nullptr);
  CHECK(after->table("u")->id > after->table("v")->layout_id);
  { LiveCatalog reread{db}; CHECK(reread.current()->table("v") != nullptr); }

  // A prepare on a version another DDL of the same catalog replaced is a
  // conflict too, before the engine sees it.
  LiveCatalog fresh{db};
  auto s1 = fresh.prepare(CreateTableDdl{"w1", {col("k", SqlKind::BigInt)}, {"k"}});
  auto s2 = fresh.prepare(CreateTableDdl{"w2", {col("k", SqlKind::BigInt)}, {"k"}});
  CHECK(fresh.commit(std::move(s1)));
  CHECK_FALSE(fresh.commit(std::move(s2)));
}

TEST_CASE("catalog: two catalogs on one database interleave DDL without reusing an id", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_two"};
  {
    auto db = DB::open(dir.path());
    LiveCatalog a{db};
    LiveCatalog b{db};
    for (int i = 0; i < 10; ++i) {
      (void)a.apply(CreateTableDdl{"a" + std::to_string(i), {col("k", SqlKind::BigInt)}, {"k"}});
      // b's next pin sees a's table: a reader catches up, not only a DDL.
      CHECK(b.pin().cat->table("a" + std::to_string(i)) != nullptr);
      (void)b.apply(CreateTableDdl{"b" + std::to_string(i), {col("k", SqlKind::BigInt)}, {"k"}});
    }
    db.close();
  }
  auto db = DB::open(dir.path());
  LiveCatalog again{db};
  auto cat = again.current();
  REQUIRE(cat->tables().size() == 20);
  std::set<std::uint32_t> ids;
  for (const auto& [id, t] : cat->tables()) {
    CHECK(ids.insert(t.id).second);
    CHECK(ids.insert(t.layout_id).second);
  }
  CHECK(cat->version() == 20);
}

TEST_CASE("catalog: a backfill past the batch limit is refused before it gathers the table", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_budget"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  for (std::int64_t i = 0; i < 50; ++i) put_t_row(db, *live.current(), i, "name", i);
  live.set_backfill_limit_for_testing(512);  // about 25 index entries
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}}), std::length_error);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{"t", col("x", SqlKind::BigInt, true, std::int64_t{1})}), std::length_error);
  CHECK(live.current()->version() == 1);
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  live.set_backfill_limit_for_testing(LiveCatalog::kMaxBackfillBytes);
  (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}});
  CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 50);
}

TEST_CASE("catalog: name entries and records must agree at load", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_names"};
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    // A name entry with no object behind it.
    db.put({}, name_entry_key("ghost"), Bytes{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{9}});
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    (void)db.del({}, name_entry_key("ghost"));
    LiveCatalog ok{db};  // consistent again
    // An object whose name entry is missing.
    (void)db.del({}, name_entry_key("t"));
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    db.close();
  }
}

TEST_CASE("catalog: the constants block a plan needs", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_consts"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  auto cat = live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}});
  const auto& t = *cat->table("t");
  auto cs = Catalog::constants(t);
  CHECK(constant(cs, "TBL_1") == 1);
  CHECK(constant(cs, "IDX_3") == 3);
  // A column is named by its permanent id: no position, so no layout
  // generation, reaches a plan.
  CHECK(constant(cs, "COL_1_0") == 0);
  CHECK(constant(cs, "COL_1_1") == 1);
  CHECK(constant(cs, "COL_1_2") == 2);
  CHECK(std::ranges::none_of(cs, [](const auto& c) { return c.name.starts_with("LAY_"); }));
  // Key parts by position: t's key is (id); t_by_name's is (name, id).
  CHECK(constant(cs, "PART_1_0") == 0);
  CHECK(constant(cs, "PART_3_1") == 0);
  CHECK(constant(cs, "PART_3_0") == 1);
}
