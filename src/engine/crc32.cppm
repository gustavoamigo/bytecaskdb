module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <nmmintrin.h>
#include <span>
#include <stdexcept>
#include <utility>

export module bytecask.crc32;

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
// CRC-32C (Castagnoli, polynomial 0x1EDC6F41) via SSE4.2 hardware instruction.
//
// Uses _mm_crc32_u64 to process 8 bytes per cycle, then _mm_crc32_u8 for the
// trailing remainder. The result is not compatible with CRC-32/ISO-HDLC
// (polynomial 0xEDB88320) used in earlier versions of ByteCask.
//
// Requires: x86-64 with SSE4.2 (compile with -msse4.2).
// ---------------------------------------------------------------------------

// Stateful CRC-32C accumulator. Feed chunks via update(), read via finalize().
export class Crc32 {
public:
  void update(std::span<const std::byte> data) noexcept {
    const auto *ptr = data.data();
    auto len = data.size();
    std::uint64_t crc64 = state_; // widen; upper 32 bits are zero
    while (len >= 8) {
      std::uint64_t word{};
      std::memcpy(&word, ptr, 8);
      crc64 = _mm_crc32_u64(crc64, word);
      ptr += 8;
      len -= 8;
    }
    auto crc32 = static_cast<std::uint32_t>(crc64);
    while (len > 0) {
      crc32 = _mm_crc32_u8(crc32, std::to_integer<std::uint8_t>(*ptr));
      ++ptr;
      --len;
    }
    state_ = crc32;
  }

  [[nodiscard]] auto finalize() const noexcept -> std::uint32_t {
    return state_ ^ 0xFFFFFFFFU;
  }

private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

} // namespace bytecask
