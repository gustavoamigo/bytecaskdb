// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// text.cppm — each SQL type's text form, as PostgreSQL's input and output
// functions read and write it, for the forms docs/server_design.md (*Types
// and text*) lists: a client's text parameter and a row's text on the wire
// (server/pg_text.cppm), and a string literal, which PostgreSQL types by
// where it is used and reads with the input function of that type
// (docs/frontend_design.md, *Literals*). Pure functions: no I/O, no state.

module;
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.text;

export import bytecask.sql.catalog;

export namespace bytecask::sql {

// Text that its type does not accept, with the SQLSTATE PostgreSQL's input
// function reports for it.
class TextError : public std::runtime_error {
public:
  TextError(const std::string& what, const char* sqlstate) : std::runtime_error{what}, sqlstate_{sqlstate} {}
  TextError(const TextError&) = default;
  TextError& operator=(const TextError&) = default;
  ~TextError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto sqlstate() const noexcept -> const char* { return sqlstate_; }

private:
  const char* sqlstate_;  // a literal
};

// A value, not NULL, as PostgreSQL's text output writes a value of type t:
// DateStyle ISO, extra_float_digits 1, bytea_output hex.
[[nodiscard]] auto to_text(const Datum& v, SqlType t) -> std::string;

// Text as a value of type t, as t's input function reads it. A DECIMAL
// finer than its scale is rounded half away from zero when the value is
// only assigned, as PostgreSQL rounds on assignment, and refused otherwise.
// A value outside the type's range (a SMALLINT past 32767, a VARCHAR too
// long) is left to the layer's range check, which reports it as PostgreSQL
// does. Throws TextError.
[[nodiscard]] auto from_text(std::string_view text, SqlType t, bool assigned_only) -> Datum;

}  // namespace bytecask::sql

