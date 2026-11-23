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
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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
import bytecask.sql.numeric;
import bytecask.sql.jsonb;

export namespace bytecask::sql {

// Text that its type does not accept, with the SQLSTATE PostgreSQL's input
// function reports for it.
class TextError : public std::runtime_error {
public:
  TextError(const std::string& what, const char* sqlstate) : std::runtime_error{what}, sqlstate_{sqlstate} {}
  // With PostgreSQL's DETAIL and CONTEXT: jsonb's input errors carry both.
  TextError(const std::string& what, const char* sqlstate, std::string detail, std::string context)
      : std::runtime_error{what},
        sqlstate_{sqlstate},
        notes_{std::make_shared<const std::pair<std::string, std::string>>(std::move(detail), std::move(context))} {}
  TextError(const TextError&) = default;
  TextError& operator=(const TextError&) = default;
  ~TextError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto sqlstate() const noexcept -> const char* { return sqlstate_; }
  [[nodiscard]] auto detail() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->first} : ""; }
  [[nodiscard]] auto context() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->second} : ""; }

private:
  const char* sqlstate_;  // a literal
  // Shared, so copying the error cannot throw.
  std::shared_ptr<const std::pair<std::string, std::string>> notes_;
};

// A value, not NULL, as PostgreSQL's text output writes a value of type t:
// DateStyle ISO, extra_float_digits 1, bytea_output hex.
[[nodiscard]] auto to_text(const Datum& v, SqlType t) -> std::string;

// Text as a value of type t, as t's input function reads it. A DECIMAL
// finer than its scale is rounded half away from zero when the value is
// only assigned, as PostgreSQL rounds on assignment, and refused otherwise.
// An integer outside its type is the input function's 22003, naming the
// text, as PostgreSQL 17's int2in, int4in and int8in raise it; a REAL
// outside real's range is float4in's. Any other value outside the type's
// range (a DECIMAL past its precision, a VARCHAR too long) is left to the
// layer's range check, which reports it as PostgreSQL does. Throws TextError.
[[nodiscard]] auto from_text(std::string_view text, SqlType t, bool assigned_only) -> Datum;

// PostgreSQL's AVG of an exact number, as numeric's text: its numeric_div
// of the sum (the total at the argument's `scale`) by the count, the scale
// select_div_scale chooses (at least 16 significant digits), rounded half
// away from zero. More digits than 64 bits hold: a plan carries it as this
// text (PhysType::Num, bc_num_quotient).
[[nodiscard]] auto average_text(std::int64_t sum, std::int64_t count, std::uint8_t scale) -> std::string;

// Text as PostgreSQL's numeric_in reads it, written back as numeric_out
// writes it: the form a NUMERIC column stores (PhysType::Num). Every
// digit and the scale are kept ("1.50" stays 1.50, "1.5e1" is 15.0); NaN
// and the infinities are their names; 0x, 0o and 0b integers and _
// between digits are read as PostgreSQL 16 reads them. Throws TextError:
// 22P02 for text that is not a number, 22003 past numeric's format (more
// than 131072 digits before the point or 16383 after it).
[[nodiscard]] auto numeric_in(std::string_view text) -> std::string;

// Text as PostgreSQL 17's jsonb_in reads it, written back as jsonb_out
// writes it: the canonical text a jsonb constant or parameter is folded to
// (docs/jsonb_design.md, *One form for a jsonb value at each boundary*).
// Throws TextError with jsonb_in's SQLSTATE, DETAIL and CONTEXT.
[[nodiscard]] auto jsonb_in(std::string_view text) -> std::string;

// Text functions of UTF-8 text, counted in characters as PostgreSQL's
// are, shared by the binder's constant folds and the plan API's runtime
// (bc_plan.h). UPPER and LOWER fold ASCII only, the layer's one collation
// being COLLATE "C".

inline auto utf8_width(std::string_view s, std::size_t i) -> std::size_t {
  auto b = static_cast<unsigned char>(s[i]);
  auto w = b < 0xC0 ? 1U : b < 0xE0 ? 2U : b < 0xF0 ? 3U : 4U;  // a stray continuation byte is a character of its own
  return std::min<std::size_t>(w, s.size() - i);
}

inline auto char_count(std::string_view s) -> std::int64_t {
  std::int64_t n = 0;
  for (std::size_t i = 0; i < s.size(); i += utf8_width(s, i)) ++n;
  return n;
}

// The byte offset of the character `n` characters in; s.size() past the end.
inline auto char_offset(std::string_view s, std::int64_t n) -> std::size_t {
  std::size_t i = 0;
  while (n-- > 0 && i < s.size()) i += utf8_width(s, i);
  return i;
}

// 1-based character position of the first `needle` in `s`, 0 if none; an
// empty needle is found at 1.
inline auto position_of(std::string_view needle, std::string_view s) -> std::int64_t {
  auto at = s.find(needle);
  return at == std::string_view::npos ? 0 : char_count(s.substr(0, at)) + 1;
}

