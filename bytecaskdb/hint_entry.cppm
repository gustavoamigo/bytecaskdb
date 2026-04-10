// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — compact hint entry encoding for accelerated recovery

module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

export module bytecask.hint_entry;

import bytecask.types;
import bytecask.serialization;

namespace bytecask {

// Hint entry layout (all fields little-endian):
//
//   Offset  0: sequence    (u64) — monotonic LSN copied from the data file
//   Offset  8: entry_type  (u8)  — Put/Delete
//   Offset  9: file_offset (u64) — byte offset in the companion .data file
//   Offset 17: value_size  (u32) — value length in bytes
//   Offset 21: prefix_len  (u8)  — bytes shared with previous entry's key
//                                   (0 for the first entry; capped at 255)
//   Offset 22: suffix_len  (u16) — new bytes following the shared prefix
//   Offset 24: suffix_data (suffix_len bytes)
//   ─────────────────────────────────────────
//   File trailer: crc32 (u32) — CRC-32C over all entry bytes; written once
//                               at end of file by HintFile::sync().

export constexpr std::size_t kHintHeaderSize =
    24; // sequence(8) + entry_type(1) + file_offset(8) + value_size(4) +
        // prefix_len(1) + suffix_len(2)

// Parsed hint file entry (zero-copy).
// key is a span into the Scanner's internal key buffer; valid only until
// the next call to Scanner::next().
export struct HintEntry {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint64_t file_offset{};
  std::span<const std::byte> key;
  std::uint32_t value_size{};
};

// Serializes one hint entry into a flat byte vector (header + suffix, no CRC).
// prefix_len and suffix must be pre-computed by the caller (HintFile::append).
export auto serialize_entry(std::uint64_t sequence, EntryType entry_type,
                            std::uint64_t file_offset, std::uint32_t value_size,
                            std::uint8_t prefix_len,
                            std::span<const std::byte> suffix)
    -> std::vector<std::byte> {
  std::vector<std::byte> buf(kHintHeaderSize + suffix.size());
  ByteWriter w{buf};
  w.put(sequence);
  w.put(static_cast<std::uint8_t>(entry_type));
  w.put(file_offset);
  w.put(value_size);
  w.put(prefix_len);
  w.put(narrow<std::uint16_t>(suffix.size()));
  w.put_bytes(suffix);
  return buf;
}

// Deserializes one hint entry from the start of buf, updating key_buf
// in-place for zero-copy prefix decompression.
// Returns {entry, bytes_consumed}. entry.key spans key_buf; valid until
// the next call. Throws std::runtime_error on a truncated entry.
export auto deserialize_entry(std::span<const std::byte> buf,
                              std::vector<std::byte> &key_buf)
    -> std::pair<HintEntry, std::size_t> {
  if (buf.size() < kHintHeaderSize) {
    throw std::runtime_error{"deserialize_entry (hint): truncated header"};
  }
  ByteReader r{buf};
  const auto sequence    = r.get<std::uint64_t>();
  const auto entry_type  = static_cast<EntryType>(r.get<std::uint8_t>());
  const auto file_offset = r.get<std::uint64_t>();
  const auto value_size  = r.get<std::uint32_t>();
  const auto prefix_len  = r.get<std::uint8_t>();
  const auto suffix_len  = r.get<std::uint16_t>();

  const auto total = kHintHeaderSize + suffix_len;
  if (buf.size() < total) {
    throw std::runtime_error{"deserialize_entry (hint): truncated entry"};
  }
  // Resize keeps prefix bytes untouched; only the suffix delta is overwritten.
  key_buf.resize(std::size_t{prefix_len} + suffix_len);
  std::memcpy(key_buf.data() + prefix_len,
              buf.data() + kHintHeaderSize,
              suffix_len);

  return {HintEntry{.sequence    = sequence,
                    .entry_type  = entry_type,
                    .file_offset = file_offset,
                    .key         = std::span<const std::byte>{key_buf},
                    .value_size  = value_size},
          total};
}

} // namespace bytecask
