module;
// bitsery is a legacy (non-module) library; include it in the global module
// fragment so its headers are available before the named module begins.
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/vector.h>

export module bytecask.data_entry;

import bytecask.serialization;
import std;

namespace bytecask {

// Entry layout (all fields little-endian, total = 19 + key_size + value_size):
//
//   Offset  0: sequence   (u64) — monotonic LSN
//   Offset  8: key_size   (u16) — key length in bytes
//   Offset 10: value_size (u32) — value length in bytes  (0 = tombstone)
//   Offset 14: flags      (u8)  — bit 0: deleted; bits 1-7: reserved
//   Offset 15: key data   (key_size bytes)
//   Offset 15+key_size: value data (value_size bytes)
//   Trailing: crc32 (u32) — CRC-32/ISO-HDLC over all preceding bytes

// Note: This struct is purely semantic for representing header fields in code.
// It contains implicit padding bytes and must not be used directly for raw
// buffer copying or mapped via memory casts (reinterpret_cast/memcpy).
export struct EntryHeader {
  std::uint64_t sequence{};
  std::uint16_t key_size{};
  std::uint32_t value_size{};
  std::uint8_t flags{};
};

export constexpr std::size_t kHeaderSize = 15; // fixed leading fields
export constexpr std::size_t kCrcSize = 4;     // trailing CRC

// Serialize a single entry into a flat byte buffer using bitsery.
// The CRC-32 checksum covers all bytes of the entry except itself and is
// appended as the final four bytes.
export auto
serialize_entry(std::uint64_t sequence, std::span<const std::byte> key,
                std::span<const std::byte> value, std::uint8_t flags = 0)
    -> std::vector<std::uint8_t> {
  using Buffer = std::vector<std::uint8_t>;
  using BaseAdapter = bitsery::OutputBufferAdapter<Buffer>;

  Buffer raw;
  raw.reserve(kHeaderSize + key.size() + value.size() + kCrcSize);

  Crc32 crc{};
  BaseAdapter base{raw};
  CrcOutputAdapter<BaseAdapter> crc_adapter{base, crc};
  bitsery::Serializer<CrcOutputAdapter<BaseAdapter>> ser{crc_adapter};

  // Serialize the fixed header fields.
  ser.value8b(sequence);
  ser.value2b(narrow<std::uint16_t>(key.size()));
  ser.value4b(narrow<std::uint32_t>(value.size()));
  ser.value1b(flags);

  // Write the variable-length key and value bytes through the CRC adapter
  // so they are included in the checksum.
  write_bytes(ser, key);
  write_bytes(ser, value);

  // Append the trailing CRC directly through the base adapter (bypassing the
  // CRC adapter so the checksum does not include itself).
  const auto final_crc = crc.finalize();
  base.template writeBytes<4, std::uint32_t>(final_crc);

  // Trim the buffer to the exact written size: bitsery over-allocates using a
  // cache-line-aligned growth strategy, so raw.size() may exceed kHeaderSize +
  // key.size() + value.size() + kCrcSize immediately after serialization.
  raw.resize(base.writtenBytesCount());
  return raw;
}

} // namespace bytecask