// s without the characters of `set` from the front, the back, or both.
inline auto trimmed(std::string_view s, std::string_view set, bool front, bool back) -> std::string {
  auto in_set = [&](std::string_view ch) {
    for (std::size_t i = 0; i < set.size(); i += utf8_width(set, i)) {
      if (set.substr(i, utf8_width(set, i)) == ch) return true;
    }
    return false;
  };
  std::size_t b = 0;
  std::size_t e = s.size();
  while (front && b < e && in_set(s.substr(b, utf8_width(s, b)))) b += utf8_width(s, b);
  while (back && e > b) {
    auto i = e - 1;
    while (i > b && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    if (!in_set(s.substr(i, e - i))) break;
    e = i;
  }
  return std::string{s.substr(b, e - b)};
}

inline auto ascii_case(std::string s, bool upper) -> std::string {
  for (auto& ch : s) {
    if (upper && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (!upper && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return s;
}

// EXTRACT's fields, PostgreSQL's date_part units. The values are bc_plan.h's
// bc_date_field codes, which the plan API checks against these.
enum class DateField : std::uint8_t {
  Microseconds = 1,
  Milliseconds,
  Second,
  Minute,
  Hour,
  Day,
  Month,
  Quarter,
  Week,
  Year,
  IsoYear,
  Decade,
  Century,
  Millennium,
  Dow,
  IsoDow,
  Doy,
  Julian,
  Epoch,
  Timezone,
  TimezoneHour,
  TimezoneMinute,
};

// A unit's name as PostgreSQL's date_part reads it, already lower-cased:
// matched on its first ten characters, as its token tables are. A reserved
// word other than epoch ("now", "infinity") names no unit any type has.
struct DateUnit {
  enum class Kind : std::uint8_t { Field, Reserved, Unknown };
  Kind kind;
  DateField field;
};
[[nodiscard]] auto date_unit(std::string_view lowered) -> DateUnit;

// Whether PostgreSQL 17's EXTRACT takes `f` from a value of `source`, one
// of DATE, TIME, TIMESTAMP and TIMESTAMPTZ.
[[nodiscard]] auto date_field_of(DateField f, SqlKind source) -> bool;

// Whether the layer computes it: date_field_of less EPOCH and JULIAN of a
// timestamp, whose values pass the 18 digits of the layer's numeric or are
// fractions of a day.
[[nodiscard]] auto date_field_built(DateField f, SqlKind source) -> bool;

// The scale of EXTRACT's numeric: 6 for SECOND, and EPOCH of other than a
// DATE; 3 for MILLISECONDS; 0 for the rest.
[[nodiscard]] auto date_field_scale(DateField f, SqlKind source) -> std::uint8_t;

// EXTRACT(f FROM v), v not NULL and of `source`, as the scaled value of
// date_field_scale: the proleptic Gregorian calendar, a year before 1 AD
// counted as PostgreSQL counts it (1 BC is -1), a TIMESTAMPTZ in UTC, the
// layer's one time zone. `f` is one date_field_built takes.
[[nodiscard]] auto extract_field(DateField f, SqlKind source, std::int64_t v) -> std::int64_t;

// Casts between the layer's types (docs/frontend_design.md, *Casts of an
// expression*): the one list of which casts exist, shared by the binder's
// constant fold, the checker (plan/physical.cppm), codegen and the plan
// API's cast helpers, so a constant, a parameter and a value computed per
// row cast alike or are refused alike.
//
// What a cast does, as pg_cast and CoerceViaIO say: nothing (a relabel:
// the bytes kept, the type changed); a function of the value; the output
// function of one type read by the input function of the other (a string
// on either side); or no cast at all (42846).
enum class CastKind : std::uint8_t { None, Relabel, Function, InOut };
[[nodiscard]] auto cast_kind(SqlType from, SqlType to) -> CastKind;

// A cast PostgreSQL has that the layer does not take yet, and why: to or
// from NUMERIC of no precision (but jsonb to it, and it to a string type,
// its text); to VARCHAR(n) or CHAR(n), which cut and pad; a float to a
// string, whose text is not float8out's yet; a CHAR(n) to other than a
// string type, whose padded text the layer does not hold; jsonb to
// DECIMAL(p, s). None for a cast cast_kind says is none or a relabel.
enum class CastRefusal : std::uint8_t { None, Numeric, Length, FloatText, CharInput, JsonbDecimal };
[[nodiscard]] auto cast_refusal(SqlType from, SqlType to) -> CastRefusal;

// The cast functions, of a value not NULL. Each throws TextError with
// PostgreSQL 17's SQLSTATE, message and DETAIL.
//
// Text read by `to`'s input function, as a cast reads it: a DECIMAL rounded
// half away from zero to its scale, and outside its precision 22003
// "numeric field overflow" with numeric's DETAIL; any other value outside
// the type out_of_range's message (22003, 22008). from_text's errors else.
[[nodiscard]] auto cast_from_text(std::string_view text, SqlType to) -> Datum;
// A value's text as a cast to TEXT or NAME gives it: a BOOLEAN's is
// booltext's ("true") to text and boolout's ("t") to a name, which is an
// I/O cast; a CHAR(n)'s without trailing spaces (rtrim1); every other
// type's output function's (to_text). Cut to a NAME's 63 bytes for `to`
// NAME.
[[nodiscard]] auto cast_to_text(const Datum& v, SqlType from, SqlType to) -> std::string;
// A DECIMAL at `from_scale` (an integer at 0) as `to`: an integer type,
// rounded half away from zero, outside it "integer out of range" (22003,
// the type's name); or a DECIMAL(p, s), rounded half away from zero to s,
// past p 22003 "numeric field overflow" with numeric's DETAIL.
[[nodiscard]] auto rescale_decimal(std::int64_t v, std::uint8_t from_scale, SqlType to) -> std::int64_t;
// numeric's 22003 DETAIL for a value past DECIMAL(p, s)'s precision.
[[nodiscard]] auto decimal_overflow_detail(SqlType to) -> std::string;
// A float as an integer type: rint (ties to even); NaN, an infinity or a
// value outside the type 22003 "integer out of range" (the type's name).
[[nodiscard]] auto float_to_integer(double d, SqlType to) -> std::int64_t;
// A float as DECIMAL(p, s), as float8_numeric (float4_numeric for `real`)
// reads it: its 15 (6) significant digits' text, read by numeric's input
// and rounded to s. NaN and the infinities 22003 with numeric's DETAIL.
[[nodiscard]] auto float_to_decimal(double d, bool real, SqlType to) -> std::int64_t;
// A DECIMAL as a double, or the nearest REAL (as a double) for `real`: its
// decimal text read correctly rounded, as numeric_float8 and
// numeric_float4 read it. Never fails.
[[nodiscard]] auto decimal_to_float(std::int64_t v, std::uint8_t scale, bool real) -> double;
// A double as the nearest REAL: 22003 "value out of range: overflow" or
// "underflow" for a finite value REAL does not reach.
[[nodiscard]] auto double_to_real(double d) -> double;
// An integer as the nearest REAL, rounded once (int8 to float4 is not
// int8 to float8 to float4).
[[nodiscard]] auto integer_to_real(std::int64_t v) -> double;
// A date or time value as another date or time type (cast_kind's function
// casts between them): a date to a timestamp at midnight, 22008 "date out
// of range for timestamp" past 294276 AD; a timestamp to its date or time
// of day (in UTC); a time to a time with time zone at offset 0, and back;
// a timestamp to a timestamptz and back, unchanged in the layer's UTC.
[[nodiscard]] auto convert_temporal(std::int64_t v, SqlKind from, SqlKind to) -> std::int64_t;

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

// With `utc`, the offset PostgreSQL writes a timestamptz at, before the era.
auto timestamp_text(std::int64_t micros, bool utc = false) -> std::string {
  auto per_day = kSecondsPerDay * kMicrosPerSecond;
  auto days = floor_div(micros, per_day);
  auto c = civil_from_days(days);
  return date_part(c) + " " + time_part(micros - days * per_day) + (utc ? "+00" : "") + (c.year <= 0 ? " BC" : "");
}

// A UTC offset in seconds east as PostgreSQL's EncodeTimezone writes it:
// +HH, then :MM and :SS only where they are not zero.
auto offset_text(std::int64_t offset) -> std::string {
  auto a = offset < 0 ? -offset : offset;
  auto out = std::string{offset < 0 ? "-" : "+"} + pad(a / 3600, 2);
  if (a % 3600 != 0) out += ":" + pad(a / 60 % 60, 2);
  if (a % 60 != 0) out += ":" + pad(a % 60, 2);
  return out;
}

auto timetz_text(std::int64_t v) -> std::string {
  auto t = timetz_unpack(v);
  return time_part(t.time) + offset_text(t.offset);
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

// HH:MM[:SS[.f…f]] as microseconds, as PostgreSQL reads it: the fraction
// of any number of digits (none too) rounded to the microsecond as
// PostgreSQL's rint(strtod(".f…f") * 10^6) does, so to even, carrying into
// the seconds; second 60 (a leap second) as the next minute's first; at most
// 24:00:00 in all, which a timestamp reads as the next day's midnight.
auto read_time(Cursor& c) -> std::int64_t {
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
      std::string fraction = "0.";
      while (std::isdigit(static_cast<unsigned char>(c.peek())) != 0) {
        fraction.push_back(c.peek());
        c.s.remove_prefix(1);
      }
      double f = 0;
      (void)std::from_chars(fraction.data(), fraction.data() + fraction.size(), f);  // "0." alone reads as 0
      frac = static_cast<std::int64_t>(std::rint(f * kMicrosPerSecond));
    }
  }
  auto total = ((h * 60 + m) * 60 + sec) * kMicrosPerSecond + frac;
  if (h > 24 || m > 59 || sec > 60 || total > 24 * 3600 * kMicrosPerSecond) field_out_of_range(c.whole);
  return total;
}

// A UTC offset after a time: Z, or + or - with HH, HH:MM, HHMM or HH:MM:SS,
// as microseconds east of UTC. None where the text has none. PostgreSQL's
// bound is under 16 hours.
auto read_offset(Cursor& c) -> std::optional<std::int64_t> {
  while (c.peek() == ' ' && !(c.s.size() >= 3 && iequals(c.s.substr(1, 2), "BC"))) c.s.remove_prefix(1);
  if (c.peek() == 'Z' || c.peek() == 'z') {
    c.s.remove_prefix(1);
    return 0;
  }
  if (c.peek() != '+' && c.peek() != '-') return std::nullopt;
  auto sign = c.peek() == '-' ? -1 : 1;
  c.s.remove_prefix(1);
  auto h = c.digits(1, 2);
  std::int64_t m = 0;
  std::int64_t sec = 0;
  if (c.peek() == ':' || std::isdigit(static_cast<unsigned char>(c.peek())) != 0) {
    if (c.peek() == ':') c.s.remove_prefix(1);
    m = c.digits(2, 2);
    if (c.peek() == ':') {
      c.s.remove_prefix(1);
      sec = c.digits(2, 2);
    }
  }
  if (h > 15 || m > 59 || sec > 59) {
    throw TextError("time zone displacement out of range: \"" + std::string{c.whole} + "\"", "22009");
  }
  return sign * ((h * 60 + m) * 60 + sec) * kMicrosPerSecond;
}

// A timestamp's text; with `tz`, one that may end in a UTC offset, which is
// taken off (a text without one is UTC, the session's time zone).
auto timestamp_read(std::string_view text, bool tz) -> std::int64_t {
  Cursor c{trim(text), text, tz ? "timestamp with time zone" : "timestamp"};
  auto [y, md] = read_date(c);
  std::int64_t micros = 0;
  std::int64_t offset = 0;
  if (c.peek() == ' ' || c.peek() == 'T') {
    if (c.peek() == ' ' && c.s.size() >= 3 && iequals(c.s.substr(1, 2), "BC")) {
      // the date's era, no time
    } else {
      c.s.remove_prefix(1);
      micros = read_time(c);
      if (tz) offset = read_offset(c).value_or(0);
    }
  }
  auto days = finish_date(c, y, md);
  return days * kSecondsPerDay * kMicrosPerSecond + micros - offset;
}

auto timestamp_from(std::string_view text) -> std::int64_t { return timestamp_read(text, false); }

auto timestamptz_from(std::string_view text) -> std::int64_t { return timestamp_read(text, true); }

auto date_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "date"};
  auto [y, md] = read_date(c);
  return finish_date(c, y, md);
}

auto time_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "time without time zone"};
  auto t = read_time(c);
  if (!c.at_end()) c.fail();
  return t;
}

