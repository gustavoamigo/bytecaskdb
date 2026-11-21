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
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
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
  return {{kPublicSchema, "t"}, {col("id", SqlKind::BigInt), col("name", SqlKind::Bytea), col("qty", SqlKind::BigInt)}, {"id"}};
}

auto constant(const std::vector<Catalog::PlanConstant>& cs, std::string_view n) -> std::uint32_t {
  auto it = std::ranges::find(cs, n, &Catalog::PlanConstant::name);
  REQUIRE(it != cs.end());
  return it->value;
}

// A row of t: its primary key and its row.
auto t_row(const Catalog& cat, std::int64_t id, std::string_view name, std::int64_t qty) -> std::pair<Bytes, Bytes> {
  const auto& t = *cat.table({kPublicSchema, "t"});
  RowWriter w{t.layout};
  REQUIRE(w.set_i64(t.column("id")->id, id));
  REQUIRE(w.set_bytes(t.column("name")->id, bytes(name)));
  REQUIRE(w.set_i64(t.column("qty")->id, qty));
  Bytes row, pk;
  REQUIRE(w.finish(row));
  REQUIRE(encode_pk_from_row(t, row, pk));
  return {std::move(pk), std::move(row)};
}

// A row of t, written straight to the engine.
void put_t_row(DB& db, const Catalog& cat, std::int64_t id, std::string_view name, std::int64_t qty) {
  auto [pk, row] = t_row(cat, id, name, qty);
  db.put({}, pk, row);
}

// Rows held in memory, not in a snapshot: what a backfill reads in a
// transaction is its view, not the engine's.
class MapRows final : public RowSource {
public:
  std::map<Bytes, Bytes> rows;
  void scan(BytesView prefix, const std::function<void(BytesView, BytesView)>& visit) const override {
    for (auto it = rows.lower_bound(Bytes{prefix.begin(), prefix.end()}); it != rows.end(); ++it) {
      if (!has_prefix(it->first, prefix)) break;
      visit(it->first, it->second);
    }
  }
};

