module;
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/vector.h>

export module bytecask.serialization;

export import bytecask.crc32;
import std;

namespace bytecask {

// Wrap bitsery's raw writeBuffer call behind a readable name.
// Hides the reinterpret_cast required by the legacy C-style API.
export template <typename Serializer>
void write_bytes(Serializer &ser, std::span<const std::byte> data) {
  ser.adapter().template writeBuffer<1>(
      reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

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
export template <typename TAdapter> class CrcOutputAdapter {
public:
  using TConfig = typename TAdapter::TConfig;
  using TValue = typename TAdapter::TValue;
  using BitPackingEnabled =
      bitsery::details::OutputAdapterBitPackingWrapper<CrcOutputAdapter>;

  CrcOutputAdapter(TAdapter &inner, Crc32 &crc) : inner_{inner}, crc_{crc} {}

  template <std::size_t SIZE, typename T> void writeBytes(const T &v) {
    static_assert(std::is_integral_v<T>);
    static_assert(sizeof(T) == SIZE);
    crc_.update(std::as_bytes(std::span{&v, 1}));
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

} // namespace bytecask
