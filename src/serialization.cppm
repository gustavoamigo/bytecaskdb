module;
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bytecask.serialization;

export import bytecask.util;

namespace bytecask {

// Parse a little-endian integer of type T from a byte span at the given offset.
export template <typename T>
auto read_le(std::span<const std::byte> buf, std::size_t offset) -> T {
  static_assert(std::is_integral_v<T>);
  T v{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v |= static_cast<T>(
        static_cast<T>(std::to_integer<std::uint8_t>(buf[offset + i]))
        << (8U * i));
  }
  return v;
}

// Write a little-endian integer of type T into a byte span at the given offset.
export template <typename T>
void write_le(std::span<std::byte> buf, std::size_t offset, T v) {
  static_assert(std::is_integral_v<T>);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    buf[offset + i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
  }
}

// ---------------------------------------------------------------------------
// ByteWriter — auto-advancing cursor over a mutable byte span.
// Optionally feeds every written byte into a Crc32 accumulator.
// ---------------------------------------------------------------------------
export class ByteWriter {
public:
  explicit ByteWriter(std::span<std::byte> buf, Crc32 *crc = nullptr)
      : buf_{buf}, crc_{crc} {}

  template <typename T> void put(T v) {
    static_assert(std::is_integral_v<T>);
    write_le(buf_, pos_, v);
    if (crc_) {
      crc_->update(buf_.subspan(pos_, sizeof(T)));
    }
    pos_ += sizeof(T);
  }

  void put_bytes(std::span<const std::byte> data) {
    for (std::size_t i = 0; i < data.size(); ++i) {
      buf_[pos_ + i] = data[i];
    }
    if (crc_) {
      crc_->update(data);
    }
    pos_ += data.size();
  }

  [[nodiscard]] auto pos() const -> std::size_t { return pos_; }

private:
  std::span<std::byte> buf_;
  std::size_t pos_{0};
  Crc32 *crc_;
};

// ---------------------------------------------------------------------------
// ByteReader — auto-advancing cursor over a const byte span.
// ---------------------------------------------------------------------------
export class ByteReader {
public:
  explicit ByteReader(std::span<const std::byte> buf) : buf_{buf} {}

  template <typename T> auto get() -> T {
    static_assert(std::is_integral_v<T>);
    auto v = read_le<T>(buf_, pos_);
    pos_ += sizeof(T);
    return v;
  }

  auto get_bytes(std::size_t n) -> std::span<const std::byte> {
    auto s = buf_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  [[nodiscard]] auto pos() const -> std::size_t { return pos_; }

private:
  std::span<const std::byte> buf_;
  std::size_t pos_{0};
};

} // namespace bytecask