// The keys `ops` puts under `prefix`.
auto puts_under(const std::vector<Catalog::DdlOp>& ops, const Bytes& prefix) -> std::vector<Bytes> {
  std::vector<Bytes> out;
  for (const auto& op : ops) {
    if (op.kind == Catalog::DdlOp::Kind::Put && has_prefix(op.a, prefix)) out.push_back(op.a);
  }
  return out;
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

TEST_CASE("catalog: every table record is format 7, its CHECK constraints its tail", "[sql][catalog]") {
  auto plain = rich_table();
  auto rec_plain = serialize_table(plain);
  CHECK(rec_plain[0] == std::byte{7});
  auto t = rich_table();
  t.columns.back().sql = SqlType{SqlKind::TimestampTz};
  t.columns[2].sql = SqlType{SqlKind::Char, 7};
  t.checks = {{"c1", "a > 0"}, {"c2", "a < b AND b <> 'x(y'"}};
  auto rec = serialize_table(t);
  CHECK(rec[0] == std::byte{7});
  auto u = parse_table(rec);
  CHECK(u.columns.back().sql.kind == SqlKind::TimestampTz);
  CHECK(u.columns[2].sql == SqlType{SqlKind::Char, 7});
  REQUIRE(u.checks.size() == 2);
  CHECK(u.checks[1].name == "c2");
  CHECK(u.checks[1].expr == "a < b AND b <> 'x(y'");
  CHECK(serialize_table(u) == rec);
  CHECK(parse_table(rec_plain).checks.empty());
  // Every cut is refused, and so is a record that names a CHECK twice.
  for (std::size_t n = 0; n < rec.size(); ++n) {
    INFO(n);
    CHECK_THROWS_AS(parse_table(BytesView{rec}.first(n)), std::runtime_error);
  }
  t.checks = {{"c1", "a > 0"}, {"c1", "a < 9"}};
  CHECK_THROWS_AS(parse_table(serialize_table(t)), std::runtime_error);
}

TEST_CASE("catalog: a clock default trails its column's entry, and only a column of a type that takes it holds one",
          "[sql][catalog]") {
  auto t = rich_table();
  t.checks.clear();
  auto plain = serialize_table(t);
  t.columns.back().sql = SqlType{SqlKind::Timestamp};
  t.columns.back().default_value = std::monostate{};
  t.columns.back().clock_default = ClockDefault{ClockDefault::Fn::LocalTimestamp, 3};
  auto rec = serialize_table(t);
  CHECK(rec.size() == plain.size() + 2);  // the function and the precision, nothing else
  auto u = parse_table(rec);
  REQUIRE(u.columns.back().clock_default);
  CHECK(*u.columns.back().clock_default == ClockDefault{ClockDefault::Fn::LocalTimestamp, 3});
  CHECK(!parse_table(plain).columns.back().clock_default);
  CHECK(serialize_table(u) == rec);
  for (std::size_t n = 0; n < rec.size(); ++n) {
    INFO(n);
    CHECK_THROWS_AS(parse_table(BytesView{rec}.first(n)), std::runtime_error);
  }
  // A clock no column of its type takes, an unknown function or precision, and one beside a constant default.
  auto refused = [&](auto change) {
    auto v = t;
    change(v.columns.back());
    CHECK_THROWS_AS(parse_table(serialize_table(v)), std::runtime_error);
  };
  refused([](Column& c) { c.clock_default->fn = ClockDefault::Fn::CurrentDate; });  // a date to a timestamp: midnight
  refused([](Column& c) { c.clock_default->fn = static_cast<ClockDefault::Fn>(9); });
  refused([](Column& c) { c.clock_default->precision = 7; });
  refused([](Column& c) { c.default_value = std::int64_t{1}; });
}

TEST_CASE("catalog: a session function's default is a NAME's, taken by a string column only", "[sql][catalog][name]") {
  auto t = rich_table();
  t.checks.clear();
  auto& c = t.columns.at(2);  // a variable column, in the unique index
  c.sql = SqlType{SqlKind::Name};
  c.default_value = std::monostate{};
  std::vector<ColumnDef> defs;
  for (const auto& col : t.columns) defs.emplace_back(col.id, col.sql, col.nullable);
  t.layout = make_layout(defs);
  for (auto fn : {ClockDefault::Fn::CurrentUser, ClockDefault::Fn::SessionUser, ClockDefault::Fn::CurrentCatalog,
                  ClockDefault::Fn::CurrentSchema}) {
    c.clock_default = ClockDefault{fn, 6};
    auto u = parse_table(serialize_table(t));
    CHECK(u.columns.at(2).sql == SqlType{SqlKind::Name});
    CHECK(*u.columns.at(2).clock_default == ClockDefault{fn, 6});
  }
  // The clock to a NAME is no assignment PostgreSQL has: refused at load.
  c.clock_default = ClockDefault{ClockDefault::Fn::CurrentDate, 6};
  CHECK_THROWS_AS(parse_table(serialize_table(t)), std::runtime_error);
  // A session function to a TIMESTAMP neither.
  auto v = rich_table();
  v.checks.clear();
  v.columns.back().default_value = std::monostate{};
  v.columns.back().clock_default = ClockDefault{ClockDefault::Fn::CurrentUser, 6};
  CHECK_THROWS_AS(parse_table(serialize_table(v)), std::runtime_error);
}

TEST_CASE("catalog: a TIME WITH TIME ZONE column and its CURRENT_TIME default round-trip", "[sql][catalog][timetz]") {
  auto t = rich_table();
  t.checks.clear();
  auto& c = t.columns.back();
  c.sql = SqlType{SqlKind::TimeTz};
  c.default_value = std::monostate{};
  std::vector<ColumnDef> defs;
  for (const auto& col : t.columns) defs.emplace_back(col.id, col.sql, col.nullable);
  t.layout = make_layout(defs);
  for (auto fn : {ClockDefault::Fn::CurrentTime, ClockDefault::Fn::LocalTime, ClockDefault::Fn::CurrentTimestamp}) {
    c.clock_default = ClockDefault{fn, 0};
    auto u = parse_table(serialize_table(t));
    CHECK(u.columns.back().sql == SqlType{SqlKind::TimeTz});
    CHECK(*u.columns.back().clock_default == ClockDefault{fn, 0});
  }
  // No assignment cast from a date or a timestamp: refused at load.
  for (auto fn : {ClockDefault::Fn::CurrentDate, ClockDefault::Fn::LocalTimestamp}) {
    c.clock_default = ClockDefault{fn, 6};
    CHECK_THROWS_AS(parse_table(serialize_table(t)), std::runtime_error);
  }
  // CURRENT_TIME to a TIME drops the offset; to a DATE it has no cast.
  CHECK(clock_assignable(ClockDefault::Fn::CurrentTime, SqlKind::Time));
  CHECK_FALSE(clock_assignable(ClockDefault::Fn::CurrentTime, SqlKind::Date));
  CHECK_FALSE(clock_assignable(ClockDefault::Fn::CurrentTime, SqlKind::TimestampTz));
}

TEST_CASE("catalog: a TIME WITH TIME ZONE's integer order is PostgreSQL's timetz_cmp", "[sql][catalog][timetz]") {
  // timetz_cmp: the UTC time (time + zone, zone seconds west), then the zone.
  auto pg_cmp = [](TimeTz a, TimeTz b) {
    auto ua = a.time - a.offset * 1'000'000;
    auto ub = b.time - b.offset * 1'000'000;
    if (ua != ub) return ua < ub ? -1 : 1;
    if (a.offset != b.offset) return -a.offset < -b.offset ? -1 : 1;
    return 0;
  };
  std::mt19937_64 rng{29};
  std::uniform_int_distribution<std::int64_t> time{0, kMicrosPerDay};
  std::uniform_int_distribution<std::int64_t> offset{-kMaxTimeTzOffset, kMaxTimeTzOffset};
  std::vector<TimeTz> values{{0, -kMaxTimeTzOffset}, {0, kMaxTimeTzOffset}, {kMicrosPerDay, -kMaxTimeTzOffset},
                             {kMicrosPerDay, kMaxTimeTzOffset}, {43'200'000'000, 3600}, {39'600'000'000, 0}};
  for (int n = 0; n < 2000; ++n) {
    // Some share a UTC time, to reach the tie on the zone.
    auto o = offset(rng);
    auto t = n % 3 == 0 ? std::clamp<std::int64_t>(values.back().time - values.back().offset * 1'000'000 + o * 1'000'000, 0, kMicrosPerDay)
                        : time(rng);
    values.push_back({t, n % 5 == 0 ? std::int64_t{(o / 900) * 900} : o});
  }
  for (std::size_t i = 0; i + 1 < values.size(); ++i) {
    auto a = values[i];
    auto b = values[i + 1];
    auto pa = timetz_pack(a);
    auto pb = timetz_pack(b);
    INFO(a.time << " " << a.offset << " vs " << b.time << " " << b.offset);
    CHECK((pa < pb ? -1 : pa > pb ? 1 : 0) == pg_cmp(a, b));
    CHECK(timetz_valid(pa));
    CHECK(timetz_unpack(pa).time == a.time);
    CHECK(timetz_unpack(pa).offset == a.offset);
  }
  CHECK_FALSE(timetz_valid(timetz_pack({kMicrosPerDay + 1, 0})));
  CHECK_FALSE(timetz_valid(timetz_pack({-1, 0})));
  CHECK_FALSE(timetz_valid(std::numeric_limits<std::int64_t>::max()));
  CHECK_FALSE(timetz_valid(std::numeric_limits<std::int64_t>::min()));
  // A zone field past the offsets (kTimeTzZones > 2 * kMaxTimeTzOffset + 1).
  CHECK_FALSE(timetz_valid(2 * kMaxTimeTzOffset + 1));
  CHECK(out_of_range(SqlType{SqlKind::TimeTz}, std::int64_t{2 * kMaxTimeTzOffset + 1}) == "time out of range");
}

TEST_CASE("catalog: CHECK constraints are named as PostgreSQL names them and survive a reopen", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_check"};
  auto db = DB::open(dir.path());
  {
    LiveCatalog live{db};
    auto cols = std::vector<ColumnSpec>{col("a", SqlKind::Integer), col("b", SqlKind::Integer)};
    // Named ones first; an unnamed one is table_column_check, or table_check for the table's,
    // then 1, 2… where the name is taken.
    CreateTableDdl ddl{{kPublicSchema, "c1"}, cols, {}, {}, {}};
    ddl.checks = {{"", "a > 0", "a"}, {"", "a < b", ""}, {"named", "b > 0", "b"}, {"", "b < 9", "b"}, {"", "b < 10", "b"}};
    (void)live.apply(ddl);
    std::vector<std::string> names;
    for (const auto& c : live.current()->table({kPublicSchema, "c1"})->checks) names.push_back(c.name);
    CHECK(names == std::vector<std::string>{"named", "c1_a_check", "c1_check", "c1_b_check", "c1_b_check1"});
    ddl.name = {kPublicSchema, "c2"};
    ddl.checks = {{"x", "a > 0", ""}, {"x", "a > 1", ""}};
    try {
      (void)live.apply(ddl);
      FAIL("a constraint name used twice");
    } catch (const DdlRefused& e) {
      CHECK(e.sqlstate() == "42710");
    }
    CHECK(live.current()->table({kPublicSchema, "c2"}) == nullptr);
  }
  LiveCatalog again{db};
  REQUIRE(again.current()->table({kPublicSchema, "c1"})->checks.size() == 5);
  CHECK(again.current()->table({kPublicSchema, "c1"})->checks[1].expr == "a > 0");
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
  // Formats 1 to 6 laid rows and keys out in 8-byte words: refused, so no
  // row or key of theirs is read as this build's (#780).
  for (std::uint8_t f = 1; f <= 6; ++f) {
    INFO(int{f});
    CHECK(format(f).find("earlier SQL-layer build") != std::string::npos);
  }
  CHECK(format(8).find("later SQL-layer build") != std::string::npos);

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

TEST_CASE("catalog: a table without a primary key has a hidden row id as its key", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_row_id"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(CreateTableDdl{{kPublicSchema, "k"}, {col("a", SqlKind::Integer), col("b", SqlKind::Text)}, {}});
  auto cat = live.current();  // held: t is in it
  const auto& t = *cat->table({kPublicSchema, "k"});
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
  CHECK(live.current()->table({kPublicSchema, "t"})->row_id() == nullptr);

  // The record round-trips the flag.
  auto rec = serialize_table(t);
  auto u = parse_table(rec);
  REQUIRE(u.row_id() != nullptr);
  CHECK(u.row_id()->id == rid->id);
  CHECK(serialize_table(u) == rec);

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
    (void)live.apply(CreateTableDdl{{kPublicSchema, "a"}, {col("v", SqlKind::BigInt)}, {}});
    (void)live.apply(CreateTableDdl{{kPublicSchema, "b"}, {col("v", SqlKind::BigInt)}, {}});
    (void)live.apply(CreateTableDdl{{kPublicSchema, "c"}, {col("v", SqlKind::BigInt)}, {}});
    auto cat = live.current();
    const auto& a = *cat->table({kPublicSchema, "a"});
    const auto& b = *cat->table({kPublicSchema, "b"});
    // An empty table starts at 1; each table counts on its own.
    CHECK(live.next_row_id(a) == 1);
    CHECK(live.next_row_id(a) == 2);
    // A table's first draw reads the greatest id the engine holds, not the
    // neighbouring table's.
    put_row(db, b, 41);
    put_row(db, b, 7);
    put_row(db, *cat->table({kPublicSchema, "c"}), 1000);
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
  CHECK(live.next_row_id(*cat->table({kPublicSchema, "a"})) == 501);
  CHECK(live.next_row_id(*cat->table({kPublicSchema, "b"})) == 42);
  CHECK(live.next_row_id(*cat->table({kPublicSchema, "c"})) == 1001);
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
  (void)live.apply(CreateTableDdl{{kPublicSchema, "a-b c"}, {col("my col", SqlKind::BigInt, false), col("x*/y", SqlKind::BigInt)},
                                  {"my col"}});
  // Table idx with column foo, and an index foo_col: by name both would be
  // IDX_FOO_COL; by id they are COL_<t>_<c> and IDX_<i>.
  (void)live.apply(CreateTableDdl{{kPublicSchema, "idx"}, {col("foo", SqlKind::BigInt, false), col("v", SqlKind::BigInt)}, {"foo"}});
  (void)live.apply(CreateIndexDdl{"foo_col", {kPublicSchema, "idx"}, {{"v"}}});
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
  CHECK(live.apply(CreateIndexDdl{"idx_v", {kPublicSchema, "idx"}, {{"v"}}})->version() == v + 1);
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
            live.apply(CreateTableDdl{{kPublicSchema, std::string{prefix} + std::to_string(i)}, {col("k", SqlKind::BigInt)}, {"k"}}));
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
      CHECK(seen[i]->table({kPublicSchema, std::string{prefix} + std::to_string(i)}) != nullptr);
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
  std::jthread first{[&] { (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}}); }};
  entered.get_future().wait();
  // Held mid-commit: the second DDL returns at once, whatever the first
  // takes, and writes nothing.
  auto second = std::async(std::launch::async, [&] {
    try {
      (void)live.apply(CreateTableDdl{{kPublicSchema, "u"}, {col("k", SqlKind::BigInt)}, {"k"}});
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
  CHECK(live.current()->table({kPublicSchema, "u"}) == nullptr);
  CHECK(live.current()->index({kPublicSchema, "t_by_name"}).second != nullptr);
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
  std::jthread ddl{[&] { (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}}); }};
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
    const auto& t = *cat->table({kPublicSchema, "t"});
    CHECK(t.id == 1);
    CHECK(t.layout_id == 2);
    CHECK_FALSE(t.column("id")->nullable);  // a primary key column is never null
    CHECK(t.column("name")->nullable);
    CHECK(t.column("qty")->sql == SqlType{SqlKind::BigInt});
    // CREATE UNIQUE INDEX with a DESC part, backfilled from the two rows.
    (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name", true}}, true});
    CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 2);
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();
    CHECK(cat->version() == 2);
    const auto& t = *cat->table({kPublicSchema, "t"});
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
    CHECK(cat->index({kPublicSchema, "t_by_name"}).first == &t);
    // The DESC entry for "bob": the part inverted, then the primary key.
    Bytes key;
    key_put_prefix(key, kNsIndex, 3);
    key_put_bytes(key, bytes("bob"), kl->parts[0]);
    REQUIRE(key_put_i64(key, 1, kl->parts[1]));
    Bytes out;
    CHECK(db.get({}, key, out));
    // ADD COLUMN with a default rewrites the rows; positions do not move.
    auto qty_pos = t.layout.find(t.column("qty")->id)->pos;
    (void)live.apply(AddColumnDdl{{kPublicSchema, "t"}, col("tag", SqlKind::Integer, false, std::int64_t{7})});
    auto cat2 = live.current();  // held: t2 points into it
    const auto& t2 = *cat2->table({kPublicSchema, "t"});
    CHECK(t2.layout.find(t2.column("qty")->id)->pos == qty_pos);
    CHECK(t2.layout_id == t.layout_id);
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();  // held: t points into it
    const auto& t = *cat->table({kPublicSchema, "t"});
    REQUIRE(t.column("tag") != nullptr);
    CHECK(t.column("tag")->id == 3);
    CHECK(t.next_col_id == 4);
    CHECK(t.version == 3);
    CHECK_FALSE(t.column("tag")->nullable);
    CHECK(t.column("tag")->default_value == Datum{std::int64_t{7}});
    Bytes pk, row;
    key_put_prefix(pk, kNsRow, 1);
    REQUIRE(key_put_i64(pk, 2, {PhysType::I64}));
    REQUIRE(db.get({}, pk, row));
    CHECK(row_column(row, *t.layout.find(3)).i64() == 7);
    CHECK(row_column(row, *t.layout.find(2)).i64() == 20);
    (void)live.apply(DropIndexDdl{{kPublicSchema, "t_by_name"}});
    CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 0);
    CHECK_FALSE(db.contains_key({}, name_entry_key("t_by_name")));
  });
  reopen([](DB& db, LiveCatalog& live) {
    auto cat = live.current();
    CHECK(cat->table({kPublicSchema, "t"})->indexes.empty());
    CHECK(cat->key_layout(3) == nullptr);
    CHECK(cat->version() == 4);
    (void)live.apply(CreateIndexDdl{"t_by_qty", {kPublicSchema, "t"}, {{"qty"}}});
    (void)live.apply(DropTableDdl{{kPublicSchema, "t"}});
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
    CHECK(live.current()->table({kPublicSchema, "t"})->id == 5);
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
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t", {kPublicSchema, "t"}, {{"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", {kPublicSchema, "t"}, {{"nope"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", {kPublicSchema, "t"}, {{"qty"}, {"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"i2", {kPublicSchema, "nope"}, {{"qty"}}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(DropTableDdl{{kPublicSchema, "nope"}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(DropIndexDdl{{kPublicSchema, "t"}}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{{kPublicSchema, "t"}, col("qty", SqlKind::BigInt)}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{{kPublicSchema, "t"}, col("x", SqlKind::BigInt, false)}), std::invalid_argument);
  CHECK_THROWS_AS(live.apply(CreateTableDdl{{kPublicSchema, "u"}, {col("k", SqlKind::BigInt)}, {"k", "k"}}), std::invalid_argument);
  // A unique index over rows that already collide.
  try {
    (void)live.apply(CreateIndexDdl{"t_u", {kPublicSchema, "t"}, {{"name"}}, true});
    FAIL("a unique index over duplicates must be refused");
  } catch (const ConstraintViolation& e) {
    CHECK(std::string{e.what()}.find("duplicate key") != std::string::npos);
    CHECK(e.detail().constraint == BC_C_UNIQUE);
  }
  // IF EXISTS on nothing is a no-op.
  CHECK(live.apply(DropTableDdl{{kPublicSchema, "nope"}, true}) == before);
  CHECK(live.apply(DropIndexDdl{{kPublicSchema, "nope"}, true}) == before);

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
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt)}, {"c"}}) == "42P07");
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt)}, {}}) == "42P07");                          // no key
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt, true, 1.5)}, {"c"}}) == "42P07");            // a default not of its type
  // Its own columns and key, which PostgreSQL checks first.
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt), col("c", SqlKind::BigInt)}, {"c"}}) == "42701");
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt)}, {"zz"}}) == "42703");
  CHECK(state({{kPublicSchema, "t"}, {col("c", SqlKind::BigInt)}, {"c", "c"}}) == "42701");
  // No columns: PostgreSQL accepts the list, so a key names a column that is not there (42703), ahead of
  // an existing name too; with no key either, the layer's own rule (a table needs columns) applies after
  // the name check.
  CHECK(state({{kPublicSchema, "t"}, {}, {"k"}}) == "42703");
  CHECK(state({{kPublicSchema, "u"}, {}, {"k"}}) == "42703");
  CHECK(state({{kPublicSchema, "t"}, {}, {}}) == "42P07");
  CHECK(state({{kPublicSchema, "u"}, {}, {}}) == "0A000");
  // A new name: the layer's own rule, which PostgreSQL does not have; with no key, a row id is the key.
  CHECK(state({{kPublicSchema, "u"}, {col("c", SqlKind::BigInt, true, 1.5)}, {"c"}}) == "42804");
  CHECK(state({{kPublicSchema, "u"}, {col("c", SqlKind::BigInt)}, {}}) == "created");
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
    for (const auto& ix : cat->table({kPublicSchema, std::string{table}})->indexes) {
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
  (void)live.apply(CreateTableDdl{{kPublicSchema, "u1"}, ints({"k", "a", "b"}), {"k"}, {{"", {"a"}}, {"", {"a", "b"}}, {"", {"a"}}, {"", {"k"}}}});
  CHECK(indexes("u1") == Named{{"u1_a_key", {1}}, {"u1_a_b_key", {1, 2}}});
  // A first unnamed one takes a repeat's name; column order matters.
  (void)live.apply(CreateTableDdl{{kPublicSchema, "u2"}, ints({"a", "b"}), {}, {{"", {"a"}}, {"named", {"a"}}, {"", {"b", "a"}}}});
  CHECK(indexes("u2") == Named{{"named", {0}}, {"u2_b_a_key", {1, 0}}});
  // A chosen name past every name taken, this statement's too.
  (void)live.apply(CreateIndexDdl{"u3_a_key", {kPublicSchema, "u1"}, {{"b"}}});
  (void)live.apply(CreateTableDdl{{kPublicSchema, "u3"}, ints({"a", "b"}), {}, {{"u3_a_key1", {"b"}}, {"", {"a"}}}});
  CHECK(indexes("u3") == Named{{"u3_a_key1", {1}}, {"u3_a_key2", {0}}});
  // Within 63 bytes: the longer part shortened first, clipped to a
  // character.
  auto long_table = std::string(60, 'x');
  (void)live.apply(CreateTableDdl{{kPublicSchema, long_table}, ints({"aaaaaaaaaa"}), {}, {{"", {"aaaaaaaaaa"}}}});
  CHECK(indexes(long_table)[0].first == std::string(48, 'x') + "_aaaaaaaaaa_key");
  auto e = std::string{"\xc3\xa9"};  // é, two bytes
  std::string wide_table, wide_col = "b";
  for (int i = 0; i < 31; ++i) wide_table += e;
  for (int i = 0; i < 20; ++i) wide_col += "\xc3\xbc";  // ü
  (void)live.apply(CreateTableDdl{{kPublicSchema, wide_table}, ints({wide_col}), {}, {{"", {wide_col}}}});
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
  CHECK(state({{kPublicSchema, "u1"}, ints({"a"}), {}, {{"", {"z"}}}}) == "42703");
  CHECK(state({{kPublicSchema, "u9"}, ints({"a"}), {}, {{"", {"a", "a"}}}}) == "42701");
  CHECK(state({{kPublicSchema, "u9"}, ints({"a"}), {}, {{"u1", {"a"}}}}) == "42P07");
  CHECK(state({{kPublicSchema, "u9"}, ints({"a"}), {}, {{"u9", {"a"}}}}) == "42P07");  // its own table's name
  CHECK(state({{kPublicSchema, "u9"}, ints({"a", "b"}), {}, {{"", {"a"}}, {"u9_a_key", {"b"}}}}) == "42P07");
  // A refused one writes nothing, its table included.
  CHECK(live.current()->table({kPublicSchema, "u9"}) == nullptr);
  CHECK(live.current()->index({kPublicSchema, "u9_a_key"}).first == nullptr);

  // A duplicate is refused, NULLs are not duplicates, and all of it
  // survives a reopen.
  auto cat = live.current();
  CHECK(cat->index({kPublicSchema, "u1_a_b_key"}).first == cat->table({kPublicSchema, "u1"}));
  LiveCatalog again{db};
  CHECK(again.current()->table({kPublicSchema, "u1"})->indexes.size() == 3);  // its two constraints and u3_a_key
  CHECK(again.current()->index({kPublicSchema, "u3_a_key2"}).first != nullptr);
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
    CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}}), DbFollowerMode);
    CHECK_THROWS_AS(live.apply(AddColumnDdl{{kPublicSchema, "t"}, col("x", SqlKind::BigInt, true, std::int64_t{1})}), DbFollowerMode);
    CHECK_THROWS_AS(live.apply(DropTableDdl{{kPublicSchema, "t"}}), DbFollowerMode);
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
  CHECK(cat->table({kPublicSchema, "t"})->indexes.empty());
  CHECK(cat->table({kPublicSchema, "t"})->columns.size() == 3);
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  CHECK(count_prefix(db, prefix_of(kNsRow, 1)) == 1);
  // The ids the failed DDL would have taken are free.
  (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  CHECK(live.current()->table({kPublicSchema, "t"})->indexes[0].id == 3);
}

