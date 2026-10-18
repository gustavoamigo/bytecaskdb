// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Property tests for the key codec and the row format: encode then decode
// gives the value back, memcmp order on encoded keys is the typed order, and
// a row shorter than the layout reads the columns it predates as null.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <compare>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <tuple>

#include "bytecaskdb-sql-layer/codec/key_codec.hpp"
#include "bytecaskdb-sql-layer/codec/row_codec.hpp"

using namespace bytecask::sql;

namespace {

using OptI64 = std::optional<std::int64_t>;
using OptBytes = std::optional<Bytes>;

auto random_i64(std::mt19937_64& rng) -> std::int64_t {
  // Mix the edges in: they are where a sign flip goes wrong.
  switch (rng() % 8) {
    case 0: return std::numeric_limits<std::int64_t>::min();
    case 1: return std::numeric_limits<std::int64_t>::max();
    case 2: return 0;
    case 3: return -1;
    case 4: return static_cast<std::int64_t>(rng() % 200) - 100;
    default: return static_cast<std::int64_t>(rng());
  }
}

auto random_bytes(std::mt19937_64& rng) -> Bytes {
  Bytes b(rng() % 6);
  // A small alphabet with 0x00 and 0xFF makes prefixes, escapes and ties common.
  constexpr std::array<std::uint8_t, 4> kAlphabet{0x00, 0x01, 0xFE, 0xFF};
  for (auto& x : b) x = std::byte{kAlphabet[rng() % kAlphabet.size()]};
  return b;
}

// Nulls first, then by value: the order the encoding must reproduce.
template <typename T>
auto typed_cmp(const std::optional<T>& a, const std::optional<T>& b) -> std::strong_ordering {
  if (!a || !b) return a.has_value() <=> b.has_value();
  if constexpr (std::is_same_v<T, Bytes>) {
    return std::lexicographical_compare_three_way(
        a->begin(), a->end(), b->begin(), b->end(),
        [](std::byte x, std::byte y) { return std::to_integer<int>(x) <=> std::to_integer<int>(y); });
  } else {
    return *a <=> *b;
  }
}

struct Composite {
  OptBytes name;  // nullable bytes part
  std::int64_t id;  // non-nullable i64 part
};

auto encode(const Composite& c) -> Bytes {
  Bytes k;
  key_put_prefix(k, kNsIndex, 7);
  if (c.name) {
    key_put_bytes(k, *c.name, true);
  } else {
    key_put_null(k);
  }
  key_put_i64(k, c.id, false);
  return k;
}

auto memcmp_order(const Bytes& a, const Bytes& b) -> std::strong_ordering {
  auto n = std::min(a.size(), b.size());
  auto r = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (r != 0) return r <=> 0;
  return a.size() <=> b.size();
}

}  // namespace

TEST_CASE("key codec: i64 and bytes parts round-trip", "[sql][codec]") {
  std::mt19937_64 rng{42};
  for (int i = 0; i < 2000; ++i) {
    auto v = random_i64(rng);
    auto b = random_bytes(rng);
    auto nullable = rng() % 2 == 0;
    Bytes k;
    key_put_prefix(k, kNsRow, 0x01020304);
    key_put_i64(k, v, nullable);
    key_put_bytes(k, b, nullable);
    key_put_null(k);

    REQUIRE(k[0] == std::byte{kNsRow});
    KeyReader r{k, kKeyPrefixBytes};
    CHECK(r.read_i64(nullable) == v);
    CHECK(r.read_bytes(nullable) == b);
    CHECK_FALSE(r.read_i64(true).has_value());
    CHECK(r.last_was_null());
    CHECK(r.ok());
    CHECK(r.at_end());
  }
}

TEST_CASE("key codec: memcmp order is the typed order", "[sql][codec]") {
  std::mt19937_64 rng{7};
  for (int i = 0; i < 5000; ++i) {
    Composite a{rng() % 4 == 0 ? std::nullopt : OptBytes{random_bytes(rng)}, random_i64(rng)};
    Composite b{rng() % 4 == 0 ? std::nullopt : OptBytes{random_bytes(rng)}, random_i64(rng)};
    if (rng() % 4 == 0) b.name = a.name;  // ties on the first part
    auto typed = typed_cmp(a.name, b.name);
    if (std::is_eq(typed)) typed = a.id <=> b.id;
    CHECK(memcmp_order(encode(a), encode(b)) == typed);
  }
}

TEST_CASE("key codec: a prefix's successor bounds every key under it", "[sql][codec]") {
  std::mt19937_64 rng{3};
  for (int i = 0; i < 1000; ++i) {
    Composite a{random_bytes(rng), random_i64(rng)};
    Bytes prefix;
    key_put_prefix(prefix, kNsIndex, 7);
    key_put_bytes(prefix, *a.name, true);
    auto full = encode(a);
    auto succ = key_successor(prefix);
    CHECK(memcmp_order(prefix, full) != std::strong_ordering::greater);
    CHECK(memcmp_order(full, succ) == std::strong_ordering::less);
  }
  CHECK(key_successor(Bytes{std::byte{0x01}, std::byte{0xFF}}) == Bytes{std::byte{0x02}});
  CHECK(key_successor(Bytes{std::byte{0xFF}}).empty());
}