// A time, after an optional date PostgreSQL reads and ignores (a space
// between them; not ISO 8601's T, which timetz_in rejects), and an
// optional UTC offset; none is the session's time zone, UTC.
auto timetz_from(std::string_view text) -> std::int64_t {
  Cursor c{trim(text), text, "time with time zone"};
  auto digits = c.s.find_first_not_of("0123456789");
  if (digits != std::string_view::npos && digits > 0 && c.s[digits] == '-') {
    auto [y, md] = read_date(c);
    if (c.peek() != ' ') c.fail();
    auto rest = c.s.substr(1);
    c.s = {};
    (void)finish_date(c, y, md);  // the date must be one
    c.s = rest;
  }
  auto t = read_time(c);
  auto offset = read_offset(c).value_or(0);
  if (!c.at_end()) c.fail();
  return timetz_pack({t, offset / kMicrosPerSecond});
}

// ---------------------------------------------------------------- numbers

// An integer's text as PostgreSQL 17's pg_strtoint16/32/64 read it, for a
// type of `bits`: spaces around it, a sign, then decimal digits or a 0x, 0o
// or 0b prefix and its digits, a single _ between digits (and after the
// prefix). A value past the type's range is 22003 naming the text, found as
// the digits are read, so it wins over trailing junk as PostgreSQL's does.
auto integer_from(std::string_view text, std::string_view type, int bits) -> std::int64_t {
  auto s = text;
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  auto negative = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  auto out_of_range = [&] [[noreturn]] {
    throw TextError("value \"" + std::string{text} + "\" is out of range for type " + std::string{type}, "22003");
  };
  std::uint64_t base = 10;
  auto prefixed = false;
  if (s.size() >= 2 && s[0] == '0') {
    auto p = static_cast<char>(std::tolower(static_cast<unsigned char>(s[1])));
    if (p == 'x' || p == 'o' || p == 'b') {
      base = p == 'x' ? 16 : p == 'o' ? 8 : 2;
      prefixed = true;
      s.remove_prefix(2);
    }
  }
  auto digit_of = [&](char ch) -> std::optional<std::uint64_t> {
    auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::uint64_t d = 0;
    if (lower >= '0' && lower <= '9') {
      d = static_cast<std::uint64_t>(lower - '0');
    } else if (lower >= 'a' && lower <= 'f') {
      d = static_cast<std::uint64_t>(lower - 'a') + 10;
    } else {
      return std::nullopt;
    }
    if (d >= base) return std::nullopt;
    return d;
  };
  // The magnitude of the type's most negative value, 2^(bits - 1); checked
  // before each digit as PostgreSQL checks it.
  const auto most = std::uint64_t{1} << (bits - 1);
  std::uint64_t magnitude = 0;
  std::size_t i = 0;
  auto any = false;
  while (i < s.size()) {
    if (auto d = digit_of(s[i])) {
      if (magnitude > most / base) out_of_range();
      magnitude = magnitude * base + *d;
      any = true;
      ++i;
    } else if (s[i] == '_') {
      // Never first (but after a prefix), and a digit must follow.
      if (i == 0 && !prefixed) bad_text(type, text);
      if (i + 1 >= s.size() || !digit_of(s[i + 1])) bad_text(type, text);
      ++i;
    } else {
      break;
    }
  }
  if (!any) bad_text(type, text);
  for (; i < s.size(); ++i) {
    if (!is_space(s[i])) bad_text(type, text);
  }
  if (magnitude > most - (negative ? 0u : 1u)) out_of_range();
  return negative ? static_cast<std::int64_t>(0 - magnitude) : static_cast<std::int64_t>(magnitude);
}

