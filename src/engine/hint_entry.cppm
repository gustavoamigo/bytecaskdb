module;
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/vector.h>
#include <cstddef>
#include <cstdint>
#include <span>
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
//   Trailing:  crc32        (u32) — CRC-32/ISO-HDLC over all preceding bytes

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

// Serialize one hint entry into a flat byte buffer using bitsery.
// CRC-32 covers all bytes except itself and is appended as the final four
// bytes.
export auto serialize_hint_entry(std::uint64_t sequence, EntryType entry_type,
                                 std::uint64_t file_offset,
                                 std::span<const std::byte> key,
                                 std::uint32_t value_size)
    -> std::vector<std::uint8_t> {
  using Buffer = std::vector<std::uint8_t>;
  using BaseAdapter = bitsery::OutputBufferAdapter<Buffer>;

  Buffer raw;
  raw.reserve(kHintHeaderSize + key.size() + kHintCrcSize);

  Crc32 crc{};
  BaseAdapter base{raw};
  CrcOutputAdapter<BaseAdapter> crc_adapter{base, crc};
  bitsery::Serializer<CrcOutputAdapter<BaseAdapter>> ser{crc_adapter};

  auto entry_type_u8 = static_cast<std::uint8_t>(entry_type);
  ser.value8b(sequence);
  ser.value1b(entry_type_u8);
  ser.value8b(file_offset);
  ser.value2b(narrow<std::uint16_t>(key.size()));
  ser.value4b(value_size);
  write_bytes(ser, key);

  // Append the trailing CRC directly through the base adapter so it is not
  // included in the checksum.
  base.template writeBytes<4, std::uint32_t>(crc.finalize());
  raw.resize(base.writtenBytesCount());
  return raw;
}

} // namespace bytecask
