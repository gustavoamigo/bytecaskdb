// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// key_codec.cppm — the mem-comparable key encoding of
// docs/types_design.md, *Key encoding*: namespace(1) id(4,
// big-endian) parts…, each part encoded so that memcmp order is the typed
// order and the key parses back without a length prefix. A nullable part has
// a marker byte (0x00 null, 0x01 present); a DESC part is inverted bit for
// bit, marker included. Definitions are in key_codec.cpp.

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
};

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
