// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// key_codec.cppm — the mem-comparable key encoding of docs/plan_api_design.md,
// *Key encoding*: namespace(1) id(4, big-endian) parts…, each part encoded so
// that memcmp order is the typed order and the key parses back without a
// length prefix. DESC parts are not implemented yet. Definitions are in
// key_codec.cpp.

module;
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
