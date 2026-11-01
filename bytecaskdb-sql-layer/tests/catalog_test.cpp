// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The catalog of docs/catalog_design.md: the record round-trips
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
#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

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
    defs.emplace_back(c.id, c.sql, c.nullable);
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

TEST_CASE("catalog: a table's CHECK constraints are a format-5 tail; a table without any is still format 4", "[sql][catalog]") {
  auto plain = rich_table();
  auto rec4 = serialize_table(plain);
  CHECK(rec4[0] == std::byte{4});  // an earlier build still opens a database without a CHECK
  auto t = rich_table();
  t.checks = {{"c1", "a > 0"}, {"c2", "a < b AND b <> 'x(y'"}};
  auto rec = serialize_table(t);
  CHECK(rec[0] == std::byte{5});
  auto u = parse_table(rec);
  REQUIRE(u.checks.size() == 2);
  CHECK(u.checks[1].name == "c2");
  CHECK(u.checks[1].expr == "a < b AND b <> 'x(y'");
  CHECK(serialize_table(u) == rec);
  // Every cut is refused, and so is a format-5 record that holds none and one that names a CHECK twice.
  for (std::size_t n = 0; n < rec.size(); ++n) {
    INFO(n);
    CHECK_THROWS_AS(parse_table(BytesView{rec}.first(n)), std::runtime_error);
  }
  auto none = rec4;
  none[0] = std::byte{5};
  none.push_back(std::byte{0});
  none.push_back(std::byte{0});
  CHECK_THROWS_AS(parse_table(none), std::runtime_error);
  t.checks = {{"c1", "a > 0"}, {"c1", "a < 9"}};
  CHECK_THROWS_AS(parse_table(serialize_table(t)), std::runtime_error);
}

TEST_CASE("catalog: a table with a TIMESTAMPTZ or CHAR(n) column is a format-6 record; any other keeps format 4 or 5", "[sql][catalog]") {
  auto t = rich_table();
  t.columns.back().sql = SqlType{SqlKind::TimestampTz};  // an earlier build would call the tag corrupt
  auto rec = serialize_table(t);
  CHECK(rec[0] == std::byte{6});
  auto u = parse_table(rec);
  CHECK(u.columns.back().sql.kind == SqlKind::TimestampTz);
  CHECK(u.checks.empty());
  CHECK(serialize_table(u) == rec);
  // A CHAR(n) column is a format-6 record too, its length kept.
  t.columns[2].sql = SqlType{SqlKind::Char, 7};
  auto char_rec = serialize_table(t);
  CHECK(char_rec[0] == std::byte{6});
  CHECK(parse_table(char_rec).columns[2].sql == SqlType{SqlKind::Char, 7});
  t.checks = {{"c1", "f > 0"}};
  auto with_check = serialize_table(t);
  CHECK(with_check[0] == std::byte{6});
  CHECK(parse_table(with_check).checks.size() == 1);
  // A format-5 record still may not hold an empty CHECK list.
  auto five = rec;
  five[0] = std::byte{5};
  CHECK_THROWS_AS(parse_table(five), std::runtime_error);
  for (std::size_t n = 0; n < rec.size(); ++n) {
    INFO(n);
    CHECK_THROWS_AS(parse_table(BytesView{rec}.first(n)), std::runtime_error);
  }
}

