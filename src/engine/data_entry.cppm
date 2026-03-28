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

export struct ReadResult {
  std::uint64_t sequence;
  std::vector<std::byte> key;
  std::vector<std::byte> value;
};

namespace {

// Parse a little-endian integer of type T from a byte span at the given offset.
template <typename T>
auto read_le(std::span<const std::byte> buf, std::size_t offset) -> T {
  static_assert(std::is_integral_v<T>);
  T v{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v |= static_cast<T>(static_cast<T>(std::to_integer<std::uint8_t>(
                            buf[offset + i]))
                        << (8U * i));
  }
  return v;
}

} // namespace

// Parses the fixed header fields from the first kHeaderSize bytes of buf.
export auto parse_header(std::span<const std::byte> buf) -> EntryHeader {
  return EntryHeader{
      .sequence   = read_le<std::uint64_t>(buf, 0),
      .key_size   = read_le<std::uint16_t>(buf, 8),
      .value_size = read_le<std::uint32_t>(buf, 10),
      .flags      = read_le<std::uint8_t>(buf, 14),
  };
}

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

// Deserializes a single entry from a flat byte buffer and verifies its CRC.
// buf must span exactly one complete entry: kHeaderSize + key_size + value_size + kCrcSize bytes.
// Throws std::runtime_error on size mismatch or CRC failure.
export auto deserialize_entry(std::span<const std::byte> buf) -> ReadResult {
  if (buf.size() < kHeaderSize + kCrcSize) {
    throw std::runtime_error{"deserialize_entry: buffer too small"};
  }

  const auto header = parse_header(buf);

  if (buf.size() !=
      kHeaderSize + header.key_size + header.value_size + kCrcSize) {
    throw std::runtime_error{"deserialize_entry: buffer size mismatch"};
  }

  // Verify CRC over all bytes except the trailing checksum.
  Crc32 crc{};
  crc.update(buf.subspan(0, buf.size() - kCrcSize));
  const auto computed = crc.finalize();
  const auto stored   = read_le<std::uint32_t>(buf, buf.size() - kCrcSize);
  if (computed != stored) {
    throw std::runtime_error{"deserialize_entry: CRC mismatch"};
  }

  const auto key_span = buf.subspan(kHeaderSize, header.key_size);
  const auto val_span = buf.subspan(kHeaderSize + header.key_size, header.value_size);

  return ReadResult{
      .sequence = header.sequence,
      .key      = {key_span.begin(), key_span.end()},
      .value    = {val_span.begin(), val_span.end()},
  };
}

} // namespace bytecask
