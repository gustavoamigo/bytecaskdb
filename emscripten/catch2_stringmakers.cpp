// Provides Catch2 StringMaker specializations that are declared but not
// instantiated in the WASM-cross-compiled Catch2 library.
#include <catch2/catch_tostring.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Catch {
std::string StringMaker<std::byte, void>::convert(std::byte value) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%02x",
                static_cast<unsigned>(value));
  return std::string(buf);
}

std::string StringMaker<std::string_view, void>::convert(std::string_view value) {
  return std::string(value);
}
} // namespace Catch