TEST_CASE("catalog: a backfill raced by a write, and two catalogs on one database, conflict", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_race"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  put_t_row(db, *live.current(), 1, "bob", 10);

  // A row written between the backfill's snapshot and its commit: the range
  // guard refuses the commit, and nothing is written.
  auto p = live.prepare(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  put_t_row(db, *live.current(), 2, "amy", 20);
  CHECK_FALSE(live.commit(std::move(p)));
  CHECK(live.current()->table({kPublicSchema, "t"})->indexes.empty());
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  // apply retries on a fresh snapshot and indexes both rows.
  (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 2);

  // A DDL without a backfill does not guard the rows.
  auto q = live.prepare(AddColumnDdl{{kPublicSchema, "t"}, col("note", SqlKind::Text)});
  put_t_row(db, *live.current(), 3, "cy", 30);
  CHECK(live.commit(std::move(q)));

  // Two catalogs on one database: the second one's DDL commits first, and
  // the first one's, prepared on the version before, is a conflict.
  LiveCatalog other{db};
  auto r = live.prepare(CreateTableDdl{{kPublicSchema, "u"}, {col("k", SqlKind::BigInt)}, {"k"}});
  (void)other.apply(CreateTableDdl{{kPublicSchema, "v"}, {col("k", SqlKind::BigInt)}, {"k"}});
  CHECK_FALSE(live.commit(std::move(r)));
  CHECK(live.current()->table({kPublicSchema, "u"}) == nullptr);
  // The next DDL of the first catalog builds on what the database holds, not
  // on its stale version: it takes fresh ids and leaves v's record alone.
  auto after = live.apply(CreateTableDdl{{kPublicSchema, "u"}, {col("k", SqlKind::BigInt)}, {"k"}});
  REQUIRE(after->table({kPublicSchema, "v"}) != nullptr);
  CHECK(after->table({kPublicSchema, "u"})->id > after->table({kPublicSchema, "v"})->layout_id);
  { LiveCatalog reread{db}; CHECK(reread.current()->table({kPublicSchema, "v"}) != nullptr); }

  // A prepare on a version another DDL of the same catalog replaced is a
  // conflict too, before the engine sees it.
  LiveCatalog fresh{db};
  auto s1 = fresh.prepare(CreateTableDdl{{kPublicSchema, "w1"}, {col("k", SqlKind::BigInt)}, {"k"}});
  auto s2 = fresh.prepare(CreateTableDdl{{kPublicSchema, "w2"}, {col("k", SqlKind::BigInt)}, {"k"}});
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
      (void)a.apply(CreateTableDdl{{kPublicSchema, "a" + std::to_string(i)}, {col("k", SqlKind::BigInt)}, {"k"}});
      // b's next pin sees a's table: a reader catches up, not only a DDL.
      CHECK(b.pin().cat->table({kPublicSchema, "a" + std::to_string(i)}) != nullptr);
      (void)b.apply(CreateTableDdl{{kPublicSchema, "b" + std::to_string(i)}, {col("k", SqlKind::BigInt)}, {"k"}});
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
  CHECK_THROWS_AS(live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}}), std::length_error);
  CHECK_THROWS_AS(live.apply(AddColumnDdl{{kPublicSchema, "t"}, col("x", SqlKind::BigInt, true, std::int64_t{1})}), std::length_error);
  CHECK(live.current()->version() == 1);
  CHECK(count_prefix(db, Bytes{std::byte{kNsIndex}}) == 0);
  live.set_backfill_limit_for_testing(LiveCatalog::kMaxBackfillBytes);
  (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  CHECK(count_prefix(db, prefix_of(kNsIndex, 3)) == 50);
}

