module;
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

export module bytecask.hint_entry;

import bytecask.types;
import bytecask.serialization;

namespace bytecask {

// Hint entry layout (all fields little-endian):
//
//   Offset  0: sequence     (u64) — monotonic LSN copied from the data file
//   Offset  8: entry_type   (u8)  — mirrors DataFile EntryType (Put/Delete/…)
//   Offset  9: file_offset  (u64) — byte offset of the entry in the companion
//                                   .data file
//   Offset 17: key_size     (u16) — key length in bytes
//   Offset 19: value_size   (u32) — value length in bytes (for size tracking
//                                   without reading the data file)
//   Offset 23: key data     (key_size bytes)
//   Trailing:  crc32        (u32) — CRC-32C (Castagnoli) over all preceding
//   bytes

export constexpr std::size_t kHintHeaderSize =
    23; // sequence(8) + entry_type(1) + file_offset(8) + key_size(2) +
        // value_size(4)
export constexpr std::size_t kHintCrcSize = 4; // trailing CRC

// Parsed representation of a single hint file entry.
export struct HintEntry {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint64_t file_offset{};
  std::vector<std::byte> key;
  std::uint32_t value_size{};
};

// Zero-copy view of a single hint file entry.
// key is a span into the HintFile's backing buffer; valid only while the
// HintFile is alive and scan_view() has not been called again on the same
// offset. Use in tight scan loops where ownership is not needed.
export struct HintEntryView {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint64_t file_offset{};
  std::span<const std::byte> key;
  std::uint32_t value_size{};
};

// Serialize one hint entry into a flat byte buffer.
// CRC-32 covers all bytes except itself and is appended as the final four
// bytes.
export auto serialize_entry(std::uint64_t sequence, EntryType entry_type,
                            std::uint64_t file_offset,
                            std::span<const std::byte> key,
                            std::uint32_t value_size)
    -> std::vector<std::byte> {
  const auto total = kHintHeaderSize + key.size() + kHintCrcSize;
  std::vector<std::byte> buf(total);

  Crc32 crc{};
  ByteWriter w{buf, &crc};
  w.put(sequence);
  w.put(static_cast<std::uint8_t>(entry_type));
  w.put(file_offset);
  w.put(narrow<std::uint16_t>(key.size()));
  w.put(value_size);
  w.put_bytes(key);

  // CRC itself is NOT covered by the checksum — write through a bare writer.
  ByteWriter tail{std::span{buf}.subspan(w.pos())};
  tail.put(crc.finalize());
  return buf;
}

// Deserialize a hint entry from a contiguous buffer (header + key + CRC).
// Verifies CRC integrity. Throws std::runtime_error on CRC mismatch.
export auto deserialize_entry(std::span<const std::byte> buf) -> HintEntry {
  ByteReader r{buf};
  const auto sequence = r.get<std::uint64_t>();
  const auto entry_type = static_cast<EntryType>(r.get<std::uint8_t>());
  const auto file_offset_val = r.get<std::uint64_t>();
  const auto key_size = r.get<std::uint16_t>();
  const auto value_size = r.get<std::uint32_t>();
  auto key_span = r.get_bytes(key_size);

  // CRC covers everything before the trailing 4 bytes.
  Crc32 crc{};
  crc.update(buf.subspan(0, buf.size() - kHintCrcSize));
  const auto computed = crc.finalize();
  const auto stored = read_le<std::uint32_t>(buf, buf.size() - kHintCrcSize);
  if (computed != stored) {
    throw std::runtime_error{
        std::format("deserialize_entry (hint): CRC mismatch")};
  }

  return HintEntry{.sequence = sequence,
                   .entry_type = entry_type,
                   .file_offset = file_offset_val,
                   .key = {key_span.begin(), key_span.end()},
                   .value_size = value_size};
}

// Zero-copy variant: key is a span directly into buf. buf must outlive the
// returned HintEntryView. CRC is still verified.
export auto deserialize_entry_view(std::span<const std::byte> buf)
    -> HintEntryView {
  ByteReader r{buf};
  const auto sequence = r.get<std::uint64_t>();
  const auto entry_type = static_cast<EntryType>(r.get<std::uint8_t>());
  const auto file_offset_val = r.get<std::uint64_t>();
  const auto key_size = r.get<std::uint16_t>();
  const auto value_size = r.get<std::uint32_t>();
  const auto key_span = r.get_bytes(key_size);

  Crc32 crc{};
  crc.update(buf.subspan(0, buf.size() - kHintCrcSize));
  const auto computed = crc.finalize();
  const auto stored = read_le<std::uint32_t>(buf, buf.size() - kHintCrcSize);
  if (computed != stored) {
    throw std::runtime_error{
        std::format("deserialize_entry_view (hint): CRC mismatch")};
  }

  return HintEntryView{.sequence = sequence,
                       .entry_type = entry_type,
                       .file_offset = file_offset_val,
                       .key = key_span,
                       .value_size = value_size};
}

} // namespace bytecask