TEST_CASE("catalog: CHECK constraints are named as PostgreSQL names them and survive a reopen", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_check"};
  auto db = DB::open(dir.path());
  {
    LiveCatalog live{db};
    auto cols = std::vector<ColumnSpec>{col("a", SqlKind::Integer), col("b", SqlKind::Integer)};
    // Named ones first; an unnamed one is table_column_check, or table_check for the table's,
    // then 1, 2… where the name is taken.
    CreateTableDdl ddl{"c1", cols, {}, {}, {}};
    ddl.checks = {{"", "a > 0", "a"}, {"", "a < b", ""}, {"named", "b > 0", "b"}, {"", "b < 9", "b"}, {"", "b < 10", "b"}};
    (void)live.apply(ddl);
    std::vector<std::string> names;
    for (const auto& c : live.current()->table("c1")->checks) names.push_back(c.name);
    CHECK(names == std::vector<std::string>{"named", "c1_a_check", "c1_check", "c1_b_check", "c1_b_check1"});
    ddl.name = "c2";
    ddl.checks = {{"x", "a > 0", ""}, {"x", "a > 1", ""}};
    try {
      (void)live.apply(ddl);
      FAIL("a constraint name used twice");
    } catch (const DdlRefused& e) {
      CHECK(e.sqlstate() == "42710");
    }
    CHECK(live.current()->table("c2") == nullptr);
  }
  LiveCatalog again{db};
  REQUIRE(again.current()->table("c1")->checks.size() == 5);
  CHECK(again.current()->table("c1")->checks[1].expr == "a > 0");
  db.close();
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
  CHECK(format(7).find("later SQL-layer build") != std::string::npos);

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

namespace {

// `rec`, a format-4 record, as format 3 wrote it: no hidden flag at the end
// of each column entry.
auto as_format_3(const Bytes& rec) -> Bytes {
  auto u16 = [&](std::size_t at) {
    return std::to_integer<std::size_t>(rec[at]) | std::to_integer<std::size_t>(rec[at + 1]) << 8;
  };
  auto u32 = [&](std::size_t at) { return u16(at) | u16(at + 2) << 16; };
  Bytes out{std::byte{3}, std::byte{0}};
  auto at = std::size_t{2};
  auto copy = [&](std::size_t n) {
    auto from = rec.begin() + static_cast<std::ptrdiff_t>(at);
    out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>(n));
    at += n;
  };
  copy(4);              // id
  copy(2 + u16(at));    // name
  copy(8);              // version, next_col_id
  auto ncols = u16(at);
  copy(2);
  for (std::size_t i = 0; i < ncols; ++i) {
    auto len = u32(at) - 1;  // the entry without its last byte, the hidden flag
    for (std::size_t b = 0; b < 4; ++b) out.push_back(static_cast<std::byte>(len >> (8 * b)));
    at += 4;
    copy(len);
    ++at;
  }
  copy(rec.size() - at);
  return out;
}

}  // namespace

TEST_CASE("catalog: a table without a primary key has a hidden row id as its key", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_row_id"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(CreateTableDdl{"k", {col("a", SqlKind::Integer), col("b", SqlKind::Text)}, {}});
  auto cat = live.current();  // held: t is in it
  const auto& t = *cat->table("k");
  const auto* rid = t.row_id();
  REQUIRE(rid != nullptr);
  CHECK(rid->hidden);
  CHECK(rid->sql == SqlType{SqlKind::BigInt});
  CHECK_FALSE(rid->nullable);
  CHECK(&t.columns.back() == rid);  // after the declared columns
  CHECK(t.pk_col_ids == std::vector{rid->id});
  CHECK(t.column("") == nullptr);  // no name resolves to it
  CHECK(t.column("a")->id == 0);
  // A table with a declared key has none.
  (void)live.apply(create_t());
  CHECK(live.current()->table("t")->row_id() == nullptr);

  // The record round-trips the flag, and a format-3 record, which has none,
  // reads with no hidden column; a format-3 entry read as format 4 runs one
  // byte short.
  auto rec = serialize_table(t);
  auto u = parse_table(rec);
  REQUIRE(u.row_id() != nullptr);
  CHECK(u.row_id()->id == rid->id);
  CHECK(serialize_table(u) == rec);
  auto old = parse_table(as_format_3(serialize_table(rich_table())));
  CHECK(std::ranges::none_of(old.columns, &Column::hidden));
  CHECK(old.columns.size() == rich_table().columns.size());
  auto short_entries = as_format_3(rec);
  short_entries[0] = std::byte{4};
  CHECK_THROWS_AS(parse_table(short_entries), std::runtime_error);

  // A hidden column is only ever the whole key: a BIGINT with no name, no
  // default and no index part, the table's only one.
  auto broken = [&](auto&& mutate) {
    auto v = t;
    mutate(v);
    return serialize_table(v);
  };
  auto hidden = [](Table& v) -> Column& { return *std::ranges::find_if(v.columns, &Column::hidden); };
  CHECK_NOTHROW(parse_table(broken([](Table&) {})));
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) { hidden(v).name = "rid"; })), std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) { hidden(v).default_value = std::int64_t{1}; })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) {
                    hidden(v).sql = {SqlKind::Integer};
                    hidden(v).type = PhysType::I64;
                  })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) { v.pk_col_ids = {0}; v.columns[0].nullable = false; })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) { v.pk_col_ids.push_back(0); v.columns[0].nullable = false; })),
                  std::runtime_error);
  CHECK_THROWS_AS(parse_table(broken([&](Table& v) { v.columns[0].hidden = true; })), std::runtime_error);
  CHECK_THROWS_AS(
      parse_table(broken([&](Table& v) { v.indexes.push_back({99, "k_rid", false, {{hidden(v).id, false}}}); })),
      std::runtime_error);
}

