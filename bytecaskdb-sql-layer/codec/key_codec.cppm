// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// key_codec.cppm — the mem-comparable key encoding of docs/plan_api_design.md,
// *Key encoding*: namespace(1) id(4, big-endian) parts…, each part encoded so
// that memcmp order is the typed order and the key parses back without a
// length prefix. DESC parts are not implemented yet.

module;
#include <bit>
#include <cstddef>
#include <cstdint>
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

// The physical types of docs/plan_api_design.md. Only I64 and Bytes are
// implemented by the prototype.
enum class PhysType : std::uint8_t { I64 = 1, F64 = 2, Bool = 3, Bytes = 4 };

// One part of a key layout: its type and whether it carries a null marker.
struct KeyPart {
  PhysType type;
  bool nullable;
};

void key_put_prefix(Bytes& out, std::uint8_t ns, std::uint32_t id);
void key_put_i64(Bytes& out, std::int64_t v, bool nullable);
void key_put_bytes(Bytes& out, BytesView v, bool nullable);
// A null part: only a nullable part can be null.
void key_put_null(Bytes& out);

// Reads parts back in order. A malformed key makes every later read fail:
// ok() is false and reads return nullopt.
class KeyReader {
public:
  explicit KeyReader(BytesView key, std::size_t pos = 0) : key_{key}, pos_{pos} {}

  // nullopt for a null part or a malformed key; is_null() tells them apart.
  [[nodiscard]] auto read_i64(bool nullable) -> std::optional<std::int64_t>;
  [[nodiscard]] auto read_bytes(bool nullable) -> std::optional<Bytes>;
  // Skips one part of the given layout; false if the key is malformed.
  auto skip(KeyPart part) -> bool;

  [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }
  [[nodiscard]] auto last_was_null() const noexcept -> bool { return null_; }
  [[nodiscard]] auto pos() const noexcept -> std::size_t { return pos_; }
  [[nodiscard]] auto at_end() const noexcept -> bool { return pos_ == key_.size(); }
  [[nodiscard]] auto rest() const -> BytesView { return key_.subspan(pos_); }

private:
  auto read_marker(bool nullable) -> bool;  // false: null or malformed

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
constexpr std::byte kEscape{0xFF};
constexpr auto kSignBit = std::uint64_t{1} << 63;

void put_be64(Bytes& out, std::uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
  }
}

}  // namespace

void key_put_prefix(Bytes& out, std::uint8_t ns, std::uint32_t id) {
  out.push_back(std::byte{ns});
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::byte>((id >> shift) & 0xFF));
  }
}

void key_put_i64(Bytes& out, std::int64_t v, bool nullable) {
  if (nullable) out.push_back(kPresentMarker);
  put_be64(out, std::bit_cast<std::uint64_t>(v) ^ kSignBit);
}

void key_put_bytes(Bytes& out, BytesView v, bool nullable) {
  if (nullable) out.push_back(kPresentMarker);
  for (auto b : v) {
    out.push_back(b);
    if (b == std::byte{0}) out.push_back(kEscape);
  }
  out.push_back(std::byte{0});
  out.push_back(std::byte{0});
}

void key_put_null(Bytes& out) { out.push_back(kNullMarker); }

auto KeyReader::read_marker(bool nullable) -> bool {
  null_ = false;
  if (!ok_) return false;
  if (!nullable) return true;
  if (pos_ >= key_.size()) {
    ok_ = false;
    return false;
  }
  auto m = key_[pos_++];
  if (m == kNullMarker) {
    null_ = true;
    return false;
  }
  if (m != kPresentMarker) ok_ = false;
  return ok_;
}

auto KeyReader::read_i64(bool nullable) -> std::optional<std::int64_t> {
  if (!read_marker(nullable)) return std::nullopt;
  if (key_.size() - pos_ < 8) {
    ok_ = false;
    return std::nullopt;
  }
  std::uint64_t u = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    u = (u << 8) | std::to_integer<std::uint64_t>(key_[pos_ + i]);
  }
  pos_ += 8;
  return std::bit_cast<std::int64_t>(u ^ kSignBit);
}

auto KeyReader::read_bytes(bool nullable) -> std::optional<Bytes> {
  if (!read_marker(nullable)) return std::nullopt;
  Bytes v;
  while (pos_ < key_.size()) {
    auto b = key_[pos_++];
    if (b != std::byte{0}) {
      v.push_back(b);
      continue;
    }
    if (pos_ >= key_.size()) break;
    auto next = key_[pos_++];
    if (next == std::byte{0}) return v;  // terminator
    if (next != kEscape) break;
    v.push_back(std::byte{0});
  }
  ok_ = false;
  return std::nullopt;
}

auto KeyReader::skip(KeyPart part) -> bool {
  switch (part.type) {
    case PhysType::I64:
      (void)read_i64(part.nullable);
      break;
    case PhysType::Bytes:
      (void)read_bytes(part.nullable);
      break;
    case PhysType::F64:
    case PhysType::Bool:
      ok_ = false;  // prototype: not implemented
      break;
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
