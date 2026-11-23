// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// numeric.cppm — the decimal reading of PostgreSQL's numeric_in, split from
// its text (docs/jsonb_design.md, *Parsing a jsonb value*): numeric_parse
// reads [digits][.digits][e[+-]digits] into its parts and never throws or
// allocates, and numeric_text writes numeric_out's text from those parts.
// NUMERIC's numeric_in (sql/text.cppm) and jsonb's number scanner
// (sql/jsonb.cppm) both read numbers through it, so the two agree on every
// digit, scale and range rule.

module;
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module bytecask.sql.numeric;

export namespace bytecask::sql {

// numeric's format: base-10000 digits of weight at most 32767, so 131072
// decimal digits before the point, and a display scale of at most 16383.
inline constexpr std::int64_t kNumericMaxWholeDigits = 131072;
inline constexpr std::int64_t kNumericMaxScale = 16383;
// numeric_in refuses an exponent past this before it looks further.
inline constexpr std::int64_t kNumericMaxExponent = std::numeric_limits<std::int32_t>::max() / 2;

enum class NumericStatus : std::uint8_t {
  Ok,
  Syntax,    // not [digits][.digits][e[+-]digits] (22P02)
  Overflow,  // past numeric's format (22003)
};

// A decimal number's parts: its digits (in the caller's buffer, when it gave
// one) sit at places whole-1 down to whole-ndigits; its text runs to place
// -scale. `first` and `last` are the indexes of its first and last digit
// that is not '0', none for a zero.
struct NumericParts {
  std::int64_t whole{0};  // digits before the point, the exponent added
  std::int64_t scale{0};  // digits after it, the exponent taken off, never below 0
  std::size_t ndigits{0};
  std::optional<std::size_t> first;
  std::optional<std::size_t> last;
  std::size_t end{0};  // where the reading stopped in the text
};

// Reads s from its start, as numeric_in's set_var_from_str reads the digits
// after a sign: _ between digits, an exponent bounded at kNumericMaxExponent
// (Overflow at once, as numeric_in refuses it before reading on). Stops at
// the first byte that cannot continue the number (parts.end); the caller
// decides whether what follows is allowed, then calls numeric_in_range.
// The digits go to `digits` when it is not empty, which must hold s.size()
// characters. Never throws.
[[nodiscard]] auto numeric_parse(std::string_view s, std::span<char> digits, NumericParts& parts) -> NumericStatus;

// Whether the parts are inside numeric's format: a scale of at most
// kNumericMaxScale, at most kNumericMaxWholeDigits digits from the first
// significant one to the point. numeric_in's checks, in its order.
[[nodiscard]] auto numeric_in_range(const NumericParts& parts) -> bool;

// numeric_out's text of the parts and their digits, "-" first when negative
// and not zero. Every place from the larger of whole-1 and 0 down to -scale.
[[nodiscard]] auto numeric_text(bool negative, const NumericParts& parts, std::string_view digits) -> std::string;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {
auto is_digit(char c) -> bool { return c >= '0' && c <= '9'; }
}  // namespace

auto numeric_parse(std::string_view s, std::span<char> digits, NumericParts& parts) -> NumericStatus {
  parts = {};
  std::size_t at = 0;
  auto point = false;
  auto note_digit = [&](char c) {
    if (!digits.empty()) digits[parts.ndigits] = c;
    if (c != '0') {
      if (!parts.first) parts.first = parts.ndigits;
      parts.last = parts.ndigits;
    }
    ++parts.ndigits;
  };
  if (at < s.size() && s[at] == '.') {
    point = true;
    ++at;
  }
  if (at >= s.size() || !is_digit(s[at])) return NumericStatus::Syntax;
  while (at < s.size()) {
    auto c = s[at];
    if (is_digit(c)) {
      note_digit(c);
      ++(point ? parts.scale : parts.whole);
      ++at;
    } else if (c == '.') {
      if (point) return NumericStatus::Syntax;
      point = true;
      ++at;
      if (at < s.size() && s[at] == '_') return NumericStatus::Syntax;
    } else if (c == '_') {
      ++at;
      if (at >= s.size() || !is_digit(s[at])) return NumericStatus::Syntax;
    } else {
      break;
    }
  }
  if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
    ++at;
    auto neg_exp = false;
    if (at < s.size() && (s[at] == '+' || s[at] == '-')) neg_exp = s[at++] == '-';
    if (at >= s.size() || !is_digit(s[at])) return NumericStatus::Syntax;
    std::int64_t exp = 0;
    while (at < s.size()) {
      auto c = s[at];
      if (is_digit(c)) {
        exp = exp * 10 + (c - '0');
        if (exp > kNumericMaxExponent) return NumericStatus::Overflow;
        ++at;
      } else if (c == '_') {
        ++at;
        if (at >= s.size() || !is_digit(s[at])) return NumericStatus::Syntax;
      } else {
        break;
      }
    }
    if (neg_exp) exp = -exp;
    parts.whole += exp;
    parts.scale = parts.scale - exp < 0 ? 0 : parts.scale - exp;
  }
  parts.end = at;
  return NumericStatus::Ok;
}

auto numeric_in_range(const NumericParts& parts) -> bool {
  if (parts.scale > kNumericMaxScale) return false;
  return !parts.first || parts.whole - static_cast<std::int64_t>(*parts.first) <= kNumericMaxWholeDigits;
}

auto numeric_text(bool negative, const NumericParts& parts, std::string_view digits) -> std::string {
  auto zero = !parts.first;
  auto place_digit = [&](std::int64_t place) -> char {
    auto i = parts.whole - 1 - place;
    return i >= 0 && i < static_cast<std::int64_t>(digits.size()) ? digits[static_cast<std::size_t>(i)] : '0';
  };
  std::string out;
  if (negative && !zero) out.push_back('-');
  auto top = zero ? 0 : parts.whole - 1 - static_cast<std::int64_t>(*parts.first);
  if (top < 0) top = 0;
  for (auto place = top; place >= 0; --place) out.push_back(place_digit(place));
  if (parts.scale > 0) {
    out.push_back('.');
    for (std::int64_t place = -1; place >= -parts.scale; --place) out.push_back(place_digit(place));
  }
  return out;
}

}  // namespace bytecask::sql