TEST_CASE("catalog: a backfill reads the rows of its RowSource, not the engine's", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_row_source"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  auto cat = live.current();
  // The engine holds rows 1 and 2; the source holds rows 1 and 3.
  put_t_row(db, *cat, 1, "bob", 10);
  put_t_row(db, *cat, 2, "amy", 20);
  MapRows source;
  source.rows.insert(t_row(*cat, 1, "bob", 10));
  source.rows.insert(t_row(*cat, 3, "cy", 30));
  const auto& t = *cat->table({kPublicSchema, "t"});

  SECTION("CREATE INDEX indexes the source's rows") {
    std::vector<Catalog::DdlOp> ops;
    auto backfill = false;
    auto next = cat->next_version(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}}, source, ops, backfill,
                                  LiveCatalog::kMaxBackfillBytes);
    REQUIRE(next != nullptr);
    CHECK(backfill);
    const auto& ix = next->table({kPublicSchema, "t"})->indexes.at(0);
    std::vector<Bytes> want;
    for (const auto& [pk, row] : source.rows) {
      Bytes k;
      REQUIRE(encode_index_key_from_row(*next->table({kPublicSchema, "t"}), ix, row, k));
      want.push_back(std::move(k));
    }
    CHECK(puts_under(ops, prefix_of(kNsIndex, ix.id)) == want);
  }
  SECTION("ADD COLUMN … DEFAULT rewrites the source's rows") {
    std::vector<Catalog::DdlOp> ops;
    auto backfill = false;
    auto next = cat->next_version(AddColumnDdl{{kPublicSchema, "t"}, col("x", SqlKind::BigInt, true, std::int64_t{7})}, source, ops,
                                  backfill, LiveCatalog::kMaxBackfillBytes);
    REQUIRE(next != nullptr);
    CHECK(backfill);
    std::vector<Bytes> want;
    for (const auto& [pk, row] : source.rows) want.push_back(pk);
    CHECK(puts_under(ops, row_prefix(t.id)) == want);
  }
  SECTION("a unique index over the source's duplicates is refused, though the engine's rows are unique") {
    source.rows.insert(t_row(*cat, 4, "bob", 40));
    std::vector<Catalog::DdlOp> ops;
    auto backfill = false;
    CHECK_THROWS_AS((void)cat->next_version(CreateIndexDdl{"t_name_u", {kPublicSchema, "t"}, {{"name"}}, true}, source, ops, backfill,
                                            LiveCatalog::kMaxBackfillBytes),
                    ConstraintViolation);
  }
  SECTION("an autocommit DDL reads the engine's snapshot") {
    (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
    CHECK(count_prefix(db, prefix_of(kNsIndex, live.current()->table({kPublicSchema, "t"})->indexes.at(0).id)) == 2);
  }
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
  auto cat = live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  const auto& t = *cat->table({kPublicSchema, "t"});
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
    t_id = live.current()->table({kPublicSchema, "t"})->id;
    (void)live.apply(CreateViewDdl{{kPublicSchema, "v"}, "CREATE VIEW v AS SELECT id FROM t", {t_id, t_id}, {"id"}});
    auto cat = live.current();
    REQUIRE(cat->view({kPublicSchema, "v"}) != nullptr);
    v_id = cat->view({kPublicSchema, "v"})->id;
    CHECK(v_id > t_id);  // one id counter, never reused
    CHECK(cat->view({kPublicSchema, "v"})->definition == "CREATE VIEW v AS SELECT id FROM t");
    CHECK(cat->view({kPublicSchema, "v"})->depends_on == std::vector<std::uint32_t>{t_id});  // each once
    CHECK(cat->view({kPublicSchema, "v"})->columns == std::vector<std::string>{"id"});
    CHECK(cat->table({kPublicSchema, "v"}) == nullptr);
    CHECK(cat->view({kPublicSchema, "t"}) == nullptr);
    // A name is one namespace for tables, indexes and views.
    CHECK(refused(live, CreateViewDdl{{kPublicSchema, "v"}, "x", {t_id}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateViewDdl{{kPublicSchema, "t"}, "x", {t_id}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateTableDdl{{kPublicSchema, "v"}, {col("id", SqlKind::BigInt)}, {"id"}}) == "42P07");
    CHECK(refused(live, CreateViewDdl{{kPublicSchema, "w"}, "x", {9999}, {"id"}}) == "42P01");  // reads what is not there
    // The wrong statement for the object.
    CHECK(refused(live, DropTableDdl{{kPublicSchema, "v"}}) == "42809");
    CHECK(refused(live, DropViewDdl{{kPublicSchema, "t"}}) == "42809");
    CHECK(refused(live, DropIndexDdl{{kPublicSchema, "v"}}) == "42809");
    CHECK(refused(live, CreateIndexDdl{"i", {kPublicSchema, "v"}, {{"id"}}}) == "42809");
    CHECK(refused(live, AddColumnDdl{{kPublicSchema, "v"}, col("c", SqlKind::BigInt)}) == "42809");
    // A table a view reads cannot be dropped; nor a view another reads.
    CHECK(refused(live, DropTableDdl{{kPublicSchema, "t"}}) == "2BP01");
    (void)live.apply(CreateViewDdl{{kPublicSchema, "w"}, "CREATE VIEW w AS SELECT id FROM v", {v_id}, {"id"}});
    CHECK(refused(live, DropViewDdl{{kPublicSchema, "v"}}) == "2BP01");
    CHECK(live.current()->view({kPublicSchema, "v"}) != nullptr);
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    auto cat = live.current();
    REQUIRE(cat->view({kPublicSchema, "v"}) != nullptr);
    CHECK(cat->view({kPublicSchema, "v"})->id == v_id);
    CHECK(cat->view({kPublicSchema, "w"})->depends_on == std::vector<std::uint32_t>{v_id});
    CHECK(count_prefix(db, view_record_key(v_id)) == 1);
    (void)live.apply(DropViewDdl{{kPublicSchema, "w"}});
    (void)live.apply(DropViewDdl{{kPublicSchema, "v"}});
    CHECK(count_prefix(db, view_record_key(v_id)) == 0);
    CHECK(live.apply(DropViewDdl{{kPublicSchema, "v"}, true}) == live.current());  // IF EXISTS of nothing: no new version
    CHECK(refused(live, DropViewDdl{{kPublicSchema, "v"}}) == "42P01");
    (void)live.apply(DropTableDdl{{kPublicSchema, "t"}});  // nothing reads it now
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
    (void)live.apply(CreateViewDdl{{kPublicSchema, "v"}, "CREATE VIEW v AS SELECT id FROM t", {live.current()->table({kPublicSchema, "t"})->id}, {"id"}});
    v_id = live.current()->view({kPublicSchema, "v"})->id;
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
    CHECK(ok.current()->view({kPublicSchema, "v"}) != nullptr);
    db.close();
  }
}

namespace {

auto neword_args() -> std::vector<ProcedureArg> {
  return {{ProcedureArg::Mode::In, "w_id", SqlType{SqlKind::Integer}},
          {ProcedureArg::Mode::InOut, "c_last", SqlType{SqlKind::Varchar, 16}},
          {ProcedureArg::Mode::Out, "total", SqlType{SqlKind::Decimal, 0, 12, 2}}};
}

auto create_p(std::string name = "p", bool replace = false, std::string body = "BEGIN END") -> CreateProcedureDdl {
  return {{kPublicSchema, std::move(name)}, "CREATE PROCEDURE p(...) LANGUAGE plpgsql AS $$" + body + "$$", replace, neword_args()};
}

}  // namespace

namespace {
// Each object a name in public.
auto in_public(std::vector<std::string> names) -> std::vector<QualifiedName> {
  std::vector<QualifiedName> out;
  for (auto& n : names) out.push_back({kPublicSchema, std::move(n)});
  return out;
}
auto grant(std::vector<std::string> objects, std::vector<std::string> grantees, std::vector<GrantDdl::Privilege> privileges = {},
           bool option = false) -> GrantDdl {
  return GrantDdl{true, option, std::move(privileges), in_public(std::move(objects)), std::move(grantees), std::nullopt};
}
auto revoke(std::vector<std::string> objects, std::vector<std::string> grantees, std::vector<GrantDdl::Privilege> privileges = {},
            bool option_only = false) -> GrantDdl {
  return GrantDdl{false, option_only, std::move(privileges), in_public(std::move(objects)), std::move(grantees), std::nullopt};
}
auto refused_state(LiveCatalog& live, const Ddl& ddl) -> std::string {
  try {
    (void)live.apply(ddl);
  } catch (const DdlRefused& e) {
    return std::string{e.sqlstate()};
  }
  return {};
}
}  // namespace

TEST_CASE("catalog: lookups are keyed by (schema, name), and a DDL naming a schema but public is refused",
          "[sql][catalog][schemas]") {
  sql_test::TempDir dir{"catalog_schemas"};
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  (void)live.apply(create_t());
  (void)live.apply(CreateIndexDdl{"t_by_name", {kPublicSchema, "t"}, {{"name"}}});
  (void)live.apply(CreateTypeDdl{{kPublicSchema, "ty"}});
  (void)live.apply(CreateSequenceDdl{{kPublicSchema, "sq"}});
  auto cat = live.current();
  REQUIRE(cat->table({kPublicSchema, "t"}) != nullptr);
  constexpr std::uint32_t kOther = 7;  // no schema exists but public (stage 1a)
  CHECK(cat->table({kOther, "t"}) == nullptr);
  CHECK(cat->index({kOther, "t_by_name"}).second == nullptr);
  CHECK(cat->index({kPublicSchema, "t_by_name"}).second != nullptr);
  CHECK(cat->type({kOther, "ty"}) == nullptr);
  CHECK(cat->sequence({kOther, "sq"}) == nullptr);
  // Only the Catalog API can name another schema: refused, nothing written,
  // never created in public instead.
  auto version = cat->version();
  CHECK(refused_state(live, CreateTableDdl{{kOther, "u"}, {col("id", SqlKind::BigInt)}, {"id"}}) == "3F000");
  CHECK(refused_state(live, DropTableDdl{{kOther, "t"}}) == "3F000");
  CHECK(refused_state(live, CreateIndexDdl{"t_by_qty", {kOther, "t"}, {{"qty"}}}) == "3F000");
  CHECK(refused_state(live, CreateTypeDdl{{kOther, "ty2"}}) == "3F000");
  GrantDdl g;
  g.objects = {{kPublicSchema, "t"}, {kOther, "t"}};
  g.grantees = {""};
  CHECK(refused_state(live, Ddl{g}) == "3F000");
  CHECK(live.current()->version() == version);
  CHECK(live.current()->table({kPublicSchema, "u"}) == nullptr);
  CHECK(live.current()->table({kPublicSchema, "t"}) != nullptr);
}

TEST_CASE("catalog: roles and privileges are records of their own, survive a reopen, and go with their objects",
          "[sql][catalog][roles]") {
  sql_test::TempDir dir{"catalog_roles"};
  namespace pv = privilege;
  std::uint32_t t_id = 0, r1 = 0, r2 = 0;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    t_id = live.current()->table({kPublicSchema, "t"})->id;
    (void)live.apply(CreateRoleDdl{"r1"});
    (void)live.apply(CreateRoleDdl{"r2"});
    r1 = live.current()->role("r1")->id;
    r2 = live.current()->role("r2")->id;
    CHECK(r2 > r1);
    CHECK(r1 > t_id);  // one id counter
    // A namespace of its own: a role and a table may share a name.
    (void)live.apply(CreateRoleDdl{"t"});
    CHECK(refused_state(live, CreateRoleDdl{"r1"}) == "42710");
    CHECK(refused_state(live, CreateRoleDdl{"public"}) == "42939");
    CHECK(refused_state(live, CreateRoleDdl{"pg_x"}) == "42939");
    // PostgreSQL's order: the object, the grantees, the grantor, the privileges, their columns.
    CHECK(refused_state(live, grant({"nope"}, {"nope"})) == "42P01");
    CHECK(refused_state(live, grant({"t"}, {"nope"}, {{"usage", {}}})) == "42704");
    auto by_other = grant({"t"}, {"r1"});
    by_other.grantor = "r2";
    CHECK(refused_state(live, by_other) == "0A000");
    by_other.grantor = "nope";
    CHECK(refused_state(live, by_other) == "42704");
    CHECK(refused_state(live, grant({"t"}, {"r1"}, {{"usage", {}}})) == "0LP01");
    CHECK(refused_state(live, grant({"t"}, {"r1"}, {{"under", {}}})) == "42601");
    CHECK(refused_state(live, grant({"t"}, {"r1"}, {{"delete", {"qty"}}})) == "0LP01");
    CHECK(refused_state(live, grant({"t"}, {"r1"}, {{"select", {"nope"}}})) == "42703");
    CHECK(refused_state(live, grant({"t"}, {"r1", ""}, {}, true)) == "0LP01");  // a grant option to PUBLIC
    CHECK(live.current()->acl(t_id).empty());  // nothing of a refused one
    // ALL on the table; SELECT and UPDATE of one column, with the grant option; PUBLIC.
    (void)live.apply(grant({"t"}, {"r1", ""}, {{"all", {}}}));
    (void)live.apply(grant({"t", "t"}, {"r2"}, {{"select", {"qty"}}, {"update", {"qty", "name"}}}, true));
    auto acl = live.current()->acl(t_id);
    auto qty = live.current()->table({kPublicSchema, "t"})->column("qty")->id;
    auto name = live.current()->table({kPublicSchema, "t"})->column("name")->id;
    CHECK(std::vector<AclEntry>(acl.begin(), acl.end()) ==
          std::vector<AclEntry>{{r2, name, pv::kUpdate, pv::kUpdate, kBootstrapRole},
                                {r2, qty, pv::kSelect | pv::kUpdate, pv::kSelect | pv::kUpdate, kBootstrapRole},
                                {kPublic, kWholeObject, pv::kAll, 0, kBootstrapRole},
                                {r1, kWholeObject, pv::kAll, 0, kBootstrapRole}});
    // A role that holds a privilege is not dropped (2BP01), nor any of a list with it.
    CHECK(refused_state(live, DropRoleDdl{{"r1"}}) == "2BP01");
    CHECK(refused_state(live, DropRoleDdl{{"t", "r2"}}) == "2BP01");
    CHECK(live.current()->role("t") != nullptr);
    // REVOKE GRANT OPTION FOR keeps the privilege; a table-level REVOKE takes it from every column.
    (void)live.apply(revoke({"t"}, {"r2"}, {{"update", {"qty"}}}, true));
    (void)live.apply(revoke({"t"}, {"r2"}, {{"update", {}}}));
    (void)live.apply(revoke({"t"}, {""}, {{"all", {}}}));
    acl = live.current()->acl(t_id);
    CHECK(std::vector<AclEntry>(acl.begin(), acl.end()) ==
          std::vector<AclEntry>{{r2, qty, pv::kSelect, pv::kSelect, kBootstrapRole},
                                {r1, kWholeObject, pv::kAll, 0, kBootstrapRole}});
    CHECK(refused_state(live, DropRoleDdl{{"nope"}}).empty() == false);
    CHECK(refused_state(live, DropRoleDdl{{"nope"}, true}).empty());
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    auto cat = live.current();
    REQUIRE(cat->role("r1") != nullptr);
    CHECK(cat->role("r1")->id == r1);
    CHECK(cat->roles().size() == 3);
    CHECK(cat->acl(t_id).size() == 2);
    // A drop takes the object's privileges with it, and frees the role.
    (void)live.apply(DropTableDdl{{kPublicSchema, "t"}});
    CHECK(live.current()->acls().empty());
    CHECK(!db.snapshot().contains_key({}, acl_record_key(t_id)));
    (void)live.apply(DropRoleDdl{{"r1", "r2", "t"}});
    CHECK(live.current()->roles().empty());
    CHECK(!db.snapshot().contains_key({}, role_name_key("r1")));
    db.close();
  }
}

