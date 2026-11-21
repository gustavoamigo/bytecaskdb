// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb.cppm — PostgreSQL 17's jsonb, stage 1 of docs/jsonb_design.md: the
// input parser (jsonb_in's grammar, errors and normalisation), the encoder of
// the binary layout (*Binary layout of a jsonb value*), the full check of a
// stored value and the output writer (jsonb_out's text, byte for byte).
//
// Nothing here recurses over a value: the parser keeps its open containers
// on its node array, and every walk of a stored value keeps an explicit
// stack, bounded at kMaxDepth. Every buffer is charged to the caller's
// Memory before it is allocated. A stored value is read by the rules of
// *Decoding a stored jsonb value without overflow*: positions are size_t,
// every number read from the value is checked against its extent before it
// is used, and a break of any rule is Status::Corrupt, never a read outside
// the value. The PL/pgSQL reader (sql/json.cppm) is a separate thing.

module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bytecask.sql.jsonb;

export import bytecask.sql.sqlstate;
import bytecask.sql.numeric;

export namespace bytecask::sql::jsonb {

using ByteView = std::span<const std::byte>;

// The format byte of the layout this build writes; a later layout takes the
// next one. It can never start UTF-8 text.
inline constexpr std::uint8_t kFormat = 0xF8;
inline constexpr std::uint8_t kTagNull = 0x01;
inline constexpr std::uint8_t kTagFalse = 0x02;
inline constexpr std::uint8_t kTagTrue = 0x03;
inline constexpr std::uint8_t kTagNumber = 0x04;
inline constexpr std::uint8_t kTagString = 0x05;
inline constexpr std::uint8_t kTagArray = 0x06;
inline constexpr std::uint8_t kTagObject = 0x07;
// Nested containers a value may hold (owner's decision 9): 54001 past it on
// input, Corrupt past it in a stored value.
inline constexpr std::size_t kMaxDepth = 10000;
// PostgreSQL's bound on one container and one string, and the engine's
// ceiling on a value: every offset of an encoded value fits a u32.
inline constexpr std::size_t kMaxBytes = (std::size_t{1} << 28) - 1;
// MaxAllocSize - 1: the most bytes a text a value reads from or prints to.
inline constexpr std::size_t kMaxText = (std::size_t{1} << 30) - 1;
// The size of a parser node (*Parsing a jsonb value*).
inline constexpr std::size_t kNodeBytes = 28;

// The working buffer a charge is for: the parser's nodes, its key index and
// its pool of de-escaped keys, and a walk's stack. Named so an
// allocation-failure sweep can show it failed each one (tests/jsonb_test.cpp).
enum class Site : std::uint8_t { Nodes, Keys, Pool, Stack };

// Where a parse or a walk charges what it allocates: take() before the
// allocation, false when refused (the caller's error is then set), and
// release() when it is freed. Without functions, nothing is charged.
struct Memory {
  void* self{nullptr};
  bool (*take_fn)(void*, std::uint64_t, Site){nullptr};
  void (*release_fn)(void*, std::uint64_t) noexcept {nullptr};

  [[nodiscard]] auto take(std::uint64_t n, Site site) const -> bool { return take_fn == nullptr || take_fn(self, n, site); }
  void release(std::uint64_t n) const noexcept {
    if (release_fn != nullptr) release_fn(self, n);
  }
};

// Where an encoded value or a text goes: n bytes, charged by the callee;
// nullptr when the charge is refused. May throw std::bad_alloc.
struct Alloc {
  void* self{nullptr};
  std::byte* (*fn)(void*, std::size_t){nullptr};
};

// A user error, as PostgreSQL raises it: its SQLSTATE, message, DETAIL, HINT
// and CONTEXT (each "" for none).
struct Error {
  SqlState state;
  std::string message;
  std::string detail;
  std::string hint;
  std::string context;
};

enum class Status : std::uint8_t {
  Ok,
  Data,     // a user error: Outcome::error
  Refused,  // a Memory or Alloc charge was refused; the caller's error is set
  Corrupt,  // a stored value broke a decoding rule: Outcome::corrupt names it
};

struct Outcome {
  Status status{Status::Ok};
  std::optional<Error> error;
  const char* corrupt{""};  // a literal

  [[nodiscard]] auto ok() const noexcept -> bool { return status == Status::Ok; }
};

// Text as PostgreSQL 17's jsonb_in reads it, encoded as a stored value (the
// format byte, then the node) into what `out` gives. The text is checked as
// UTF-8 first (22021). Errors: 22P02 with jsonb_in's DETAIL and CONTEXT,
// 22P05 for \u0000, 22003 past numeric's range, 54001 past kMaxDepth,
// 54000 past kMaxBytes. `value` is the encoded value on success.
[[nodiscard]] auto parse(std::string_view text, Memory mem, Alloc out, std::span<std::byte>& value) -> Outcome;

// The node of a stored value: what follows a known format byte, at least
// one byte. nullopt otherwise.
[[nodiscard]] auto node_of(ByteView value) -> std::optional<ByteView>;

// The full check of a stored value (*Validation of a stored jsonb value*):
// its format byte, then every decoding rule, keys strictly increasing in
// storage order, every string and key valid UTF-8 without a NUL byte.
[[nodiscard]] auto check_value(ByteView value, Memory mem) -> Outcome;
// The same check of a node, without the format byte.
[[nodiscard]] auto check_node(ByteView node, Memory mem) -> Outcome;

// The length of a node's text (jsonb_out's), every rule checked as the walk
// goes. Not capped: the caller refuses one past kMaxText.
[[nodiscard]] auto text_length(ByteView node, Memory mem, std::uint64_t& length) -> Outcome;
// Writes the text into `out`, which text_length sized; a write past it is
// Corrupt, never a write outside `out`.
[[nodiscard]] auto write_text(ByteView node, Memory mem, std::span<char> out) -> Outcome;
// Both, into what `out` gives: past kMaxText is 54000, refused before the
// text is allocated.
[[nodiscard]] auto to_text(ByteView node, Memory mem, Alloc out, std::span<char>& text) -> Outcome;

// Text parsed and printed again, uncharged: the canonical text of a constant
// or a catalog default. The parse's Outcome on a failure.
[[nodiscard]] auto canonical_text(std::string_view text, std::string& out) -> Outcome;
// A stored value's text, uncharged; Corrupt when it does not decode.
[[nodiscard]] auto value_text(ByteView value, std::string& out) -> Outcome;
// Text parsed into a stored value, uncharged.
[[nodiscard]] auto parse_value(std::string_view text, std::vector<std::byte>& out) -> Outcome;

// What a call returns for 54001 and the 54000 messages: one place for
// PostgreSQL's words.
[[nodiscard]] auto depth_error() -> Error;

}  // namespace bytecask::sql::jsonb

