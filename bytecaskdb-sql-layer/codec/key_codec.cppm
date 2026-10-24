// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// key_codec.cppm — the mem-comparable key encoding of
// docs/types_design.md, *Key encoding*: namespace(1) id(4,
// big-endian) parts…, each part encoded so that memcmp order is the typed
// order and the key parses back without a length prefix. A nullable part has
// a marker byte (0x00 null, 0x01 present); a DESC part is inverted bit for
// bit, marker included. A sort key (*Sort keys*) has a marker before every
// part that is never inverted, so its NULL placement is its own.

module;
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

export module bytecask.sql.key_codec;

export namespace bytecask::sql {

using Bytes = std::vector<std::byte>;
using BytesView = std::span<const std::byte>;

inline constexpr std::uint8_t kNsCatalog = 0x01;
inline constexpr std::uint8_t kNsRow = 0x02;
inline constexpr std::uint8_t kNsIndex = 0x03;
inline constexpr std::size_t kKeyPrefixBytes = 5;
// The engine's hard ceiling on a key; its configured limit may be lower.
inline constexpr std::size_t kMaxKeyBytes = 65535;

// The physical types of docs/plan_api_design.md; the values are bc_type's.
enum class PhysType : std::uint8_t { I64 = 1, F64 = 2, Bool = 3, Bytes = 4 };

// Whether a column of this type lives in a row's fixed region.
[[nodiscard]] constexpr auto is_fixed(PhysType t) noexcept -> bool { return t != PhysType::Bytes; }

// One part of a key layout: its type, whether it carries a null marker, and
// whether it is inverted (DESC).
struct KeyPart {
  PhysType type;
  bool nullable{false};
  bool desc{false};
  bool real{false};  // a REAL column: a double is keyed as its float rounding, as the row stores it
};

// A double as a float holds it, widened back: PostgreSQL's float8 to
// float4. Infinite at or past 2^128 - 2^103, the first value that rounds
// up; FLT_MAX between that and FLT_MAX; NaN and the infinities as given.
// Never an out-of-range conversion. What a REAL stores and is keyed by.
[[nodiscard]] auto float_rounding(double v) -> double;

void key_put_prefix(Bytes& out, std::uint8_t ns, std::uint32_t id);
// The typed writers use part.nullable and part.desc; the type is the caller's
// to have checked.
void key_put_i64(Bytes& out, std::int64_t v, KeyPart part);
// -0.0 is written as 0.0 and every NaN as one canonical NaN above +inf.
void key_put_f64(Bytes& out, double v, KeyPart part);
void key_put_bool(Bytes& out, bool v, KeyPart part);
void key_put_bytes(Bytes& out, BytesView v, KeyPart part);
// A null part: the marker alone. Only a nullable part can be null.
void key_put_null(Bytes& out, KeyPart part);

// A sort key's part (docs/types_design.md, *Sort keys*): a marker byte,
// never inverted, then for a value the value as a non-nullable key part,
// inverted under DESC. 0x01 marks a value; a null is its marker alone, 0x00
// to sort before every value of the part or 0x02 after.
void sort_put_present(Bytes& out);
void sort_put_null(Bytes& out, bool nulls_first);

// Reads parts back in order. A malformed key makes every later read fail:
// ok() is false and reads return nullopt.
class KeyReader {
public:
  explicit KeyReader(BytesView key, std::size_t pos = 0) : key_{key}, pos_{pos} {}

  // nullopt for a null part or a malformed key; last_was_null() tells them apart.
  [[nodiscard]] auto read_i64(KeyPart part) -> std::optional<std::int64_t>;
  [[nodiscard]] auto read_f64(KeyPart part) -> std::optional<double>;
  [[nodiscard]] auto read_bool(KeyPart part) -> std::optional<bool>;
  [[nodiscard]] auto read_bytes(KeyPart part) -> std::optional<Bytes>;
  // Skips one part, null or not; false if the key is malformed.
  [[nodiscard]] auto skip(KeyPart part) -> bool;

