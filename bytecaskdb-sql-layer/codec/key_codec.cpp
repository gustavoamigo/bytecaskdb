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