namespace bytecask::sql {

TextError::~TextError() = default;

namespace {

constexpr std::int64_t kMicrosPerSecond = 1'000'000;
constexpr std::int64_t kSecondsPerDay = 86'400;

[[noreturn]] void bad_text(std::string_view type, std::string_view text) {
  throw TextError("invalid input syntax for type " + std::string{type} + ": \"" + std::string{text} + "\"", "22P02");
}

auto is_space(char c) -> bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

auto trim(std::string_view s) -> std::string_view {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

auto iequals(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// ---------------------------------------------------------------- dates

struct Civil {
  std::int64_t year;  // astronomical: 0 is 1 BC
  int month;
  int day;
};

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
// days_from_civil).
auto days_from_civil(std::int64_t y, int m, int d) -> std::int64_t {
  y -= m <= 2 ? 1 : 0;
  auto era = (y >= 0 ? y : y - 399) / 400;
  auto yoe = y - era * 400;
  auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146'097 + doe - 719'468;
}

auto civil_from_days(std::int64_t z) -> Civil {
  z += 719'468;
  auto era = (z >= 0 ? z : z - 146'096) / 146'097;
  auto doe = z - era * 146'097;
  auto yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
  auto y = yoe + era * 400;
  auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  auto mp = (5 * doy + 2) / 153;
  auto d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  auto m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return {y + (m <= 2 ? 1 : 0), m, d};
}

auto leap(std::int64_t y) -> bool { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

auto days_in_month(std::int64_t y, int m) -> int {
  constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : kDays.at(static_cast<std::size_t>(m - 1));
}

auto floor_div(std::int64_t a, std::int64_t b) -> std::int64_t {
  auto q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

auto pad(std::int64_t v, int width) -> std::string {
  auto s = std::to_string(v);
  return s.size() < static_cast<std::size_t>(width) ? std::string(static_cast<std::size_t>(width) - s.size(), '0') + s
                                                     : s;
}

// "YYYY-MM-DD" with ISO's at least four year digits; " BC" is the caller's.
auto date_part(const Civil& c) -> std::string {
  auto year = c.year <= 0 ? 1 - c.year : c.year;
  return pad(year, 4) + "-" + pad(c.month, 2) + "-" + pad(c.day, 2);
}

// "HH:MM:SS", then the fraction's digits with trailing zeros dropped.
auto time_part(std::int64_t micros) -> std::string {
  auto secs = micros / kMicrosPerSecond;
  auto frac = micros % kMicrosPerSecond;
  auto out = pad(secs / 3600, 2) + ":" + pad(secs / 60 % 60, 2) + ":" + pad(secs % 60, 2);
  if (frac != 0) {
    auto f = pad(frac, 6);
    while (f.back() == '0') f.pop_back();
    out += "." + f;
  }
  return out;
}

auto date_text(std::int64_t days) -> std::string {
  auto c = civil_from_days(days);
  return date_part(c) + (c.year <= 0 ? " BC" : "");
}

auto timestamp_text(std::int64_t micros) -> std::string {
  auto per_day = kSecondsPerDay * kMicrosPerSecond;
  auto days = floor_div(micros, per_day);
  auto c = civil_from_days(days);
  return date_part(c) + " " + time_part(micros - days * per_day) + (c.year <= 0 ? " BC" : "");
}

// A cursor over a datetime's text. Fields are digits; anything unexpected
// is 22007, as PostgreSQL's datetime parser reports a form it does not read.
struct Cursor {
  std::string_view s;
  std::string_view whole;
  std::string_view type;

  [[noreturn]] void fail() const {
    throw TextError("invalid input syntax for type " + std::string{type} + ": \"" + std::string{whole} + "\"",
                    "22007");
  }
  auto at_end() const -> bool { return s.empty(); }
  auto peek() const -> char { return s.empty() ? '\0' : s.front(); }
  void expect(char c) {
    if (peek() != c) fail();
    s.remove_prefix(1);
  }
  // `min` to `max` digits.
  auto digits(std::size_t min, std::size_t max) -> std::int64_t {
    std::size_t n = 0;
    std::int64_t v = 0;
    while (n < s.size() && n < max && std::isdigit(static_cast<unsigned char>(s[n])) != 0) {
      v = v * 10 + (s[n] - '0');
      ++n;
    }
    if (n < min) fail();
    s.remove_prefix(n);
    return v;
  }
};

[[noreturn]] void field_out_of_range(std::string_view text) {
  throw TextError("date/time field value out of range: \"" + std::string{text} + "\"", "22008");
}

// YYYY-MM-DD as days since 1970, before an optional " BC".
auto read_date(Cursor& c) -> std::pair<std::int64_t, int> {
  auto y = c.digits(1, 9);
  c.expect('-');
  auto m = static_cast<int>(c.digits(1, 2));
  c.expect('-');
  auto d = static_cast<int>(c.digits(1, 2));
  return {y, m * 100 + d};
}

auto finish_date(Cursor& c, std::int64_t year, int month_day) -> std::int64_t {
  auto bc = false;
  if (c.s.size() >= 3 && c.s[0] == ' ' && iequals(c.s.substr(1, 2), "BC")) {
    bc = true;
    c.s.remove_prefix(3);
  }
  if (!c.at_end()) c.fail();
  auto month = month_day / 100;
  auto day = month_day % 100;
  if (year == 0) field_out_of_range(c.whole);  // there is no year 0: 1 BC precedes 1 AD
  auto y = bc ? 1 - year : year;
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(y, month)) field_out_of_range(c.whole);
  return days_from_civil(y, month, day);
}

// HH:MM[:SS[.f…f]], up to six fraction digits, as microseconds; 24:00:00
// only where `allow_24`.
auto read_time(Cursor& c, bool allow_24) -> std::int64_t {
  auto h = c.digits(1, 2);
  c.expect(':');
  auto m = c.digits(2, 2);
  std::int64_t sec = 0;
  std::int64_t frac = 0;
  if (c.peek() == ':') {
    c.expect(':');
    sec = c.digits(2, 2);
    if (c.peek() == '.') {
      c.expect('.');
      auto before = c.s.size();
      frac = c.digits(1, 6);
      auto n = before - c.s.size();
      for (auto i = n; i < 6; ++i) frac *= 10;
      if (std::isdigit(static_cast<unsigned char>(c.peek())) != 0) c.fail();  // more than six digits
    }
  }
  auto total = ((h * 60 + m) * 60 + sec) * kMicrosPerSecond + frac;
  if (m > 59 || sec > 59 || (h > 23 && !(allow_24 && h == 24 && total == 24 * 3600 * kMicrosPerSecond))) {
    field_out_of_range(c.whole);
  }
  return total;
}

auto timestamp_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "timestamp"};
  auto [y, md] = read_date(c);
  std::int64_t micros = 0;
  if (c.peek() == ' ' || c.peek() == 'T') {
    if (c.peek() == ' ' && c.s.size() >= 3 && iequals(c.s.substr(1, 2), "BC")) {
      // the date's era, no time
    } else {
      c.s.remove_prefix(1);
      micros = read_time(c, false);
    }
  }
  auto days = finish_date(c, y, md);
  return days * kSecondsPerDay * kMicrosPerSecond + micros;
}

auto date_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "date"};
  auto [y, md] = read_date(c);
  return finish_date(c, y, md);
}

auto time_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "time without time zone"};
  auto t = read_time(c, true);
  if (!c.at_end()) c.fail();
  return t;
}

