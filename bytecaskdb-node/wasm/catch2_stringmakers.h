// Catch2 StringMaker specializations needed for Emscripten builds where
// Catch2's own instantiations are not emitted by the library.
#pragma once
#include <catch2/catch_tostring.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Catch {
template <> struct StringMaker<std::byte> {
  static auto convert(std::byte value) -> std::string {
    return std::format("0x{:02x}", std::to_integer<unsigned>(value));
  }
};
template <> struct StringMaker<std::string_view> {
  static auto convert(std::string_view value) -> std::string {
    return std::string(value);
  }
};
} // namespace Catch
