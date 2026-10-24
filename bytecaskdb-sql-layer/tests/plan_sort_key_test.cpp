// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The sort-key builder (bc_sort_key_*, #579): memcmp order on the keys it
// builds is PostgreSQL's ORDER BY order, for every type, direction and NULL
// placement and for keys of several parts; equal values give equal keys.
// The expected orders are PostgreSQL 16's, from the queries quoted with
// them; the random cases check against a comparator written from
// PostgreSQL's rules. Then the builder's own: a column with its NULL, a
// sorter fed by it, the charge, and the calls it refuses.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::kKeyEntryBytes;
using bytecask::sql::kViewEntryBytes;
using bytecask::sql::PhysType;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1;

constexpr auto kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr auto kInf = std::numeric_limits<double>::infinity();
constexpr auto kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr auto kI64Max = std::numeric_limits<std::int64_t>::max();
const bc_value kNull = std::monostate{};
constexpr std::array<PhysType, 1> kBytesRow{PhysType::Bytes};
constexpr std::array<PhysType, 1> kI64Row{PhysType::I64};

struct SortPart {
  PhysType type;
  std::uint32_t dir;
  std::uint32_t nulls;
};

// What encode_keys reads: the parts of the key, and the values of every
// row's parts as the plan's parameters, row by row.
// Destroyed at exit, which is harmless: the plans are captureless, so they
// read their spec from here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
std::vector<SortPart> g_parts;
#pragma clang diagnostic pop
std::size_t g_rows = 0;

// One result row per row of parameters: its sort key.
void encode_keys(bc_ctx* cx) {
  const auto n = static_cast<std::uint32_t>(g_parts.size());
  for (std::uint32_t r = 0; r < g_rows; ++r) {
    bc_key k = bc_sort_key_begin(cx);
    for (std::uint32_t p = 0; p < n; ++p) {
      const auto i = r * n + p;
      const auto [type, dir, nulls] = g_parts[p];
      if (bc_param_null(cx, i)) {
        bc_sort_key_null(cx, k, dir, nulls);
        continue;
      }
      switch (type) {
        case PhysType::I64: bc_sort_key_i64(cx, k, bc_param_i64(cx, i), dir, nulls); break;
        case PhysType::F64: bc_sort_key_f64(cx, k, bc_param_f64(cx, i), dir, nulls); break;
        case PhysType::Bool: bc_sort_key_bool(cx, k, bc_param_bool(cx, i), dir, nulls); break;
        case PhysType::Bytes: bc_sort_key_bytes(cx, k, bc_param_bytes(cx, i), dir, nulls); break;
      }
    }
    bc_emit_begin(cx);
    bc_emit_bytes(cx, bc_sort_key_end(cx, k));
    bc_emit_end(cx);
  }
}

auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, std::span<const PhysType> result = {},
         bc_budget budget = {}) -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(result), f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, budget, false, 0);
}

using Row = std::vector<bc_value>;

// The sort key of each row, as encode_keys builds them.
auto encode(Fixture& f, const std::vector<SortPart>& parts, const std::vector<Row>& rows) -> std::vector<Bytes> {
  g_parts = parts;
  g_rows = rows.size();
  std::vector<bc_value> flat;
  for (const auto& r : rows) flat.insert(flat.end(), r.begin(), r.end());
  REQUIRE(run(f, encode_keys, std::move(flat), kBytesRow) == BC_COMMITTED);
  std::vector<Bytes> keys;
  for (auto& r : f.results(kBytesRow)) keys.push_back(std::get<Bytes>(r[0]));
  REQUIRE(keys.size() == rows.size());
  return keys;
}

// The rows' indexes in the order a sorter gives: by key bytes, ties in input order.
auto key_order(const std::vector<Bytes>& keys) -> std::vector<std::size_t> {
  std::vector<std::size_t> order(keys.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, [&](auto a, auto b) {
    return std::ranges::lexicographical_compare(keys[a], keys[b]);
  });
  return order;
}

// The rows sorted by their keys.
auto sorted(Fixture& f, const std::vector<SortPart>& parts, const std::vector<Row>& rows) -> std::vector<Row> {
  std::vector<Row> out;
  for (auto i : key_order(encode(f, parts, rows))) out.push_back(rows[i]);
  return out;
}