// A float's text, as PostgreSQL's float8in reads it, or float4in when
// `real`: what strtod (strtof) reads, NaN and Infinity in any case. A REAL
// is rounded once, from its text, never through a double; a value whose
// rounding is infinite, or zero from a nonzero text, is out of range.
// Hexadecimal floats, which strtod reads, are 22P02 (from_chars does not).
auto float_from(std::string_view text, bool real) -> double {
  const std::string_view type = real ? "real" : "double precision";
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
  // The number as written, sign included, for the out-of-range message.
  const auto number = s;
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  // As float4in and float8in: a value out of range is 22003 naming only the
  // number read, before any text after it is looked at; then anything left
  // is 22P02 naming the whole input.
  auto read = [&]<typename F>(F& v) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc::result_out_of_range) {
      const auto read_size = static_cast<std::size_t>(end - number.data());
      throw TextError("\"" + std::string{number.substr(0, read_size)} + "\" is out of range for type " +
                          std::string{type},
                      "22003");
    }
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size()) bad_text(type, text);
  };
  if (real) {
    float f = 0;
    read(f);
    return static_cast<double>(f);
  }
  double d = 0;
  read(d);
  return d;
}

// A DECIMAL's text as its integer at `scale`. A value finer than the scale
// is rounded half away from zero when the parameter is only assigned, as
// PostgreSQL rounds on assignment, and refused otherwise: a compared or
// added value is used unrounded there (docs/server_design.md).
//
// NaN and the infinities, which numeric_in reads, a DECIMAL cannot hold: 22003
// with apply_typmod_special's DETAIL for the infinities, and for NaN (which
// PostgreSQL stores) a DETAIL in its form.
auto decimal_from(std::string_view text, SqlType t, bool assigned_only) -> std::int64_t {
  const auto scale = t.scale;
  auto s = trim(text);
  auto special = [&] [[noreturn]] (std::string_view what) {
    throw TextError("numeric field overflow", "22003",
                    "A field with precision " + std::to_string(t.precision) + ", scale " + std::to_string(scale) +
                        " cannot hold " + std::string{what} + ".",
                    "");
  };
  if (iequals(s, "nan")) special("NaN");
  auto negative = false;
  if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (iequals(s, "infinity") || iequals(s, "inf")) special("an infinite value");
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

// PG's hex_decode: only these four separate digit pairs, and a bad digit is
// named whole (a UTF-8 character, not its first byte).
auto hex_space(char c) -> bool { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

[[noreturn]] void bad_hex_digit(std::string_view s, std::size_t at) {
  auto lead = static_cast<unsigned char>(s[at]);
  auto len = lead >= 0xF0 ? 4U : lead >= 0xE0 ? 3U : lead >= 0xC0 ? 2U : 1U;
  throw TextError("invalid hexadecimal digit: \"" + std::string{s.substr(at, len)} + "\"", "22023");
}

auto bytea_from(std::string_view text) -> Bytes {
  Bytes out;
  if (text.starts_with("\\x")) {
    auto s = text.substr(2);
    for (std::size_t i = 0; i < s.size();) {
      if (hex_space(s[i])) {
        ++i;
        continue;
      }
      auto hi = hex_digit(s[i]);
      if (hi < 0) bad_hex_digit(s, i);
      if (i + 1 >= s.size()) throw TextError("invalid hexadecimal data: odd number of digits", "22023");
      auto lo = hex_digit(s[i + 1]);
      if (lo < 0) bad_hex_digit(s, i + 1);
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
    // PG's byteain names no value here, unlike the other input functions.
    throw TextError("invalid input syntax for type bytea", "22P02");
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

auto date_unit(std::string_view lowered) -> DateUnit {
  using F = DateField;
  // PostgreSQL's deltatktbl, then the units and reserved words of its
  // datetktbl; a longer name is matched on its first TOKMAXLEN (10)
  // characters.
  static constexpr std::array<std::pair<std::string_view, F>, 68> kUnits{{
      {"c", F::Century},          {"cent", F::Century},          {"centuries", F::Century},
      {"century", F::Century},    {"d", F::Day},                 {"day", F::Day},
      {"days", F::Day},           {"dec", F::Decade},            {"decade", F::Decade},
      {"decades", F::Decade},     {"decs", F::Decade},           {"h", F::Hour},
      {"hour", F::Hour},          {"hours", F::Hour},            {"hr", F::Hour},
      {"hrs", F::Hour},           {"m", F::Minute},              {"microsecon", F::Microseconds},
      {"mil", F::Millennium},     {"millennia", F::Millennium},  {"millennium", F::Millennium},
      {"millisecon", F::Milliseconds}, {"mils", F::Millennium},  {"min", F::Minute},
      {"mins", F::Minute},        {"minute", F::Minute},         {"minutes", F::Minute},
      {"mon", F::Month},          {"mons", F::Month},            {"month", F::Month},
      {"months", F::Month},       {"ms", F::Milliseconds},       {"msec", F::Milliseconds},
      {"msecond", F::Milliseconds}, {"mseconds", F::Milliseconds}, {"msecs", F::Milliseconds},
      {"qtr", F::Quarter},        {"quarter", F::Quarter},       {"s", F::Second},
      {"sec", F::Second},         {"second", F::Second},         {"seconds", F::Second},
      {"secs", F::Second},        {"timezone", F::Timezone},     {"timezone_h", F::TimezoneHour},
      {"timezone_m", F::TimezoneMinute}, {"us", F::Microseconds}, {"usec", F::Microseconds},
      {"usecond", F::Microseconds}, {"useconds", F::Microseconds}, {"usecs", F::Microseconds},
      {"w", F::Week},             {"week", F::Week},
      {"weeks", F::Week},         {"y", F::Year},                {"year", F::Year},
      {"years", F::Year},         {"yr", F::Year},               {"yrs", F::Year},
      {"dow", F::Dow},            {"doy", F::Doy},               {"isodow", F::IsoDow},
      {"isoyear", F::IsoYear},    {"j", F::Julian},              {"jd", F::Julian},
      {"julian", F::Julian},      {"mm", F::Minute},             {"epoch", F::Epoch},
  }};
  static constexpr std::array<std::string_view, 7> kReserved{"-infinity", "infinity", "now",     "today",
                                                             "tomorrow",  "yesterday", "allballs"};
  auto name = lowered.substr(0, 10);
  for (const auto& [unit, field] : kUnits) {
    if (unit == name) return {DateUnit::Kind::Field, field};
  }
  if (std::ranges::find(kReserved, name) != kReserved.end()) return {DateUnit::Kind::Reserved, F::Epoch};
  return {DateUnit::Kind::Unknown, F::Epoch};
}

auto date_field_of(DateField f, SqlKind source) -> bool {
  using F = DateField;
  auto timestamp = source == SqlKind::Timestamp || source == SqlKind::TimestampTz;
  switch (f) {
    case F::Microseconds:
    case F::Milliseconds:
    case F::Second:
    case F::Minute:
    case F::Hour: return timestamp || source == SqlKind::Time;
    case F::Epoch: return true;
    case F::Day:
    case F::Month:
    case F::Quarter:
    case F::Week:
    case F::Year:
    case F::IsoYear:
    case F::Decade:
    case F::Century:
    case F::Millennium:
    case F::Dow:
    case F::IsoDow:
    case F::Doy:
    case F::Julian: return timestamp || source == SqlKind::Date;
    case F::Timezone:
    case F::TimezoneHour:
    case F::TimezoneMinute: return source == SqlKind::TimestampTz;
  }
  return false;
}

auto date_field_built(DateField f, SqlKind source) -> bool {
  auto timestamp = source == SqlKind::Timestamp || source == SqlKind::TimestampTz;
  return date_field_of(f, source) && !(timestamp && (f == DateField::Epoch || f == DateField::Julian));
}

auto date_field_scale(DateField f, SqlKind source) -> std::uint8_t {
  if (f == DateField::Second || (f == DateField::Epoch && source != SqlKind::Date)) return 6;
  return f == DateField::Milliseconds ? 3 : 0;
}

auto extract_field(DateField f, SqlKind source, std::int64_t v) -> std::int64_t {
  using F = DateField;
  constexpr auto kPerDay = kSecondsPerDay * kMicrosPerSecond;
  constexpr auto kPerMinute = 60 * kMicrosPerSecond;
  std::int64_t days = 0;  // since 1970-01-01
  std::int64_t time = 0;  // microseconds since midnight
  if (source == SqlKind::Date) {
    days = v;
  } else if (source == SqlKind::Time) {
    time = v;
  } else {
    days = floor_div(v, kPerDay);
    time = v - days * kPerDay;
  }
  auto year = [&] { return civil_from_days(days).year; };  // astronomical: 0 is 1 BC
  auto dow = [&] { return days + 4 - floor_div(days + 4, 7) * 7; };  // 1970-01-01 was a Thursday, 0 is Sunday
  auto iso_dow = [&] { return dow() == 0 ? std::int64_t{7} : dow(); };
  // The ISO week's year is the year of its Thursday; week 1 holds the
  // year's first Thursday.
  auto thursday = [&] { return days - iso_dow() + 4; };
  auto bc = [](std::int64_t y) { return y > 0 ? y : y - 1; };
  switch (f) {
    case F::Microseconds:
    case F::Milliseconds:
    case F::Second: return time % kPerMinute;  // the same microseconds, at scales 0, 3 and 6
    case F::Minute: return time / kPerMinute % 60;
    case F::Hour: return time / (60 * kPerMinute);
    case F::Day: return civil_from_days(days).day;
    case F::Month: return civil_from_days(days).month;
    case F::Quarter: return (civil_from_days(days).month - 1) / 3 + 1;
    case F::Week: {
      auto th = thursday();
      return (th - days_from_civil(civil_from_days(th).year, 1, 1)) / 7 + 1;
    }
    case F::Year: return bc(year());
    case F::IsoYear: return bc(civil_from_days(thursday()).year);
    // PostgreSQL's: decade 0 is 9 BC to 1 BC, century and millennium 1
    // start in 1 AD, and before that they count down from -1.
    case F::Decade: return year() >= 0 ? year() / 10 : -((8 - (year() - 1)) / 10);
    case F::Century: return year() > 0 ? (year() + 99) / 100 : -((99 - (year() - 1)) / 100);
    case F::Millennium: return year() > 0 ? (year() + 999) / 1000 : -((999 - (year() - 1)) / 1000);
    case F::Dow: return dow();
    case F::IsoDow: return iso_dow();
    case F::Doy: return days - days_from_civil(year(), 1, 1) + 1;
    case F::Julian: return days + 2'440'588;  // the Julian Day of 1970-01-01
    case F::Epoch: return source == SqlKind::Date ? days * kSecondsPerDay : v;
    case F::Timezone:
    case F::TimezoneHour:
    case F::TimezoneMinute: return 0;  // UTC
  }
  return 0;
}

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
            case SqlKind::TimestampTz: return timestamp_text(x, true);
            case SqlKind::Date: return date_text(x);
            case SqlKind::Time: return time_part(x);
            case SqlKind::TimeTz: return timetz_text(x);
            case SqlKind::SmallInt:
            case SqlKind::Integer:
            case SqlKind::BigInt:
            case SqlKind::Real:
            case SqlKind::Double:
            case SqlKind::Boolean:
            case SqlKind::Bytea:
            case SqlKind::Text:
            case SqlKind::Varchar:
            case SqlKind::Char:
            case SqlKind::Name:
            case SqlKind::Numeric:
            case SqlKind::Jsonb: return std::to_string(x);
          }
          return std::to_string(x);
        }
      },
      v);
}