  [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }
  [[nodiscard]] auto last_was_null() const noexcept -> bool { return null_; }
  [[nodiscard]] auto pos() const noexcept -> std::size_t { return pos_; }
  [[nodiscard]] auto at_end() const noexcept -> bool { return pos_ == key_.size(); }
  [[nodiscard]] auto rest() const -> BytesView { return key_.subspan(pos_); }

private:
  auto read_marker(KeyPart part) -> bool;  // false: null or malformed
  auto next_byte(bool desc) -> std::optional<std::byte>;
  auto read_be(bool desc, std::size_t width) -> std::optional<std::uint64_t>;

  BytesView key_;
  std::size_t pos_;
  bool ok_{true};
  bool null_{false};
};

// The smallest key greater than every key with `prefix` as a prefix: the last
// byte below 0xFF incremented and everything after it dropped. Empty if the
// prefix is all 0xFF (no such key).
[[nodiscard]] auto key_successor(BytesView prefix) -> Bytes;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::byte kNullMarker{0x00};
constexpr std::byte kPresentMarker{0x01};
constexpr std::byte kSortNullLast{0x02};
constexpr std::byte kEscape{0xFF};
constexpr std::byte kInvert{0xFF};
constexpr auto kSignBit = std::uint64_t{1} << 63;
constexpr auto kCanonicalNaN = std::uint64_t{0x7FF8'0000'0000'0000};

void put_be(Bytes& out, std::uint64_t v, std::size_t width) {
  for (auto i = width; i-- > 0;) {
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  }
}

// A DESC part is written as its ASC bytes, then inverted in place.
void invert_from(Bytes& out, std::size_t start) {
  for (auto i = start; i < out.size(); ++i) out[i] ^= kInvert;
}

void put_marker(Bytes& out, KeyPart part) {
  if (part.nullable) out.push_back(kPresentMarker);
}

// The order-preserving transform of IEEE 754 bits: positive values get the
// sign bit set, negative values are inverted, so unsigned order is numeric
// order. -0.0 and NaN are canonicalised first.
auto f64_to_ordered(double v) -> std::uint64_t {
  if (v != v) return kCanonicalNaN | kSignBit;
  if (v == 0.0) v = 0.0;  // drops the sign of -0.0
  auto bits = std::bit_cast<std::uint64_t>(v);
  return (bits & kSignBit) != 0 ? ~bits : bits | kSignBit;
}

auto ordered_to_f64(std::uint64_t u) -> double {
  return std::bit_cast<double>((u & kSignBit) != 0 ? u & ~kSignBit : ~u);
}

}  // namespace

void key_put_prefix(Bytes& out, std::uint8_t ns, std::uint32_t id) {
  out.push_back(std::byte{ns});
  put_be(out, id, 4);
}

void key_put_i64(Bytes& out, std::int64_t v, KeyPart part) {
  auto start = out.size();
  put_marker(out, part);
  put_be(out, std::bit_cast<std::uint64_t>(v) ^ kSignBit, 8);
  if (part.desc) invert_from(out, start);
}

auto float_rounding(double v) -> double {
  if (!std::isfinite(v)) return v;
  constexpr auto kFloatMax = static_cast<double>(std::numeric_limits<float>::max());
  constexpr auto kFirstInfinite = 0x1p128 - 0x1p103;
  if (std::fabs(v) >= kFirstInfinite) return std::copysign(std::numeric_limits<double>::infinity(), v);
  if (std::fabs(v) > kFloatMax) return std::copysign(kFloatMax, v);
  return static_cast<double>(static_cast<float>(v));
}

void key_put_f64(Bytes& out, double v, KeyPart part) {
  if (part.real) v = float_rounding(v);
  auto start = out.size();
  put_marker(out, part);
  put_be(out, f64_to_ordered(v), 8);
  if (part.desc) invert_from(out, start);
}

void key_put_bool(Bytes& out, bool v, KeyPart part) {
  auto start = out.size();
  put_marker(out, part);
  out.push_back(v ? std::byte{1} : std::byte{0});
  if (part.desc) invert_from(out, start);
}

void key_put_bytes(Bytes& out, BytesView v, KeyPart part) {
  auto start = out.size();
  put_marker(out, part);
  for (auto b : v) {
    out.push_back(b);
    if (b == std::byte{0}) out.push_back(kEscape);
  }
  out.push_back(std::byte{0});
  out.push_back(std::byte{0});
  if (part.desc) invert_from(out, start);
}

void key_put_null(Bytes& out, KeyPart part) {
  out.push_back(part.desc ? kNullMarker ^ kInvert : kNullMarker);
}

void sort_put_present(Bytes& out) { out.push_back(kPresentMarker); }

void sort_put_null(Bytes& out, bool nulls_first) {
  out.push_back(nulls_first ? kNullMarker : kSortNullLast);
}

auto KeyReader::next_byte(bool desc) -> std::optional<std::byte> {
  if (!ok_) return std::nullopt;
  if (pos_ >= key_.size()) {
    ok_ = false;
    return std::nullopt;
  }
  auto b = key_[pos_++];
  return desc ? b ^ kInvert : b;
}

auto KeyReader::read_be(bool desc, std::size_t width) -> std::optional<std::uint64_t> {
  std::uint64_t u = 0;
  for (std::size_t i = 0; i < width; ++i) {
    auto b = next_byte(desc);
    if (!b) return std::nullopt;
    u = (u << 8) | std::to_integer<std::uint64_t>(*b);
  }
  return u;
}

auto KeyReader::read_marker(KeyPart part) -> bool {
  null_ = false;
  if (!ok_) return false;
  if (!part.nullable) return true;
  auto m = next_byte(part.desc);
  if (!m) return false;
  if (*m == kNullMarker) {
    null_ = true;
    return false;
  }
  if (*m != kPresentMarker) ok_ = false;
  return ok_;
}

auto KeyReader::read_i64(KeyPart part) -> std::optional<std::int64_t> {
  if (!read_marker(part)) return std::nullopt;
  auto u = read_be(part.desc, 8);
  if (!u) return std::nullopt;
  return std::bit_cast<std::int64_t>(*u ^ kSignBit);
}

auto KeyReader::read_f64(KeyPart part) -> std::optional<double> {
  if (!read_marker(part)) return std::nullopt;
  auto u = read_be(part.desc, 8);
  if (!u) return std::nullopt;
  return ordered_to_f64(*u);
}

auto KeyReader::read_bool(KeyPart part) -> std::optional<bool> {
  if (!read_marker(part)) return std::nullopt;
  auto b = next_byte(part.desc);
  if (!b) return std::nullopt;
  if (*b != std::byte{0} && *b != std::byte{1}) {
    ok_ = false;
    return std::nullopt;
  }
  return *b == std::byte{1};
}

auto KeyReader::read_bytes(KeyPart part) -> std::optional<Bytes> {
  if (!read_marker(part)) return std::nullopt;
  Bytes v;
  while (auto b = next_byte(part.desc)) {
    if (*b != std::byte{0}) {
      v.push_back(*b);
      continue;
    }
    auto next = next_byte(part.desc);
    if (!next) break;
    if (*next == std::byte{0}) return v;  // terminator
    if (*next != kEscape) break;
    v.push_back(std::byte{0});
  }
  ok_ = false;
  return std::nullopt;
}

auto KeyReader::skip(KeyPart part) -> bool {
  switch (part.type) {
    case PhysType::I64: (void)read_i64(part); break;
    case PhysType::F64: (void)read_f64(part); break;
    case PhysType::Bool: (void)read_bool(part); break;
    case PhysType::Bytes: (void)read_bytes(part); break;
  }
  return ok_;
}

auto key_successor(BytesView prefix) -> Bytes {
  Bytes out{prefix.begin(), prefix.end()};
  while (!out.empty()) {
    if (out.back() != std::byte{0xFF}) {
      out.back() = static_cast<std::byte>(std::to_integer<unsigned>(out.back()) + 1);
      return out;
    }
    out.pop_back();
  }
  return out;
}

}  // namespace bytecask::sql