namespace bytecask::sql::jsonb {

namespace {

constexpr std::uint32_t kNoParent = std::numeric_limits<std::uint32_t>::max();

auto byte_at(std::string_view s, std::size_t i) -> unsigned { return static_cast<unsigned char>(s[i]); }

// A vector whose capacity is charged to Memory before it grows, and released
// when it goes. Grows by doubling, so its charge is at most twice its use.
template <typename T>
class Scratch {
public:
  Scratch(Memory mem, Site site) : mem_{mem}, site_{site} {}
  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  ~Scratch() { mem_.release(held_); }

  // Capacity for n elements: false when the charge is refused. Throws
  // std::bad_alloc with nothing charged.
  [[nodiscard]] auto reserve(std::size_t n) -> bool {
    if (n <= v_.capacity()) return true;
    auto want = std::max(n, v_.capacity() * 2);
    auto bytes = static_cast<std::uint64_t>(want) * sizeof(T);
    if (!mem_.take(bytes - held_, site_)) return false;
    try {
      v_.reserve(want);
    } catch (...) {
      mem_.release(bytes - held_);
      throw;
    }
    held_ = bytes;
    return true;
  }
  [[nodiscard]] auto push(const T& x) -> bool {
    if (!reserve(v_.size() + 1)) return false;
    v_.push_back(x);
    return true;
  }
  // Exactly n elements of T{}: false when the charge is refused.
  [[nodiscard]] auto assign(std::size_t n) -> bool {
    if (n > v_.capacity()) {
      auto bytes = static_cast<std::uint64_t>(n) * sizeof(T);
      if (!mem_.take(bytes - held_, site_)) return false;
      try {
        v_.reserve(n);
      } catch (...) {
        mem_.release(bytes - held_);
        throw;
      }
      held_ = bytes;
    }
    v_.assign(n, T{});
    return true;
  }
  auto vec() -> std::vector<T>& { return v_; }

private:
  Memory mem_;
  Site site_;
  std::vector<T> v_;
  std::uint64_t held_{0};
};

// --------------------------------------------------------------- UTF-8

// pg_utf_mblen: the length a first byte announces, 1 for a byte that
// starts no sequence.
auto mblen(unsigned b) -> std::size_t {
  if ((b & 0x80u) == 0) return 1;
  if ((b & 0xE0u) == 0xC0u) return 2;
  if ((b & 0xF0u) == 0xE0u) return 3;
  if ((b & 0xF8u) == 0xF0u) return 4;
  return 1;
}

// The length of the valid UTF-8 sequence (RFC 3629, no NUL) at s[i], 0 for
// none: the shortest form, inside U+0001..U+10FFFF, no surrogate.
auto utf8_sequence(const unsigned char* s, std::size_t n, std::size_t i) -> std::size_t {
  auto b0 = s[i];
  if (b0 == 0x00) return 0;
  if (b0 < 0x80) return 1;
  std::size_t len = 0;
  unsigned lo = 0x80;
  unsigned hi = 0xBF;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    len = 2;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    len = 3;
    if (b0 == 0xE0) lo = 0xA0;
    if (b0 == 0xED) hi = 0x9F;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    len = 4;
    if (b0 == 0xF0) lo = 0x90;
    if (b0 == 0xF4) hi = 0x8F;
  } else {
    return 0;
  }
  if (n - i < len) return 0;
  if (s[i + 1] < lo || s[i + 1] > hi) return 0;
  for (std::size_t k = 2; k < len; ++k) {
    if (s[i + k] < 0x80 || s[i + k] > 0xBF) return 0;
  }
  return len;
}

auto utf8_valid(const unsigned char* s, std::size_t n) -> bool {
  for (std::size_t i = 0; i < n;) {
    auto len = utf8_sequence(s, n, i);
    if (len == 0) return false;
    i += len;
  }
  return true;
}

auto utf8_width(std::uint32_t cp) -> std::uint32_t { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

auto put_utf8(std::byte* out, std::uint32_t cp) -> std::size_t {
  auto b = [](std::uint32_t v) { return static_cast<std::byte>(v); };
  if (cp < 0x80) {
    out[0] = b(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = b(0xC0 | (cp >> 6));
    out[1] = b(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = b(0xE0 | (cp >> 12));
    out[1] = b(0x80 | ((cp >> 6) & 0x3F));
    out[2] = b(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = b(0xF0 | (cp >> 18));
  out[1] = b(0x80 | ((cp >> 12) & 0x3F));
  out[2] = b(0x80 | ((cp >> 6) & 0x3F));
  out[3] = b(0x80 | (cp & 0x3F));
  return 4;
}

// ----------------------------------------------------------------- lexer

enum class Token : std::uint8_t {
  Invalid,
  End,
  ObjectStart,
  ObjectEnd,
  ArrayStart,
  ArrayEnd,
  Comma,
  Colon,
  String,
  Number,
  True,
  False,
  Null,
};

// PostgreSQL 17's JsonParseErrorType, as far as jsonb_in raises them.
enum class Fault : std::uint8_t {
  InvalidToken,
  EscapingRequired,
  EscapingInvalid,
  UnicodeEscapeFormat,
  UnicodeHighSurrogate,
  UnicodeLowSurrogate,
  CodePointZero,
  ExpectedMore,
  ExpectedEnd,
  ExpectedJson,
  ExpectedString,
  ExpectedArrayNext,
  ExpectedObjectFirst,
  ExpectedColon,
  ExpectedObjectNext,
};

// The context a parse error is reported in (report_parse_error's).
enum class Expect : std::uint8_t { End, Value, String, ArrayNext, ObjectFirst, Label, ObjectNext };

// src/common/jsonapi.c's json_lex, for a whole input: the same tokens, the
// same extent of an invalid one, the same line numbers.
struct Lexer {
  std::string_view in;
  std::size_t pos{0};  // the next byte to read
  std::size_t token_start{0};
  std::size_t token_end{0};
  Token token{Token::Invalid};
  int line{1};
  std::size_t line_start{0};
  // A string token's: its de-escaped length, and whether it holds a '\'.
  std::uint64_t str_len{0};
  bool escaped{false};

  // FAIL_AT_CHAR_END: the token ends after the character at s.
  auto fail_at_char_end(std::size_t s, Fault f) -> Fault {
    token_end = std::min(s + mblen(byte_at(in, s)), in.size());
    return f;
  }

  // The next token; a Fault when it does not lex.
  auto lex() -> std::optional<Fault> {
    auto end = in.size();
    auto s = pos;
    while (s < end && (in[s] == ' ' || in[s] == '\t' || in[s] == '\n' || in[s] == '\r')) {
      if (in[s++] == '\n') {
        ++line;
        line_start = s;
      }
    }
    token_start = s;
    if (s >= end) {
      token = Token::End;
      token_end = s;
      pos = s;
      return std::nullopt;
    }
    auto single = [&](Token t) {
      token = t;
      token_end = s + 1;
      pos = token_end;
      return std::nullopt;
    };
    switch (in[s]) {
      case '{': return single(Token::ObjectStart);
      case '}': return single(Token::ObjectEnd);
      case '[': return single(Token::ArrayStart);
      case ']': return single(Token::ArrayEnd);
      case ',': return single(Token::Comma);
      case ':': return single(Token::Colon);
      case '"': return lex_string(s);
      case '-': return lex_number(s + 1);
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9': return lex_number(s);
      default: break;
    }
    auto p = s;
    while (p < end && alnum(byte_at(in, p))) ++p;
    if (p == s) {
      token_end = s + 1;
      return Fault::InvalidToken;
    }
    auto word = in.substr(s, p - s);
    token_end = p;
    if (word == "true") {
      token = Token::True;
    } else if (word == "false") {
      token = Token::False;
    } else if (word == "null") {
      token = Token::Null;
    } else {
      return Fault::InvalidToken;
    }
    pos = p;
    return std::nullopt;
  }

  // JSON_ALPHANUMERIC_CHAR: letters, digits, _ and every byte >= 0x80.
  static auto alnum(unsigned c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
  }

  auto lex_number(std::size_t s) -> std::optional<Fault> {
    auto end = in.size();
    auto digit = [&](std::size_t i) { return i < end && in[i] >= '0' && in[i] <= '9'; };
    auto error = false;
    if (s < end && in[s] == '0') {
      ++s;
    } else if (s < end && in[s] >= '1' && in[s] <= '9') {
      do ++s;
      while (digit(s));
    } else {
      error = true;
    }
    if (s < end && in[s] == '.') {
      ++s;
      if (!digit(s)) {
        error = true;
      } else {
        do ++s;
        while (digit(s));
      }
    }
    if (s < end && (in[s] == 'e' || in[s] == 'E')) {
      ++s;
      if (s < end && (in[s] == '+' || in[s] == '-')) ++s;
      if (!digit(s)) {
        error = true;
      } else {
        do ++s;
        while (digit(s));
      }
    }
    for (; s < end && alnum(byte_at(in, s)); ++s) error = true;
    token_end = s;
    if (error) return Fault::InvalidToken;
    token = Token::Number;
    pos = s;
    return std::nullopt;
  }

  auto lex_string(std::size_t s) -> std::optional<Fault> {
    auto end = in.size();
    str_len = 0;
    escaped = false;
    std::int32_t hi = -1;
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (;;) {
      ++s;
      if (s >= end) {
        token_end = end;
        return Fault::InvalidToken;
      }
      auto c = in[s];
      if (c == '"') break;
      if (c == '\\') {
        escaped = true;
        ++s;
        if (s >= end) {
          token_end = end;
          return Fault::InvalidToken;
        }
        if (in[s] == 'u') {
          std::int32_t ch = 0;
          for (int i = 1; i <= 4; ++i) {
            ++s;
            if (s >= end) {
              token_end = end;
              return Fault::InvalidToken;
            }
            auto h = hex(in[s]);
            if (h < 0) return fail_at_char_end(s, Fault::UnicodeEscapeFormat);
            ch = ch * 16 + h;
          }
          if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (hi != -1) return fail_at_char_end(s, Fault::UnicodeHighSurrogate);
            hi = ch;
            continue;
          }
          if (ch >= 0xDC00 && ch <= 0xDFFF) {
            if (hi == -1) return fail_at_char_end(s, Fault::UnicodeLowSurrogate);
            ch = 0x10000 + ((hi - 0xD800) << 10) + (ch - 0xDC00);
            hi = -1;
          }
          if (hi != -1) return fail_at_char_end(s, Fault::UnicodeLowSurrogate);
          if (ch == 0) return fail_at_char_end(s, Fault::CodePointZero);
          str_len += utf8_width(static_cast<std::uint32_t>(ch));
        } else {
          if (hi != -1) return fail_at_char_end(s, Fault::UnicodeLowSurrogate);
          switch (in[s]) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't': ++str_len; break;
            default:
              // Only the escape sequence is reported, not the whole string.
              token_start = s;
              return fail_at_char_end(s, Fault::EscapingInvalid);
          }
        }
      } else {
        if (hi != -1) return fail_at_char_end(s, Fault::UnicodeLowSurrogate);
        auto p = s;
        for (; p < end; ++p) {
          if (in[p] == '\\' || in[p] == '"') break;
          if (byte_at(in, p) <= 31) {
            token_end = p;  // the character itself is not shown
            return Fault::EscapingRequired;
          }
        }
        str_len += p - s;
        s = p - 1;
      }
    }
    if (hi != -1) {
      token_end = s + 1;
      return Fault::UnicodeLowSurrogate;
    }
    token = Token::String;
    token_end = s + 1;
    pos = token_end;
    return std::nullopt;
  }

  // report_parse_error: the fault a token in the wrong place is.
  auto unexpected(Expect e) const -> Fault {
    if (token == Token::End) return Fault::ExpectedMore;
    switch (e) {
      case Expect::End: return Fault::ExpectedEnd;
      case Expect::Value: return Fault::ExpectedJson;
      case Expect::String: return Fault::ExpectedString;
      case Expect::ArrayNext: return Fault::ExpectedArrayNext;
      case Expect::ObjectFirst: return Fault::ExpectedObjectFirst;
      case Expect::Label: return Fault::ExpectedColon;
      case Expect::ObjectNext: return Fault::ExpectedObjectNext;
    }
    return Fault::ExpectedJson;
  }

  // json_errdetail's DETAIL and report_json_context's CONTEXT for a fault
  // at the current token.
  [[nodiscard]] auto error(Fault f) const -> Error {
    auto tok = std::string{in.substr(token_start, token_end - token_start)};
    std::string detail;
    switch (f) {
      case Fault::InvalidToken: detail = "Token \"" + tok + "\" is invalid."; break;
      case Fault::EscapingRequired: {
        static constexpr char kHex[] = "0123456789abcdef";
        auto c = token_end < in.size() ? byte_at(in, token_end) : 0u;
        detail = std::string{"Character with value 0x"} + kHex[c >> 4] + kHex[c & 0xF] + " must be escaped.";
        break;
      }
      case Fault::EscapingInvalid: detail = "Escape sequence \"\\" + tok + "\" is invalid."; break;
      case Fault::UnicodeEscapeFormat: detail = "\"\\u\" must be followed by four hexadecimal digits."; break;
      case Fault::UnicodeHighSurrogate: detail = "Unicode high surrogate must not follow a high surrogate."; break;
      case Fault::UnicodeLowSurrogate: detail = "Unicode low surrogate must follow a high surrogate."; break;
      case Fault::CodePointZero: detail = "\\u0000 cannot be converted to text."; break;
      case Fault::ExpectedMore: detail = "The input string ended unexpectedly."; break;
      case Fault::ExpectedEnd: detail = "Expected end of input, but found \"" + tok + "\"."; break;
      case Fault::ExpectedJson: detail = "Expected JSON value, but found \"" + tok + "\"."; break;
      case Fault::ExpectedString: detail = "Expected string, but found \"" + tok + "\"."; break;
      case Fault::ExpectedArrayNext: detail = "Expected \",\" or \"]\", but found \"" + tok + "\"."; break;
      case Fault::ExpectedObjectFirst: detail = "Expected string or \"}\", but found \"" + tok + "\"."; break;
      case Fault::ExpectedColon: detail = "Expected \":\", but found \"" + tok + "\"."; break;
      case Fault::ExpectedObjectNext: detail = "Expected \",\" or \"}\", but found \"" + tok + "\"."; break;
    }
    // The line of the error from its start, or from "..." and at most 49
    // bytes ending at the token's end, starting on a character boundary.
    auto start = line_start;
    auto stop = token_end;
    while (stop - start >= 50) start += mblen(byte_at(in, start));
    if (start - line_start <= 3) start = line_start;
    auto prefix = start > line_start ? "..." : "";
    auto suffix = token != Token::End && stop < in.size() && in[stop] != '\n' && in[stop] != '\r' ? "..." : "";
    auto context = "JSON data, line " + std::to_string(line) + ": " + prefix + std::string{in.substr(start, stop - start)} + suffix;
    if (f == Fault::CodePointZero) {
      return {kUntranslatableCharacter, "unsupported Unicode escape sequence", std::move(detail), "", std::move(context)};
    }
    return {kInvalidTextRepresentation, "invalid input syntax for type json", std::move(detail), "", std::move(context)};
  }
};

// ---------------------------------------------------------------- parser

constexpr std::uint8_t kEscaped = 1;   // a string that holds a '\'
constexpr std::uint8_t kNegative = 2;  // a number below zero
constexpr std::uint8_t kPlaced = 4;    // pass 3: its parent gave it a position
constexpr std::uint8_t kKey = 8;       // an object's key

// One value of the input, in document order (*Parsing a jsonb value*).
struct Node {
  std::uint32_t src{0};    // its token's offset; an object: its pairs' start in the key index
  std::uint32_t len{0};    // its token's length; an object: its pairs once duplicates are dropped
  std::uint32_t end{0};    // the index past its subtree; an open container: its parent's
  std::uint32_t count{0};  // array: elements; object: pairs; string: de-escaped bytes; number: digits
  std::uint32_t aux{0};    // number: its weight; an escaped key: its offset in the key pool
  std::uint32_t size{0};   // its encoded size; in pass 3, its position in the value
  std::uint16_t dscale{0};
  std::uint8_t tag{0};
  std::uint8_t flags{0};
};
static_assert(sizeof(Node) == kNodeBytes);

auto put_u32(std::byte* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

auto size_error(std::uint8_t tag) -> Error {
  if (tag == kTagString) {
    return {kProgramLimitExceeded, "string too long to represent as jsonb string",
            "Due to an implementation restriction, jsonb strings cannot exceed 268435455 bytes.", "", ""};
  }
  return {kProgramLimitExceeded,
          std::string{"total size of jsonb "} + (tag == kTagArray ? "array" : "object") +
              " elements exceeds the maximum of 268435455 bytes",
          "", "", ""};
}

auto data(Error e) -> Outcome { return {Status::Data, std::move(e), ""}; }
auto refused() -> Outcome { return {Status::Refused, std::nullopt, ""}; }
auto corrupt(const char* why) -> Outcome { return {Status::Corrupt, std::nullopt, why}; }

// De-escapes a lexed string's body (between its quotes) into out, which
// holds its de-escaped length. The lexer accepted it.
void de_escape(std::string_view body, std::byte* out) {
  std::size_t o = 0;
  std::uint32_t hi = 0;
  auto hex4 = [&](std::size_t i) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      auto c = body[i + k];
      v = v * 16 + static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
  };
  for (std::size_t i = 0; i < body.size();) {
    if (body[i] != '\\') {
      out[o++] = static_cast<std::byte>(body[i++]);
      continue;
    }
    auto e = body[i + 1];
    i += 2;
    switch (e) {
      case 'b': out[o++] = std::byte{'\b'}; break;
      case 'f': out[o++] = std::byte{'\f'}; break;
      case 'n': out[o++] = std::byte{'\n'}; break;
      case 'r': out[o++] = std::byte{'\r'}; break;
      case 't': out[o++] = std::byte{'\t'}; break;
      case 'u': {
        auto ch = hex4(i);
        i += 4;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
          hi = ch;
          break;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF) ch = 0x10000 + ((hi - 0xD800) << 10) + (ch - 0xDC00);
        o += put_utf8(out + o, ch);
        break;
      }
      default: out[o++] = static_cast<std::byte>(e); break;  // " \ /
    }
  }
}

class Parser {
public:
  Parser(std::string_view in, Memory mem) : in_{in}, nodes_{mem, Site::Nodes}, keys_{mem, Site::Keys}, pool_{mem, Site::Pool} { lx_.in = in; }

  auto run(Alloc out, std::span<std::byte>& value) -> Outcome {
    if (in_.size() > kMaxText) {
      return data({kProgramLimitExceeded, "total size of jsonb input exceeds the maximum of 1073741823 bytes", "", "", ""});
    }
    if (!utf8_valid(reinterpret_cast<const unsigned char*>(in_.data()), in_.size())) {
      return data({kCharacterNotInRepertoire, "invalid byte sequence for encoding \"UTF8\"", "", "", ""});
    }
    if (auto r = pass1(); !r.ok()) return r;
    if (auto r = pass2(); !r.ok()) return r;
    return pass3(out, value);
  }

private:
  auto key_bytes(const Node& k) const -> std::string_view {
    if ((k.flags & kEscaped) != 0) {
      return {reinterpret_cast<const char*>(pool_bytes_.data()) + k.aux, k.count};
    }
    return in_.substr(k.src + 1, k.count);
  }

  // Lexes the next token, or the fault as the statement's error.
  auto advance() -> std::optional<Outcome> {
    if (auto f = lx_.lex()) return data(lx_.error(*f));
    return std::nullopt;
  }

  auto add(Node n) -> std::optional<Outcome> {
    if (!nodes_.push(n)) return refused();
    return std::nullopt;
  }

  // Pass 1: the grammar, in document order, the open containers linked
  // through their `end` to their parents.
  auto pass1() -> Outcome {
    enum class State : std::uint8_t { Value, ArrayFirst, ObjectFirst, Field, After };
    auto& nodes = nodes_.vec();
    if (auto r = advance()) return *r;
    auto state = State::Value;
    auto open = kNoParent;  // the innermost open container
    std::size_t depth = 0;
    auto close = [&] {
      auto& c = nodes[open];
      auto parent = c.end;
      c.end = static_cast<std::uint32_t>(nodes.size());
      open = parent;
      --depth;
    };
    for (;;) {
      switch (state) {
        case State::Value: {
          auto tok = lx_.token;
          if (tok == Token::ObjectStart || tok == Token::ArrayStart) {
            if (depth == kMaxDepth) return data(depth_error());
            Node n;
            n.tag = tok == Token::ArrayStart ? kTagArray : kTagObject;
            n.end = open;
            if (open != kNoParent && nodes[open].tag == kTagArray) ++nodes[open].count;
            if (auto r = add(n)) return *r;
            open = static_cast<std::uint32_t>(nodes.size() - 1);
            ++depth;
            if (auto r = advance()) return *r;
            state = tok == Token::ArrayStart ? State::ArrayFirst : State::ObjectFirst;
            break;
          }
          if (tok != Token::String && tok != Token::Number && tok != Token::True && tok != Token::False &&
              tok != Token::Null) {
            return data(lx_.error(lx_.unexpected(Expect::Value)));
          }
          Node n;
          n.src = static_cast<std::uint32_t>(lx_.token_start);
          n.len = static_cast<std::uint32_t>(lx_.token_end - lx_.token_start);
          n.end = static_cast<std::uint32_t>(nodes.size() + 1);
          n.tag = tok == Token::String ? kTagString : tok == Token::Number ? kTagNumber
                  : tok == Token::True ? kTagTrue : tok == Token::False ? kTagFalse : kTagNull;
          auto str_len = lx_.str_len;
          if (tok == Token::String && lx_.escaped) n.flags |= kEscaped;
          if (open != kNoParent && nodes[open].tag == kTagArray) ++nodes[open].count;
          // PostgreSQL consumes the token, lexing the next, before it reads
          // the scalar's value: a bad next token is reported first.
          if (auto r = advance()) return *r;
          if (n.tag == kTagString) {
            if (str_len > kMaxBytes) return data(size_error(kTagString));
            n.count = static_cast<std::uint32_t>(str_len);
          } else if (n.tag == kTagNumber) {
            if (auto r = number(n)) return *r;
          }
          if (auto r = add(n)) return *r;
          state = State::After;
          break;
        }
        case State::ArrayFirst:
          if (lx_.token == Token::ArrayEnd) {
            close();
            if (auto r = advance()) return *r;
            state = State::After;
          } else {
            state = State::Value;
          }
          break;
        case State::ObjectFirst:
          if (lx_.token == Token::String) {
            state = State::Field;
          } else if (lx_.token == Token::ObjectEnd) {
            close();
            if (auto r = advance()) return *r;
            state = State::After;
          } else {
            return data(lx_.error(lx_.unexpected(Expect::ObjectFirst)));
          }
          break;
        case State::Field: {
          if (lx_.token != Token::String) return data(lx_.error(lx_.unexpected(Expect::String)));
          Node k;
          k.src = static_cast<std::uint32_t>(lx_.token_start);
          k.len = static_cast<std::uint32_t>(lx_.token_end - lx_.token_start);
          k.end = static_cast<std::uint32_t>(nodes.size() + 1);
          k.tag = kTagString;
          k.flags = kKey;
          auto str_len = lx_.str_len;
          if (lx_.escaped) k.flags |= kEscaped;
          if (auto r = advance()) return *r;
          if (lx_.token != Token::Colon) return data(lx_.error(lx_.unexpected(Expect::Label)));
          if (auto r = advance()) return *r;
          if (str_len > kMaxBytes) return data(size_error(kTagString));
          k.count = static_cast<std::uint32_t>(str_len);
          if ((k.flags & kEscaped) != 0) pool_size_ += str_len;
          ++nodes[open].count;
          ++pairs_;
          if (auto r = add(k)) return *r;
          state = State::Value;
          break;
        }
        case State::After:
          if (open == kNoParent) {
            if (lx_.token != Token::End) return data(lx_.error(lx_.unexpected(Expect::End)));
            return {};
          }
          if (nodes[open].tag == kTagArray) {
            if (lx_.token == Token::Comma) {
              if (auto r = advance()) return *r;
              state = State::Value;
            } else if (lx_.token == Token::ArrayEnd) {
              close();
              if (auto r = advance()) return *r;
            } else {
              return data(lx_.error(lx_.unexpected(Expect::ArrayNext)));
            }
          } else {
            if (lx_.token == Token::Comma) {
              if (auto r = advance()) return *r;
              state = State::Field;
            } else if (lx_.token == Token::ObjectEnd) {
              close();
              if (auto r = advance()) return *r;
            } else {
              return data(lx_.error(lx_.unexpected(Expect::ObjectNext)));
            }
          }
          break;
      }
    }
  }

  // A number token's parts, as numeric_in reads it: 22003 past its range.
  auto number(Node& n) -> std::optional<Outcome> {
    auto tok = in_.substr(n.src, n.len);
    auto negative = tok.front() == '-';
    if (negative) tok.remove_prefix(1);
    NumericParts p;
    auto st = numeric_parse(tok, {}, p);
    if (st != NumericStatus::Ok || p.end != tok.size() || !numeric_in_range(p)) {
      if (st == NumericStatus::Syntax) return data({kInvalidTextRepresentation, "invalid input syntax for type numeric", "", "", ""});
      return data({kNumericValueOutOfRange, "value overflows numeric format", "", "", ""});
    }
    n.dscale = static_cast<std::uint16_t>(p.scale);
    if (!p.first) return std::nullopt;  // zero: weight 0, no digits
    n.count = static_cast<std::uint32_t>(*p.last - *p.first + 1);
    n.aux = static_cast<std::uint32_t>(static_cast<std::int32_t>(p.whole - 1 - static_cast<std::int64_t>(*p.first)));
    if (negative) n.flags |= kNegative;
    return std::nullopt;
  }

  // Pass 2: each object's pairs in storage order (de-escaped length, then
  // bytes), the last of equal keys kept.
  auto pass2() -> Outcome {
    auto& nodes = nodes_.vec();
    if (!keys_.assign(pairs_) || !pool_.assign(pool_size_)) return refused();
    pool_bytes_ = std::span<std::byte>{pool_.vec()};
    std::size_t pool_at = 0;
    auto& index = keys_.vec();
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      auto& n = nodes[i];
      if (n.tag == kTagString && (n.flags & (kKey | kEscaped)) == (kKey | kEscaped)) {
        n.aux = static_cast<std::uint32_t>(pool_at);
        de_escape(in_.substr(n.src + 1, n.len - 2), pool_bytes_.data() + pool_at);
        pool_at += n.count;
      }
      if (n.tag != kTagObject) continue;
      n.src = static_cast<std::uint32_t>(at);
      auto j = i + 1;
      for (std::uint32_t c = 0; c < n.count; ++c) {
        index[at++] = j;
        j = nodes[j + 1].end;
      }
    }
    // Escaped keys are de-escaped above, before any object is sorted.
    for (auto& n : nodes) {
      if (n.tag != kTagObject) continue;
      auto first = index.begin() + n.src;
      auto last = first + n.count;
      std::sort(first, last, [&](std::uint32_t a, std::uint32_t b) {
        const auto& ka = nodes[a];
        const auto& kb = nodes[b];
        if (ka.count != kb.count) return ka.count < kb.count;
        auto c = key_bytes(ka).compare(key_bytes(kb));
        return c != 0 ? c < 0 : a < b;
      });
      // The last of each run of equal keys: the highest position.
      auto out = first;
      for (auto it = first; it != last; ++it) {
        auto next = it + 1;
        if (next != last && nodes[*next].count == nodes[*it].count && key_bytes(nodes[*next]) == key_bytes(nodes[*it])) {
          continue;
        }
        *out++ = *it;
      }
      n.len = static_cast<std::uint32_t>(out - first);
    }
    return {};
  }

  // Pass 3: sizes bottom-up, then the value written top-down.
  auto pass3(Alloc alloc, std::span<std::byte>& value) -> Outcome {
    auto& nodes = nodes_.vec();
    const auto& index = keys_.vec();
    for (auto i = nodes.size(); i-- > 0;) {
      auto& n = nodes[i];
      std::uint64_t size = 0;
      switch (n.tag) {
        case kTagNull:
        case kTagFalse:
        case kTagTrue: size = 1; break;
        case kTagNumber: size = 8 + std::uint64_t{n.count}; break;
        case kTagString: size = 1 + std::uint64_t{n.count}; break;
        case kTagArray: {
          size = 5 + 4 * std::uint64_t{n.count};
          auto j = static_cast<std::uint32_t>(i + 1);
          for (std::uint32_t c = 0; c < n.count; ++c) {
            size += nodes[j].size;
            j = nodes[j].end;
          }
          break;
        }
        case kTagObject:
          size = 5 + 8 * std::uint64_t{n.len};
          for (std::uint32_t c = 0; c < n.len; ++c) {
            auto k = index[n.src + c];
            size += std::uint64_t{nodes[k].count} + nodes[k + 1].size;
          }
          break;
        default: break;
      }
      if ((n.flags & kKey) != 0) size = 0;
      if (size > kMaxBytes) return data(size_error(n.tag));
      n.size = static_cast<std::uint32_t>(size);
    }
    auto total = std::uint64_t{nodes[0].size} + 1;
    if (total > kMaxBytes) return data(size_error(nodes[0].tag));
    auto* out = alloc.fn(alloc.self, static_cast<std::size_t>(total));
    if (out == nullptr) return refused();
    out[0] = std::byte{kFormat};
    nodes[0].size = 1;
    nodes[0].flags |= kPlaced;
    for (std::size_t i = 0; i < nodes.size();) {
      auto& n = nodes[i];
      if ((n.flags & kPlaced) == 0) {
        i = n.end;
        continue;
      }
      auto* p = out + n.size;
      p[0] = static_cast<std::byte>(n.tag);
      switch (n.tag) {
        case kTagNumber: write_number(n, p); break;
        case kTagString: de_escape(in_.substr(n.src + 1, n.len - 2), p + 1); break;
        case kTagArray: {
          put_u32(p + 1, n.count);
          auto area = n.size + 5 + 4 * n.count;
          std::uint32_t run = 0;
          auto j = static_cast<std::uint32_t>(i + 1);
          for (std::uint32_t c = 0; c < n.count; ++c) {
            auto& child = nodes[j];
            auto at = area + run;
            run += child.size;
            put_u32(p + 5 + 4 * c, run);
            child.size = at;
            child.flags |= kPlaced;
            j = child.end;
          }
          break;
        }
        case kTagObject: {
          put_u32(p + 1, n.len);
          auto keys_at = 5 + 8 * n.len;
          std::uint32_t kr = 0;
          for (std::uint32_t c = 0; c < n.len; ++c) {
            const auto& k = nodes[index[n.src + c]];
            auto kb = key_bytes(k);
            std::memcpy(p + keys_at + kr, kb.data(), kb.size());
            kr += k.count;
            put_u32(p + 5 + 4 * c, kr);
          }
          auto vals = n.size + keys_at + kr;
          std::uint32_t vr = 0;
          for (std::uint32_t c = 0; c < n.len; ++c) {
            auto& v = nodes[index[n.src + c] + 1];
            auto at = vals + vr;
            vr += v.size;
            put_u32(p + 5 + 4 * n.len + 4 * c, vr);
            v.size = at;
            v.flags |= kPlaced;
          }
          break;
        }
        default: break;
      }
      ++i;
    }
    value = {out, static_cast<std::size_t>(total)};
    return {};
  }

  void write_number(const Node& n, std::byte* p) {
    auto zero = n.count == 0;
    p[1] = std::byte{static_cast<std::uint8_t>(zero ? 0 : (n.flags & kNegative) != 0 ? 2 : 1)};
    p[2] = static_cast<std::byte>(n.dscale & 0xFF);
    p[3] = static_cast<std::byte>(n.dscale >> 8);
    put_u32(p + 4, zero ? 0 : n.aux);
    if (zero) return;
    auto tok = in_.substr(n.src, n.len);
    if (tok.front() == '-') tok.remove_prefix(1);
    NumericParts parts;
    (void)numeric_parse(tok, {}, parts);
    std::size_t d = 0;
    std::size_t o = 8;
    auto first = *parts.first;
    for (auto c : tok) {
      if (c == 'e' || c == 'E') break;
      if (c == '.') continue;
      if (d >= first && o < 8 + std::size_t{n.count}) p[o++] = static_cast<std::byte>(c);
      ++d;
    }
  }

  std::string_view in_;
  Lexer lx_;
  Scratch<Node> nodes_;
  Scratch<std::uint32_t> keys_;
  Scratch<std::byte> pool_;
  std::span<std::byte> pool_bytes_;
  std::uint64_t pairs_{0};
  std::uint64_t pool_size_{0};
};

// ----------------------------------------------------------------- walks

auto rd16(ByteView v, std::size_t at) -> std::uint32_t {
  return std::to_integer<std::uint32_t>(v[at]) | (std::to_integer<std::uint32_t>(v[at + 1]) << 8);
}
auto rd32(ByteView v, std::size_t at) -> std::uint32_t {
  std::uint32_t x = 0;
  for (std::size_t i = 0; i < 4; ++i) x |= std::to_integer<std::uint32_t>(v[at + i]) << (8 * i);
  return x;
}

// Counts the text the emitter writes.
struct CountSink {
  std::uint64_t n{0};
  bool put(char) {
    ++n;
    return true;
  }
  bool put(std::string_view s) {
    n += s.size();
    return true;
  }
  bool repeat(char, std::uint64_t k) {
    n += k;
    return true;
  }
  bool put_bytes(const std::byte*, std::size_t k) {
    n += k;
    return true;
  }
};

// Writes the text into its buffer: false past its end, never a write past it.
struct WriteSink {
  std::span<char> out;
  std::size_t at{0};
  bool put(char c) {
    if (at >= out.size()) return false;
    out[at++] = c;
    return true;
  }
  bool put(std::string_view s) {
    if (out.size() - at < s.size()) return false;
    std::memcpy(out.data() + at, s.data(), s.size());
    at += s.size();
    return true;
  }
  bool repeat(char c, std::uint64_t k) {
    if (out.size() - at < k) return false;
    std::memset(out.data() + at, c, static_cast<std::size_t>(k));
    at += static_cast<std::size_t>(k);
    return true;
  }
  bool put_bytes(const std::byte* p, std::size_t k) {
    if (out.size() - at < k) return false;
    std::memcpy(out.data() + at, p, k);
    at += k;
    return true;
  }
};

// One open container of a walk.
struct Frame {
  std::size_t table{0};  // its first end table
  std::size_t area{0};   // its elements, or its keys
  std::size_t vals{0};   // an object's values
  std::size_t e{0};      // its end
  std::uint32_t n{0};
  std::uint32_t i{0};
  std::uint32_t prev{0};      // the previous element's or value's end
  std::uint32_t key_prev{0};  // the previous key's end
  std::uint32_t keys_size{0};
  std::size_t last_key{0};    // the previous key's start, for the order check
  std::uint32_t last_key_len{0};
  bool object{false};
};

// A string's or key's bytes, escaped as escape_json writes them: valid
// UTF-8 with no NUL byte, or Corrupt.
template <typename Sink>
auto emit_string(ByteView v, std::size_t b, std::size_t e, Sink& out) -> Outcome {
  const auto* s = reinterpret_cast<const unsigned char*>(v.data()) + b;
  auto n = e - b;
  if (!out.put('"')) return corrupt("text past its length");
  std::size_t run = 0;  // bytes copied as they are, pending
  auto flush = [&](std::size_t i) { return out.put_bytes(v.data() + b + run, i - run); };
  for (std::size_t i = 0; i < n;) {
    auto len = utf8_sequence(s, n, i);
    if (len == 0) return corrupt("a string that is not UTF-8 or holds a NUL byte");
    auto c = s[i];
    const char* esc = nullptr;
    switch (c) {
      case '"': esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\b': esc = "\\b"; break;
      case '\f': esc = "\\f"; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
      default: break;
    }
    if (esc == nullptr && c >= 0x20) {
      i += len;
      continue;
    }
    if (!flush(i)) return corrupt("text past its length");
    if (esc != nullptr) {
      if (!out.put(std::string_view{esc})) return corrupt("text past its length");
    } else {
      static constexpr char kHex[] = "0123456789abcdef";
      char u[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0xF]};
      if (!out.put(std::string_view{u, 6})) return corrupt("text past its length");
    }
    i += len;
    run = i;
  }
  if (!flush(n) || !out.put('"')) return corrupt("text past its length");
  return {};
}

// numeric_out's text of a number node at [b, e), after its checks.
template <typename Sink>
auto emit_number(ByteView v, std::size_t b, std::size_t e, Sink& out) -> Outcome {
  if (e - b < 8) return corrupt("a number shorter than its header");
  auto sign = std::to_integer<std::uint32_t>(v[b + 1]);
  auto dscale = rd16(v, b + 2);
  auto weight = static_cast<std::int32_t>(rd32(v, b + 4));
  auto k = e - b - 8;
  if (sign > 2) return corrupt("a number of an unknown sign");
  if (std::int64_t{dscale} > kNumericMaxScale) return corrupt("a number's scale past 16383");
  auto fail = [] { return corrupt("text past its length"); };
  if (sign == 0) {
    if (weight != 0 || k != 0) return corrupt("a zero with a weight or digits");
    if (!out.put('0')) return fail();
    if (dscale > 0 && (!out.put('.') || !out.repeat('0', dscale))) return fail();
    return {};
  }
  if (weight < -kNumericMaxScale || weight > kNumericMaxWholeDigits - 1) return corrupt("a number's weight out of range");
  if (k == 0 || k > static_cast<std::size_t>(kNumericMaxWholeDigits + kNumericMaxScale)) {
    return corrupt("a number of no digits or too many");
  }
  const auto* d = v.data() + b + 8;
  for (std::size_t i = 0; i < k; ++i) {
    auto c = std::to_integer<unsigned>(d[i]);
    if (c < '0' || c > '9') return corrupt("a number digit that is not ASCII 0-9");
  }
  if (std::to_integer<unsigned>(d[0]) == '0' || std::to_integer<unsigned>(d[k - 1]) == '0') {
    return corrupt("a number with a leading or trailing zero digit");
  }
  auto w = std::int64_t{weight};
  auto kk = static_cast<std::int64_t>(k);
  if (w - kk + 1 < -std::int64_t{dscale}) return corrupt("a number whose digits pass its scale");
  if (sign == 2 && !out.put('-')) return fail();
  if (w >= 0) {
    auto lead = std::min(kk, w + 1);
    if (!out.put_bytes(d, static_cast<std::size_t>(lead)) || !out.repeat('0', static_cast<std::uint64_t>(w + 1 - lead))) {
      return fail();
    }
  } else if (!out.put('0')) {
    return fail();
  }
  if (dscale == 0) return {};
  // Place q after the point is digit w + q.
  auto ds = std::int64_t{dscale};
  auto zeros = std::clamp<std::int64_t>(-w - 1, 0, ds);
  auto lo = std::max<std::int64_t>(0, w + 1);
  auto hi = std::min<std::int64_t>(kk, w + ds + 1);
  auto mid = hi > lo ? hi - lo : 0;
  if (!out.put('.') || !out.repeat('0', static_cast<std::uint64_t>(zeros))) return fail();
  if (mid > 0 && !out.put_bytes(d + lo, static_cast<std::size_t>(mid))) return fail();
  if (!out.repeat('0', static_cast<std::uint64_t>(ds - zeros - mid))) return fail();
  return {};
}

// The walk every read of a whole node takes: the output text into `out`,
// every rule of *Decoding a stored jsonb value without overflow* and the
// full check's key order and UTF-8 checked on the way.
template <typename Sink>
auto walk(ByteView v, Memory mem, Sink& out) -> Outcome {
  Scratch<Frame> stack{mem, Site::Stack};
  auto fail = [] { return corrupt("text past its length"); };
  // Visits the node at [b, e): a scalar is written, a container opened.
  auto visit = [&](std::size_t b, std::size_t e) -> Outcome {
    if (e <= b) return corrupt("an empty node");
    auto tag = std::to_integer<std::uint8_t>(v[b]);
    switch (tag) {
      case kTagNull:
      case kTagFalse:
      case kTagTrue:
        if (e - b != 1) return corrupt("a null or boolean longer than its tag");
        if (!out.put(tag == kTagNull ? std::string_view{"null"} : tag == kTagTrue ? std::string_view{"true"} : std::string_view{"false"})) {
          return fail();
        }
        return {};
      case kTagNumber: return emit_number(v, b, e, out);
      case kTagString: return emit_string(v, b + 1, e, out);
      case kTagArray:
      case kTagObject: {
        if (e - b < 5) return corrupt("a container shorter than its header");
        auto n = rd32(v, b + 1);
        auto object = tag == kTagObject;
        auto room = (e - b - 5) / (object ? 9 : 5);
        if (n > room) return corrupt("a container count past its extent");
        if (n == 0 && e - b != 5) return corrupt("an empty container with trailing bytes");
        if (stack.vec().size() >= kMaxDepth) return corrupt("a value nested past 10000 containers");
        Frame f;
        f.table = b + 5;
        f.e = e;
        f.n = n;
        f.object = object;
        if (object) {
          f.area = b + 5 + 8 * std::size_t{n};
          f.keys_size = n == 0 ? 0 : rd32(v, b + 5 + 4 * (std::size_t{n} - 1));
          if (f.keys_size > e - f.area) return corrupt("an object's keys past its extent");
          f.vals = f.area + f.keys_size;
        } else {
          f.area = b + 5 + 4 * std::size_t{n};
        }
        if (!stack.push(f)) return refused();
        if (!out.put(object ? '{' : '[')) return fail();
        return {};
      }
      default: return corrupt("an unknown tag");
    }
  };
  if (auto r = visit(0, v.size()); !r.ok()) return r;
  auto& frames = stack.vec();
  while (!frames.empty()) {
    auto& f = frames.back();
    if (f.i == f.n) {
      if (!out.put(f.object ? '}' : ']')) return fail();
      frames.pop_back();
      continue;
    }
    if (f.i > 0 && !out.put(std::string_view{", "})) return fail();
    std::size_t cb = 0;
    std::size_t ce = 0;
    auto last = f.i + 1 == f.n;
    if (!f.object) {
      auto size = f.e - f.area;
      auto cur = rd32(v, f.table + 4 * std::size_t{f.i});
      if (cur <= f.prev || cur > size || (last && cur != size)) return corrupt("an element end out of order or past its area");
      cb = f.area + f.prev;
      ce = f.area + cur;
      f.prev = cur;
    } else {
      auto kcur = rd32(v, f.table + 4 * std::size_t{f.i});
      if (kcur < f.key_prev || kcur > f.keys_size) return corrupt("a key end out of order or past its area");
      auto kb = f.area + f.key_prev;
      auto klen = kcur - f.key_prev;
      if (f.i > 0) {
        auto shorter = f.last_key_len < klen;
        auto same = f.last_key_len == klen &&
                    std::memcmp(v.data() + f.last_key, v.data() + kb, klen) < 0;
        if (!shorter && !same) return corrupt("keys not strictly increasing in storage order");
      }
      f.last_key = kb;
      f.last_key_len = klen;
      f.key_prev = kcur;
      if (auto r = emit_string(v, kb, kb + klen, out); !r.ok()) return r;
      if (!out.put(std::string_view{": "})) return fail();
      auto size = f.e - f.vals;
      auto vcur = rd32(v, f.table + 4 * std::size_t{f.n} + 4 * std::size_t{f.i});
      if (vcur <= f.prev || vcur > size || (last && vcur != size)) return corrupt("a value end out of order or past its area");
      cb = f.vals + f.prev;
      ce = f.vals + vcur;
      f.prev = vcur;
    }
    ++f.i;
    // visit may push a frame: f is not used after it.
    if (auto r = visit(cb, ce); !r.ok()) return r;
  }
  return {};
}

}  // namespace

