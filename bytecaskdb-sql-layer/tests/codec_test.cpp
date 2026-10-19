// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Property tests for the key codec and the row format
// (docs/sql_layer_types_design.md): encode then decode gives the value back,
// memcmp order on encoded keys is the typed order, for every part type, ASC
// and DESC, with nulls first; and a row shorter than its layout reads the
// columns it predates as null, in both directions.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

import bytecask.sql.row_codec;

using namespace bytecask::sql;

namespace {

using OptI64 = std::optional<std::int64_t>;
using OptF64 = std::optional<double>;
using OptBool = std::optional<bool>;
using OptBytes = std::optional<Bytes>;

constexpr auto kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr auto kInf = std::numeric_limits<double>::infinity();

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

auto random_f64(std::mt19937_64& rng) -> double {
  switch (rng() % 12) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return kInf;
    case 3: return -kInf;
    case 4: return kNaN;
    case 5: return -kNaN;  // the sign bit set: must encode as the one NaN
    case 6: return std::numeric_limits<double>::denorm_min();
    case 7: return -std::numeric_limits<double>::max();
    case 8: return static_cast<double>(static_cast<std::int64_t>(rng() % 2000) - 1000) / 8;
    default: {
      auto d = std::bit_cast<double>(rng());
      return d != d ? 1.5 : d;  // a random bit pattern that is NaN was case 4
    }
  }
}

auto random_bytes(std::mt19937_64& rng) -> Bytes {
  Bytes b(rng() % 6);
  // A small alphabet with 0x00 and 0xFF makes prefixes, escapes and ties common.
  constexpr std::array<std::uint8_t, 4> kAlphabet{0x00, 0x01, 0xFE, 0xFF};
  for (auto& x : b) x = std::byte{kAlphabet[rng() % kAlphabet.size()]};
  return b;
}

