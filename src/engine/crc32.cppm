export module bytecask.crc32;

import std;

namespace bytecask {

// Checked narrowing conversion: validates that the source value fits in the
// target type using std::in_range and returns the converted value.
export template <typename To, typename From>
constexpr auto narrow(From value) -> To {
  if (!std::in_range<To>(value)) {
    throw std::runtime_error{"narrowing conversion out of range"};
  }
  return static_cast<To>(value);
}

// ---------------------------------------------------------------------------
// CRC-32 (reflected polynomial 0xEDB88320, CRC-32/ISO-HDLC).
// Calculated iteratively.
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

} // anonymous namespace

// Stateful CRC-32 accumulator fed byte-by-byte via update().
export class Crc32 {
public:
  void update(std::span<const std::byte> data) noexcept {
    for (const auto b : data) {
      const auto idx = static_cast<std::uint8_t>(
          (state_ ^ std::to_integer<std::uint32_t>(b)) & 0xFFU);
      state_ = (state_ >> 8U) ^ crc32_table[idx];
    }
  }

  [[nodiscard]] auto finalize() const noexcept -> std::uint32_t {
    return state_ ^ 0xFFFFFFFFU;
  }

private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

} // namespace bytecask