// One value of one part, compared as PostgreSQL's ORDER BY does: NULL by its
// placement whatever the direction; doubles with -0 equal to 0 and NaN equal
// to itself and above every other value; bytes and text bytewise, a prefix
// first; the value order reversed under DESC.
auto pg_cmp(const bc_value& a, const bc_value& b, const SortPart& part) -> std::weak_ordering {
  const auto an = std::holds_alternative<std::monostate>(a), bn = std::holds_alternative<std::monostate>(b);
  if (an || bn) {
    if (an && bn) return std::weak_ordering::equivalent;
    const auto first = part.nulls == BC_SORT_NULLS_FIRST;
    return an == first ? std::weak_ordering::less : std::weak_ordering::greater;
  }
  auto c = std::weak_ordering::equivalent;
  switch (part.type) {
    case PhysType::I64: c = std::get<std::int64_t>(a) <=> std::get<std::int64_t>(b); break;
    case PhysType::Bool: c = std::get<bool>(a) <=> std::get<bool>(b); break;
    case PhysType::Bytes: {
      const auto& x = std::get<Bytes>(a);
      const auto& y = std::get<Bytes>(b);
      c = std::lexicographical_compare_three_way(x.begin(), x.end(), y.begin(), y.end());
      break;
    }
    case PhysType::F64: {
      const auto x = std::get<double>(a), y = std::get<double>(b);
      const auto xn = x != x, yn = y != y;
      if (xn || yn) {
        c = xn == yn ? std::weak_ordering::equivalent : xn ? std::weak_ordering::greater : std::weak_ordering::less;
      } else {
        c = x < y ? std::weak_ordering::less : x > y ? std::weak_ordering::greater : std::weak_ordering::equivalent;
      }
      break;
    }
  }
  if (part.dir == BC_SORT_ASC || std::is_eq(c)) return c;
  return std::is_lt(c) ? std::weak_ordering::greater : std::weak_ordering::less;
}

auto pg_row_cmp(const Row& a, const Row& b, const std::vector<SortPart>& parts) -> std::weak_ordering {
  for (std::size_t p = 0; p < parts.size(); ++p) {
    if (auto c = pg_cmp(a[p], b[p], parts[p]); std::is_neq(c)) return c;
  }
  return std::weak_ordering::equivalent;
}

auto i64(std::int64_t v) -> bc_value { return v; }
auto f64(double v) -> bc_value { return v; }
auto boolean(bool v) -> bc_value { return v; }
auto raw(std::initializer_list<unsigned char> b) -> bc_value {
  Bytes out;
  for (auto c : b) out.push_back(std::byte{c});
  return out;
}
auto one(std::vector<bc_value> values) -> std::vector<Row> {
  std::vector<Row> rows;
  for (auto& v : values) rows.push_back({std::move(v)});
  return rows;
}

// Doubles compared as PostgreSQL does: -0 equals 0, NaN equals NaN.
auto same_rows(const std::vector<Row>& a, const std::vector<Row>& b, const std::vector<SortPart>& parts) -> bool {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::is_neq(pg_row_cmp(a[i], b[i], parts))) return false;
  }
  return true;
}

constexpr std::array<std::uint32_t, 2> kDirs{BC_SORT_ASC, BC_SORT_DESC};
constexpr std::array<std::uint32_t, 2> kPlacements{BC_SORT_NULLS_FIRST, BC_SORT_NULLS_LAST};

// The same order PostgreSQL gives, under the four combinations: `asc` is
// ORDER BY v ASC with the values present, NULL placed by each combination.
void check_four(Fixture& f, PhysType type, const std::vector<bc_value>& asc) {
  for (auto dir : kDirs) {
    for (auto nulls : kPlacements) {
      INFO("dir " << dir << ", nulls " << nulls);
      std::vector<bc_value> want = asc;
      if (dir == BC_SORT_DESC) std::ranges::reverse(want);
      if (nulls == BC_SORT_NULLS_FIRST) {
        want.insert(want.begin(), kNull);
      } else {
        want.push_back(kNull);
      }
      // Fed in a scrambled order, NULL in the middle.
      std::vector<bc_value> in = asc;
      std::ranges::reverse(in);
      std::rotate(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(in.size() / 2), in.end());
      in.insert(in.begin() + static_cast<std::ptrdiff_t>(in.size() / 2), kNull);
      const std::vector<SortPart> parts{{type, dir, nulls}};
      CHECK(same_rows(sorted(f, parts, one(in)), one(want), parts));
    }
  }
}

}  // namespace