auto depth_error() -> Error {
  return {kStatementTooComplex, "stack depth limit exceeded", "", "", ""};
}

auto parse(std::string_view text, Memory mem, Alloc out, std::span<std::byte>& value) -> Outcome {
  Parser p{text, mem};
  return p.run(out, value);
}

auto node_of(ByteView value) -> std::optional<ByteView> {
  if (value.size() < 2 || std::to_integer<std::uint8_t>(value[0]) != kFormat) return std::nullopt;
  return value.subspan(1);
}

auto check_node(ByteView node, Memory mem) -> Outcome {
  CountSink sink;
  return walk(node, mem, sink);
}

auto check_value(ByteView value, Memory mem) -> Outcome {
  auto node = node_of(value);
  if (!node) return corrupt("not a jsonb value of a known format");
  return check_node(*node, mem);
}

auto text_length(ByteView node, Memory mem, std::uint64_t& length) -> Outcome {
  CountSink sink;
  auto r = walk(node, mem, sink);
  length = sink.n;
  return r;
}

auto write_text(ByteView node, Memory mem, std::span<char> out) -> Outcome {
  WriteSink sink{out};
  auto r = walk(node, mem, sink);
  if (r.ok() && sink.at != out.size()) return corrupt("text shorter than its length");
  return r;
}