// ---------------------------------------------------------------- numbers

auto integer_from(std::string_view text, std::string_view type) -> std::int64_t {
  auto s = trim(text);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || end != s.data() + s.size() || (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
    bad_text(type, text);
  }
  if (ec == std::errc::result_out_of_range) {
    throw TextError("value \"" + std::string{text} + "\" is out of range for type " + std::string{type}, "22003");
  }
  return v;
}

// A float's text, as PostgreSQL's float8in reads it: what strtod reads,
// NaN and Infinity in any case. A REAL is read as a double too: the layer
// rounds it and checks its range, in column order, as it does every REAL it
// is given, so a REAL behaves as over the embedded API.
auto float_from(std::string_view text) -> double {
  auto s = trim(text);
  auto negative = false;
  auto body = s;
  if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  if (iequals(body, "nan") && body.size() == s.size()) return std::numeric_limits<double>::quiet_NaN();
  if (iequals(body, "infinity") || iequals(body, "inf")) {
    return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
  }
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  double d = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
  if (s.empty() || end != s.data() + s.size()) bad_text("double precision", text);
  if (ec == std::errc::result_out_of_range) {
    throw TextError("\"" + std::string{text} + "\" is out of range for type double precision", "22003");
  }
  return d;
}

// A DECIMAL's text as its integer at `scale`. A value finer than the scale
// is rounded half away from zero when the parameter is only assigned, as
// PostgreSQL rounds on assignment, and refused otherwise: a compared or
// added value is used unrounded there (docs/server_design.md).
auto decimal_from(std::string_view text, std::uint8_t scale, bool assigned_only) -> std::int64_t {
  auto s = trim(text);
  auto negative = false;
  if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  std::string digits;
  std::int64_t point = 0;  // digits before the decimal point
  auto seen_digit = false;
  auto seen_point = false;
  std::size_t i = 0;
  for (; i < s.size(); ++i) {
    auto ch = s[i];
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      seen_digit = true;
      if (digits.empty() && ch == '0') {  // a leading zero carries nothing
        if (seen_point) --point;
        continue;
      }
      digits.push_back(ch);
      if (!seen_point) ++point;
    } else if (ch == '.' && !seen_point) {
      seen_point = true;
    } else {
      break;
    }
  }
  if (!seen_digit) bad_text("numeric", text);
  if (i < s.size()) {
    if (s[i] != 'e' && s[i] != 'E') bad_text("numeric", text);
    auto exp_text = s.substr(i + 1);
    if (!exp_text.empty() && exp_text.front() == '+') exp_text.remove_prefix(1);
    int e = 0;
    auto [end, ec] = std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), e);
    if (exp_text.empty() || end != exp_text.data() + exp_text.size() || ec != std::errc{} || e > 100'000 ||
        e < -100'000) {
      bad_text("numeric", text);
    }
    point += e;
  }
  while (!digits.empty() && digits.back() == '0') digits.pop_back();  // trailing zeros carry nothing either
  point += scale;  // the scaled integer's digits before its point
  auto overflow = [&] { throw TextError("numeric field overflow", "22003"); };
  if (digits.empty()) return 0;
  if (point > 19) overflow();
  std::uint64_t magnitude = 0;
  for (std::int64_t k = 0; k < point; ++k) {
    auto d = k < std::ssize(digits) ? static_cast<std::uint64_t>(digits[static_cast<std::size_t>(k)] - '0') : 0u;
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10) overflow();
    magnitude = magnitude * 10 + d;
  }
  // The digits past the scaled point: rounded, or refused.
  auto first_dropped = point < 0 ? std::size_t{0} : static_cast<std::size_t>(point);
  if (first_dropped < digits.size()) {
    if (!assigned_only) {
      throw TextError("value \"" + std::string{text} + "\" has more fractional digits than its column's scale of " +
                          std::to_string(scale),
                      "22P02");
    }
    // Leading zeros of a value below the point are not in `digits`: one
    // whose first digit lies past the first dropped place rounds to zero.
    auto first = point < 0 ? '0' : digits[first_dropped];
    if (first >= '5') {
      if (magnitude == std::numeric_limits<std::uint64_t>::max()) overflow();
      ++magnitude;
    }
  }
  auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (magnitude > limit + (negative ? 1u : 0u)) overflow();
  return negative ? static_cast<std::int64_t>(0 - magnitude) : static_cast<std::int64_t>(magnitude);
}