// SQL's order on doubles: NaN equals NaN and is above everything; -0.0 is 0.0.
auto f64_order(double a, double b) -> std::strong_ordering {
  auto a_nan = a != a;
  auto b_nan = b != b;
  if (a_nan || b_nan) return a_nan <=> b_nan;
  if (a < b) return std::strong_ordering::less;
  if (a > b) return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

auto bytes_order(const Bytes& a, const Bytes& b) -> std::strong_ordering {
  return std::lexicographical_compare_three_way(
      a.begin(), a.end(), b.begin(), b.end(),
      [](std::byte x, std::byte y) { return std::to_integer<int>(x) <=> std::to_integer<int>(y); });
}

// Nulls first, then by value: the order an ASC part must reproduce.
template <typename T>
auto typed_order(const std::optional<T>& a, const std::optional<T>& b) -> std::strong_ordering {
  if (!a || !b) return a.has_value() <=> b.has_value();
  if constexpr (std::is_same_v<T, Bytes>) {
    return bytes_order(*a, *b);
  } else if constexpr (std::is_same_v<T, double>) {
    return f64_order(*a, *b);
  } else {
    return *a <=> *b;
  }
}

auto reversed(std::strong_ordering o) -> std::strong_ordering {
  return o == std::strong_ordering::less ? std::strong_ordering::greater
                                         : o == std::strong_ordering::greater ? std::strong_ordering::less
                                                                              : o;
}

auto memcmp_order(const Bytes& a, const Bytes& b) -> std::strong_ordering {
  auto n = std::min(a.size(), b.size());
  auto r = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (r != 0) return r <=> 0;
  return a.size() <=> b.size();
}

template <typename T>
void put(Bytes& out, const std::optional<T>& v, KeyPart part) {
  if (!v) {
    key_put_null(out, part);
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    key_put_i64(out, *v, part);
  } else if constexpr (std::is_same_v<T, double>) {
    key_put_f64(out, *v, part);
  } else if constexpr (std::is_same_v<T, bool>) {
    key_put_bool(out, *v, part);
  } else {
    key_put_bytes(out, *v, part);
  }
}

template <typename T>
auto read(KeyReader& r, KeyPart part) -> std::optional<T> {
  if constexpr (std::is_same_v<T, std::int64_t>) {
    return r.read_i64(part);
  } else if constexpr (std::is_same_v<T, double>) {
    return r.read_f64(part);
  } else if constexpr (std::is_same_v<T, bool>) {
    return r.read_bool(part);
  } else {
    return r.read_bytes(part);
  }
}

// What decoding gives back: the canonical value of equal values.
auto canonical(double v) -> double { return v != v ? kNaN : v == 0.0 ? 0.0 : v; }

constexpr std::array<KeyPart, 4> kI64Parts{{{PhysType::I64, false, false},
                                            {PhysType::I64, true, false},
                                            {PhysType::I64, false, true},
                                            {PhysType::I64, true, true}}};

auto with_type(KeyPart p, PhysType t) -> KeyPart { return {t, p.nullable, p.desc}; }

// Encodes one part after a prefix, checks the typed order of two values
// against memcmp order on their keys, and the round trip of each.
template <typename T, typename Gen>
void check_single_part(PhysType type, Gen gen, std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  for (int i = 0; i < 3000; ++i) {
    auto part = with_type(kI64Parts[rng() % 4], type);
    auto maybe = [&]() -> std::optional<T> {
      if (part.nullable && rng() % 4 == 0) return std::nullopt;
      return gen(rng);
    };
    auto a = maybe();
    auto b = maybe();
    Bytes ka, kb;
    key_put_prefix(ka, kNsIndex, 9);
    key_put_prefix(kb, kNsIndex, 9);
    put(ka, a, part);
    put(kb, b, part);
    auto want = typed_order(a, b);
    if (part.desc) want = reversed(want);
    CHECK(memcmp_order(ka, kb) == want);

    KeyReader r{ka, kKeyPrefixBytes};
    auto got = read<T>(r, part);
    CHECK(r.ok());
    CHECK(r.at_end());
    CHECK(r.last_was_null() == !a);
    if constexpr (std::is_same_v<T, double>) {
      if (a) {
        REQUIRE(got);
        CHECK(std::bit_cast<std::uint64_t>(*got) == std::bit_cast<std::uint64_t>(canonical(*a)));
      } else {
        CHECK_FALSE(got);
      }
    } else {
      CHECK(got == a);
    }
  }
}

struct Composite {
  OptBytes name;  // nullable bytes, DESC
  std::int64_t id;  // non-nullable i64
  OptF64 score;  // nullable f64
  OptBool flag;  // nullable bool
};

constexpr KeyPart kNamePart{PhysType::Bytes, true, true};
constexpr KeyPart kIdPart{PhysType::I64, false, false};
constexpr KeyPart kScorePart{PhysType::F64, true, false};
constexpr KeyPart kFlagPart{PhysType::Bool, true, false};

auto encode(const Composite& c) -> Bytes {
  Bytes k;
  key_put_prefix(k, kNsIndex, 7);
  put(k, c.name, kNamePart);
  key_put_i64(k, c.id, kIdPart);
  put(k, c.score, kScorePart);
  put(k, c.flag, kFlagPart);
  return k;
}

auto composite_order(const Composite& a, const Composite& b) -> std::strong_ordering {
  auto o = reversed(typed_order(a.name, b.name));
  if (std::is_neq(o)) return o;
  o = a.id <=> b.id;
  if (std::is_neq(o)) return o;
  o = typed_order(a.score, b.score);
  if (std::is_neq(o)) return o;
  return typed_order(a.flag, b.flag);
}

auto random_composite(std::mt19937_64& rng) -> Composite {
  return {rng() % 4 == 0 ? std::nullopt : OptBytes{random_bytes(rng)}, random_i64(rng) % 4,
          rng() % 4 == 0 ? std::nullopt : OptF64{random_f64(rng)},
          rng() % 4 == 0 ? std::nullopt : OptBool{rng() % 2 == 0}};
}

}  // namespace

TEST_CASE("key codec: i64 parts order and round-trip, ASC and DESC, with and without a marker", "[sql][codec]") {
  check_single_part<std::int64_t>(PhysType::I64, random_i64, 42);
}

