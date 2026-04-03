module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

export module bytecask.data_entry;

import bytecask.types;
import bytecask.serialization;

namespace bytecask {

// Entry layout (all fields little-endian, total = 15 + key_size + value_size +
// 4):
//
//   Offset  0: sequence   (u64) — monotonic LSN
//   Offset  8: entry_type (u8)  — entry kind; 0 is always corrupt/uninitialized
//   Offset  9: key_size   (u16) — key length in bytes (0 for BulkBegin/BulkEnd)
//   Offset 11: value_size (u32) — value length in bytes (0 for Delete/Bulk*)
//   Offset 15: key data   (key_size bytes)
//   Offset 15+key_size: value data (value_size bytes)
//   Trailing: crc32 (u32) — CRC-32C (Castagnoli) over all preceding bytes

// Note: This struct is purely semantic for representing header fields in code.
// It contains implicit padding bytes and must not be used directly for raw
// buffer copying or mapped via memory casts (reinterpret_cast/memcpy).
export struct EntryHeader {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint16_t key_size{};
  std::uint32_t value_size{};
};

export constexpr std::size_t kHeaderSize = 15; // fixed leading fields
export constexpr std::size_t kCrcSize = 4;     // trailing CRC

export struct DataEntry {
  std::uint64_t sequence;
  EntryType entry_type;
  std::vector<std::byte> key;
  std::vector<std::byte> value;
};

// Parses the fixed header fields from the first kHeaderSize bytes of buf.
export auto parse_header(std::span<const std::byte> buf) -> EntryHeader {
  return EntryHeader{
      .sequence = read_le<std::uint64_t>(buf, 0),
      .entry_type = static_cast<EntryType>(read_le<std::uint8_t>(buf, 8)),
      .key_size = read_le<std::uint16_t>(buf, 9),
      .value_size = read_le<std::uint32_t>(buf, 11),
  };
}

// Fills hdr_crc (19 bytes) with the 15-byte LE header at [0..14] and the
// 4-byte CRC at [15..18]. CRC covers header + key + value.
// All format knowledge (field layout, byte order, CRC scope) lives here.
export void serialize_header_and_crc(
    std::span<std::byte, kHeaderSize + kCrcSize> hdr_crc,
    std::uint64_t sequence, EntryType entry_type,
    std::span<const std::byte> key,
    std::span<const std::byte> value) {
  write_le(hdr_crc, 0, sequence);
  hdr_crc[8] = static_cast<std::byte>(entry_type);
  write_le(hdr_crc, 9, narrow<std::uint16_t>(key.size()));
  write_le(hdr_crc, 11, narrow<std::uint32_t>(value.size()));

  Crc32 crc{};
  crc.update(hdr_crc.first<kHeaderSize>());
  crc.update(key);
  crc.update(value);
  write_le(hdr_crc, kHeaderSize, crc.finalize());
}

// Serialize a single entry into a contiguous byte buffer.
// Used by tests and any path that needs a complete in-memory entry.
export auto serialize_entry(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value)
    -> std::vector<std::uint8_t> {
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc{};
  serialize_header_and_crc(hdr_crc, sequence, entry_type, key, value);

  std::vector<std::uint8_t> raw;
  raw.reserve(kHeaderSize + key.size() + value.size() + kCrcSize);

  const auto push = [&raw](std::span<const std::byte> s) {
    for (auto b : s) {
      raw.push_back(std::to_integer<std::uint8_t>(b));
    }
  };
  push(std::span<const std::byte>{hdr_crc.data(), kHeaderSize});
  push(key);
  push(value);
  push(std::span<const std::byte>{hdr_crc.data() + kHeaderSize, kCrcSize});
  return raw;
}

// Validates buffer size and CRC integrity. Returns the parsed header.
// Throws std::runtime_error on size mismatch or CRC failure.
export auto verify_entry(std::span<const std::byte> buf) -> EntryHeader {
  if (buf.size() < kHeaderSize + kCrcSize) {
    throw std::runtime_error{"verify_entry: buffer too small"};
  }

  const auto header = parse_header(buf);

  if (buf.size() !=
      kHeaderSize + header.key_size + header.value_size + kCrcSize) {
    throw std::runtime_error{"verify_entry: buffer size mismatch"};
  }

  Crc32 crc{};
  crc.update(buf.subspan(0, buf.size() - kCrcSize));
  const auto computed = crc.finalize();
  const auto stored = read_le<std::uint32_t>(buf, buf.size() - kCrcSize);
  if (computed != stored) {
    throw std::runtime_error{"verify_entry: CRC mismatch"};
  }

  return header;
}

// Deserializes a complete entry from buf (CRC-verified).
export auto deserialize_entry(std::span<const std::byte> buf) -> DataEntry {
  const auto h = verify_entry(buf);
  const auto key_span = buf.subspan(kHeaderSize, h.key_size);
  const auto val_span = buf.subspan(kHeaderSize + h.key_size, h.value_size);
  return DataEntry{
      .sequence = h.sequence,
      .entry_type = h.entry_type,
      .key = {key_span.begin(), key_span.end()},
      .value = {val_span.begin(), val_span.end()},
  };
}

// Extracts only the value portion into out (CRC-verified, reuses capacity).
export void extract_value_into(std::span<const std::byte> buf,
                               std::vector<std::byte> &out) {
  const auto h = verify_entry(buf);
  const auto val_span = buf.subspan(kHeaderSize + h.key_size, h.value_size);
  out.assign(val_span.begin(), val_span.end());
}

} // namespace bytecask