auto from_text(std::string_view text, SqlType t, bool assigned_only) -> Datum {
  switch (t.kind) {
    case SqlKind::SmallInt: return integer_from(text, "smallint", 16);
    case SqlKind::Integer: return integer_from(text, "integer", 32);
    case SqlKind::BigInt: return integer_from(text, "bigint", 64);
    case SqlKind::Real: return float_from(text, true);
    case SqlKind::Double: return float_from(text, false);
    case SqlKind::Boolean: return bool_from(text);
    case SqlKind::Bytea: return bytea_from(text);
    case SqlKind::Text:
    case SqlKind::Char:
    case SqlKind::Varchar: return text_bytes(text);
    case SqlKind::Name: {
      // Cut to its 63 bytes, as namein reads it.
      auto b = text_bytes(text);
      b.resize(name_prefix(b).size());
      return b;
    }
    case SqlKind::Decimal: return decimal_from(text, t, assigned_only);
    case SqlKind::Timestamp: return timestamp_from(text);
    case SqlKind::TimestampTz: return timestamptz_from(text);
    case SqlKind::Date: return date_from(text);
    case SqlKind::Time: return time_from(text);
    case SqlKind::TimeTz: return timetz_from(text);
    case SqlKind::Numeric: return text_bytes(numeric_in(text));  // its numeric_out text
    case SqlKind::Jsonb: return text_bytes(jsonb_in(text));       // its canonical text
  }
  return text_bytes(text);
}