TEST_CASE("key codec: malformed keys are reported, not over-read", "[sql][codec]") {
  Bytes k;
  key_put_bytes(k, Bytes{std::byte{1}, std::byte{0}}, true);
  for (std::size_t n = 0; n < k.size(); ++n) {
    KeyReader r{BytesView{k}.first(n)};
    CHECK_FALSE(r.read_bytes(true).has_value());
    CHECK_FALSE(r.ok());
  }
  Bytes bad_marker{std::byte{0x07}};
  KeyReader r{bad_marker};
  CHECK_FALSE(r.read_i64(true).has_value());
  CHECK_FALSE(r.ok());
}

TEST_CASE("row format: fixed i64 columns and one variable column round-trip", "[sql][codec]") {
  const std::array<ColumnDef, 3> defs{{
      {0, PhysType::I64, false},
      {1, PhysType::Bytes, true},
      {2, PhysType::I64, true},
  }};
  auto layout = make_layout(defs);
  CHECK(layout.fixed_start % 8 == 0);
  std::mt19937_64 rng{11};
  RowWriter w{layout};
  for (int i = 0; i < 2000; ++i) {
    w.reset();
    auto id = random_i64(rng);
    auto name = rng() % 3 == 0 ? std::nullopt : OptBytes{random_bytes(rng)};
    auto qty = rng() % 3 == 0 ? std::nullopt : OptI64{random_i64(rng)};
    REQUIRE(w.set_i64(0, id));
    if (name) REQUIRE(w.set_bytes(1, *name));
    if (qty) REQUIRE(w.set_i64(2, *qty));
    CHECK_FALSE(w.missing_required());
    Bytes row;
    REQUIRE(w.finish(row));
    REQUIRE(row_valid(row));

    const auto& c_id = *layout.find(0);
    const auto& c_name = *layout.find(1);
    const auto& c_qty = *layout.find(2);
    CHECK_FALSE(row_is_null(row, c_id));
    CHECK(row_i64(row, c_id.pos) == id);
    CHECK(row_is_null(row, c_name) == !name);
    if (name) {
      auto v = row_bytes(row, c_name.pos);
      REQUIRE(v);
      CHECK(Bytes(v->begin(), v->end()) == *name);
    }
    CHECK(row_is_null(row, c_qty) == !qty);
    CHECK(row_i64(row, c_qty.pos) == qty.value_or(0));  // a null fixed column reads as zero

    RowWriter copy{layout};
    copy.copy_from(row);
    Bytes again;
    REQUIRE(copy.finish(again));
    CHECK(again == row);
  }
}

TEST_CASE("row format: a row shorter than the layout reads as null", "[sql][codec]") {
  const std::array<ColumnDef, 2> old_defs{{{0, PhysType::I64, false}, {1, PhysType::Bytes, true}}};
  auto old_layout = make_layout(old_defs);
  RowWriter w{old_layout};
  REQUIRE(w.set_i64(0, 5));
  REQUIRE(w.set_bytes(1, Bytes{std::byte{'a'}}));
  Bytes old_row;
  REQUIRE(w.finish(old_row));

  // ADD COLUMN, twice: a fixed column and a variable one, after the row was written.
  auto new_layout = old_layout;
  const std::array<ColumnDef, 2> added{{{2, PhysType::I64, true}, {3, PhysType::Bytes, true}}};
  REQUIRE(append_columns(new_layout, added));
  CHECK(new_layout.find(0)->pos == old_layout.find(0)->pos);
  CHECK(new_layout.find(1)->pos == old_layout.find(1)->pos);

  CHECK(row_i64(old_row, new_layout.find(0)->pos) == 5);
  CHECK_FALSE(row_is_null(old_row, *new_layout.find(1)));
  CHECK(row_is_null(old_row, *new_layout.find(2)));
  CHECK(row_i64(old_row, new_layout.find(2)->pos) == 0);
  CHECK(row_is_null(old_row, *new_layout.find(3)));
  CHECK(row_bytes(old_row, new_layout.find(3)->pos)->empty());

  // And the reverse: an old reader ignores what a new row added.
  RowWriter nw{new_layout};
  REQUIRE(nw.set_i64(0, 9));
  REQUIRE(nw.set_i64(2, 10));
  REQUIRE(nw.set_bytes(3, Bytes{std::byte{'z'}}));
  Bytes new_row;
  REQUIRE(nw.finish(new_row));
  CHECK(row_i64(new_row, old_layout.find(0)->pos) == 9);
  CHECK(row_is_null(new_row, *old_layout.find(1)));
}

TEST_CASE("row format: malformed rows are rejected, not over-read", "[sql][codec]") {
  const std::array<ColumnDef, 2> defs{{{0, PhysType::I64, false}, {1, PhysType::Bytes, true}}};
  auto layout = make_layout(defs);
  RowWriter w{layout};
  REQUIRE(w.set_i64(0, 1));
  REQUIRE(w.set_bytes(1, Bytes(10, std::byte{'x'})));
  Bytes row;
  REQUIRE(w.finish(row));
  for (std::size_t n = 0; n < row.size(); ++n) {
    auto cut = BytesView{row}.first(n);
    CHECK_FALSE(row_valid(cut));
    CHECK_FALSE(row_i64(cut, layout.find(0)->pos).has_value());
    CHECK(row_is_null(cut, *layout.find(1)));
  }
  REQUIRE(w.set_bytes(1, Bytes(kMaxRowBytes, std::byte{'x'})));
  Bytes too_big;
  CHECK_FALSE(w.finish(too_big));
  w.reset();
  CHECK(w.missing_required());
}