TEST_CASE("catalog: a role's record and name entry, and a privileges record, must agree at load; a damaged one is refused",
          "[sql][catalog][roles]") {
  sql_test::TempDir dir{"catalog_roles_damage"};
  std::uint32_t t_id = 0, r_id = 0;
  Bytes role_record, role_entry, acl;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    (void)live.apply(CreateRoleDdl{"r"});
    (void)live.apply(grant({"t"}, {"r"}, {{"select", {"qty"}}, {"insert", {}}}, true));
    (void)live.apply(grant({"t"}, {""}, {{"select", {"qty"}}, {"insert", {}}}));
    t_id = live.current()->table({kPublicSchema, "t"})->id;
    r_id = live.current()->role("r")->id;
    REQUIRE(db.snapshot().get({}, role_record_key(r_id), role_record));
    REQUIRE(db.snapshot().get({}, role_name_key("r"), role_entry));
    REQUIRE(db.snapshot().get({}, acl_record_key(t_id), acl));
    db.close();
  }
  auto put = [&](const Bytes& key, const Bytes& value) {
    auto db = DB::open(dir.path());
    db.put({}, key, value);
    db.close();
  };
  auto restore = [&] {
    put(role_record_key(r_id), role_record);
    put(role_name_key("r"), role_entry);
    put(acl_record_key(t_id), acl);
  };
  auto refused_with = [&](const Bytes& key, const Bytes& value) {
    put(key, value);
    {
      auto db = DB::open(dir.path());
      CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
      (void)db.del({}, key);
      db.close();
    }
    restore();
  };
  for (const auto& [key, record] : {std::pair{role_record_key(r_id), role_record}, std::pair{acl_record_key(t_id), acl}}) {
    INFO("record size " << record.size());
    for (std::size_t n = 0; n < record.size(); ++n) {
      refused_with(key, Bytes(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    auto later = record;
    later[1] = std::byte{2};
    refused_with(key, later);
    auto longer = record;
    longer.push_back(std::byte{0});
    refused_with(key, longer);
  }
  refused_with(role_record_key(r_id + 1), role_record);  // under another id's key
  refused_with(acl_record_key(t_id + 100), acl);        // of no table or view
  // The privileges record, little-endian: u16 format, u32 object, u32 n, then
  // entries of 16 bytes, (column, grantee) sorted: the column's SELECT of PUBLIC's and r's,
  // then the table's INSERT of PUBLIC's and r's.
  constexpr std::size_t kFirst = 2 + 4 + 4;
  auto entry = [&](std::size_t i, std::size_t field) { return kFirst + 16 * i + field; };
  auto changed = [&](Bytes r, std::size_t at, std::byte b) {
    r[at] = b;
    return r;
  };
  REQUIRE(acl.size() == kFirst + 16 * 4);
  refused_with(acl_record_key(t_id), changed(acl, entry(0, 0), std::byte{0x7F}));     // a grantee that is no role
  refused_with(acl_record_key(t_id), changed(acl, entry(0, 4), std::byte{0x7F}));     // a column the table lacks
  refused_with(acl_record_key(t_id), changed(acl, entry(0, 8), std::byte{0x08}));     // DELETE of a column
  refused_with(acl_record_key(t_id), changed(acl, entry(1, 8), std::byte{0}));        // no privilege
  refused_with(acl_record_key(t_id), changed(acl, entry(1, 10), std::byte{0x04}));    // an option it lacks
  {
    auto swapped = acl;
    std::swap_ranges(swapped.begin() + static_cast<std::ptrdiff_t>(entry(1, 0)),
                     swapped.begin() + static_cast<std::ptrdiff_t>(entry(2, 0)),
                     swapped.begin() + static_cast<std::ptrdiff_t>(entry(2, 0)));
    refused_with(acl_record_key(t_id), swapped);  // out of order
  }
  // A role with no name entry; an entry of another kind; an entry with no record.
  {
    auto db = DB::open(dir.path());
    (void)db.del({}, role_name_key("r"));
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    db.close();
  }
  restore();
  refused_with(role_name_key("r"), changed(role_entry, 0, std::byte{4}));
  refused_with(role_name_key("s"), role_entry);
  // Intact, it loads.
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  CHECK(live.current()->acl(t_id).size() == 4);
  db.close();
}

TEST_CASE("catalog: domains, shell types and sequences are records of their namespaces, survive a reopen, and take grants",
          "[sql][catalog][roles]") {
  sql_test::TempDir dir{"catalog_types"};
  std::uint32_t d_id = 0, s_id = 0;
  auto on = [](GrantDdl g, GrantDdl::Target target) {
    g.target = target;
    return g;
  };
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    (void)live.apply(CreateRoleDdl{"r"});
    // The type namespace holds domains, shell types and every table's and view's row type.
    (void)live.apply(CreateDomainDdl{{kPublicSchema, "d"}, SqlType{SqlKind::Integer}});
    CHECK(refused_state(live, CreateDomainDdl{{kPublicSchema, "d"}, SqlType{SqlKind::Integer}}) == "42710");
    CHECK(refused_state(live, CreateDomainDdl{{kPublicSchema, "t"}, SqlType{SqlKind::Integer}}) == "42710");
    CHECK(refused_state(live, CreateTypeDdl{{kPublicSchema, "t"}}) == "42710");
    CHECK(refused_state(live, CreateTableDdl{{kPublicSchema, "d"}, {col("a", SqlKind::BigInt)}, {"a"}}) == "42710");
    (void)live.apply(CreateTypeDdl{{kPublicSchema, "sh"}});
    CHECK(refused_state(live, CreateTypeDdl{{kPublicSchema, "sh"}}) == "42710");
    // A sequence is a relation, with no row type.
    (void)live.apply(CreateSequenceDdl{{kPublicSchema, "s"}});
    CHECK(refused_state(live, CreateSequenceDdl{{kPublicSchema, "t"}}) == "42P07");
    CHECK(refused_state(live, CreateTableDdl{{kPublicSchema, "s"}, {col("a", SqlKind::BigInt)}, {"a"}}) == "42P07");
    CHECK(refused_state(live, CreateSequenceDdl{{kPublicSchema, "s"}, true}).empty());
    (void)live.apply(CreateDomainDdl{{kPublicSchema, "s"}, SqlType{SqlKind::Text}});
    d_id = live.current()->type({kPublicSchema, "d"})->id;
    s_id = live.current()->sequence({kPublicSchema, "s"})->id;
    // Grants by the kind ON names: USAGE of a domain; USAGE, SELECT, UPDATE of a sequence.
    CHECK(refused_state(live, on(grant({"d"}, {"r"}, {{"select", {}}}), GrantDdl::Target::Domain)) == "0LP01");
    CHECK(refused_state(live, on(grant({"s"}, {"r"}, {{"insert", {}}}), GrantDdl::Target::Sequence)) == "0LP01");
    CHECK(refused_state(live, on(grant({"t"}, {"r"}), GrantDdl::Target::Domain)) == "42809");
    CHECK(refused_state(live, on(grant({"t"}, {"r"}), GrantDdl::Target::Sequence)) == "42809");
    CHECK(refused_state(live, on(grant({"nope"}, {"r"}), GrantDdl::Target::Domain)) == "42704");
    CHECK(refused_state(live, on(grant({"sh"}, {"r"}), GrantDdl::Target::Type)) == "42704");  // only a shell
    (void)live.apply(on(grant({"d"}, {"r"}), GrantDdl::Target::Domain));
    (void)live.apply(on(grant({"s"}, {"r"}, {{"select", {}}, {"usage", {}}}), GrantDdl::Target::Sequence));
    CHECK(live.current()->acl(d_id).front().privileges == privilege::kUsage);
    CHECK(live.current()->acl(s_id).front().privileges == (privilege::kSelect | privilege::kUsage));
    CHECK(refused_state(live, DropRoleDdl{{"r"}}) == "2BP01");
    // The wrong drop for the kind.
    CHECK(refused_state(live, DropTableDdl{{kPublicSchema, "s"}}) == "42809");
    CHECK(refused_state(live, DropSequenceDdl{{kPublicSchema, "t"}}) == "42809");
    CHECK(refused_state(live, DropTypeDdl{{kPublicSchema, "sh"}, true}) == "42809");  // DROP DOMAIN of a shell type
    CHECK(refused_state(live, DropTypeDdl{{kPublicSchema, "nope"}, true}) == "42704");
    CHECK(refused_state(live, DropSequenceDdl{{kPublicSchema, "nope"}}) == "42P01");
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    auto cat = live.current();
    REQUIRE(cat->type({kPublicSchema, "d"}) != nullptr);
    CHECK(cat->type({kPublicSchema, "d"})->kind == TypeObject::Kind::Domain);
    CHECK(cat->type({kPublicSchema, "d"})->base == SqlType{SqlKind::Integer});
    CHECK(cat->type({kPublicSchema, "sh"})->kind == TypeObject::Kind::Shell);
    CHECK(cat->sequence({kPublicSchema, "s"})->id == s_id);
    CHECK(cat->acl(s_id).size() == 1);
    // CREATE DOMAIN fills a shell type in, under its id.
    auto sh = cat->type({kPublicSchema, "sh"})->id;
    (void)live.apply(CreateDomainDdl{{kPublicSchema, "sh"}, SqlType{SqlKind::BigInt}});
    CHECK(live.current()->type({kPublicSchema, "sh"})->id == sh);
    CHECK(live.current()->type({kPublicSchema, "sh"})->kind == TypeObject::Kind::Domain);
    // Drops take the grants with them.
    (void)live.apply(DropTypeDdl{{kPublicSchema, "d"}, true});
    (void)live.apply(DropSequenceDdl{{kPublicSchema, "s"}});
    (void)live.apply(DropTypeDdl{{kPublicSchema, "sh"}, false});
    CHECK(live.current()->acls().empty());
    (void)live.apply(DropRoleDdl{{"r"}});
    CHECK(!db.snapshot().contains_key({}, type_record_key(d_id)));
    CHECK(!db.snapshot().contains_key({}, sequence_record_key(s_id)));
    db.close();
  }
}

TEST_CASE("catalog: a type's and a sequence's records and name entries must agree at load; a damaged one is refused",
          "[sql][catalog][roles]") {
  sql_test::TempDir dir{"catalog_types_damage"};
  std::uint32_t d_id = 0, s_id = 0;
  Bytes domain, domain_entry, seq, seq_entry;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(CreateDomainDdl{{kPublicSchema, "d"}, SqlType{SqlKind::Decimal, 0, 10, 2}});
    (void)live.apply(CreateSequenceDdl{{kPublicSchema, "s"}});
    d_id = live.current()->type({kPublicSchema, "d"})->id;
    s_id = live.current()->sequence({kPublicSchema, "s"})->id;
    REQUIRE(db.snapshot().get({}, type_record_key(d_id), domain));
    REQUIRE(db.snapshot().get({}, type_name_key("d"), domain_entry));
    REQUIRE(db.snapshot().get({}, sequence_record_key(s_id), seq));
    REQUIRE(db.snapshot().get({}, name_entry_key("s"), seq_entry));
    db.close();
  }
  auto put = [&](const Bytes& key, const Bytes& value) {
    auto db = DB::open(dir.path());
    db.put({}, key, value);
    db.close();
  };
  auto restore = [&] {
    put(type_record_key(d_id), domain);
    put(type_name_key("d"), domain_entry);
    put(sequence_record_key(s_id), seq);
    put(name_entry_key("s"), seq_entry);
  };
  auto refused_with = [&](const Bytes& key, const Bytes& value) {
    put(key, value);
    {
      auto db = DB::open(dir.path());
      CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
      (void)db.del({}, key);
      db.close();
    }
    restore();
  };
  for (const auto& [key, record] : {std::pair{type_record_key(d_id), domain}, std::pair{sequence_record_key(s_id), seq}}) {
    INFO("record size " << record.size());
    for (std::size_t n = 0; n < record.size(); ++n) {
      refused_with(key, Bytes(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    auto later = record;
    later[1] = std::byte{2};
    refused_with(key, later);
    auto longer = record;
    longer.push_back(std::byte{0});
    refused_with(key, longer);
  }
  refused_with(type_record_key(d_id + 1), domain);
  refused_with(sequence_record_key(s_id + 1), seq);
  auto changed = [](Bytes r, std::size_t at, std::byte b) {
    r[at] = b;
    return r;
  };
  // The domain's kind (after the format, the id and the name "d"), then its base type's kind.
  constexpr std::size_t kKindAt = 2 + 4 + 2 + 1;
  refused_with(type_record_key(d_id), changed(domain, kKindAt, std::byte{9}));
  refused_with(type_record_key(d_id), changed(domain, kKindAt + 1, std::byte{0xEE}));
  // A name entry of another kind, or missing.
  refused_with(type_name_key("d"), changed(domain_entry, 0, std::byte{5}));
  refused_with(name_entry_key("s"), changed(seq_entry, 0, std::byte{1}));
  refused_with(type_name_key("e"), domain_entry);
  {
    auto db = DB::open(dir.path());
    (void)db.del({}, name_entry_key("s"));
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    db.close();
  }
  restore();
  auto db = DB::open(dir.path());
  LiveCatalog live{db};
  CHECK(live.current()->type({kPublicSchema, "d"})->base == SqlType{SqlKind::Decimal, 0, 10, 2});
  db.close();
}

TEST_CASE("catalog: a procedure is a record and a name entry of its own namespace, survives a reopen, and is replaced or dropped",
          "[sql][catalog][procedure]") {
  sql_test::TempDir dir{"catalog_procedure"};
  auto refused = [](LiveCatalog& live, const Ddl& ddl) -> std::string {
    try {
      (void)live.apply(ddl);
    } catch (const DdlRefused& e) {
      return std::string{e.sqlstate()};
    }
    return {};
  };
  std::uint32_t p_id = 0;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_t());
    auto t_id = live.current()->table({kPublicSchema, "t"})->id;
    (void)live.apply(create_p());
    auto cat = live.current();
    REQUIRE(cat->procedure({kPublicSchema, "p"}) != nullptr);
    p_id = cat->procedure({kPublicSchema, "p"})->id;
    CHECK(p_id > t_id);  // one id counter
    CHECK(cat->procedure({kPublicSchema, "p"})->args == neword_args());
    CHECK(cat->procedure({kPublicSchema, "p"})->definition == create_p().definition);
    // A namespace of its own: a procedure and a table may share a name.
    (void)live.apply(create_p("t"));
    CHECK(live.current()->table({kPublicSchema, "t"}) != nullptr);
    CHECK(live.current()->procedure({kPublicSchema, "t"}) != nullptr);
    (void)live.apply(CreateTableDdl{{kPublicSchema, "p"}, {col("id", SqlKind::BigInt)}, {"id"}});
    CHECK(refused(live, DropTableDdl{{kPublicSchema, "t"}}).empty());  // no dependency recorded: nothing refuses the drop
    CHECK(live.current()->procedure({kPublicSchema, "t"}) != nullptr);
    // One procedure per name: no overloading, and OR REPLACE keeps the arguments.
    CHECK(refused(live, create_p()) == "42723");
    auto other = create_p("p", true);
    other.args.pop_back();
    CHECK(refused(live, other) == "0A000");
    other = create_p("p", true);
    other.args[0].name = "warehouse";
    CHECK(refused(live, other) == "0A000");
    auto twice = create_p("q");
    twice.args[1].name = "w_id";
    CHECK(refused(live, twice) == "42P13");
    CHECK(live.current()->procedure({kPublicSchema, "q"}) == nullptr);
    (void)live.apply(create_p("p", true, "BEGIN NULL; END"));
    CHECK(live.current()->procedure({kPublicSchema, "p"})->id == p_id);  // replaced in place
    CHECK(live.current()->procedure({kPublicSchema, "p"})->definition.find("NULL") != std::string::npos);
    (void)live.apply(create_p("r", true));  // OR REPLACE of nothing creates it
    CHECK(live.current()->procedure({kPublicSchema, "r"}) != nullptr);
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    auto cat = live.current();
    REQUIRE(cat->procedure({kPublicSchema, "p"}) != nullptr);
    CHECK(cat->procedure({kPublicSchema, "p"})->id == p_id);
    CHECK(cat->procedure({kPublicSchema, "p"})->args == neword_args());
    CHECK(cat->procedure({kPublicSchema, "p"})->definition.find("NULL") != std::string::npos);
    CHECK(cat->procedures().size() == 3);
    // A DROP's argument list names the input arguments, or all of them; modifiers
    // are not compared, and VARCHAR is TEXT.
    using Types = std::vector<SqlType>;
    CHECK(refused(live, DropProcedureDdl{{kPublicSchema, "p"}, Types{SqlType{SqlKind::BigInt}, SqlType{SqlKind::Text}}}) == "42883");
    CHECK(refused(live, DropProcedureDdl{{kPublicSchema, "p"}, Types{SqlType{SqlKind::Integer}}}) == "42883");
    CHECK(live.apply(DropProcedureDdl{{kPublicSchema, "p"}, Types{SqlType{SqlKind::Integer}}, true}) == live.current());  // IF EXISTS: no-op
    (void)live.apply(DropProcedureDdl{{kPublicSchema, "p"}, Types{SqlType{SqlKind::Integer}, SqlType{SqlKind::Text}}});
    CHECK(count_prefix(db, procedure_record_key(p_id)) == 0);
    CHECK(count_prefix(db, procedure_name_key("p")) == 0);
    (void)live.apply(DropProcedureDdl{{kPublicSchema, "t"}, Types{SqlType{SqlKind::Integer}, SqlType{SqlKind::Varchar, 3},
                                                 SqlType{SqlKind::Decimal, 0, 5, 0}}});
    (void)live.apply(DropProcedureDdl{{kPublicSchema, "r"}});
    CHECK(refused(live, DropProcedureDdl{{kPublicSchema, "r"}}) == "42883");
    CHECK(live.apply(DropProcedureDdl{{kPublicSchema, "r"}, std::nullopt, true}) == live.current());
    CHECK(live.current()->table({kPublicSchema, "p"}) != nullptr);  // the table of that name stays
    db.close();
  }
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    CHECK(live.current()->procedures().empty());
    // Dropped and created again: another object.
    (void)live.apply(create_p());
    CHECK(live.current()->procedure({kPublicSchema, "p"})->id > p_id);
    db.close();
  }
}