TEST_CASE("sort key: each type sorts as PostgreSQL orders it, in each direction and NULL placement",
          "[sql][algebra][sort_key]") {
  Fixture f;
  SECTION("BIGINT") {
    // SELECT v FROM (VALUES (-9223372036854775808::int8), (9223372036854775807), (0), (-1), (1), (NULL)) t(v)
    // ORDER BY v ASC NULLS FIRST: NULL, -9223372036854775808, -1, 0, 1, 9223372036854775807
    check_four(f, PhysType::I64, {i64(kI64Min), i64(-1), i64(0), i64(1), i64(kI64Max)});
  }
  SECTION("DOUBLE PRECISION, NaN above infinity and -0 equal to 0") {
    // ORDER BY v ASC NULLS LAST over -Infinity, NaN, Infinity, -0.0, -1.5, 1e-300, -1e-308, ±5e-324, NULL:
    // -Infinity -1.5 -1e-308 -5e-324 -0 5e-324 1e-300 Infinity NaN NULL
    check_four(f, PhysType::F64,
               {f64(-kInf), f64(-1.5), f64(-1e-308), f64(-5e-324), f64(-0.0), f64(5e-324), f64(1e-300), f64(kInf),
                f64(kNaN)});
  }
  SECTION("BOOLEAN") {
    // ORDER BY v DESC NULLS FIRST: NULL, true, false
    check_four(f, PhysType::Bool, {boolean(false), boolean(true)});
  }
  SECTION("BYTEA: the empty value, embedded zero bytes, a prefix of another value") {
    // ORDER BY v ASC NULLS LAST over bytea:
    // '' 00 0000 00ff 01 61 6100 6162 ff NULL
    check_four(f, PhysType::Bytes,
               {raw({}), raw({0x00}), raw({0x00, 0x00}), raw({0x00, 0xff}), raw({0x01}), raw({0x61}),
                raw({0x61, 0x00}), raw({0x61, 0x62}), raw({0xff})});
  }
  SECTION("TEXT, bytewise: the layer's collation, PostgreSQL's COLLATE \"C\"") {
    // ORDER BY v COLLATE "C" ASC NULLS LAST: '' B a ab z é NULL
    check_four(f, PhysType::Bytes, {bytes(""), bytes("B"), bytes("a"), bytes("ab"), bytes("z"), bytes("\xc3\xa9")});
  }
  SECTION("-0 and 0 make one key; so does every NaN") {
    const std::vector<SortPart> parts{{PhysType::F64, BC_SORT_ASC, BC_SORT_NULLS_LAST}};
    auto quiet = std::bit_cast<double>(std::uint64_t{0x7FF8'0000'0000'0001});
    auto negative = std::bit_cast<double>(std::uint64_t{0xFFF8'0000'0000'0000});
    auto keys = encode(f, parts, one({f64(-0.0), f64(0.0), f64(kNaN), f64(quiet), f64(negative)}));
    CHECK(keys[0] == keys[1]);
    CHECK(keys[2] == keys[3]);
    CHECK(keys[2] == keys[4]);
  }
}

TEST_CASE("sort key: parts after the first keep their own direction and NULL placement", "[sql][algebra][sort_key]") {
  Fixture f;
  // SELECT a, b FROM (VALUES (1,2),(1,NULL),(2,1),(2,NULL),(1,1)) t(a,b) ORDER BY a DESC, b NULLS FIRST:
  // 2:N 2:1 1:N 1:1 1:2
  const std::vector<SortPart> parts{{PhysType::I64, BC_SORT_DESC, BC_SORT_NULLS_FIRST},
                                    {PhysType::I64, BC_SORT_ASC, BC_SORT_NULLS_FIRST}};
  std::vector<Row> in{{i64(1), i64(2)}, {i64(1), kNull}, {i64(2), i64(1)}, {i64(2), kNull}, {i64(1), i64(1)}};
  std::vector<Row> want{{i64(2), kNull}, {i64(2), i64(1)}, {i64(1), kNull}, {i64(1), i64(1)}, {i64(1), i64(2)}};
  CHECK(sorted(f, parts, in) == want);

  // A text part that is a prefix of another, then a part that would sort the
  // other way if the first part's end were not its own: ORDER BY s, n DESC.
  const std::vector<SortPart> text_first{{PhysType::Bytes, BC_SORT_ASC, BC_SORT_NULLS_LAST},
                                         {PhysType::I64, BC_SORT_DESC, BC_SORT_NULLS_FIRST}};
  std::vector<Row> t_in{{bytes("ab"), i64(0)}, {bytes("a"), i64(kI64Min)}, {raw({0x61, 0x00}), i64(5)},
                        {bytes("a"), i64(kI64Max)}, {kNull, i64(1)}, {bytes(""), kNull}};
  std::vector<Row> t_want{{bytes(""), kNull}, {bytes("a"), i64(kI64Max)}, {bytes("a"), i64(kI64Min)},
                          {raw({0x61, 0x00}), i64(5)}, {bytes("ab"), i64(0)}, {kNull, i64(1)}};
  CHECK(sorted(f, text_first, t_in) == t_want);
  // The same under DESC on the text: ORDER BY s DESC NULLS LAST, n DESC.
  const std::vector<SortPart> text_desc{{PhysType::Bytes, BC_SORT_DESC, BC_SORT_NULLS_LAST},
                                        {PhysType::I64, BC_SORT_DESC, BC_SORT_NULLS_FIRST}};
  std::vector<Row> d_want{{bytes("ab"), i64(0)}, {raw({0x61, 0x00}), i64(5)}, {bytes("a"), i64(kI64Max)},
                          {bytes("a"), i64(kI64Min)}, {bytes(""), kNull}, {kNull, i64(1)}};
  CHECK(sorted(f, text_desc, t_in) == d_want);
}

TEST_CASE("sort key: random keys of one to three parts sort as PostgreSQL's rules order them",
          "[sql][algebra][sort_key]") {
  Fixture f;
  std::mt19937_64 rng{579};
  constexpr std::array<PhysType, 4> kTypes{PhysType::I64, PhysType::F64, PhysType::Bool, PhysType::Bytes};
  // Few distinct values, edges among them, so leading parts tie often.
  auto value = [&](PhysType t) -> bc_value {
    if (rng() % 6 == 0) return kNull;
    switch (t) {
      case PhysType::I64: {
        constexpr std::array<std::int64_t, 7> kEdges{kI64Min, kI64Min + 1, -1, 0, 1, kI64Max - 1, kI64Max};
        return rng() % 2 == 0 ? kEdges[rng() % kEdges.size()] : static_cast<std::int64_t>(rng() % 5) - 2;
      }
      case PhysType::F64: {
        constexpr std::array<double, 10> kEdges{-kInf, -1.5, -5e-324, -0.0, 0.0, 5e-324, 1.5, kInf, kNaN, -kNaN};
        return kEdges[rng() % kEdges.size()];
      }
      case PhysType::Bool: return rng() % 2 == 0;
      case PhysType::Bytes: {
        constexpr std::array<unsigned char, 5> kAlphabet{0x00, 0x01, 0x61, 0xfe, 0xff};
        Bytes b(rng() % 4);
        for (auto& c : b) c = std::byte{kAlphabet[rng() % kAlphabet.size()]};
        return b;
      }
    }
    return kNull;
  };
  for (int round = 0; round < 200; ++round) {
    std::vector<SortPart> parts(1 + rng() % 3);
    for (auto& p : parts) {
      p = {kTypes[rng() % kTypes.size()], kDirs[rng() % 2], kPlacements[rng() % 2]};
    }
    std::vector<Row> rows(40);
    for (auto& r : rows) {
      for (const auto& p : parts) r.push_back(value(p.type));
    }
    INFO("round " << round);
    auto keys = encode(f, parts, rows);
    std::vector<std::size_t> want(rows.size());
    std::iota(want.begin(), want.end(), std::size_t{0});
    std::ranges::stable_sort(want, [&](auto a, auto b) { return std::is_lt(pg_row_cmp(rows[a], rows[b], parts)); });
    CHECK(key_order(keys) == want);
    // Equal keys exactly where PostgreSQL sees equal rows: a sort stable on
    // the key is stable on the row.
    for (std::size_t a = 0; a < rows.size(); ++a) {
      for (std::size_t b = 0; b < rows.size(); ++b) {
        CHECK((keys[a] == keys[b]) == std::is_eq(pg_row_cmp(rows[a], rows[b], parts)));
      }
    }
  }
}

namespace {

void insert_t(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_param(cx, r, kNameCol, 1);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}

// SELECT id FROM t ORDER BY name $1 NULLS $2, id DESC: a sorter keyed by
// bc_sort_key_col, $1 the name's direction and $2 its placement.
void order_by_name(bc_ctx* cx) {
  auto dir = static_cast<uint32_t>(bc_param_i64(cx, 0));
  auto nulls = static_cast<uint32_t>(bc_param_i64(cx, 1));
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_key k = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, k, row, kNameCol, dir, nulls);
    bc_sort_key_col(cx, k, row, kIdCol, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    bc_sorter_add(cx, s, bc_sort_key_end(cx, k), row);
  }
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p, kIdCol);
    bc_emit_end(cx);
  }
}

auto ids(std::initializer_list<std::int64_t> v) -> std::vector<std::vector<bc_value>> {
  std::vector<std::vector<bc_value>> out;
  for (auto i : v) out.push_back({i});
  return out;
}

// A sort key of a BOOLEAN, a BIGINT and a NULL built in each step of a
// scan: what each step holds past its scan's entry, as a result row.
void sort_key_charges(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    auto before = bc_ctx_held_for_testing(cx);
    bc_key k = bc_sort_key_begin(cx);
    bc_sort_key_bool(cx, k, true, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sort_key_i64(cx, k, 7, BC_SORT_DESC, BC_SORT_NULLS_LAST);
    bc_sort_key_null(cx, k, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    (void)bc_sort_key_end(cx, k);
    auto held = bc_ctx_held_for_testing(cx) - before;
    bc_emit_begin(cx);
    bc_emit_i64(cx, static_cast<int64_t>(held));
    bc_emit_end(cx);
  }
}
// A sort key over the budget: its builder fits, its part does not.
void sort_key_over_budget(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  bc_sort_key_bytes(cx, k, bc_param_bytes(cx, 0), BC_SORT_ASC, BC_SORT_NULLS_LAST);
}

void sort_key_on_index_key(bc_ctx* cx) {
  bc_key k = bc_key_begin(cx, kIdx);
  bc_sort_key_i64(cx, k, 1, BC_SORT_ASC, BC_SORT_NULLS_LAST);
}
void index_part_on_sort_key(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  bc_key_i64(cx, k, 1);
}
void index_null_on_sort_key(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  bc_key_null(cx, k);
}
void key_end_on_sort_key(bc_ctx* cx) { (void)bc_key_end(cx, bc_sort_key_begin(cx)); }
void key_fits_on_sort_key(bc_ctx* cx) { (void)bc_key_fits(cx, bc_sort_key_begin(cx)); }
void sort_end_on_index_key(bc_ctx* cx) { (void)bc_sort_key_end(cx, bc_key_begin(cx, kTbl)); }
void sort_key_ended_twice(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  (void)bc_sort_key_end(cx, k);
  (void)bc_sort_key_end(cx, k);
}
void sort_part_after_end(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  (void)bc_sort_key_end(cx, k);
  bc_sort_key_null(cx, k, BC_SORT_ASC, BC_SORT_NULLS_LAST);
}
void sort_key_bad_dir(bc_ctx* cx) { bc_sort_key_i64(cx, bc_sort_key_begin(cx), 1, 2, BC_SORT_NULLS_LAST); }
void sort_key_bad_nulls(bc_ctx* cx) { bc_sort_key_null(cx, bc_sort_key_begin(cx), BC_SORT_ASC, 2); }
// A sort key's view used after the step it was made in.
void sort_key_after_step(bc_ctx* cx) {
  bc_view key = bc_empty(cx);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_key k = bc_sort_key_begin(cx);
    bc_sort_key_i64(cx, k, 1, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    key = bc_sort_key_end(cx, k);
  }
  (void)bc_bytes_eq(cx, key, key);
}
// A key past the engine's key ceiling: a sort key never reaches the engine.
void long_sort_key(bc_ctx* cx) {
  bc_key k = bc_sort_key_begin(cx);
  bc_sort_key_bytes(cx, k, bc_param_bytes(cx, 0), BC_SORT_DESC, BC_SORT_NULLS_LAST);
  bc_emit_begin(cx);
  bc_emit_bytes(cx, bc_sort_key_end(cx, k));
  bc_emit_end(cx);
}

}  // namespace

TEST_CASE("sort key: bc_sort_key_col keys a column with its NULL, and a sorter orders rows by it",
          "[sql][algebra][sort_key][structures]") {
  Fixture f;
  for (auto [id, name] : std::vector<std::pair<std::int64_t, bc_value>>{
           {3, bytes("cherry")}, {1, bytes("apple")}, {2, kNull}, {4, bytes("apple")}, {5, bytes("")}}) {
    REQUIRE(run(f, insert_t, {id, name}) == BC_COMMITTED);
  }
  auto order = [&](std::int64_t dir, std::int64_t nulls) {
    REQUIRE(run(f, order_by_name, {dir, nulls}, kI64Row) == BC_COMMITTED);
    return f.results(kI64Row);
  };
  // ORDER BY name, id DESC: PostgreSQL's ASC NULLS LAST, its default.
  CHECK(order(BC_SORT_ASC, BC_SORT_NULLS_LAST) == ids({5, 4, 1, 3, 2}));
  CHECK(order(BC_SORT_ASC, BC_SORT_NULLS_FIRST) == ids({2, 5, 4, 1, 3}));
  // ORDER BY name DESC, id DESC: DESC NULLS FIRST, the default.
  CHECK(order(BC_SORT_DESC, BC_SORT_NULLS_FIRST) == ids({2, 3, 4, 1, 5}));
  CHECK(order(BC_SORT_DESC, BC_SORT_NULLS_LAST) == ids({3, 4, 1, 5, 2}));
}

TEST_CASE("sort key: charged as its parts are added, and owned by its step", "[sql][algebra][sort_key][memory]") {
  Fixture f;
  REQUIRE(run(f, insert_t, {std::int64_t{1}, bytes("a")}) == BC_COMMITTED);
  REQUIRE(run(f, insert_t, {std::int64_t{2}, bytes("b")}) == BC_COMMITTED);
  // Its builder, its bytes (each part a marker and its value) and its view,
  // the same at every step: the last step's key went with it.
  REQUIRE(run(f, sort_key_charges, {}, kI64Row) == BC_COMMITTED);
  const auto each = static_cast<std::int64_t>(kKeyEntryBytes + (1 + 1) + (1 + 8) + 1 + kViewEntryBytes);
  CHECK(f.results(kI64Row) == ids({each, each}));
  // Past the budget, at the part that passes it.
  Bytes value(1000, std::byte{0x61});
  const auto budget = kViewEntryBytes + kKeyEntryBytes + 1000;
  CHECK(run(f, sort_key_over_budget, {value}, {}, {.max_reads = 0, .max_bytes = budget, .deadline_ns = 0}) ==
        BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(run(f, sort_key_over_budget, {value}, {}, {.max_reads = 0, .max_bytes = budget + 3, .deadline_ns = 0}) ==
        BC_COMMITTED);
  // A view from a step that ended is stale.
  CHECK(run(f, sort_key_after_step, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // No key ceiling: a sort key is the plan's, and never reaches the engine.
  Bytes big(70000, std::byte{0x61});
  REQUIRE(run(f, long_sort_key, {big}, kBytesRow) == BC_COMMITTED);
  CHECK(std::get<Bytes>(f.results(kBytesRow)[0][0]).size() == big.size() + 3);
}

TEST_CASE("sort key: a sort key and an index's key do not take each other's calls; bad arguments are BC_E_HANDLE",
          "[sql][algebra][sort_key]") {
  Fixture f;
  for (auto plan : {sort_key_on_index_key, index_part_on_sort_key, index_null_on_sort_key, key_end_on_sort_key,
                    key_fits_on_sort_key, sort_end_on_index_key, sort_key_ended_twice, sort_part_after_end,
                    sort_key_bad_dir, sort_key_bad_nulls}) {
    CHECK(run(f, plan, {}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_HANDLE);
  }
}