TEST_CASE("catalog: row ids start past the greatest the data holds and are never handed out twice", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_row_ids"};
  auto put_row = [](DB& db, const Table& t, std::int64_t id) {
    RowWriter w{t.layout};
    REQUIRE(w.set_i64(t.row_id()->id, id));
    Bytes row, pk;
    REQUIRE(w.finish(row));
    REQUIRE(encode_pk_from_row(t, row, pk));
    db.put({}, pk, row);
  };
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(CreateTableDdl{"a", {col("v", SqlKind::BigInt)}, {}});
    (void)live.apply(CreateTableDdl{"b", {col("v", SqlKind::BigInt)}, {}});
    (void)live.apply(CreateTableDdl{"c", {col("v", SqlKind::BigInt)}, {}});
    auto cat = live.current();
    const auto& a = *cat->table("a");
    const auto& b = *cat->table("b");
    // An empty table starts at 1; each table counts on its own.
    CHECK(live.next_row_id(a) == 1);
    CHECK(live.next_row_id(a) == 2);
    // A table's first draw reads the greatest id the engine holds, not the
    // neighbouring table's.
    put_row(db, b, 41);
    put_row(db, b, 7);
    put_row(db, *cat->table("c"), 1000);
    CHECK(live.next_row_id(b) == 42);
    // Seeded once: a row written behind the counter's back is not read again.
    put_row(db, a, 500);
    CHECK(live.next_row_id(a) == 3);

    // Concurrent draws never repeat.
    constexpr int kThreads = 4;
    constexpr int kEach = 500;
    std::vector<std::vector<std::int64_t>> drawn(kThreads);
    {
      std::vector<std::jthread> threads;
      for (auto& d : drawn) {
        threads.emplace_back([&] {
          for (int i = 0; i < kEach; ++i) d.push_back(live.next_row_id(b));
        });
      }
    }
    std::set<std::int64_t> all;
    for (const auto& d : drawn) all.insert(d.begin(), d.end());
    CHECK(all.size() == std::size_t{kThreads} * kEach);
    CHECK(*all.begin() == 43);
  }
  // Reopened: past the greatest id the data holds, whatever was drawn.
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  auto cat = live.current();
  CHECK(live.next_row_id(*cat->table("a")) == 501);
  CHECK(live.next_row_id(*cat->table("b")) == 42);
  CHECK(live.next_row_id(*cat->table("c")) == 1001);
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
  // A DDL that finds the other in progress fails at once and is retried,
  // as a client retries a 40001.
  auto run = [&](char prefix, std::vector<std::shared_ptr<const Catalog>>& seen) {
    for (int i = 0; i < kEach;) {
      try {
        seen.push_back(
            live.apply(CreateTableDdl{std::string{prefix} + std::to_string(i), {col("k", SqlKind::BigInt)}, {"k"}}));
        ++i;
      } catch (const DdlInProgress&) {
        std::this_thread::yield();
      }
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

TEST_CASE("catalog: a DDL that finds another in progress fails at once and writes nothing", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_ddl_busy"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  live.set_commit_hook_for_testing([&, released] {
    entered.set_value();
    released.wait();
  });
  std::jthread first{[&] { (void)live.apply(CreateIndexDdl{"t_by_name", "t", {{"name"}}}); }};
  entered.get_future().wait();
  // Held mid-commit: the second DDL returns at once, whatever the first
  // takes, and writes nothing.
  auto second = std::async(std::launch::async, [&] {
    try {
      (void)live.apply(CreateTableDdl{"u", {col("k", SqlKind::BigInt)}, {"k"}});
      return false;
    } catch (const DdlInProgress&) {
      return true;
    }
  });
  REQUIRE(second.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
  CHECK(second.get());
  release.set_value();
  first.join();
  live.set_commit_hook_for_testing({});
  CHECK(live.current()->version() == 2);
  CHECK(live.current()->table("u") == nullptr);
  CHECK(live.current()->index("t_by_name").second != nullptr);
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
    auto cat = live.current();  // held: t points into it, across the DDL below
    const auto& t = *cat->table("t");
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
    auto cat2 = live.current();  // held: t2 points into it
    const auto& t2 = *cat2->table("t");
    CHECK(t2.layout.find(t2.column("qty")->id)->pos == qty_pos);
    CHECK(t2.layout_id == t.layout_id);
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();  // held: t points into it
    const auto& t = *cat->table("t");
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
    CHECK(row_column(row, *t.layout.find(3)).i64() == 7);
    CHECK(row_column(row, *t.layout.find(2)).i64() == 20);
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
  } catch (const ConstraintViolation& e) {
    CHECK(std::string{e.what()}.find("duplicate key") != std::string::npos);
    CHECK(e.detail().constraint == BC_C_UNIQUE);
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

TEST_CASE("catalog: CREATE TABLE refuses in PostgreSQL's order", "[sql][catalog][sqlstate]") {
  sql_test::TempDir dir{"catalog_create_order"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  auto state = [&](const CreateTableDdl& ddl) -> std::string {
    try {
      (void)live.apply(ddl);
    } catch (const DdlRefused& e) {
      return std::string{e.sqlstate()};
    }
    return "created";
  };
  // PostgreSQL's code for an existing name, with a column list it accepts.
  CHECK(state({"t", {col("c", SqlKind::BigInt)}, {"c"}}) == "42P07");
  CHECK(state({"t", {col("c", SqlKind::BigInt)}, {}}) == "42P07");                          // no key
  CHECK(state({"t", {col("c", SqlKind::BigInt, true, 1.5)}, {"c"}}) == "42P07");            // a default not of its type
  // Its own columns and key, which PostgreSQL checks first.
  CHECK(state({"t", {col("c", SqlKind::BigInt), col("c", SqlKind::BigInt)}, {"c"}}) == "42701");
  CHECK(state({"t", {col("c", SqlKind::BigInt)}, {"zz"}}) == "42703");
  CHECK(state({"t", {col("c", SqlKind::BigInt)}, {"c", "c"}}) == "42701");
  // No columns: PostgreSQL accepts the list, so a key names a column that is not there (42703), ahead of
  // an existing name too; with no key either, the layer's own rule (a table needs columns) applies after
  // the name check.
  CHECK(state({"t", {}, {"k"}}) == "42703");
  CHECK(state({"u", {}, {"k"}}) == "42703");
  CHECK(state({"t", {}, {}}) == "42P07");
  CHECK(state({"u", {}, {}}) == "0A000");
  // A new name: the layer's own rule, which PostgreSQL does not have; with no key, a row id is the key.
  CHECK(state({"u", {col("c", SqlKind::BigInt, true, 1.5)}, {"c"}}) == "42804");
  CHECK(state({"u", {col("c", SqlKind::BigInt)}, {}}) == "created");
}

TEST_CASE("catalog: UNIQUE constraints are unique indexes of the CREATE TABLE's batch, named as PostgreSQL names them",
          "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_unique"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  auto ints = [](std::vector<std::string> names) {
    std::vector<ColumnSpec> out;
    for (auto& n : names) out.push_back(col(std::move(n), SqlKind::Integer));
    return out;
  };
  auto indexes = [&](std::string_view table) {
    auto cat = live.current();
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> out;
    for (const auto& ix : cat->table(table)->indexes) {
      CHECK(ix.unique);
      std::vector<std::uint32_t> cols;
      for (const auto& part : ix.parts) cols.push_back(part.col_id);
      out.emplace_back(ix.name, cols);
    }
    return out;
  };
  using Named = std::vector<std::pair<std::string, std::vector<std::uint32_t>>>;
  // a's, then (a, b)'s; a repeated one is the first, and one on the
  // primary key's columns is the key.
  (void)live.apply(CreateTableDdl{"u1", ints({"k", "a", "b"}), {"k"}, {{"", {"a"}}, {"", {"a", "b"}}, {"", {"a"}}, {"", {"k"}}}});
  CHECK(indexes("u1") == Named{{"u1_a_key", {1}}, {"u1_a_b_key", {1, 2}}});
  // A first unnamed one takes a repeat's name; column order matters.
  (void)live.apply(CreateTableDdl{"u2", ints({"a", "b"}), {}, {{"", {"a"}}, {"named", {"a"}}, {"", {"b", "a"}}}});
  CHECK(indexes("u2") == Named{{"named", {0}}, {"u2_b_a_key", {1, 0}}});
  // A chosen name past every name taken, this statement's too.
  (void)live.apply(CreateIndexDdl{"u3_a_key", "u1", {{"b"}}});
  (void)live.apply(CreateTableDdl{"u3", ints({"a", "b"}), {}, {{"u3_a_key1", {"b"}}, {"", {"a"}}}});
  CHECK(indexes("u3") == Named{{"u3_a_key1", {1}}, {"u3_a_key2", {0}}});
  // Within 63 bytes: the longer part shortened first, clipped to a
  // character.
  auto long_table = std::string(60, 'x');
  (void)live.apply(CreateTableDdl{long_table, ints({"aaaaaaaaaa"}), {}, {{"", {"aaaaaaaaaa"}}}});
  CHECK(indexes(long_table)[0].first == std::string(48, 'x') + "_aaaaaaaaaa_key");
  auto e = std::string{"\xc3\xa9"};  // é, two bytes
  std::string wide_table, wide_col = "b";
  for (int i = 0; i < 31; ++i) wide_table += e;
  for (int i = 0; i < 20; ++i) wide_col += "\xc3\xbc";  // ü
  (void)live.apply(CreateTableDdl{wide_table, ints({wide_col}), {}, {{"", {wide_col}}}});
  auto wide = indexes(wide_table)[0].first;
  CHECK(wide.size() <= 63);
  CHECK(wide.substr(0, 28) == wide_table.substr(0, 28));  // 14 é, cut at a character
  CHECK(wide.ends_with("_key"));

  // Refusals, in PostgreSQL's order: the constraint's columns before a
  // name in use, which a named constraint's name may also be.
  auto state = [&](const CreateTableDdl& ddl) -> std::string {
    try {
      (void)live.apply(ddl);
    } catch (const DdlRefused& err) {
      return std::string{err.sqlstate()};
    }
    return "created";
  };
  CHECK(state({"u1", ints({"a"}), {}, {{"", {"z"}}}}) == "42703");
  CHECK(state({"u9", ints({"a"}), {}, {{"", {"a", "a"}}}}) == "42701");
  CHECK(state({"u9", ints({"a"}), {}, {{"u1", {"a"}}}}) == "42P07");
  CHECK(state({"u9", ints({"a"}), {}, {{"u9", {"a"}}}}) == "42P07");  // its own table's name
  CHECK(state({"u9", ints({"a", "b"}), {}, {{"", {"a"}}, {"u9_a_key", {"b"}}}}) == "42P07");
  // A refused one writes nothing, its table included.
  CHECK(live.current()->table("u9") == nullptr);
  CHECK(live.current()->index("u9_a_key").first == nullptr);

  // A duplicate is refused, NULLs are not duplicates, and all of it
  // survives a reopen.
  auto cat = live.current();
  CHECK(cat->index("u1_a_b_key").first == cat->table("u1"));
  LiveCatalog again{db};
  CHECK(again.current()->table("u1")->indexes.size() == 3);  // its two constraints and u3_a_key
  CHECK(again.current()->index("u3_a_key2").first != nullptr);
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

TEST_CASE("catalog: a view is a record and a name entry, survives a reopen, and guards what it reads", "[sql][catalog][view]") {
  sql_test::TempDir dir{"catalog_view"};
  auto refused = [](LiveCatalog& live, const Ddl& ddl) -> std::string {
    try {
      (void)live.apply(ddl);
    } catch (const DdlRefused& e) {
      return std::string{e.sqlstate()};
    }
    return {};
  };
  std::uint32_t t_id = 0, v_id = 0;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    t_id = live.current()->table("t")->id;
    (void)live.apply(CreateViewDdl{"v", "CREATE VIEW v AS SELECT id FROM t", {t_id, t_id}, {"id"}});
    auto cat = live.current();
    REQUIRE(cat->view("v") != nullptr);
    v_id = cat->view("v")->id;
    CHECK(v_id > t_id);  // one id counter, never reused
    CHECK(cat->view("v")->definition == "CREATE VIEW v AS SELECT id FROM t");
    CHECK(cat->view("v")->depends_on == std::vector<std::uint32_t>{t_id});  // each once
    CHECK(cat->view("v")->columns == std::vector<std::string>{"id"});
    CHECK(cat->table("v") == nullptr);
    CHECK(cat->view("t") == nullptr);
    // A name is one namespace for tables, indexes and views.
    CHECK(refused(live, CreateViewDdl{"v", "x", {t_id}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateViewDdl{"t", "x", {t_id}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateTableDdl{"v", {col("id", SqlKind::BigInt)}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateViewDdl{"w", "x", {9999}, {"id"}}) == "42P01");  // reads what is not there
    // The wrong statement for the object.
    CHECK(refused(live, DropTableDdl{"v"}) == "42809");
    CHECK(refused(live, DropViewDdl{"t"}) == "42809");
    CHECK(refused(live, DropIndexDdl{"v"}) == "42809");
    CHECK(refused(live, CreateIndexDdl{"i", "v", {{"id"}}}) == "42809");
    CHECK(refused(live, AddColumnDdl{"v", col("c", SqlKind::BigInt)}) == "42809");
    // A table a view reads cannot be dropped; nor a view another reads.
    CHECK(refused(live, DropTableDdl{"t"}) == "2BP01");
    (void)live.apply(CreateViewDdl{"w", "CREATE VIEW w AS SELECT id FROM v", {v_id}, {"id"}});
    CHECK(refused(live, DropViewDdl{"v"}) == "2BP01");
    CHECK(live.current()->view("v") != nullptr);
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    auto cat = live.current();
    REQUIRE(cat->view("v") != nullptr);
    CHECK(cat->view("v")->id == v_id);
    CHECK(cat->view("w")->depends_on == std::vector<std::uint32_t>{v_id});
    CHECK(count_prefix(db, view_record_key(v_id)) == 1);
    (void)live.apply(DropViewDdl{"w"});
    (void)live.apply(DropViewDdl{"v"});
    CHECK(count_prefix(db, view_record_key(v_id)) == 0);
    CHECK(live.apply(DropViewDdl{"v", true}) == live.current());  // IF EXISTS of nothing: no new version
    CHECK(refused(live, DropViewDdl{"v"}) == "42P01");
    (void)live.apply(DropTableDdl{"t"});  // nothing reads it now
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    CHECK(live.current()->views().empty());
    CHECK(live.current()->tables().empty());
    db.close();
  }
}

TEST_CASE("catalog: a view's record and name entry must agree at load, and a damaged record is refused",
          "[sql][catalog][view]") {
  sql_test::TempDir dir{"catalog_view_damage"};
  std::uint32_t v_id = 0;
  Bytes record;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    (void)live.apply(CreateViewDdl{"v", "CREATE VIEW v AS SELECT id FROM t", {live.current()->table("t")->id}, {"id"}});
    v_id = live.current()->view("v")->id;
    Bytes v;
    REQUIRE(db.snapshot().get({}, view_record_key(v_id), v));
    record = v;
    db.close();
  }
  auto with = [&](auto&& damage) {
    auto db = DB::open(dir.path());
    damage(db);
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    db.close();
  };
  // Cut at every byte; a later format; trailing bytes.
  for (std::size_t n = 0; n < record.size(); ++n) {
    with([&](DB& db) { db.put({}, view_record_key(v_id), Bytes(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(n))); });
    {
      auto db = DB::open(dir.path());
      db.put({}, view_record_key(v_id), record);
      db.close();
    }
  }
  with([&](DB& db) {
    auto later = record;
    later[0] = std::byte{2};
    db.put({}, view_record_key(v_id), later);
  });
  {
    auto db = DB::open(dir.path());
    db.put({}, view_record_key(v_id), record);
    db.close();
  }
  with([&](DB& db) {
    auto longer = record;
    longer.push_back(std::byte{0});
    db.put({}, view_record_key(v_id), longer);
  });
  {
    auto db = DB::open(dir.path());
    db.put({}, view_record_key(v_id), record);
    db.close();
  }
  // A view with no name entry, a name entry that says table.
  with([&](DB& db) { (void)db.del({}, name_entry_key("v")); });
  {
    auto db = DB::open(dir.path());
    db.put({}, name_entry_key("v"), Bytes{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{static_cast<unsigned char>(v_id)}});
    db.close();
  }
  with([&](DB&) {});
  {
    auto db = DB::open(dir.path());
    db.put({}, name_entry_key("v"), Bytes{std::byte{3}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{static_cast<unsigned char>(v_id)}});
    LiveCatalog ok{db};  // consistent again
    CHECK(ok.current()->view("v") != nullptr);
    db.close();
  }
}