auto decimal_text(std::int64_t v, std::uint8_t scale) -> std::string {
  auto negative = v < 0;
  auto magnitude = negative ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
  auto digits = std::to_string(magnitude);
  if (scale == 0) return (negative ? "-" : "") + digits;
  if (digits.size() <= scale) digits = std::string(scale + 1 - digits.size(), '0') + digits;
  auto cut = digits.size() - scale;
  return (negative ? "-" : "") + digits.substr(0, cut) + "." + digits.substr(cut);
}

// v's significant digits and decimal exponent, in scientific form: the
// shortest that read back to v when precision is negative, else precision
// digits after the first, correctly rounded; trailing zeros dropped.
template <typename F>
auto sci_digits(F v, int precision) -> std::pair<std::string, int> {
  std::array<char, 64> buf{};
  auto r = precision < 0
               ? std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::scientific)
               : std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::scientific, precision);
  std::string_view sci{buf.data(), static_cast<std::size_t>(r.ptr - buf.data())};
  if (sci.front() == '-') sci.remove_prefix(1);
  auto e_at = sci.find('e');
  std::string digits;
  for (auto ch : sci.substr(0, e_at)) {
    if (ch != '.') digits.push_back(ch);
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  int exp = 0;
  auto exp_text = sci.substr(e_at + 1);
  if (!exp_text.empty() && exp_text.front() == '+') exp_text.remove_prefix(1);
  (void)std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), exp);
  return {std::move(digits), exp};
}

// Whether the decimal `digits` x 10^exp_last (exp_last the last digit's
// exponent) is exactly halfway between |v| and a neighbour, so that reading
// it back depends on rounding ties to even. Exact, in 128-bit integers: a
// midpoint is an odd multiple of a power of two below 2^(digits + 2), which
// a decimal of at most 17 digits reaches only for |exp_last| <= 27.
template <typename F>
auto is_midpoint(F v, const std::string& digits, int exp_last) -> bool {
  if (!std::isnormal(v) || exp_last < -27 || exp_last > 27) return false;
  constexpr int kBits = std::numeric_limits<F>::digits;
  int ex = 0;
  auto f = std::frexp(std::abs(v), &ex);
  auto m = static_cast<std::uint64_t>(std::ldexp(f, kBits));  // |v| = m x 2^e, m of kBits bits
  auto e = ex - kBits;
  using U = unsigned __int128;
  U d = 0;
  for (auto ch : digits) d = d * 10 + static_cast<U>(ch - '0');
  U pow5 = 1;
  for (int i = 0; i < std::abs(exp_last); ++i) pow5 *= 5;
  if (exp_last >= 0) {
    d *= pow5;
  } else {
    if (d % pow5 != 0) return false;
    d /= pow5;
  }
  if (d == 0) return false;
  auto twos = exp_last;
  while ((d & 1) == 0) {
    d >>= 1;
    ++twos;
  }
  auto at = [&](U odd, int t) { return d == odd && twos == t; };
  if (at(2 * U{m} + 1, e - 1)) return true;
  // Below a power of two the neighbour is half as far.
  if (m == std::uint64_t{1} << (kBits - 1)) return at(4 * U{m} - 1, e - 2);
  return at(2 * U{m} - 1, e - 1);
}