auto to_text(ByteView node, Memory mem, Alloc out, std::span<char>& text) -> Outcome {
  std::uint64_t length = 0;
  if (auto r = text_length(node, mem, length); !r.ok()) return r;
  if (length > kMaxText) {
    return data({kProgramLimitExceeded, "the text of a jsonb value would exceed 1073741823 bytes", "", "", ""});
  }
  auto* p = out.fn(out.self, static_cast<std::size_t>(length));
  if (p == nullptr) return refused();
  text = {reinterpret_cast<char*>(p), static_cast<std::size_t>(length)};
  return write_text(node, mem, text);
}

namespace {
auto vector_alloc(void* self, std::size_t n) -> std::byte* {
  auto& v = *static_cast<std::vector<std::byte>*>(self);
  v.resize(n);
  return v.data();
}
}  // namespace

auto parse_value(std::string_view text, std::vector<std::byte>& out) -> Outcome {
  std::span<std::byte> value;
  return parse(text, {}, {&out, vector_alloc}, value);
}

auto value_text(ByteView value, std::string& out) -> Outcome {
  auto node = node_of(value);
  if (!node) return corrupt("not a jsonb value of a known format");
  std::vector<std::byte> buf;
  std::span<char> text;
  auto r = to_text(*node, {}, {&buf, vector_alloc}, text);
  if (r.ok()) out.assign(text.data(), text.size());
  return r;
}

auto canonical_text(std::string_view text, std::string& out) -> Outcome {
  std::vector<std::byte> value;
  if (auto r = parse_value(text, value); !r.ok()) return r;
  return value_text(value, out);
}

}  // namespace bytecask::sql::jsonb