TEST_CASE("catalog: a procedure's record and name entry must agree at load, and a damaged record is refused",
          "[sql][catalog][procedure]") {
  sql_test::TempDir dir{"catalog_procedure_damage"};
  std::uint32_t p_id = 0;
  Bytes record, entry;
  {
    auto db = DB::open(dir.path());
    LiveCatalog live{db};
    (void)live.apply(create_p());
    p_id = live.current()->procedure({kPublicSchema, "p"})->id;
    REQUIRE(db.snapshot().get({}, procedure_record_key(p_id), record));
    REQUIRE(db.snapshot().get({}, procedure_name_key("p"), entry));
    db.close();
  }
  auto put = [&](const Bytes& key, const Bytes& value) {
    auto db = DB::open(dir.path());
    db.put({}, key, value);
    db.close();
  };
  auto refused_with = [&](const Bytes& key, const Bytes& value) {
    put(key, value);
    {
      auto db = DB::open(dir.path());
      CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
      (void)db.del({}, key);
      db.close();
    }
    put(procedure_record_key(p_id), record);
    put(procedure_name_key("p"), entry);
  };
  INFO("record size " << record.size());
  for (std::size_t n = 0; n < record.size(); ++n) {
    refused_with(procedure_record_key(p_id), Bytes(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(n)));
  }
  auto changed = [&](std::size_t at, std::byte b) {
    auto r = record;
    r[at] = b;
    return r;
  };
  refused_with(procedure_record_key(p_id), changed(1, std::byte{2}));  // a later format
  auto longer = record;
  longer.push_back(std::byte{0});
  refused_with(procedure_record_key(p_id), longer);
  refused_with(procedure_record_key(p_id + 1), record);  // under another id's key
  // The first argument's mode, then its type's kind: past the format, the id,
  // the name "p", the definition and the argument count; the mode, then the name "w_id".
  auto mode_at = std::size_t{2 + 4 + 2 + 1 + 4} + create_p().definition.size() + 2;
  auto kind_at = mode_at + 1 + 2 + 4;
  REQUIRE(std::to_integer<int>(record[mode_at]) == 1);  // In
  REQUIRE(std::to_integer<int>(record[kind_at]) == static_cast<int>(SqlKind::Integer));
  refused_with(procedure_record_key(p_id), changed(mode_at, std::byte{9}));
  refused_with(procedure_record_key(p_id), changed(kind_at, std::byte{0xEE}));
  // No name entry; an entry of another kind or id; an entry with no record.
  {
    auto db = DB::open(dir.path());
    (void)db.del({}, procedure_name_key("p"));
    CHECK_THROWS_AS(LiveCatalog{db}, std::runtime_error);
    db.close();
  }
  put(procedure_name_key("p"), entry);
  auto other_kind = entry;
  other_kind[0] = std::byte{3};
  refused_with(procedure_name_key("p"), other_kind);
  auto other_id = entry;
  other_id[4] = std::byte{static_cast<unsigned char>(std::to_integer<int>(other_id[4]) + 1)};
  refused_with(procedure_name_key("p"), other_id);
  refused_with(procedure_name_key("q"), entry);
  {
    auto db = DB::open(dir.path());
    LiveCatalog ok{db};  // consistent again
    CHECK(ok.current()->procedure({kPublicSchema, "p"}) != nullptr);
    db.close();
  }
}