// PostgreSQL's float output with extra_float_digits 1: the shortest digits
// strictly closer to the value than to either neighbour, so they read back
// to it without relying on ties to even (Ryu, as PostgreSQL builds it:
// 1e23 is 9.999999999999999e+22); fixed form for a decimal exponent in
// [-4, fixed_below), exponent form ("1e+15", "1.5e-05") otherwise.
template <typename F>
auto float_text(F v, int fixed_below) -> std::string {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  auto negative = std::signbit(v);
  auto [digits, exp] = sci_digits(v, -1);
  auto last = [](const std::string& ds, int x) { return x - static_cast<int>(ds.size()) + 1; };
  if (is_midpoint(v, digits, last(digits, exp))) {
    for (auto p = static_cast<int>(digits.size()); p < std::numeric_limits<F>::max_digits10; ++p) {
      auto [ds, x] = sci_digits(v, p);
      auto text = ds.substr(0, 1) + "." + ds.substr(1) + "e" + std::to_string(x);
      F back{};
      (void)std::from_chars(text.data(), text.data() + text.size(), back);
      if (back == std::abs(v) && !is_midpoint(v, ds, last(ds, x))) {
        digits = std::move(ds);
        exp = x;
        break;
      }
    }
  }
  std::string out = negative ? "-" : "";
  if (digits == "0") return out + "0";
  if (exp >= -4 && exp < fixed_below) {
    if (exp < 0) return out + "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
    auto whole = static_cast<std::size_t>(exp + 1);
    if (digits.size() <= whole) return out + digits + std::string(whole - digits.size(), '0');
    return out + digits.substr(0, whole) + "." + digits.substr(whole);
  }
  out += digits.substr(0, 1);
  if (digits.size() > 1) out += "." + digits.substr(1);
  auto e = std::abs(exp);
  return out + "e" + (exp < 0 ? "-" : "+") + (e < 10 ? "0" : "") + std::to_string(e);
}

// ---------------------------------------------------------------- bytea

constexpr std::string_view kHex = "0123456789abcdef";

auto hex_digit(char c) -> int {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

auto bytea_from(std::string_view text) -> Bytes {
  Bytes out;
  if (text.starts_with("\\x")) {
    auto s = text.substr(2);
    for (std::size_t i = 0; i < s.size();) {
      if (is_space(s[i])) {
        ++i;
        continue;
      }
      auto hi = hex_digit(s[i]);
      auto lo = i + 1 < s.size() ? hex_digit(s[i + 1]) : -1;
      if (hi < 0) throw TextError("invalid hexadecimal digit: \"" + std::string(1, s[i]) + "\"", "22023");
      if (lo < 0) throw TextError("invalid hexadecimal data: odd number of digits", "22023");
      out.push_back(static_cast<std::byte>(hi * 16 + lo));
      i += 2;
    }
    return out;
  }
  // The escape format: \\ is a backslash, \ooo an octal byte.
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] != '\\') {
      out.push_back(static_cast<std::byte>(text[i]));
      ++i;
      continue;
    }
    if (i + 1 < text.size() && text[i + 1] == '\\') {
      out.push_back(static_cast<std::byte>('\\'));
      i += 2;
      continue;
    }
    auto octal = [&](std::size_t k, char hi) { return k < text.size() && text[k] >= '0' && text[k] <= hi; };
    if (octal(i + 1, '3') && octal(i + 2, '7') && octal(i + 3, '7')) {
      out.push_back(static_cast<std::byte>((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 + (text[i + 3] - '0')));
      i += 4;
      continue;
    }
    bad_text("bytea", text);
  }
  return out;
}

