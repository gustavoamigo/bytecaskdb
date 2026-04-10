// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — on-disk data entry layout and CRC validation

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

// Reads the fixed header fields from the first kHeaderSize bytes of buf.
export auto read_header(std::span<const std::byte> buf) -> EntryHeader {
  ByteReader r{buf};
  return EntryHeader{
      .sequence = r.get<std::uint64_t>(),
      .entry_type = static_cast<EntryType>(r.get<std::uint8_t>()),
      .key_size = r.get<std::uint16_t>(),
      .value_size = r.get<std::uint32_t>(),
  };
}

// Fills hdr_crc (19 bytes) with the 15-byte LE header at [0..14] and the
// 4-byte CRC at [15..18]. CRC covers header + key + value.
// All format knowledge (field layout, byte order, CRC scope) lives here.
export void write_header_and_crc(
    std::span<std::byte, kHeaderSize + kCrcSize> hdr_crc,
    std::uint64_t sequence, EntryType entry_type,
    std::span<const std::byte> key,
    std::span<const std::byte> value) {
  Crc32 crc{};
  ByteWriter w{hdr_crc, &crc};
  w.put(sequence);
  w.put(static_cast<std::uint8_t>(entry_type));
  w.put(narrow<std::uint16_t>(key.size()));
  w.put(narrow<std::uint32_t>(value.size()));

  // Key and value are not in hdr_crc but are covered by the CRC.
  crc.update(key);
  crc.update(value);

  // Append CRC after the header (not CRC-covered).
  w.put(crc.finalize());
}

// Serialize a single entry into a contiguous byte buffer.
// Used by tests and any path that needs a complete in-memory entry.
export auto serialize_entry(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value)
    -> std::vector<std::byte> {
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc{};
  write_header_and_crc(hdr_crc, sequence, entry_type, key, value);

  const auto total = kHeaderSize + key.size() + value.size() + kCrcSize;
  std::vector<std::byte> buf(total);
  ByteWriter w{buf};
  w.put_bytes({hdr_crc.data(), kHeaderSize});
  w.put_bytes(key);
  w.put_bytes(value);
  w.put_bytes({hdr_crc.data() + kHeaderSize, kCrcSize});
  return buf;
}

// Validates buffer size and CRC integrity. Returns the parsed header.
// Throws std::runtime_error on size mismatch or CRC failure.
export auto parse_header_and_verify(std::span<const std::byte> buf) -> EntryHeader {
  if (buf.size() < kHeaderSize + kCrcSize) {
    throw std::runtime_error{"parse_header_and_verify: buffer too small"};
  }

  const auto header = read_header(buf);

  if (buf.size() !=
      kHeaderSize + header.key_size + header.value_size + kCrcSize) {
    throw std::runtime_error{"parse_header_and_verify: buffer size mismatch"};
  }

  Crc32 crc{};
  crc.update(buf.subspan(0, buf.size() - kCrcSize));
  const auto computed = crc.finalize();
  const auto stored = read_le<std::uint32_t>(buf, buf.size() - kCrcSize);
  if (computed != stored) {
    throw std::runtime_error{"parse_header_and_verify: CRC mismatch"};
  }

  return header;
}

// Deserializes a complete entry from buf (CRC-verified).
export auto deserialize_entry(std::span<const std::byte> buf) -> DataEntry {
  const auto h = parse_header_and_verify(buf);
  ByteReader r{buf.subspan(kHeaderSize)};
  auto key_span = r.get_bytes(h.key_size);
  auto val_span = r.get_bytes(h.value_size);
  return DataEntry{
      .sequence = h.sequence,
      .entry_type = h.entry_type,
      .key = {key_span.begin(), key_span.end()},
      .value = {val_span.begin(), val_span.end()},
  };
}

// Extracts only the value portion into out (reuses capacity).
// When verify is true (default), computes and checks the CRC32 checksum.
export void extract_value_into(std::span<const std::byte> buf,
                               std::vector<std::byte> &out,
                               bool verify = true) {
  const auto h = verify ? parse_header_and_verify(buf) : read_header(buf);
  ByteReader r{buf.subspan(kHeaderSize)};
  r.get_bytes(h.key_size); // skip key
  auto val_span = r.get_bytes(h.value_size);
  out.assign(val_span.begin(), val_span.end());
}

} // namespace bytecask