auto average_text(std::int64_t sum, std::int64_t count, std::uint8_t scale) -> std::string {
  // The magnitude, INT64_MIN's too.
  const auto num = sum < 0 ? 0 - static_cast<std::uint64_t>(sum) : static_cast<std::uint64_t>(sum);
  const auto den = static_cast<std::uint64_t>(count);
  // select_div_scale's weights: of each operand's first non-zero base-10000
  // digit, and that digit. A zero has weight 0 and digit 0.
  auto weight_of = [](std::uint64_t v, std::uint8_t s) -> std::pair<int, std::uint64_t> {
    if (v == 0) return {0, 0};
    auto digits = std::to_string(v);
    if (digits.size() > s) {
      // An integer part: its base-10000 digits, aligned at the point.
      auto whole = digits.substr(0, digits.size() - s);
      auto w = static_cast<int>((whole.size() - 1) / 4);
      return {w, std::stoull(whole.substr(0, whole.size() - static_cast<std::size_t>(w) * 4))};
    }
    // Only a fraction: its base-10000 digits after the point.
    auto frac = std::string(s - digits.size(), '0') + digits;
    frac.append((4 - frac.size() % 4) % 4, '0');
    for (std::size_t i = 0; i < frac.size(); i += 4) {
      auto d = std::stoull(frac.substr(i, 4));
      if (d != 0) return {-static_cast<int>(i / 4) - 1, d};
    }
    return {0, 0};
  };
  auto [w1, d1] = weight_of(num, scale);
  auto [w2, d2] = weight_of(den, 0);
  auto qweight = w1 - w2;
  if (d1 <= d2) --qweight;
  auto rscale = std::clamp(std::max(16 - qweight * 4, static_cast<int>(scale)), 0, 1000);
  // num / den's digits, the quotient's and then its fraction's, one past
  // rscale once moved `scale` places: the last one rounds.
  auto q = num / den;
  unsigned __int128 r = num % den;
  auto digits = std::to_string(q);
  for (auto i = 0; i < rscale - static_cast<int>(scale) + 1; ++i) {
    r *= 10;
    digits.push_back(static_cast<char>('0' + static_cast<int>(r / den)));
    r %= den;
  }
  auto up = digits.back() >= '5';
  digits.pop_back();
  for (auto i = digits.size(); up && i-- > 0;) {
    up = digits[i] == '9';
    digits[i] = up ? '0' : static_cast<char>(digits[i] + 1);
  }
  if (up) digits.insert(digits.begin(), '1');
  auto frac_digits = static_cast<std::size_t>(rscale);
  if (digits.size() <= frac_digits) digits.insert(0, frac_digits + 1 - digits.size(), '0');
  auto whole = digits.substr(0, digits.size() - frac_digits);
  whole.erase(0, std::min(whole.find_first_not_of('0'), whole.size() - 1));
  auto text = whole + (frac_digits > 0 ? "." + digits.substr(digits.size() - frac_digits) : "");
  auto zero = std::ranges::all_of(digits, [](char c) { return c == '0'; });
  return (sum < 0 && !zero ? "-" : "") + text;
}

