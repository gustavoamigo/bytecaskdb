module;
#include <cstddef>
#include <cstdint>
#include <crc32c/crc32c.h>
#include <span>
#include <stdexcept>
#include <utility>

export module bytecask.util;

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
// CRC-32C (Castagnoli, polynomial 0x1EDC6F41) via google/crc32c library.
//
// The library auto-detects hardware acceleration at runtime (SSE4.2 on x86-64,
// CRC instructions on AArch64) and falls back to a software implementation
// when neither is available.
// ---------------------------------------------------------------------------

// Stateful CRC-32C accumulator. Feed chunks via update(), read via finalize().
export class Crc32 {
public:
  void update(std::span<const std::byte> data) noexcept {
    state_ = crc32c::Extend(state_,
        reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }

  [[nodiscard]] auto finalize() const noexcept -> std::uint32_t {
    return state_;
  }

private:
  std::uint32_t state_ = 0;
};

} // namespace bytecask
