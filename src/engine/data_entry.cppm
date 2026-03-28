module;
// bitsery is a legacy (non-module) library; include it in the global module
// fragment so its headers are available before the named module begins.
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/vector.h>

export module bytecask.data_entry;

import std;

namespace bytecask {

// ---------------------------------------------------------------------------
// CRC-32 (reflected polynomial 0xEDB88320, CRC-32/ISO-HDLC).
// Computed over all bytes of an entry that precede the trailing checksum field.
// ---------------------------------------------------------------------------
namespace {

constexpr auto make_crc32_table() noexcept -> std::array<std::uint32_t, 256> {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 1U) {
        crc = (crc >> 1U) ^ 0xEDB88320U;
      } else {
        crc >>= 1U;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto crc32_table = make_crc32_table();

// Checked narrowing conversion: validates that the source value fits in the
// target type using std::in_range and returns the converted value.
template <typename To, typename From> constexpr auto narrow(From value) -> To {
  if (!std::in_range<To>(value)) {
    throw std::runtime_error{"narrowing conversion out of range"};
  }
  return static_cast<To>(value);
}

// Wrap bitsery's raw writeBuffer call behind a readable name.
// Hides the reinterpret_cast required by the legacy C-style API.
template <typename Serializer>
void write_bytes(Serializer &ser, std::string_view data) {
  ser.adapter().template writeBuffer<1>(
      reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

// Stateful CRC-32 accumulator fed byte-by-byte via update().
class Crc32 {
public:
  void update(std::span<const std::byte> data) noexcept {
    for (const auto b : data) {
      const auto idx = static_cast<std::uint8_t>(
          (state_ ^
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b))) &
          0xFFU);
      state_ = (state_ >> 8U) ^ crc32_table[idx];
    }
  }

  [[nodiscard]] auto finalize() const noexcept -> std::uint32_t {
    return state_ ^ 0xFFFFFFFFU;
  }

private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

// ---------------------------------------------------------------------------
// CrcOutputAdapter<TAdapter>
//
// A bitsery output adapter wrapper that intercepts every byte written to the
// underlying adapter and accumulates a running CRC-32 checksum.
//
// The CRC accumulator is owned externally so the caller can retrieve the
// final checksum after serialization and append it as a trailing field
// (which must NOT pass through this adapter, or the CRC would include itself).
//
// Follows the same non-CRTP wrapper pattern as bitsery's own
// OutputAdapterBitPackingWrapper.  BitPackingEnabled is wired up correctly so
// that bitsery's optional bit-packing extension would work if ever enabled
// (ByteCask does not use bit-packing).
//
// CRC correctness: ByteCask uses little-endian throughout and bitsery's
// DefaultConfig is also little-endian, so no byte-swapping occurs and the
// bytes seen by update() are identical to the bytes written to the buffer.
// ---------------------------------------------------------------------------
template <typename TAdapter> class CrcOutputAdapter {
public:
  using TConfig = typename TAdapter::TConfig;
  using TValue = typename TAdapter::TValue;
  using BitPackingEnabled =
      bitsery::details::OutputAdapterBitPackingWrapper<CrcOutputAdapter>;

  CrcOutputAdapter(TAdapter &inner, Crc32 &crc) : inner_{inner}, crc_{crc} {}

  template <std::size_t SIZE, typename T> void writeBytes(const T &v) {
    static_assert(std::is_integral_v<T>);
    static_assert(sizeof(T) == SIZE);
    crc_.update(std::as_bytes(std::span{&v, std::size_t{1}}));
    inner_.template writeBytes<SIZE, T>(v);
  }

  template <std::size_t SIZE, typename T>
  void writeBuffer(const T *buf, std::size_t count) {
    static_assert(std::is_integral_v<T>);
    static_assert(sizeof(T) == SIZE);
    crc_.update(std::as_bytes(std::span{buf, count}));
    inner_.template writeBuffer<SIZE, T>(buf, count);
  }

  template <typename T> void writeBits(const T &v, std::size_t bits_count) {
    inner_.writeBits(v, bits_count);
  }

  void align() { inner_.align(); }
  void flush() { inner_.flush(); }
  void currentWritePos(std::size_t pos) { inner_.currentWritePos(pos); }
  [[nodiscard]] auto currentWritePos() const -> std::size_t {
    return inner_.currentWritePos();
  }
  [[nodiscard]] auto writtenBytesCount() const -> std::size_t {
    return inner_.writtenBytesCount();
  }

private:
  TAdapter &inner_;
  Crc32 &crc_;
};

} // anonymous namespace

// Entry layout (all fields little-endian, total = 19 + key_size + value_size):
//
//   Offset  0: sequence   (u64) — monotonic LSN
//   Offset  8: key_size   (u16) — key length in bytes
//   Offset 10: value_size (u32) — value length in bytes  (0 = tombstone)
//   Offset 14: flags      (u8)  — bit 0: deleted; bits 1-7: reserved
//   Offset 15: key data   (key_size bytes)
//   Offset 15+key_size: value data (value_size bytes)
//   Trailing: crc32 (u32) — CRC-32/ISO-HDLC over all preceding bytes

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
export auto serialize_entry(std::uint64_t sequence, std::string_view key,
                            std::string_view value, std::uint8_t flags = 0)
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

// DataFile: append-only writer for a single data file.
// Opens (or creates) the file at the given path in binary append mode.
// Each call to append() serializes one entry and returns its byte offset.
export class DataFile {
public:
  explicit DataFile(std::filesystem::path path) : path_{std::move(path)} {
    file_.open(path_, std::ios::binary | std::ios::app | std::ios::out);
    if (!file_.is_open()) {
      throw std::runtime_error{
          std::format("DataFile: cannot open '{}'", path_.string())};
    }
    offset_ = std::filesystem::file_size(path_);
  }

  DataFile(const DataFile &) = delete;
  DataFile &operator=(const DataFile &) = delete;
  DataFile(DataFile &&) = default;
  DataFile &operator=(DataFile &&) = default;

  // Appends a key-value entry to the file.
  // Returns the byte offset at which the entry was written.
  auto append(std::string_view key, std::string_view value) -> std::uint64_t {
    const auto entry_offset = offset_;
    const auto buf = serialize_entry(++sequence_, key, value);

    file_.write(reinterpret_cast<const char *>(buf.data()), std::ssize(buf));
    if (!file_) {
      throw std::runtime_error{
          std::format("DataFile: write failed for '{}'", path_.string())};
    }
    file_.flush();

    offset_ += static_cast<std::uint64_t>(buf.size());
    return entry_offset;
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }
  [[nodiscard]] auto sequence() const -> std::uint64_t { return sequence_; }

private:
  std::filesystem::path path_;
  std::ofstream file_;
  std::uint64_t sequence_{0};
  std::uint64_t offset_{0};
};

} // namespace bytecask