TEST_CASE("key codec: f64 parts order as SQL does, -0.0 and NaN canonical", "[sql][codec]") {
  check_single_part<double>(PhysType::F64, random_f64, 43);
  // NaN above +inf, whatever its sign bit; -0.0 equal to 0.0.
  Bytes nan, neg_nan, inf, zero, neg_zero;
  key_put_f64(nan, kNaN, {PhysType::F64});
  key_put_f64(neg_nan, -kNaN, {PhysType::F64});
  key_put_f64(inf, kInf, {PhysType::F64});
  key_put_f64(zero, 0.0, {PhysType::F64});
  key_put_f64(neg_zero, -0.0, {PhysType::F64});
  CHECK(nan == neg_nan);
  CHECK(memcmp_order(inf, nan) == std::strong_ordering::less);
  CHECK(zero == neg_zero);
}

TEST_CASE("key codec: bool parts order false before true", "[sql][codec]") {
  check_single_part<bool>(PhysType::Bool, [](std::mt19937_64& rng) { return rng() % 2 == 0; }, 44);
}

TEST_CASE("key codec: bytes parts order by value, a prefix first", "[sql][codec]") {
  check_single_part<Bytes>(PhysType::Bytes, random_bytes, 45);
  // The terminator sorts below an escape whatever follows the part: "a" then
  // an i64 of all 0xFF bytes still sorts before "a\0" then anything.
  Bytes a, a0;
  key_put_bytes(a, Bytes{std::byte{'a'}}, {PhysType::Bytes});
  key_put_i64(a, -1, {PhysType::I64});  // 0x7F 0xFF… after the sign flip
  key_put_bytes(a0, Bytes{std::byte{'a'}, std::byte{0}}, {PhysType::Bytes});
  key_put_i64(a0, std::numeric_limits<std::int64_t>::min(), {PhysType::I64});
  CHECK(memcmp_order(a, a0) == std::strong_ordering::less);
}

TEST_CASE("key codec: composite keys order part by part, nulls first (last under DESC)", "[sql][codec]") {
  std::mt19937_64 rng{7};
  for (int i = 0; i < 5000; ++i) {
    auto a = random_composite(rng);
    auto b = random_composite(rng);
    if (rng() % 2 == 0) b.name = a.name;  // ties on the leading parts
    if (rng() % 2 == 0) b.id = a.id;
    if (rng() % 2 == 0) b.score = a.score;
    CHECK(memcmp_order(encode(a), encode(b)) == composite_order(a, b));

    // Every part reads back, and skip() steps over each type.
    auto k = encode(a);
    KeyReader r{k, kKeyPrefixBytes};
    CHECK(r.read_bytes(kNamePart) == a.name);
    CHECK(r.read_i64(kIdPart) == a.id);
    auto score = r.read_f64(kScorePart);
    CHECK(score.has_value() == a.score.has_value());
    if (score) CHECK(std::bit_cast<std::uint64_t>(*score) == std::bit_cast<std::uint64_t>(canonical(*a.score)));
    CHECK(r.read_bool(kFlagPart) == a.flag);
    CHECK(r.ok());
    CHECK(r.at_end());

    KeyReader s{k, kKeyPrefixBytes};
    CHECK(s.skip(kNamePart));
    CHECK(s.skip(kIdPart));
    auto after_id = s.pos();
    CHECK(s.skip(kScorePart));
    CHECK(s.skip(kFlagPart));
    CHECK(s.at_end());
    KeyReader t{k, after_id};
    CHECK(t.read_f64(kScorePart).has_value() == a.score.has_value());
  }
}

