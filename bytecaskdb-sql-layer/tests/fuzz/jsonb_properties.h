// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The properties the jsonb fuzz targets hold every input to
// (docs/jsonb_design.md, *Tests of jsonb by stage*, Fuzz), shared by the
// libFuzzer targets in this directory and the deterministic mutation run in
// tests/jsonb_test.cpp. Include it after `import bytecask.sql.jsonb;`.
//
// - decode: any bytes, as a stored value and as a node, through the full
//   check and the output writer, give a result or Corrupt, never undefined
//   behaviour (run under ASan and UBSan); a value the full check accepts
//   prints text that parses back to the same bytes.
// - parse: any text gives a value or one of jsonb_in's errors; the value
//   passes the full check, and its text parses back to the same value.
// Each returns false where a property breaks; the fuzz targets abort on it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jsonb_fuzz {

namespace jb = bytecask::sql::jsonb;

inline auto round_trips(std::span<const std::byte> value) -> bool {
  std::string text;
  if (!jb::value_text(value, text).ok()) return false;
  std::vector<std::byte> again;
  if (!jb::parse_value(text, again).ok()) return false;
  return std::ranges::equal(again, value);
}

inline auto decode_holds(std::span<const std::byte> bytes) -> bool {
  auto checked = jb::check_value(bytes, {});
  if (checked.status != jb::Status::Ok && checked.status != jb::Status::Corrupt) return false;
  if (checked.ok() && !round_trips(bytes)) return false;
  // The same bytes as a node: every read gives a result or Corrupt, and the
  // length pass and the write pass agree.
  std::uint64_t length = 0;
  auto counted = jb::text_length(bytes, {}, length);
  if (counted.status != jb::Status::Ok && counted.status != jb::Status::Corrupt) return false;
  if (counted.ok() != jb::check_node(bytes, {}).ok()) return false;
  if (counted.ok() && length < (std::uint64_t{1} << 24)) {
    std::string out(static_cast<std::size_t>(length), '\0');
    if (!jb::write_text(bytes, {}, std::span{out.data(), out.size()}).ok()) return false;
  }
  return true;
}

inline auto parse_holds(std::string_view text) -> bool {
  std::vector<std::byte> value;
  auto r = jb::parse_value(text, value);
  if (!r.ok()) {
    if (r.status != jb::Status::Data) return false;
    auto state = r.error->state.view();
    return state == "22P02" || state == "22P05" || state == "22003" || state == "54001" || state == "54000" || state == "22021";
  }
  return jb::check_value(value, {}).ok() && round_trips(value);
}

}  // namespace jsonb_fuzz