namespace {

[[noreturn]] void numeric_overflow() { throw TextError("value overflows numeric format", "22003"); }

auto is_digit_in(char c, int base) -> bool {
  if (base == 16) return std::isxdigit(static_cast<unsigned char>(c)) != 0;
  return c >= '0' && c < static_cast<char>('0' + base);
}

// A 0x, 0o or 0b integer's digits (after the prefix, _ between them) as
// decimal digits, without leading zeros. 9 decimal digits a limb, the
// lowest first.
auto non_decimal_digits(std::string_view s, int base, std::string_view text, std::size_t& at) -> std::string {
  std::vector<std::uint32_t> limbs{0};
  constexpr std::uint64_t kLimb = 1'000'000'000;
  auto any = false;
  while (at < s.size()) {
    auto c = s[at];
    if (c == '_') {
      ++at;
      if (at >= s.size() || !is_digit_in(s[at], base)) bad_text("numeric", text);
      continue;
    }
    if (!is_digit_in(c, base)) break;
    auto d = static_cast<std::uint64_t>(std::isdigit(static_cast<unsigned char>(c)) != 0
                                            ? c - '0'
                                            : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
    std::uint64_t carry = d;
    for (auto& l : limbs) {
      auto x = std::uint64_t{l} * static_cast<std::uint64_t>(base) + carry;
      l = static_cast<std::uint32_t>(x % kLimb);
      carry = x / kLimb;
    }
    if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
    if (std::ssize(limbs) * 9 > kNumericMaxWholeDigits + 9) numeric_overflow();
    any = true;
    ++at;
  }
  if (!any) bad_text("numeric", text);
  auto out = std::to_string(limbs.back());
  for (auto i = limbs.size() - 1; i-- > 0;) {
    auto part = std::to_string(limbs[i]);
    out += std::string(9 - part.size(), '0') + part;
  }
  return out;
}

}  // namespace

auto jsonb_in(std::string_view text) -> std::string {
  std::string out;
  auto r = jsonb::canonical_text(text, out);
  if (r.ok()) return out;
  if (r.status == jsonb::Status::Data) {
    const auto& e = *r.error;
    // A literal: the SqlState's text lives as long as the program.
    throw TextError(e.message, e.state.view().data(), e.detail, e.context);
  }
  throw TextError("jsonb value does not decode", "XX000");
}

auto numeric_in(std::string_view text) -> std::string {
  auto s = text;
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  auto rest_is_space = [&](std::size_t from) {
    if (!std::ranges::all_of(s.substr(from), is_space)) bad_text("numeric", text);
  };
  std::size_t at = 0;
  auto negative = false;
  if (at < s.size() && (s[at] == '+' || s[at] == '-')) negative = s[at++] == '-';
  // NaN, Infinity and inf, as float8in spells them: NaN takes no sign.
  if (at >= s.size() || (s[at] != '.' && std::isdigit(static_cast<unsigned char>(s[at])) == 0)) {
    auto starts = [&](std::size_t from, std::string_view word) {
      return s.size() - from >= word.size() && iequals(s.substr(from, word.size()), word);
    };
    if (starts(0, "nan")) {
      rest_is_space(3);
      return "NaN";
    }
    auto len = starts(at, "infinity") ? std::size_t{8} : starts(at, "inf") ? std::size_t{3} : std::size_t{0};
    if (len == 0) bad_text("numeric", text);
    rest_is_space(at + len);
    return negative ? "-Infinity" : "Infinity";
  }
  // 0x, 0o and 0b: an integer, scale 0.
  if (s[at] == '0' && at + 1 < s.size()) {
    auto p = std::tolower(static_cast<unsigned char>(s[at + 1]));
    auto base = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 10;
    if (base != 10) {
      at += 2;
      auto digits = non_decimal_digits(s, base, text, at);
      rest_is_space(at);
      while (digits.size() > 1 && digits.front() == '0') digits.erase(0, 1);
      if (std::ssize(digits) > kNumericMaxWholeDigits) numeric_overflow();
      return (negative && digits != "0" ? "-" : "") + digits;
    }
  }
  // [digits][.digits][e[+-]digits], _ between digits, as set_var_from_str.
  std::string digits(s.size() - at, '\0');
  NumericParts parts;
  switch (numeric_parse(s.substr(at), digits, parts)) {
    case NumericStatus::Ok: break;
    case NumericStatus::Syntax: bad_text("numeric", text);
    case NumericStatus::Overflow: numeric_overflow();
  }
  rest_is_space(at + parts.end);
  if (!numeric_in_range(parts)) numeric_overflow();
  return numeric_text(negative, parts, std::string_view{digits}.substr(0, parts.ndigits));
}

namespace {

auto is_integer_kind(SqlKind k) -> bool { return k == SqlKind::SmallInt || k == SqlKind::Integer || k == SqlKind::BigInt; }
auto is_float_kind(SqlKind k) -> bool { return k == SqlKind::Real || k == SqlKind::Double; }
auto is_string_kind(SqlKind k) -> bool {
  return k == SqlKind::Text || k == SqlKind::Varchar || k == SqlKind::Char || k == SqlKind::Name;
}
auto is_number_kind(SqlKind k) -> bool {
  return is_integer_kind(k) || is_float_kind(k) || k == SqlKind::Decimal || k == SqlKind::Numeric;
}
auto is_instant(SqlKind k) -> bool { return k == SqlKind::Timestamp || k == SqlKind::TimestampTz; }

auto integer_name(SqlKind k) -> std::string {
  return k == SqlKind::SmallInt ? "smallint" : k == SqlKind::Integer ? "integer" : "bigint";
}

[[noreturn]] void integer_out_of_range(SqlKind k) { throw TextError(integer_name(k) + " out of range", "22003"); }

// 294277-01-01, PostgreSQL's TIMESTAMP_END_JULIAN, in days since 1970-01-01.
constexpr std::int64_t kEndTimestampDays = 106'762'940;

}  // namespace

auto cast_kind(SqlType from, SqlType to) -> CastKind {
  using K = SqlKind;
  auto f = from.kind;
  auto t = to.kind;
  if (from == to) return CastKind::Relabel;
  // To a string type: of a text, a relabel (VARCHAR of no length is TEXT);
  // a CHAR(n)'s text without its trailing spaces, a name's cut, and a
  // length's cut or pad are functions; any other type's output function.
  if (is_string_kind(t)) {
    if (t == K::Text && (f == K::Text || f == K::Varchar || f == K::Name)) return CastKind::Relabel;
    if (is_string_kind(f)) return CastKind::Function;
    return f == K::Boolean && t != K::Name ? CastKind::Function : CastKind::InOut;  // booltext; boolout to a name
  }
  // From a string type: the target's input function.
  if (is_string_kind(f)) return CastKind::InOut;
  switch (t) {
    case K::SmallInt:
    case K::Integer:
    case K::BigInt:
      if (is_number_kind(f) || f == K::Jsonb || (f == K::Boolean && t == K::Integer)) return CastKind::Function;
      return CastKind::None;
    case K::Real:
    case K::Double:
    case K::Decimal:
    case K::Numeric: return is_number_kind(f) || f == K::Jsonb ? CastKind::Function : CastKind::None;
    case K::Boolean: return f == K::Integer || f == K::Jsonb ? CastKind::Function : CastKind::None;
    case K::Date: return is_instant(f) ? CastKind::Function : CastKind::None;
    case K::Time: return is_instant(f) || f == K::TimeTz ? CastKind::Function : CastKind::None;
    case K::Timestamp:
    case K::TimestampTz: return f == K::Date || is_instant(f) ? CastKind::Function : CastKind::None;
    case K::TimeTz: return f == K::Time || f == K::TimestampTz ? CastKind::Function : CastKind::None;
    case K::Bytea:
    case K::Jsonb: return CastKind::None;
    case K::Text:
    case K::Varchar:
    case K::Char:
    case K::Name: break;  // above
  }
  return CastKind::None;
}

auto cast_refusal(SqlType from, SqlType to) -> CastRefusal {
  using K = SqlKind;
  auto kind = cast_kind(from, to);
  if (kind == CastKind::None || kind == CastKind::Relabel) return CastRefusal::None;
  if (to.kind == K::Varchar || to.kind == K::Char) return CastRefusal::Length;
  if (from.kind == K::Numeric && to.kind != K::Text && to.kind != K::Name) return CastRefusal::Numeric;
  if (to.kind == K::Numeric && from.kind != K::Jsonb) return CastRefusal::Numeric;
  if (is_float_kind(from.kind) && is_string_kind(to.kind)) return CastRefusal::FloatText;
  if (from.kind == K::Char && !is_string_kind(to.kind)) return CastRefusal::CharInput;
  if (from.kind == K::Jsonb && to.kind == K::Decimal) return CastRefusal::JsonbDecimal;
  return CastRefusal::None;
}

auto decimal_overflow_detail(SqlType to) -> std::string {
  // numeric's apply_typmod: 10^0 is written 1.
  auto digits = static_cast<int>(to.precision) - static_cast<int>(to.scale);
  return "A field with precision " + std::to_string(to.precision) + ", scale " + std::to_string(to.scale) +
         " must round to an absolute value less than " + (digits > 0 ? "10^" + std::to_string(digits) : std::string{"1"}) +
         ".";
}

auto cast_from_text(std::string_view text, SqlType to) -> Datum {
  Datum d;
  try {
    d = from_text(text, to, true);
  } catch (const TextError& e) {
    // Past 64 bits: the same overflow as past the precision, with its DETAIL.
    if (to.kind == SqlKind::Decimal && e.detail().empty() && std::string_view{e.what()} == "numeric field overflow") {
      throw TextError("numeric field overflow", "22003", decimal_overflow_detail(to), "");
    }
    throw;
  }
  if (auto why = out_of_range(to, d)) {
    if (to.kind == SqlKind::Decimal) throw TextError(*why, "22003", decimal_overflow_detail(to), "");
    auto state = range_sqlstate(*why);
    throw TextError(*why, state == "22008" ? "22008" : state == "22021" ? "22021" : state == "22001" ? "22001" : "22003");
  }
  return d;
}

auto cast_to_text(const Datum& v, SqlType from, SqlType to) -> std::string {
  std::string out;
  if (from.kind == SqlKind::Boolean) {
    auto b = std::get<bool>(v);
    out = to.kind == SqlKind::Name ? (b ? "t" : "f") : (b ? "true" : "false");
  } else {
    out = to_text(v, from);
    if (from.kind == SqlKind::Char) {
      while (!out.empty() && out.back() == ' ') out.pop_back();
    }
  }
  if (to.kind == SqlKind::Name) {
    auto b = std::as_bytes(std::span{out.data(), out.size()});
    out.resize(name_prefix(b).size());
  }
  return out;
}

auto rescale_decimal(std::int64_t v, std::uint8_t from_scale, SqlType to) -> std::int64_t {
  auto to_scale = to.kind == SqlKind::Decimal ? to.scale : std::uint8_t{0};
  if (from_scale > kMaxDecimalPrecision || to_scale > kMaxDecimalPrecision) {
    throw std::invalid_argument{"a DECIMAL scale past 18"};
  }
  auto overflow = [&] [[noreturn]] {
    if (to.kind == SqlKind::Decimal) throw TextError("numeric field overflow", "22003", decimal_overflow_detail(to), "");
    integer_out_of_range(to.kind);
  };
  // The magnitude, INT64_MIN's too, rounded half away from zero.
  auto negative = v < 0;
  auto mag = negative ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
  if (to_scale >= from_scale) {
    auto factor = static_cast<std::uint64_t>(kPowersOfTen[to_scale - from_scale]);
    if (mag != 0 && mag > std::numeric_limits<std::uint64_t>::max() / factor) overflow();
    mag *= factor;
  } else {
    auto unit = static_cast<std::uint64_t>(kPowersOfTen[from_scale - to_scale]);
    auto rest = mag % unit;
    mag /= unit;
    if (rest >= unit - rest) ++mag;  // 2 * rest >= unit, without overflow
  }
  auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (mag > limit + (negative ? 1U : 0U)) overflow();
  auto out = negative ? static_cast<std::int64_t>(0 - mag) : static_cast<std::int64_t>(mag);
  if (out_of_range(to.kind == SqlKind::Decimal ? to : SqlType{to.kind}, out)) overflow();
  return out;
}

auto float_to_integer(double d, SqlType to) -> std::int64_t {
  auto r = std::rint(d);
  // 2^63 is the first double past int64_t; -2^63 is in range.
  if (!std::isfinite(r) || r >= 9223372036854775808.0 || r < -9223372036854775808.0) integer_out_of_range(to.kind);
  auto v = static_cast<std::int64_t>(r);
  if (out_of_range(SqlType{to.kind}, v)) integer_out_of_range(to.kind);
  return v;
}

auto float_to_decimal(double d, bool real, SqlType to) -> std::int64_t {
  auto special = [&](std::string_view what) {
    throw TextError("numeric field overflow", "22003",
                    "A field with precision " + std::to_string(to.precision) + ", scale " + std::to_string(to.scale) +
                        " cannot hold " + std::string{what} + ".",
                    "");
  };
  if (std::isnan(d)) special("NaN");
  if (std::isinf(d)) special("an infinite value");
  std::array<char, 64> buf{};
  auto r = real ? std::to_chars(buf.data(), buf.data() + buf.size(), static_cast<float>(d), std::chars_format::general, 6)
                : std::to_chars(buf.data(), buf.data() + buf.size(), d, std::chars_format::general, 15);
  return std::get<std::int64_t>(cast_from_text(std::string_view{buf.data(), static_cast<std::size_t>(r.ptr - buf.data())}, to));
}

auto decimal_to_float(std::int64_t v, std::uint8_t scale, bool real) -> double {
  auto text = decimal_text(v, scale);
  if (real) {
    float f = 0;
    (void)std::from_chars(text.data(), text.data() + text.size(), f);
    return static_cast<double>(f);
  }
  double d = 0;
  (void)std::from_chars(text.data(), text.data() + text.size(), d);
  return d;
}

auto double_to_real(double d) -> double {
  if (auto why = out_of_range(SqlType{SqlKind::Real}, d)) throw TextError(*why, "22003");
  return static_cast<double>(static_cast<float>(d));
}

auto integer_to_real(std::int64_t v) -> double { return static_cast<double>(static_cast<float>(v)); }

auto convert_temporal(std::int64_t v, SqlKind from, SqlKind to) -> std::int64_t {
  using K = SqlKind;
  auto day_of = [](std::int64_t us) { return floor_div(us, kMicrosPerDay); };
  if (from == to || (is_instant(from) && is_instant(to))) return v;
  if (from == K::Date && is_instant(to)) {
    if (v >= kEndTimestampDays) throw TextError("date out of range for timestamp", "22008");
    // The layer's microseconds count from 1970, not 2000: its TIMESTAMP ends
    // about 30 years before PostgreSQL's (docs/types_design.md).
    if (v > std::numeric_limits<std::int64_t>::max() / kMicrosPerDay) throw TextError("timestamp out of range", "22008");
    return v * kMicrosPerDay;  // kMinDate's micros are kMinTimestamp: in range
  }
  if (is_instant(from) && to == K::Date) return day_of(v);
  if (is_instant(from) && to == K::Time) return v - day_of(v) * kMicrosPerDay;
  if (from == K::TimestampTz && to == K::TimeTz) return timetz_pack({v - day_of(v) * kMicrosPerDay, 0});
  if (from == K::Time && to == K::TimeTz) return timetz_pack({v, 0});
  if (from == K::TimeTz && to == K::Time) return timetz_unpack(v).time;
  throw std::invalid_argument{"convert_temporal: no cast between these types"};
}

}  // namespace bytecask::sql