auto bytea_text(const Bytes& b) -> std::string {
  std::string out = "\\x";
  out.reserve(2 + b.size() * 2);
  for (auto byte : b) {
    auto v = std::to_integer<unsigned>(byte);
    out.push_back(kHex[v >> 4]);
    out.push_back(kHex[v & 0xF]);
  }
  return out;
}

// ---------------------------------------------------------------- booleans

auto bool_from(std::string_view text) -> bool {
  auto s = trim(text);
  auto prefix_of = [&](std::string_view word, std::size_t min) {
    return s.size() >= min && s.size() <= word.size() && iequals(s, word.substr(0, s.size()));
  };
  if (prefix_of("true", 1) || prefix_of("yes", 1) || prefix_of("on", 2) || s == "1") return true;
  if (prefix_of("false", 1) || prefix_of("no", 1) || prefix_of("off", 2) || s == "0") return false;
  bad_text("boolean", text);
}

auto text_bytes(std::string_view s) -> Bytes {
  auto b = std::as_bytes(std::span{s.data(), s.size()});
  return {b.begin(), b.end()};
}

}  // namespace

auto to_text(const Datum& v, SqlType t) -> std::string {
  return std::visit(
      [&](const auto& x) -> std::string {
        using V = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<V, std::monostate>) {
          return {};  // NULL has no text: the caller sends length -1
        } else if constexpr (std::is_same_v<V, bool>) {
          return x ? "t" : "f";
        } else if constexpr (std::is_same_v<V, double>) {
          if (t.kind == SqlKind::Real) return float_text(static_cast<float>(x), 6);
          return float_text(x, 15);
        } else if constexpr (std::is_same_v<V, Bytes>) {
          if (t.kind == SqlKind::Bytea) return bytea_text(x);
          std::string out;  // TEXT and VARCHAR: their UTF-8 bytes; a NUMERIC: its text
          out.reserve(x.size());
          for (auto b : x) out.push_back(static_cast<char>(b));
          return out;
        } else {
          switch (t.kind) {
            case SqlKind::Decimal: return decimal_text(x, t.scale);
            case SqlKind::Timestamp: return timestamp_text(x);
            case SqlKind::Date: return date_text(x);
            case SqlKind::Time: return time_part(x);
            case SqlKind::SmallInt:
            case SqlKind::Integer:
            case SqlKind::BigInt:
            case SqlKind::Real:
            case SqlKind::Double:
            case SqlKind::Boolean:
            case SqlKind::Bytea:
            case SqlKind::Text:
            case SqlKind::Varchar:
            case SqlKind::Numeric: return std::to_string(x);
          }
          return std::to_string(x);
        }
      },
      v);
}

auto from_text(std::string_view text, SqlType t, bool assigned_only) -> Datum {
  switch (t.kind) {
    case SqlKind::SmallInt: return integer_from(text, "smallint");
    case SqlKind::Integer: return integer_from(text, "integer");
    case SqlKind::BigInt: return integer_from(text, "bigint");
    case SqlKind::Real:
    case SqlKind::Double: return float_from(text);
    case SqlKind::Boolean: return bool_from(text);
    case SqlKind::Bytea: return bytea_from(text);
    case SqlKind::Text:
    case SqlKind::Varchar: return text_bytes(text);
    case SqlKind::Decimal: return decimal_from(text, t.scale, assigned_only);
    case SqlKind::Timestamp: return timestamp_from(text);
    case SqlKind::Date: return date_from(text);
    case SqlKind::Time: return time_from(text);
    case SqlKind::Numeric: break;  // no parameter has it: HAVING on AVG is refused
  }
  return text_bytes(text);
}

}  // namespace bytecask::sql
