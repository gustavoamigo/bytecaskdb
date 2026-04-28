// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — compact hint entry encoding for accelerated recovery

module;
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module bytecask.hint_entry;

import bytecask.types;
import bytecask.serialization;

namespace bytecask {

// Hint entry layout (all fields little-endian):
//
//   Offset  0: sequence    (u64) — monotonic sequence number copied from the data file
//   Offset  8: entry_type  (u8)  — Put/Delete
//   Offset  9: file_offset (u64) — byte offset in the companion .data file
//   Offset 17: value_size  (u32) — value length in bytes
//   Offset 21: key_len     (u16) — full key length in bytes
//   Offset 23: key_data    (key_len bytes)
//   ─────────────────────────────────────────
//   File trailer: crc32 (u32) — CRC-32C over all entry bytes; written once
//                               at end of file by HintFile::close().

export constexpr std::size_t kHintHeaderSize =
    23; // sequence(8) + entry_type(1) + file_offset(8) + value_size(4) +
        // key_len(2)

// Parsed hint file entry (zero-copy).
// key and end_key are spans into the backing file buffer; valid for the
// lifetime of the HintFile that owns the buffer.
export struct HintEntry {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint64_t file_offset{};
  std::span<const std::byte> key;
  std::uint32_t value_size{};
  std::span<const std::byte> end_key; // non-empty only for RangeDel
};

// Serializes one hint entry into a flat byte vector (header + key, no CRC).
export auto serialize_entry(std::uint64_t sequence, EntryType entry_type,
                            std::uint64_t file_offset, std::uint32_t value_size,
                            std::span<const std::byte> key)
    -> std::vector<std::byte> {
  std::vector<std::byte> buf(kHintHeaderSize + key.size());
  ByteWriter w{buf};
  w.put(sequence);
  w.put(static_cast<std::uint8_t>(entry_type));
  w.put(file_offset);
  w.put(value_size);
  w.put(narrow<std::uint16_t>(key.size()));
  w.put_bytes(key);
  return buf;
}

// Serializes a RangeDel hint entry: header + full start_key, then
// [end_key_len: u16 LE][end_key: end_key_len bytes].
export auto serialize_range_del_entry(std::uint64_t sequence,
                                      std::uint64_t file_offset,
                                      std::span<const std::byte> start_key,
                                      std::span<const std::byte> end_key)
    -> std::vector<std::byte> {
  const auto base_size = kHintHeaderSize + start_key.size();
  const auto end_key_trailer = sizeof(std::uint16_t) + end_key.size();
  std::vector<std::byte> buf(base_size + end_key_trailer);
  ByteWriter w{buf};
  w.put(sequence);
  w.put(static_cast<std::uint8_t>(EntryType::RangeDel));
  w.put(file_offset);
  w.put(narrow<std::uint32_t>(end_key.size())); // value_size = end_key length
  w.put(narrow<std::uint16_t>(start_key.size()));
  w.put_bytes(start_key);
  w.put(narrow<std::uint16_t>(end_key.size()));
  w.put_bytes(end_key);
  return buf;
}

// Deserializes one hint entry from the start of buf.
// Returns {entry, bytes_consumed}. entry.key and entry.end_key are spans
// into buf, valid for the lifetime of the backing buffer.
// Throws std::runtime_error on a truncated entry.
export auto deserialize_entry(std::span<const std::byte> buf)
    -> std::pair<HintEntry, std::size_t> {
  if (buf.size() < kHintHeaderSize) {
    throw std::runtime_error{"deserialize_entry (hint): truncated header"};
  }
  ByteReader r{buf};
  const auto sequence    = r.get<std::uint64_t>();
  const auto entry_type  = static_cast<EntryType>(r.get<std::uint8_t>());
  const auto file_offset = r.get<std::uint64_t>();
  const auto value_size  = r.get<std::uint32_t>();
  const auto key_len     = r.get<std::uint16_t>();

  auto total = kHintHeaderSize + key_len;
  if (buf.size() < total) {
    throw std::runtime_error{"deserialize_entry (hint): truncated entry"};
  }
  const auto key_span = buf.subspan(kHintHeaderSize, key_len);

  // For RangeDel, the end_key is appended after the start_key.
  std::span<const std::byte> end_key_span;
  if (entry_type == EntryType::RangeDel) {
    const auto trailer_offset = total;
    if (buf.size() < trailer_offset + sizeof(std::uint16_t)) {
      throw std::runtime_error{
          "deserialize_entry (hint): truncated RangeDel end_key_len"};
    }
    const auto end_key_len =
        read_le<std::uint16_t>(buf, trailer_offset);
    total += sizeof(std::uint16_t) + end_key_len;
    if (buf.size() < total) {
      throw std::runtime_error{
          "deserialize_entry (hint): truncated RangeDel end_key"};
    }
    end_key_span = buf.subspan(
        trailer_offset + sizeof(std::uint16_t), end_key_len);
  }

  return {HintEntry{.sequence    = sequence,
                    .entry_type  = entry_type,
                    .file_offset = file_offset,
                    .key         = key_span,
                    .value_size  = value_size,
                    .end_key     = end_key_span},
          total};
}

} // namespace bytecask