TEST_CASE("key codec: a prefix's successor bounds every key under it", "[sql][codec]") {
  std::mt19937_64 rng{3};
  for (int i = 0; i < 1000; ++i) {
    auto a = random_composite(rng);
    Bytes prefix;
    key_put_prefix(prefix, kNsIndex, 7);
    put(prefix, a.name, kNamePart);
    key_put_i64(prefix, a.id, kIdPart);
    auto full = encode(a);
    auto succ = key_successor(prefix);
    CHECK(memcmp_order(prefix, full) != std::strong_ordering::greater);
    CHECK(memcmp_order(full, succ) == std::strong_ordering::less);
  }
  CHECK(key_successor(Bytes{std::byte{0x01}, std::byte{0xFF}}) == Bytes{std::byte{0x02}});
  CHECK(key_successor(Bytes{std::byte{0xFF}}).empty());
}

TEST_CASE("key codec: malformed keys are reported, not over-read", "[sql][codec]") {
  // Every part type, cut at every byte.
  for (auto part : {KeyPart{PhysType::I64, true, false}, KeyPart{PhysType::F64, true, true},
                    KeyPart{PhysType::Bool, true, false}, KeyPart{PhysType::Bytes, true, true},
                    KeyPart{PhysType::Bytes, false, false}}) {
    Bytes k;
    switch (part.type) {
      case PhysType::I64: key_put_i64(k, -5, part); break;
      case PhysType::F64: key_put_f64(k, -2.5, part); break;
      case PhysType::Bool: key_put_bool(k, true, part); break;
      case PhysType::Bytes: key_put_bytes(k, Bytes{std::byte{1}, std::byte{0}, std::byte{2}}, part); break;
    }
    for (std::size_t n = 0; n < k.size(); ++n) {
      KeyReader r{BytesView{k}.first(n)};
      CHECK_FALSE(r.skip(part));
      CHECK_FALSE(r.ok());
    }
    KeyReader whole{k};
    CHECK(whole.skip(part));
    CHECK(whole.at_end());
  }
  Bytes bad_marker{std::byte{0x07}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                   std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  KeyReader r{bad_marker};
  CHECK_FALSE(r.read_i64({PhysType::I64, true}).has_value());
  CHECK_FALSE(r.ok());
  CHECK_FALSE(r.read_i64({PhysType::I64, false}).has_value());  // sticky

  Bytes bad_escape{std::byte{'a'}, std::byte{0}, std::byte{7}, std::byte{0}, std::byte{0}};
  KeyReader e{bad_escape};
  CHECK_FALSE(e.read_bytes({PhysType::Bytes}).has_value());
  CHECK_FALSE(e.ok());

  Bytes bad_bool{std::byte{2}};
  KeyReader b{bad_bool};
  CHECK_FALSE(b.read_bool({PhysType::Bool}).has_value());
  CHECK_FALSE(b.ok());
}

TEST_CASE("row format: every physical type round-trips, null or not", "[sql][codec]") {
  const std::array<ColumnDef, 6> defs{{
      {0, PhysType::I64, false},
      {1, PhysType::Bytes, true},
      {2, PhysType::I64, true},
      {3, PhysType::F64, true},
      {4, PhysType::Bool, true},
      {5, PhysType::Bytes, true},
  }};
  auto layout = make_layout(defs);
  CHECK(layout.bitmap_bytes == 8);
  CHECK(layout.fixed_start == kRowHeaderBytes + 8);
  CHECK(layout.fixed_end == layout.fixed_start + 4 * kFixedColumnBytes);
  CHECK(layout.nvar == 2);
  CHECK(layout.find(1)->pos == 0);
  CHECK(layout.find(5)->pos == 1);

  std::mt19937_64 rng{11};
  RowWriter w{layout};
  for (int i = 0; i < 2000; ++i) {
    w.reset();
    CHECK(w.missing_required() == std::optional<std::uint32_t>{0});
    auto id = random_i64(rng);
    auto name = rng() % 3 == 0 ? std::nullopt : OptBytes{random_bytes(rng)};
    auto qty = rng() % 3 == 0 ? std::nullopt : OptI64{random_i64(rng)};
    auto score = rng() % 3 == 0 ? std::nullopt : OptF64{random_f64(rng)};
    auto flag = rng() % 3 == 0 ? std::nullopt : OptBool{rng() % 2 == 0};
    auto blob = rng() % 3 == 0 ? std::nullopt : OptBytes{random_bytes(rng)};
    REQUIRE(w.set_i64(0, id));
    if (name) REQUIRE(w.set_bytes(1, *name));
    if (qty) REQUIRE(w.set_i64(2, *qty));
    if (score) REQUIRE(w.set_f64(3, *score));
    if (flag) REQUIRE(w.set_bool(4, *flag));
    if (blob) REQUIRE(w.set_bytes(5, *blob));
    CHECK_FALSE(w.missing_required().has_value());
    Bytes row;
    REQUIRE(w.finish(row));
    REQUIRE(row_valid(row));
    CHECK(row_generation(row) == 1);

    CHECK_FALSE(row_is_null(row, *layout.find(0)));
    CHECK(row_i64(row, layout.find(0)->pos) == id);
    CHECK(row_is_null(row, *layout.find(1)) == !name);
    if (name) {
      auto v = row_bytes(row, layout.find(1)->pos);
      REQUIRE(v);
      CHECK(Bytes(v->begin(), v->end()) == *name);
    }
    CHECK(row_is_null(row, *layout.find(2)) == !qty);
    CHECK(row_i64(row, layout.find(2)->pos) == qty.value_or(0));  // a null fixed column reads as zero
    CHECK(row_is_null(row, *layout.find(3)) == !score);
    auto got_score = row_f64(row, layout.find(3)->pos);
    REQUIRE(got_score);
    CHECK(std::bit_cast<std::uint64_t>(*got_score) == std::bit_cast<std::uint64_t>(score.value_or(0.0)));
    CHECK(row_is_null(row, *layout.find(4)) == !flag);
    CHECK(row_bool(row, layout.find(4)->pos) == flag.value_or(false));
    CHECK(row_is_null(row, *layout.find(5)) == !blob);
    if (blob) {
      auto v = row_bytes(row, layout.find(5)->pos);
      REQUIRE(v);
      CHECK(Bytes(v->begin(), v->end()) == *blob);
    }

    RowWriter copy{layout};
    copy.copy_from(row);
    Bytes again;
    REQUIRE(copy.finish(again));
    CHECK(again == row);
  }
  // The type is checked on set.
  CHECK_FALSE(w.set_f64(0, 1.0));
  CHECK_FALSE(w.set_bytes(2, Bytes{}));
  CHECK_FALSE(w.set_i64(1, 1));
  CHECK_FALSE(w.set_bool(3, true));
  CHECK_FALSE(w.set_i64(9, 1));
  CHECK_FALSE(w.set_null(9));
}

TEST_CASE("row format: a row shorter than the layout reads as null, in both directions", "[sql][codec]") {
  const std::array<ColumnDef, 2> old_defs{{{0, PhysType::I64, false}, {1, PhysType::Bytes, true}}};
  auto old_layout = make_layout(old_defs);
  RowWriter w{old_layout};
  REQUIRE(w.set_i64(0, 5));
  REQUIRE(w.set_bytes(1, Bytes{std::byte{'a'}}));
  Bytes old_row;
  REQUIRE(w.finish(old_row));

  // ADD COLUMN, three times: a fixed column, a variable one, a fixed one,
  // after the row was written. Existing positions do not move.
  auto new_layout = old_layout;
  const std::array<ColumnDef, 3> added{
      {{2, PhysType::I64, true}, {3, PhysType::Bytes, true}, {4, PhysType::Bool, true}}};
  REQUIRE(append_columns(new_layout, added));
  CHECK(new_layout.find(0)->pos == old_layout.find(0)->pos);
  CHECK(new_layout.find(1)->pos == old_layout.find(1)->pos);
  CHECK(new_layout.find(2)->pos == old_layout.fixed_end);
  CHECK(new_layout.find(3)->pos == 1);
  CHECK(new_layout.find(4)->pos == old_layout.fixed_end + kFixedColumnBytes);

  // The newer layout reads the old row: the known columns, null for the rest.
  CHECK(row_i64(old_row, new_layout.find(0)->pos) == 5);
  CHECK_FALSE(row_is_null(old_row, *new_layout.find(1)));
  CHECK(row_is_null(old_row, *new_layout.find(2)));
  CHECK(row_i64(old_row, new_layout.find(2)->pos) == 0);
  CHECK(row_is_null(old_row, *new_layout.find(3)));
  CHECK(row_bytes(old_row, new_layout.find(3)->pos)->empty());
  CHECK(row_is_null(old_row, *new_layout.find(4)));
  CHECK(row_bool(old_row, new_layout.find(4)->pos) == false);

  // And the reverse: an old reader ignores what a new row added, and finds
  // its variable column though the fixed region grew.
  RowWriter nw{new_layout};
  REQUIRE(nw.set_i64(0, 9));
  REQUIRE(nw.set_bytes(1, Bytes{std::byte{'b'}}));
  REQUIRE(nw.set_i64(2, 10));
  REQUIRE(nw.set_bytes(3, Bytes{std::byte{'z'}}));
  REQUIRE(nw.set_bool(4, true));
  Bytes new_row;
  REQUIRE(nw.finish(new_row));
  CHECK(row_i64(new_row, old_layout.find(0)->pos) == 9);
  CHECK_FALSE(row_is_null(new_row, *old_layout.find(1)));
  auto v = row_bytes(new_row, old_layout.find(1)->pos);
  REQUIRE(v);
  CHECK(Bytes(v->begin(), v->end()) == Bytes{std::byte{'b'}});

  // A new-layout copy of the old row carries the old values and nulls.
  RowWriter copy{new_layout};
  copy.copy_from(old_row);
  Bytes copied;
  REQUIRE(copy.finish(copied));
  CHECK(row_i64(copied, new_layout.find(0)->pos) == 5);
  CHECK(row_is_null(copied, *new_layout.find(2)));
  CHECK(row_is_null(copied, *new_layout.find(4)));
}

TEST_CASE("row format: the bitmap is sized at create and has a ceiling", "[sql][codec]") {
  std::vector<ColumnDef> defs;
  for (std::uint32_t i = 0; i < 48; ++i) defs.push_back({i, PhysType::I64, true});
  auto layout = make_layout(defs);
  CHECK(layout.bitmap_bytes == 8);  // 48 ids and 16 to spare fit 64 bits
  CHECK(layout.id_ceiling() == 64);
  REQUIRE(append_columns(layout, std::array{ColumnDef{63, PhysType::Bool, true}}));
  // Past the ceiling: refused, and nothing appended.
  auto before = layout.columns.size();
  CHECK_FALSE(append_columns(layout, std::array{ColumnDef{64, PhysType::I64, true}}));
  CHECK_FALSE(append_columns(layout, std::array{ColumnDef{50, PhysType::I64, true}, ColumnDef{64, PhysType::I64, true}}));
  CHECK(layout.columns.size() == before);
  CHECK(layout.find(50) == nullptr);

  defs.push_back({48, PhysType::I64, true});
  CHECK(make_layout(defs).bitmap_bytes == 16);
  CHECK(make_layout(std::span<const ColumnDef>{}).bitmap_bytes == 8);

  // The fixed region ends where the header's 16-bit var_table does.
  std::vector<ColumnDef> many;
  for (std::uint32_t i = 0; i < 9000; ++i) many.push_back({i, PhysType::I64, true});
  auto wide = make_layout(std::span<const ColumnDef>{many}.first(8000));
  CHECK(wide.fixed_end <= 0xFFFF);
  CHECK(wide.columns.size() == 8000);
  CHECK_FALSE(append_columns(wide, std::span<const ColumnDef>{many}.subspan(8000, 1000)));
  CHECK(wide.columns.size() == 8000);
  CHECK_THROWS_AS(make_layout(many), std::length_error);
  // An id large enough that the bitmap alone outgrows the 16-bit header
  // fields, with only variable columns: refused at create.
  CHECK_THROWS_AS(make_layout(std::array{ColumnDef{600'000, PhysType::Bytes, true}}), std::length_error);
  CHECK(make_layout(std::array{ColumnDef{500'000, PhysType::Bytes, true}}).fixed_start <= 0xFFFF);
  CHECK(narrow<std::uint16_t>(0xFFFFu) == 0xFFFF);
  CHECK_THROWS_AS(narrow<std::uint16_t>(0x10000u), std::overflow_error);
  RowWriter ww{wide};
  for (std::uint32_t i = 0; i < 8000; ++i) REQUIRE(ww.set_i64(i, i));
  Bytes wide_row;
  REQUIRE(ww.finish(wide_row));
  CHECK(row_valid(wide_row));
  CHECK(row_i64(wide_row, wide.find(7999)->pos) == 7999);

  // The row carries the size, and a bit past the bitmap is clear.
  RowWriter w{layout};
  for (std::uint32_t i = 0; i < 48; ++i) REQUIRE(w.set_i64(i, i));
  REQUIRE(w.set_bool(63, true));
  Bytes row;
  REQUIRE(w.finish(row));
  CHECK(row_present_bit(row, 63));
  CHECK_FALSE(row_present_bit(row, 64));
  CHECK_FALSE(row_present_bit(row, 1000));
}

TEST_CASE("row format: layout_valid holds the format's invariants, and finish refuses a broken layout", "[sql][codec]") {
  const std::array<ColumnDef, 2> defs{{{0, PhysType::I64, false}, {1, PhysType::Bytes, true}}};
  auto good = make_layout(defs);
  CHECK(layout_valid(good));
  auto past_ceiling = good;
  past_ceiling.columns[1].col_id = good.id_ceiling();
  CHECK_FALSE(layout_valid(past_ceiling));
  auto odd_bitmap = good;
  odd_bitmap.bitmap_bytes = 7;
  CHECK_FALSE(layout_valid(odd_bitmap));
  auto bad_type = good;
  bad_type.columns[0].type = static_cast<PhysType>(9);
  CHECK_FALSE(layout_valid(bad_type));
  auto off_word = good;
  off_word.columns[0].pos += 1;
  CHECK_FALSE(layout_valid(off_word));
  auto bad_slot = good;
  bad_slot.columns[1].pos = 5;
  CHECK_FALSE(layout_valid(bad_slot));
  // A writer over a broken layout appends nothing rather than writing past the bitmap.
  RowWriter w{past_ceiling};
  REQUIRE(w.set_i64(0, 1));
  REQUIRE(w.set_bytes(good.id_ceiling(), Bytes{std::byte{1}}));
  Bytes out;
  CHECK_FALSE(w.finish(out));
  CHECK(out.empty());
}

TEST_CASE("row format: result layouts come from the plan's types", "[sql][codec]") {
  const std::array<PhysType, 3> types{PhysType::I64, PhysType::Bytes, PhysType::F64};
  auto layout = make_result_layout(types);
  REQUIRE(layout.columns.size() == 3);
  CHECK(layout.find(0)->type == PhysType::I64);
  CHECK(layout.find(1)->type == PhysType::Bytes);
  CHECK(layout.find(2)->type == PhysType::F64);
  CHECK(layout.find(2)->nullable);
  CHECK(layout.nvar == 1);
}

TEST_CASE("row format: malformed rows are rejected, not over-read, and the limit holds", "[sql][codec]") {
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
    CHECK_FALSE(row_bytes(cut, layout.find(1)->pos).has_value());
    CHECK(row_is_null(cut, *layout.find(1)));
  }
  // An offset inside the header or off a word boundary is not a column.
  CHECK_FALSE(row_word(row, 0).has_value());
  CHECK_FALSE(row_word(row, layout.fixed_start + 1).has_value());
  // A var_end that runs past the row is a malformed row.
  auto bad = row;
  bad[layout.fixed_end] = std::byte{0xFF};
  CHECK_FALSE(row_valid(bad));
  CHECK_FALSE(row_bytes(bad, 0).has_value());

  // The limit is checked before anything is appended.
  Bytes out;
  CHECK_FALSE(w.finish(out, row.size() - 1));
  CHECK(out.empty());
  CHECK(w.finish(out, row.size()));
  CHECK(out == row);
}
