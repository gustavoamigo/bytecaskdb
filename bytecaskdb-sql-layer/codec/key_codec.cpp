// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// key_codec.cpp — the key encoding's definitions; see key_codec.cppm.

module;
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

module bytecask.sql.key_codec;

namespace bytecask::sql {

namespace {

constexpr std::byte kNullMarker{0x00};
constexpr std::byte kPresentMarker{0x01};
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

void key_put_f64(Bytes& out, double v, KeyPart part) {
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
