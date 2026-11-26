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
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
// An Alloc into a vector, uncharged: for constants folded by the binder.
[[nodiscard]] auto vector_out(std::vector<std::byte>& v) -> Alloc;
// A node's text, uncharged; Corrupt when it does not decode.
[[nodiscard]] auto node_text(ByteView node, std::string& out) -> Outcome;
// Text parsed into a stored value, uncharged.
[[nodiscard]] auto parse_value(std::string_view text, std::vector<std::byte>& out) -> Outcome;

// What a call returns for 54001 and the 54000 messages: one place for
// PostgreSQL's words.
[[nodiscard]] auto depth_error() -> Error;

// ------------------------------------------------- accessors (stage 3)
//
// Each reads only the parts of a node it needs, every one bounds-checked as
// the walk checks it, so a lookup is O(log n) in an object and O(1) in an
// array, and a damaged value is Corrupt, never a read outside it. A value
// found is a slice of its source, with no copy. A node is a jsonb value in
// its own right: a scalar is PostgreSQL's raw scalar, an array of itself to
// -> i.

// A node's JSON type, from its tag.
enum class Type : std::uint8_t { Null, Boolean, Number, String, Array, Object };

// The type of a node; Corrupt for an empty node or an unknown tag.
[[nodiscard]] auto type_of(ByteView node, Type& type) -> Outcome;
// jsonb_typeof's name of a type: object, array, string, number, boolean, null.
[[nodiscard]] auto typeof_name(Type type) -> std::string_view;

// j -> key: the value of `key` in an object node, by binary search over its
// sorted keys; nullopt for a node that is no object, or has no such key.
[[nodiscard]] auto field(ByteView node, std::string_view key, std::optional<ByteView>& value) -> Outcome;
// j -> i: element i of an array node, counted from its end when negative
// (-1 the last); a scalar is an array of itself. nullopt for an object, or
// an index outside the array.
[[nodiscard]] auto element(ByteView node, std::int64_t i, std::optional<ByteView>& value) -> Outcome;

// A path (docs/jsonb_design.md, *Path arguments*): a u32 count, then each
// element as a u32 length (kPathNull for a NULL element) and its bytes. The
// empty view is the empty path too.
inline constexpr std::uint32_t kPathNull = 0xFFFFFFFF;
// The count's top bit: a text[] of more than one dimension, which #>, #>>,
// ?| and ?& read flat and the functions that take the path as a list
// refuse (2202E). A text[] holds at most 134217727 elements, so the count
// never reaches it.
inline constexpr std::uint32_t kPathMulti = 0x80000000;
// array_in's reading of a text[]'s text, as a path: every dimension read
// flat, in order. Errors as array_in's: 22P02 "malformed array literal" with
// its DETAIL, 54000 past 6 dimensions, 2202E for an upper bound below its
// lower one, 22003 for a bound outside integer.
[[nodiscard]] auto parse_path(std::string_view text, Alloc out, std::span<std::byte>& path) -> Outcome;
// The number of elements of a path, or nullopt when its bytes are none:
// a count or a length past its extent, or bytes after its last element.
[[nodiscard]] auto path_size(ByteView path) -> std::optional<std::uint32_t>;
// The size of `path` with one more element (nullopt for NULL), and its
// writing into `out` of that size: `count` is path_size(path), and `out`
// may begin where `path` does (the binder grows its path in place).
[[nodiscard]] auto path_append_size(ByteView path, std::optional<ByteView> element) -> std::size_t;
void path_append(ByteView path, std::uint32_t count, std::optional<ByteView> element, std::span<std::byte> out);
// j #> path, as jsonb_get_element walks it: an object takes an element as a
// key, an array as an integer index (strtoint's text, negative from the
// end), and a scalar or a NULL element ends the walk with nullopt. The
// empty path is the node itself. `path` must be one path_size accepts.
[[nodiscard]] auto at_path(ByteView node, ByteView path, std::optional<ByteView>& value) -> Outcome;

// What ->> and #>> make of a node: SQL NULL for a JSON null, a string's own
// bytes (checked UTF-8 without a NUL byte), or the node's text otherwise.
enum class TextForm : std::uint8_t { Null, Bytes, Text };
[[nodiscard]] auto text_form(ByteView node, TextForm& form, ByteView& bytes) -> Outcome;

// jsonb_array_length: 22023 for a scalar or an object.
[[nodiscard]] auto array_length(ByteView node, std::int64_t& n) -> Outcome;

// The casts out of jsonb (docs/jsonb_design.md, *Casts to and from jsonb
// and json*), each 22023 "cannot cast jsonb <type> to type <target>" for a
// node of another type, `target` as PostgreSQL names the type.
[[nodiscard]] auto to_bool(ByteView node, bool& v) -> Outcome;
// A number rounded half away from zero, as numeric_int8 rounds it; nullopt
// past bigint, the caller's range error.
[[nodiscard]] auto to_integer(ByteView node, std::string_view target, std::optional<std::int64_t>& v) -> Outcome;
// A number's text (numeric_out's), into what `out` gives: numeric's value.
[[nodiscard]] auto to_numeric(ByteView node, std::string_view target, Memory mem, Alloc out, std::span<char>& text) -> Outcome;
// A number as float8in (or float4in, `real`) reads its text: 22003
// "\"<text>\" is out of range for type double precision" (real) past the
// type's range, or below it but zero. A real is its float's value.
[[nodiscard]] auto to_float(ByteView node, bool real, Memory mem, Alloc out, double& v) -> Outcome;

// ------------------- containment, existence, modification, construction
//                                                               (stage 4)
//
// Each walk is iterative over a stack charged to Memory, so no input
// reaches the C++ stack. A call that builds a value writes a new node into
// what `out` gives, charged: the children it does not change are copied as
// byte ranges, only the containers on its way are rebuilt, and the result's
// depth (54001 past kMaxDepth) and size (54000 past kMaxBytes) are checked
// before a byte is written. A result that is its input unchanged is the
// input itself, with nothing allocated. Every read is bounds-checked, a
// damaged value Corrupt.

// Whether a path came from a text[] of more than one dimension.
[[nodiscard]] auto path_multidimensional(ByteView path) -> bool;

// a @> b, as JsonbDeepContains: an object contains an object whose every
// pair it holds, a value contained by its own (a scalar equal, numbers by
// value; a container contained, of the same kind); an array contains an
// array each of whose scalars it holds and each of whose containers one of
// its own containers contains, in any order, duplicates allowed. A
// top-level scalar is an array of itself to an array, never containing one.
[[nodiscard]] auto contains(ByteView a, ByteView b, Memory mem, bool& result) -> Outcome;
// j ? key: a top-level key of an object, or a string element of an array
// (a top-level string scalar is an array of itself).
[[nodiscard]] auto exists(ByteView node, std::string_view key, bool& result) -> Outcome;
// j ?| path (any) and j ?& path (all): its non-NULL elements, as ?.
[[nodiscard]] auto exists_some(ByteView node, ByteView path, bool all, bool& result) -> Outcome;

// a || b, as jsonb_concat: two objects merged (b's key wins), two arrays
// joined, and anything else an array of both, a scalar as an array of
// itself and an object as one element.
[[nodiscard]] auto concat(ByteView a, ByteView b, Memory mem, Alloc out, ByteView& result) -> Outcome;
// j - key: an object without the key, an array without its string elements
// equal to it; 22023 "cannot delete from scalar".
[[nodiscard]] auto delete_key(ByteView node, std::string_view key, Memory mem, Alloc out, ByteView& result) -> Outcome;
// j - text[]: each non-NULL element deleted as by j - key; 2202E for a path
// of more than one dimension.
[[nodiscard]] auto delete_keys(ByteView node, ByteView path, Memory mem, Alloc out, ByteView& result) -> Outcome;
// j - i: element i of an array, from the end when negative; 22023 for a
// scalar or an object.
[[nodiscard]] auto delete_index(ByteView node, std::int64_t i, Memory mem, Alloc out, ByteView& result) -> Outcome;
// j #- path, as setPath deletes: 2202E, 22023 "cannot delete path in
// scalar", 22004 for a NULL element reached, 22P02 for an element read as
// an array index that is no integer.
[[nodiscard]] auto delete_path(ByteView node, ByteView path, Memory mem, Alloc out, ByteView& result) -> Outcome;
// jsonb_set(j, path, value, create): the value at path replaced, or with
// `create` added where the last step is missing; errors as delete_path's,
// 22023 "cannot set path in scalar".
[[nodiscard]] auto set(ByteView node, ByteView path, ByteView value, bool create, Memory mem, Alloc out, ByteView& result)
    -> Outcome;
// jsonb_insert(j, path, value, after): the value inserted before (after)
// the array element at path, or as a new key; 22023 "cannot replace
// existing key" for a key the object holds.
[[nodiscard]] auto insert(ByteView node, ByteView path, ByteView value, bool after, Memory mem, Alloc out, ByteView& result)
    -> Outcome;

// jsonb_set_lax's null_value_treatment, read as PostgreSQL compares it.
enum class NullTreatment : std::uint8_t { RaiseException, UseJsonNull, DeleteKey, ReturnTarget };
// 22023 for any other text, and for SQL NULL (nullopt).
[[nodiscard]] auto null_treatment(std::optional<std::string_view> text, NullTreatment& t) -> Outcome;
// jsonb_set_lax's error for a NULL value under raise_exception: 22004.
[[nodiscard]] auto null_value_error() -> Error;

// jsonb_strip_nulls: every object pair whose value is null dropped, at any
// depth; a scalar unchanged.
[[nodiscard]] auto strip_nulls(ByteView node, Memory mem, Alloc out, ByteView& result) -> Outcome;
// jsonb_pretty's text, into what `out` gives; 54000 past kMaxText.
[[nodiscard]] auto pretty(ByteView node, Memory mem, Alloc out, std::span<char>& text) -> Outcome;

// to_jsonb's scalar of a SQL value (datum_to_jsonb), from its text: a
// string; a number, or a string for text that is no JSON number ("NaN",
// "Infinity"); true; false. 54000 for a string past kMaxBytes.
enum class Scalar : std::uint8_t { String, Number, True, False };
[[nodiscard]] auto scalar(Scalar kind, std::string_view text, Memory mem, Alloc out, ByteView& node) -> Outcome;

// An argument of jsonb_build_array or jsonb_build_object: a node, SQL NULL,
// or (a key) of a type that is no scalar, jsonb.
struct BuildArg {
  ByteView node;
  bool null{false};
  bool composite{false};
};
// jsonb_build_array: each argument an element, SQL NULL a JSON null.
[[nodiscard]] auto build_array(std::span<const BuildArg> args, Memory mem, Alloc out, ByteView& result) -> Outcome;
// jsonb_build_object: alternating keys and values, the last of equal keys
// kept. A key is a string's bytes, a number's text, or true or false; 22023
// for an odd count, a NULL key ("argument N: key must not be null") or a
// composite one.
[[nodiscard]] auto build_object(std::span<const BuildArg> args, Memory mem, Alloc out, ByteView& result) -> Outcome;

// ------------------------------------------- comparison and order (stage 2)
//
// The jsonb sort key (*jsonb as a sort key and group key*): memcmp order on
// two keys is PostgreSQL 17's jsonb_ops order in a C database, byte equality
// its equality (1 = 1.0, key order irrelevant), and no key is a proper
// prefix of another, so it concatenates with other sort key parts and a
// DESC part inverts it. It is never decoded. `node` is the top-level value:
// an empty array there sorts below every other value.

// The most bytes a node's sort key takes: its length plus one.
[[nodiscard]] constexpr auto sort_key_bound(ByteView node) -> std::size_t { return node.size() + 1; }
// Writes the sort key into `out`, which sort_key_bound sized; `length` the
// bytes written. A write past `out` is Corrupt, never a write outside it.
[[nodiscard]] auto sort_key(ByteView node, Memory mem, std::span<std::byte> out, std::size_t& length) -> Outcome;
// The sort key into a vector, uncharged: for constants folded by the binder.
[[nodiscard]] auto sort_key_value(ByteView node, std::vector<std::byte>& out) -> Outcome;

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
  auto vec() const -> const std::vector<T>& { return v_; }

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

// A number node's parts, read by the rules of *Decoding a stored jsonb value
// without overflow*.
struct Number {
  std::uint32_t sign{0};  // 0 zero, 1 positive, 2 negative
  std::uint32_t dscale{0};
  std::int32_t weight{0};
  const std::byte* digits{nullptr};
  std::size_t k{0};
};

// The parts of the number node at [b, e), every rule checked.
auto read_number(ByteView v, std::size_t b, std::size_t e, Number& n) -> Outcome {
  if (e - b < 8) return corrupt("a number shorter than its header");
  n.sign = std::to_integer<std::uint32_t>(v[b + 1]);
  n.dscale = rd16(v, b + 2);
  n.weight = static_cast<std::int32_t>(rd32(v, b + 4));
  n.k = e - b - 8;
  n.digits = v.data() + b + 8;
  if (n.sign > 2) return corrupt("a number of an unknown sign");
  if (std::int64_t{n.dscale} > kNumericMaxScale) return corrupt("a number's scale past 16383");
  if (n.sign == 0) {
    if (n.weight != 0 || n.k != 0) return corrupt("a zero with a weight or digits");
    return {};
  }
  if (n.weight < -kNumericMaxScale || n.weight > kNumericMaxWholeDigits - 1) return corrupt("a number's weight out of range");
  if (n.k == 0 || n.k > static_cast<std::size_t>(kNumericMaxWholeDigits + kNumericMaxScale)) {
    return corrupt("a number of no digits or too many");
  }
  for (std::size_t i = 0; i < n.k; ++i) {
    auto c = std::to_integer<unsigned>(n.digits[i]);
    if (c < '0' || c > '9') return corrupt("a number digit that is not ASCII 0-9");
  }
  if (std::to_integer<unsigned>(n.digits[0]) == '0' || std::to_integer<unsigned>(n.digits[n.k - 1]) == '0') {
    return corrupt("a number with a leading or trailing zero digit");
  }
  if (std::int64_t{n.weight} - static_cast<std::int64_t>(n.k) + 1 < -std::int64_t{n.dscale}) {
    return corrupt("a number whose digits pass its scale");
  }
  return {};
}

// numeric_out's text of a number node at [b, e), after its checks.
template <typename Sink>
auto emit_number(ByteView v, std::size_t b, std::size_t e, Sink& out) -> Outcome {
  Number num;
  if (auto r = read_number(v, b, e, num); !r.ok()) return r;
  auto sign = num.sign;
  auto dscale = num.dscale;
  auto weight = num.weight;
  auto k = num.k;
  const auto* d = num.digits;
  auto fail = [] { return corrupt("text past its length"); };
  if (sign == 0) {
    if (!out.put('0')) return fail();
    if (dscale > 0 && (!out.put('.') || !out.repeat('0', dscale))) return fail();
    return {};
  }
  auto w = std::int64_t{weight};
  auto kk = static_cast<std::int64_t>(k);
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
// full check's key order and UTF-8 checked on the way. kPretty writes
// jsonb_pretty's text instead of jsonb_out's: each element and key on its
// own line, four spaces a level, "," between them. A sink with a depth()
// member is told the number of open containers each time one opens.
template <bool kPretty = false, typename Sink>
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
        if constexpr (requires { out.depth(std::size_t{}); }) out.depth(stack.vec().size());
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
      if constexpr (kPretty) {
        if (!out.put('\n') || !out.repeat(' ', 4 * std::uint64_t{frames.size() - 1})) return fail();
      }
      if (!out.put(f.object ? '}' : ']')) return fail();
      frames.pop_back();
      continue;
    }
    if constexpr (kPretty) {
      if ((f.i > 0 && !out.put(',')) || !out.put('\n') || !out.repeat(' ', 4 * std::uint64_t{frames.size()})) return fail();
    } else if (f.i > 0 && !out.put(std::string_view{", "})) {
      return fail();
    }
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

auto vector_out(std::vector<std::byte>& v) -> Alloc { return {&v, vector_alloc}; }

auto node_text(ByteView node, std::string& out) -> Outcome {
  std::vector<std::byte> buf;
  std::span<char> text;
  auto r = to_text(node, {}, {&buf, vector_alloc}, text);
  if (r.ok()) out.assign(text.data(), text.size());
  return r;
}

auto value_text(ByteView value, std::string& out) -> Outcome {
  auto node = node_of(value);
  if (!node) return corrupt("not a jsonb value of a known format");
  return node_text(*node, out);
}

auto canonical_text(std::string_view text, std::string& out) -> Outcome {
  std::vector<std::byte> value;
  if (auto r = parse_value(text, value); !r.ok()) return r;
  return value_text(value, out);
}

// ------------------------------------------------- accessors (stage 3)

namespace {

// An array or object node's header and tables, checked as the walk checks
// them, without reading a child.
struct Container {
  std::size_t table{0};  // its first end table
  std::size_t area{0};   // its elements, or its keys
  std::size_t vals{0};   // an object's values
  std::size_t e{0};      // its end
  std::uint32_t n{0};
  std::uint32_t keys_size{0};
  bool object{false};
};

auto open_container(ByteView v, Container& c) -> Outcome {
  auto e = v.size();
  if (e < 5) return corrupt("a container shorter than its header");
  auto tag = std::to_integer<std::uint8_t>(v[0]);
  c.object = tag == kTagObject;
  c.n = rd32(v, 1);
  c.e = e;
  c.table = 5;
  auto room = (e - 5) / (c.object ? 9 : 5);
  if (c.n > room) return corrupt("a container count past its extent");
  if (c.n == 0 && e != 5) return corrupt("an empty container with trailing bytes");
  if (c.object) {
    c.area = 5 + 8 * std::size_t{c.n};
    c.keys_size = c.n == 0 ? 0 : rd32(v, 5 + 4 * (std::size_t{c.n} - 1));
    if (c.keys_size > e - c.area) return corrupt("an object's keys past its extent");
    c.vals = c.area + c.keys_size;
  } else {
    c.area = 5 + 4 * std::size_t{c.n};
  }
  return {};
}

// Child i's extent among the children of `area` (an array's elements or an
// object's values), from the end table at `table`.
auto child(ByteView v, const Container& c, std::size_t table, std::size_t area, std::uint32_t i, ByteView& out) -> Outcome {
  auto size = c.e - area;
  auto prev = i == 0 ? std::uint32_t{0} : rd32(v, table + 4 * (std::size_t{i} - 1));
  auto cur = rd32(v, table + 4 * std::size_t{i});
  if (cur <= prev || cur > size || (i + 1 == c.n && cur != size)) return corrupt("an element end out of order or past its area");
  out = v.subspan(area + prev, cur - prev);
  return {};
}

// Key i's bytes.
auto key_at(ByteView v, const Container& c, std::uint32_t i, ByteView& out) -> Outcome {
  auto prev = i == 0 ? std::uint32_t{0} : rd32(v, c.table + 4 * (std::size_t{i} - 1));
  auto cur = rd32(v, c.table + 4 * std::size_t{i});
  if (cur < prev || cur > c.keys_size) return corrupt("a key end out of order or past its area");
  out = v.subspan(c.area + prev, cur - prev);
  return {};
}

auto tag_of(ByteView node) -> std::optional<std::uint8_t> {
  if (node.empty()) return std::nullopt;
  return std::to_integer<std::uint8_t>(node[0]);
}

// The value of key in an object node: binary search in storage order.
auto object_field(ByteView v, std::string_view key, std::optional<ByteView>& value) -> Outcome {
  Container c;
  if (auto r = open_container(v, c); !r.ok()) return r;
  std::uint32_t lo = 0;
  std::uint32_t hi = c.n;
  auto want = std::as_bytes(std::span{key.data(), key.size()});
  while (lo < hi) {
    auto mid = lo + (hi - lo) / 2;
    ByteView k;
    if (auto r = key_at(v, c, mid, k); !r.ok()) return r;
    int cmp = 0;
    if (k.size() != want.size()) {
      cmp = k.size() < want.size() ? -1 : 1;
    } else if (!k.empty()) {
      cmp = std::memcmp(k.data(), want.data(), k.size());
    }
    if (cmp == 0) {
      ByteView out;
      if (auto r = child(v, c, c.table + 4 * std::size_t{c.n}, c.vals, mid, out); !r.ok()) return r;
      value = out;
      return {};
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  value.reset();
  return {};
}

auto array_element(ByteView v, std::int64_t i, std::optional<ByteView>& value) -> Outcome {
  Container c;
  if (auto r = open_container(v, c); !r.ok()) return r;
  auto n = std::int64_t{c.n};
  if (i < 0) {
    if (i < -n) {
      value.reset();
      return {};
    }
    i += n;
  }
  if (i >= n) {
    value.reset();
    return {};
  }
  ByteView out;
  if (auto r = child(v, c, c.table, c.area, static_cast<std::uint32_t>(i), out); !r.ok()) return r;
  value = out;
  return {};
}

auto is_space(char c) -> bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// strtoint's reading of a path element as an array index: leading space, a
// sign, digits to the end, inside integer; nullopt otherwise.
auto path_index(std::string_view s) -> std::optional<std::int32_t> {
  std::size_t i = 0;
  while (i < s.size() && is_space(s[i])) ++i;
  auto negative = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
  if (i >= s.size()) return std::nullopt;
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    v = v * 10 + (s[i] - '0');
    if (v > std::int64_t{1} << 32) v = std::int64_t{1} << 32;  // past every integer, kept from overflowing
  }
  if (negative) v = -v;
  if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
  return static_cast<std::int32_t>(v);
}

auto cannot_cast(Type t, std::string_view target) -> Outcome {
  std::string_view name;
  switch (t) {
    case Type::Null: name = "null"; break;
    case Type::Boolean: name = "boolean"; break;
    case Type::Number: name = "numeric"; break;
    case Type::String: name = "string"; break;
    case Type::Array: name = "array"; break;
    case Type::Object: name = "object"; break;
  }
  return data({kInvalidParameterValue, "cannot cast jsonb " + std::string{name} + " to type " + std::string{target}, "", "", ""});
}

// --------------------------------------------------------------- array_in

constexpr int kMaxDim = 6;
constexpr std::int64_t kMaxArraySize = 134217727;

// PostgreSQL 17's array_in of a text[], each element handed to `put` as
// its source range and whether it is NULL, quoted (no trimming, every
// backslash quoting the next byte) or not. A NUL byte ends the text, as in C.
class ArrayReader {
public:
  explicit ArrayReader(std::string_view text) : s_{text} {}

  // Elements in order through put(begin, end, null); the error otherwise.
  template <typename Put>
  auto read(Put&& put) -> Outcome {
    std::array<std::int64_t, kMaxDim> dim{};
    dim.fill(-1);
    int ndim = 0;
    std::size_t p = 0;
    // Dimensions: [n] or [m:n] items, whitespace between them.
    for (;;) {
      while (is_space(at(p))) ++p;
      if (at(p) != '[') break;
      ++p;
      if (ndim >= kMaxDim) return too_many_dims();
      std::int64_t lb = 1;
      std::int64_t ub = 0;
      auto q = p;
      std::int64_t first = 0;
      if (auto r = dimension_int(p, first); !r.ok()) return r;
      if (p == q) return malformed("\"[\" must introduce explicitly-specified array dimensions.");
      if (at(p) == ':') {
        lb = first;
        ++p;
        q = p;
        if (auto r = dimension_int(p, ub); !r.ok()) return r;
        if (p == q) return malformed("Missing array dimension value.");
      } else {
        ub = first;
      }
      if (at(p) != ']') return malformed("Missing \"]\" after array dimensions.");
      ++p;
      if (ub < lb) return data({kArraySubscriptError, "upper bound cannot be less than lower bound", "", "", ""});
      if (ub == std::numeric_limits<std::int32_t>::max()) {
        return data({kProgramLimitExceeded, "array upper bound is too large: " + std::to_string(ub), "", "", ""});
      }
      auto size = ub - lb + 1;
      if (size > std::numeric_limits<std::int32_t>::max()) {
        return data({kProgramLimitExceeded, "array size exceeds the maximum allowed (" + std::to_string(kMaxArraySize) + ")", "", "", ""});
      }
      dim[static_cast<std::size_t>(ndim)] = size;
      ++ndim;
    }
    auto specified = ndim != 0;
    if (!specified) {
      if (at(p) != '{') return malformed("Array value must start with \"{\" or dimension information.");
    } else {
      if (at(p) != '=') return malformed("Missing \"=\" after array dimensions.");
      ++p;
      while (is_space(at(p))) ++p;
      if (at(p) != '{') return malformed("Array contents must start with \"{\".");
    }
    // ReadArrayStr's tokens, to the matching close brace.
    std::array<std::int64_t, kMaxDim> nelems{};
    int nest = 0;
    auto frozen = specified;
    auto expect_delim = false;
    auto dimension_error = [&] {
      return malformed(specified ? "Specified array dimensions do not match array contents."
                                 : "Multidimensional arrays must have sub-arrays with matching dimensions.");
    };
    do {
      Tok tok{};
      std::size_t b = 0;
      std::size_t e = 0;
      auto quoted = false;
      if (auto r = token(p, tok, b, e, quoted); !r.ok()) return r;
      switch (tok) {
        case Tok::LevelStart:
          if (expect_delim) return malformed("Unexpected \"{\" character.");
          if (nest >= kMaxDim) return too_many_dims();
          nelems[static_cast<std::size_t>(nest)] = 0;
          ++nest;
          if (nest > ndim) {
            if (frozen) return dimension_error();
            ndim = nest;
          }
          break;
        case Tok::LevelEnd: {
          if (nelems[static_cast<std::size_t>(nest - 1)] > 0 && !expect_delim) return malformed("Unexpected \"}\" character.");
          --nest;
          if (nest > 0) ++nelems[static_cast<std::size_t>(nest - 1)];
          auto& d = dim[static_cast<std::size_t>(nest)];
          if (d < 0) {
            d = nelems[static_cast<std::size_t>(nest)];
          } else if (nelems[static_cast<std::size_t>(nest)] != d) {
            return dimension_error();
          }
          expect_delim = true;
          break;
        }
        case Tok::Delim:
          if (!expect_delim) return malformed("Unexpected \",\" character.");
          expect_delim = false;
          break;
        case Tok::Elem:
        case Tok::ElemNull:
          if (expect_delim) return malformed("Unexpected array element.");
          if (auto r = put(b, e, tok == Tok::ElemNull, quoted); !r.ok()) return r;
          frozen = true;
          if (nest != ndim) return dimension_error();
          ++nelems[static_cast<std::size_t>(nest - 1)];
          expect_delim = true;
          break;
      }
    } while (nest > 0);
    while (p < s_.size() && s_[p] != '\0') {
      if (!is_space(s_[p++])) return malformed("Junk after closing right brace.");
    }
    ndim_ = ndim;
    return {};
  }

  // The dimensions the last read() found.
  [[nodiscard]] auto ndim() const -> int { return ndim_; }

private:
  enum class Tok : std::uint8_t { LevelStart, LevelEnd, Delim, Elem, ElemNull };

  [[nodiscard]] auto at(std::size_t i) const -> char { return i < s_.size() ? s_[i] : '\0'; }

  [[nodiscard]] auto malformed(const char* detail) const -> Outcome {
    return data({kInvalidTextRepresentation, "malformed array literal: \"" + std::string{s_.substr(0, std::min(s_.size(), s_.find('\0')))} + "\"",
                 detail, "", ""});
  }
  static auto too_many_dims() -> Outcome {
    return data({kProgramLimitExceeded, "number of array dimensions exceeds the maximum allowed (6)", "", "", ""});
  }

  // ReadDimensionInt: strtol's digits, only after a digit or a sign.
  auto dimension_int(std::size_t& p, std::int64_t& v) const -> Outcome {
    v = 0;
    auto c = at(p);
    if ((c < '0' || c > '9') && c != '-' && c != '+') return {};
    auto q = p;
    auto negative = false;
    if (c == '-' || c == '+') {
      negative = c == '-';
      ++q;
    }
    if (at(q) < '0' || at(q) > '9') return {};  // strtol reads nothing
    std::int64_t x = 0;
    auto big = false;
    while (at(q) >= '0' && at(q) <= '9') {
      x = x * 10 + (at(q) - '0');
      if (x > std::int64_t{1} << 40) {
        big = true;
        x = std::int64_t{1} << 40;
      }
      ++q;
    }
    if (negative) x = -x;
    if (big || x > std::numeric_limits<std::int32_t>::max() || x < std::numeric_limits<std::int32_t>::min()) {
      return data({kProgramLimitExceeded, "array bound is out of integer range", "", "", ""});
    }
    p = q;
    v = x;
    return {};
  }

  auto ending_error() const -> Outcome { return malformed("Unexpected end of input."); }

  // ReadArrayToken. An element is [b, e) of the source, still escaped.
  auto token(std::size_t& p, Tok& tok, std::size_t& b, std::size_t& e, bool& quoted) const -> Outcome {
    for (;;) {
      auto c = at(p);
      if (c == '\0') return ending_error();
      if (c == '{') {
        ++p;
        tok = Tok::LevelStart;
        return {};
      }
      if (c == '}') {
        ++p;
        tok = Tok::LevelEnd;
        return {};
      }
      if (c == ',') {
        ++p;
        tok = Tok::Delim;
        return {};
      }
      if (c == '"') {
        ++p;
        quoted = true;
        b = p;
        for (;;) {
          auto d = at(p);
          if (d == '\0') return ending_error();
          if (d == '\\') {
            ++p;
            if (at(p) == '\0') return ending_error();
            ++p;
            continue;
          }
          if (d == '"') {
            e = p;
            while (at(++p) != '\0') {
              auto x = at(p);
              if (x == ',' || x == '}' || x == '{') {
                tok = Tok::Elem;
                return {};
              }
              if (!is_space(x)) return malformed("Incorrectly quoted array element.");
            }
            return ending_error();
          }
          ++p;
        }
      }
      if (is_space(c)) {
        ++p;
        continue;
      }
      break;
    }
    // Unquoted: trailing whitespace dropped, an escaped byte never.
    b = p;
    e = p;
    auto escapes = false;
    for (;;) {
      auto c = at(p);
      if (c == '\0') return ending_error();
      if (c == '{') return malformed("Unexpected \"{\" character.");
      if (c == '"') return malformed("Incorrectly quoted array element.");
      if (c == '\\') {
        ++p;
        if (at(p) == '\0') return ending_error();
        ++p;
        e = p;
        escapes = true;
        continue;
      }
      if (c == ',' || c == '}') {
        auto t = s_.substr(b, e - b);
        auto null = !escapes && t.size() == 4 && (t[0] | 0x20) == 'n' && (t[1] | 0x20) == 'u' && (t[2] | 0x20) == 'l' &&
                    (t[3] | 0x20) == 'l';
        tok = null ? Tok::ElemNull : Tok::Elem;
        return {};
      }
      ++p;
      if (!is_space(c)) e = p;
    }
  }

  std::string_view s_;
  int ndim_{0};
};

// An element's bytes, its backslashes taken off: each quotes the next byte.
template <typename Put>
void de_backslash(std::string_view src, Put&& put) {
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '\\' && i + 1 < src.size()) ++i;
    put(src[i]);
  }
}

// One element of a path read in turn.
struct PathCursor {
  ByteView path;
  std::size_t at{4};
  // The next element: its bytes, or nullopt for NULL.
  auto next() -> std::optional<std::string_view> {
    auto len = rd32(path, at);
    at += 4;
    if (len == kPathNull) return std::nullopt;
    std::string_view out{reinterpret_cast<const char*>(path.data()) + at, len};
    at += len;
    return out;
  }
};

}  // namespace

auto type_of(ByteView node, Type& type) -> Outcome {
  auto tag = tag_of(node);
  if (!tag) return corrupt("an empty node");
  switch (*tag) {
    case kTagNull: type = Type::Null; return {};
    case kTagFalse:
    case kTagTrue: type = Type::Boolean; return {};
    case kTagNumber: type = Type::Number; return {};
    case kTagString: type = Type::String; return {};
    case kTagArray: type = Type::Array; return {};
    case kTagObject: type = Type::Object; return {};
    default: return corrupt("an unknown tag");
  }
}

auto typeof_name(Type type) -> std::string_view {
  switch (type) {
    case Type::Null: return "null";
    case Type::Boolean: return "boolean";
    case Type::Number: return "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
  }
  return "null";
}

auto field(ByteView node, std::string_view key, std::optional<ByteView>& value) -> Outcome {
  value.reset();
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t != Type::Object) return {};
  return object_field(node, key, value);
}

auto element(ByteView node, std::int64_t i, std::optional<ByteView>& value) -> Outcome {
  value.reset();
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  switch (t) {
    case Type::Object: return {};
    case Type::Array: return array_element(node, i, value);
    case Type::Null:
    case Type::Boolean:
    case Type::Number:
    case Type::String:
      if (i == 0 || i == -1) value = node;  // a raw scalar: an array of itself
      return {};
  }
  return {};
}

auto parse_path(std::string_view text, Alloc out, std::span<std::byte>& path) -> Outcome {
  ArrayReader reader{text};
  // Pass 1 sizes the path, pass 2 writes it.
  std::uint64_t size = 4;
  std::uint64_t count = 0;
  auto r = reader.read([&](std::size_t b, std::size_t e, bool null, bool) -> Outcome {
    ++count;
    size += 4;
    if (!null) {
      std::uint64_t n = 0;
      de_backslash(text.substr(b, e - b), [&](char) { ++n; });
      size += n;
    }
    return {};
  });
  if (!r.ok()) return r;
  if (size > kMaxText) return data({kProgramLimitExceeded, "a path of more than 1073741823 bytes", "", "", ""});
  auto* p = out.fn(out.self, static_cast<std::size_t>(size));
  if (p == nullptr) return refused();
  // array_in makes an array of no elements one of no dimensions.
  auto multi = reader.ndim() > 1 && count > 0;
  put_u32(p, static_cast<std::uint32_t>(count) | (multi ? kPathMulti : 0));
  std::size_t at = 4;
  (void)reader.read([&](std::size_t b, std::size_t e, bool null, bool) -> Outcome {
    if (null) {
      put_u32(p + at, kPathNull);
      at += 4;
      return {};
    }
    auto len_at = at;
    at += 4;
    std::uint32_t n = 0;
    de_backslash(text.substr(b, e - b), [&](char c) {
      p[at++] = static_cast<std::byte>(c);
      ++n;
    });
    put_u32(p + len_at, n);
    return {};
  });
  path = {p, static_cast<std::size_t>(size)};
  return {};
}

auto path_size(ByteView path) -> std::optional<std::uint32_t> {
  if (path.empty()) return 0;
  if (path.size() < 4) return std::nullopt;
  auto n = rd32(path, 0) & ~kPathMulti;
  std::size_t at = 4;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (path.size() - at < 4) return std::nullopt;
    auto len = rd32(path, at);
    at += 4;
    if (len == kPathNull) continue;
    if (path.size() - at < len) return std::nullopt;
    at += len;
  }
  if (at != path.size()) return std::nullopt;
  return n;
}

auto path_append_size(ByteView path, std::optional<ByteView> element) -> std::size_t {
  auto body = path.empty() ? std::size_t{0} : path.size() - 4;
  return 4 + body + 4 + (element ? element->size() : 0);
}

void path_append(ByteView path, std::uint32_t count, std::optional<ByteView> element, std::span<std::byte> out) {
  auto body = path.empty() ? ByteView{} : path.subspan(4);
  // memmove: `out` may overlap `path`.
  if (!body.empty()) std::memmove(out.data() + 4, body.data(), body.size());
  put_u32(out.data(), count + 1);
  auto at = 4 + body.size();
  put_u32(out.data() + at, element ? static_cast<std::uint32_t>(element->size()) : kPathNull);
  if (element && !element->empty()) std::memcpy(out.data() + at + 4, element->data(), element->size());
}

auto at_path(ByteView node, ByteView path, std::optional<ByteView>& value) -> Outcome {
  value.reset();
  auto n = path_size(path);
  if (!n) return corrupt("a path that does not decode");
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (*n == 0) {
    value = node;
    return {};
  }
  PathCursor cur{path};
  auto here = node;
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto key = cur.next();
    if (!key) return {};
    std::optional<ByteView> found;
    if (t == Type::Object) {
      if (auto r = object_field(here, *key, found); !r.ok()) return r;
    } else if (t == Type::Array) {
      auto index = path_index(*key);
      if (!index) return {};
      if (auto r = array_element(here, *index, found); !r.ok()) return r;
    } else {
      return {};  // a scalar: no element to extract
    }
    if (!found) return {};
    here = *found;
    if (auto r = type_of(here, t); !r.ok()) return r;
  }
  value = here;
  return {};
}

auto text_form(ByteView node, TextForm& form, ByteView& bytes) -> Outcome {
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  switch (t) {
    case Type::Null: form = TextForm::Null; return {};
    case Type::String: {
      auto body = node.subspan(1);
      if (!utf8_valid(reinterpret_cast<const unsigned char*>(body.data()), body.size())) {
        return corrupt("a string that is not UTF-8 or holds a NUL byte");
      }
      form = TextForm::Bytes;
      bytes = body;
      return {};
    }
    case Type::Boolean:
    case Type::Number:
    case Type::Array:
    case Type::Object: form = TextForm::Text; return {};
  }
  return {};
}

auto array_length(ByteView node, std::int64_t& n) -> Outcome {
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t == Type::Object) return data({kInvalidParameterValue, "cannot get array length of a non-array", "", "", ""});
  if (t != Type::Array) return data({kInvalidParameterValue, "cannot get array length of a scalar", "", "", ""});
  Container c;
  if (auto r = open_container(node, c); !r.ok()) return r;
  n = c.n;
  return {};
}

auto to_bool(ByteView node, bool& v) -> Outcome {
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t != Type::Boolean) return cannot_cast(t, "boolean");
  if (node.size() != 1) return corrupt("a null or boolean longer than its tag");
  v = std::to_integer<std::uint8_t>(node[0]) == kTagTrue;
  return {};
}

auto to_integer(ByteView node, std::string_view target, std::optional<std::int64_t>& v) -> Outcome {
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t != Type::Number) return cannot_cast(t, target);
  Number num;
  if (auto r = read_number(node, 0, node.size(), num); !r.ok()) return r;
  v = 0;
  if (num.sign == 0 || num.weight < -1) return {};
  if (num.weight >= 19) {
    v.reset();
    return {};
  }
  auto digit = [&](std::int64_t i) -> unsigned {
    return i < static_cast<std::int64_t>(num.k) ? std::to_integer<unsigned>(num.digits[i]) - '0' : 0;
  };
  unsigned __int128 mag = 0;
  for (std::int64_t i = 0; i <= num.weight; ++i) mag = mag * 10 + digit(i);
  if (digit(std::int64_t{num.weight} + 1) >= 5) mag += 1;
  constexpr auto kTop = static_cast<unsigned __int128>(std::numeric_limits<std::int64_t>::max());
  if (num.sign == 2) {
    if (mag > kTop + 1) {
      v.reset();
    } else if (mag == kTop + 1) {
      v = std::numeric_limits<std::int64_t>::min();
    } else {
      v = -static_cast<std::int64_t>(mag);
    }
  } else if (mag > kTop) {
    v.reset();
  } else {
    v = static_cast<std::int64_t>(mag);
  }
  return {};
}

auto to_numeric(ByteView node, std::string_view target, Memory mem, Alloc out, std::span<char>& text) -> Outcome {
  Type t{};
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t != Type::Number) return cannot_cast(t, target);
  return to_text(node, mem, out, text);
}

auto to_float(ByteView node, bool real, Memory mem, Alloc out, double& v) -> Outcome {
  std::span<char> text;
  if (auto r = to_numeric(node, real ? "real" : "double precision", mem, out, text); !r.ok()) return r;
  const auto* b = text.data();
  const auto* e = text.data() + text.size();
  auto range = [&] {
    return data({kNumericValueOutOfRange,
                 "\"" + std::string{text.data(), text.size()} + "\" is out of range for type " + (real ? "real" : "double precision"),
                 "", "", ""});
  };
  if (real) {
    float f = 0;
    auto [end, ec] = std::from_chars(b, e, f);
    if (ec == std::errc::result_out_of_range) return range();
    if (ec != std::errc{} || end != e) return corrupt("a number whose text is no float");
    v = static_cast<double>(f);
    return {};
  }
  double d = 0;
  auto [end, ec] = std::from_chars(b, e, d);
  if (ec == std::errc::result_out_of_range) return range();
  if (ec != std::errc{} || end != e) return corrupt("a number whose text is no float");
  v = d;
  return {};
}

// ------------------- containment, existence, modification, construction
//                                                               (stage 4)

namespace {

// A walk's deepest nesting of containers.
struct DepthSink : CountSink {
  std::size_t max{0};
  void depth(std::size_t d) { max = std::max(max, d); }
};

// The nesting of a node's containers (0 for a scalar), after the full
// check its walk makes.
auto node_depth(ByteView node, Memory mem, std::size_t& depth) -> Outcome {
  DepthSink sink;
  auto r = walk(node, mem, sink);
  depth = sink.max;
  return r;
}

auto is_container_tag(std::uint8_t tag) -> bool { return tag == kTagArray || tag == kTagObject; }

// The index of `want` among an object's keys, by binary search in storage
// order; nullopt for no such key.
auto find_key(ByteView v, const Container& c, ByteView want, std::optional<std::uint32_t>& index) -> Outcome {
  index.reset();
  std::uint32_t lo = 0;
  std::uint32_t hi = c.n;
  while (lo < hi) {
    auto mid = lo + (hi - lo) / 2;
    ByteView k;
    if (auto r = key_at(v, c, mid, k); !r.ok()) return r;
    int cmp = 0;
    if (k.size() != want.size()) {
      cmp = k.size() < want.size() ? -1 : 1;
    } else if (!k.empty()) {
      cmp = std::memcmp(k.data(), want.data(), k.size());
    }
    if (cmp == 0) {
      index = mid;
      return {};
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return {};
}

auto bytes_of(std::string_view s) -> ByteView { return std::as_bytes(std::span{s.data(), s.size()}); }

// Child i of a container: an object's key (when `key` is given) and value,
// or an array's element.
auto child_at(ByteView v, const Container& c, std::uint32_t i, ByteView* key, ByteView& value) -> Outcome {
  if (!c.object) return child(v, c, c.table, c.area, i, value);
  if (key != nullptr) {
    if (auto r = key_at(v, c, i, *key); !r.ok()) return r;
  }
  return child(v, c, c.table + 4 * std::size_t{c.n}, c.vals, i, value);
}

// Two scalars equal as JSON values: the same type, strings bytewise,
// numbers by value (their digits and weight, not their scale).
auto scalar_equal(ByteView a, ByteView b, bool& eq) -> Outcome {
  eq = false;
  auto ta = tag_of(a);
  auto tb = tag_of(b);
  if (!ta || !tb) return corrupt("an empty node");
  if (*ta != *tb) return {};
  switch (*ta) {
    case kTagNull:
    case kTagFalse:
    case kTagTrue:
      if (a.size() != 1 || b.size() != 1) return corrupt("a null or boolean longer than its tag");
      eq = true;
      return {};
    case kTagString: eq = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0; return {};
    case kTagNumber: {
      Number x;
      Number y;
      if (auto r = read_number(a, 0, a.size(), x); !r.ok()) return r;
      if (auto r = read_number(b, 0, b.size(), y); !r.ok()) return r;
      eq = x.sign == y.sign && x.weight == y.weight && x.k == y.k && (x.k == 0 || std::memcmp(x.digits, y.digits, x.k) == 0);
      return {};
    }
    default: return corrupt("an unknown tag");
  }
}

constexpr std::array<std::byte, 1> kNullNode{std::byte{kTagNull}};

// Builds a value from tokens in document order, as PostgreSQL's
// pushJsonbValue does: containers opened and closed, a key before each
// object member, leaves that are whole nodes copied as byte ranges. An
// object's members are put in storage order at finish(), the last of equal
// keys kept. Every buffer is charged; the result is written once, sized
// first, its depth and size checked before a byte is written.
class Builder {
public:
  explicit Builder(Memory mem)
      : mem_{mem}, items_{mem, Site::Nodes}, open_{mem, Site::Stack}, order_{mem, Site::Keys}, pool_{mem, Site::Pool} {}

  // Opens an array or an object as the next value.
  auto open(bool object) -> Outcome {
    if (open_.vec().size() >= kMaxDepth) return data(depth_error());
    Item it;
    it.kind = object ? kTagObject : kTagArray;
    auto at = static_cast<std::uint32_t>(items_.vec().size());
    if (auto r = place(it); !r.ok()) return r;
    if (!open_.push(at)) return refused();
    return {};
  }

  void close() {
    auto& items = items_.vec();
    items[open_.vec().back()].end = static_cast<std::uint32_t>(items.size());
    open_.vec().pop_back();
  }

  // The next value's key, in an object: bytes that outlive finish().
  void key(ByteView k) {
    pending_ = {k.data(), 0, static_cast<std::uint32_t>(k.size())};
    has_key_ = true;
  }
  // The text of a number node, copied, as the next key.
  auto key_number(ByteView node) -> Outcome {
    CountSink count;
    if (auto r = walk(node, mem_, count); !r.ok()) return r;
    auto off = pool_.vec().size();
    if (!pool_.reserve(off + count.n)) return refused();
    pool_.vec().resize(off + count.n);
    WriteSink sink{{reinterpret_cast<char*>(pool_.vec().data() + off), static_cast<std::size_t>(count.n)}};
    if (auto r = walk(node, mem_, sink); !r.ok()) return r;
    pending_ = {nullptr, off, static_cast<std::uint32_t>(count.n)};
    has_key_ = true;
    return {};
  }

  // A whole node as the next value, its bytes outliving finish(). With
  // `measure`, the node's own nesting counts toward the result's depth (a
  // value nested anew); without, it was already at this depth.
  auto leaf(ByteView node, bool measure) -> Outcome {
    if (node.empty()) return corrupt("an empty node");
    if (measure && is_container_tag(std::to_integer<std::uint8_t>(node[0]))) {
      std::size_t d = 0;
      if (auto r = node_depth(node, mem_, d); !r.ok()) return r;
      if (d + open_.vec().size() > kMaxDepth) return data(depth_error());
    }
    Item it;
    it.data = node.data();
    it.len = static_cast<std::uint32_t>(node.size());
    return place(it);
  }
  // The value built, into what `out` gives.
  auto finish(Alloc out, ByteView& result) -> Outcome {
    auto& items = items_.vec();
    if (items.empty() || !open_.vec().empty()) return corrupt("an unbalanced jsonb build");
    if (auto r = order_members(); !r.ok()) return r;
    auto& order = order_.vec();
    // Sizes, children (later in document order) before their parents.
    for (auto i = items.size(); i-- > 0;) {
      auto& it = items[i];
      if (it.kind == 0) {
        it.size = it.len;
        continue;
      }
      std::size_t s = 5;
      if (it.kind == kTagArray) {
        it.kept = 0;
        for (auto j = static_cast<std::uint32_t>(i) + 1; j < it.end; j = items[j].end) {
          s += 4 + items[j].size;
          ++it.kept;
        }
      } else {
        for (std::uint32_t k = 0; k < it.kept; ++k) {
          const auto& c = items[order[it.first + k]];
          s += 8 + std::size_t{c.key_len} + c.size;
        }
      }
      if (s > kMaxBytes) return data(size_error(it.kind));
      it.size = s;
    }
    auto total = items[0].size;
    auto* p = out.fn(out.self, total);
    if (p == nullptr) return refused();
    items[0].pos = 0;
    for (std::uint32_t i = 0; i < items.size();) {
      auto& it = items[i];
      if (it.dropped) {
        i = it.end;
        continue;
      }
      auto* w = p + it.pos;
      if (it.kind == 0) {
        auto bytes = leaf_bytes(it);
        std::memcpy(w, bytes.data(), bytes.size());
        ++i;
        continue;
      }
      w[0] = std::byte{it.kind};
      put_u32(w + 1, it.kept);
      std::size_t prev = 0;
      if (it.kind == kTagArray) {
        auto area = it.pos + 5 + 4 * std::size_t{it.kept};
        std::uint32_t k = 0;
        for (auto j = i + 1; j < it.end; j = items[j].end) {
          items[j].pos = area + prev;
          prev += items[j].size;
          put_u32(w + 5 + 4 * std::size_t{k++}, static_cast<std::uint32_t>(prev));
        }
      } else {
        auto keys = it.pos + 5 + 8 * std::size_t{it.kept};
        std::size_t koff = 0;
        for (std::uint32_t k = 0; k < it.kept; ++k) {
          const auto& c = items[order[it.first + k]];
          auto kb = key_bytes(c);
          if (!kb.empty()) std::memcpy(p + keys + koff, kb.data(), kb.size());
          koff += kb.size();
          put_u32(w + 5 + 4 * std::size_t{k}, static_cast<std::uint32_t>(koff));
        }
        auto vals = keys + koff;
        for (std::uint32_t k = 0; k < it.kept; ++k) {
          auto& c = items[order[it.first + k]];
          c.pos = vals + prev;
          prev += c.size;
          put_u32(w + 5 + 4 * std::size_t{it.kept} + 4 * std::size_t{k}, static_cast<std::uint32_t>(prev));
        }
      }
      ++i;
    }
    result = {p, total};
    return {};
  }

private:
  struct Item {
    std::size_t size{0};             // its encoded size
    std::size_t pos{0};              // its position in the result, given by its parent
    const std::byte* data{nullptr};  // a leaf's bytes
    const std::byte* key{nullptr};   // its key in an object; nullptr: at `key_off` in the pool
    std::uint64_t key_off{0};
    std::uint32_t len{0};      // a leaf's bytes
    std::uint32_t key_len{0};
    std::uint32_t end{0};      // the index past its subtree
    std::uint32_t first{0};    // an object: its kept members' start in order_
    std::uint32_t kept{0};     // a container: its children kept
    std::uint8_t kind{0};      // 0 a leaf, kTagArray or kTagObject
    bool dropped{false};       // the value of a key a later member repeats
  };
  struct Key {
    const std::byte* data{nullptr};
    std::uint64_t off{0};
    std::uint32_t len{0};
  };

  auto place(Item it) -> Outcome {
    auto& items = items_.vec();
    if (items.size() >= std::numeric_limits<std::uint32_t>::max() - 1) return data(size_error(kTagArray));
    auto in_object = !open_.vec().empty() && items[open_.vec().back()].kind == kTagObject;
    if (in_object != has_key_) return corrupt("a jsonb build with a key out of place");
    if (has_key_) {
      it.key = pending_.data;
      it.key_off = pending_.off;
      it.key_len = pending_.len;
      has_key_ = false;
    }
    it.end = static_cast<std::uint32_t>(items.size()) + 1;
    if (!items_.push(it)) return refused();
    return {};
  }

  static auto leaf_bytes(const Item& it) -> ByteView { return {it.data, it.len}; }
  auto key_bytes(const Item& it) const -> ByteView {
    if (it.key != nullptr || it.key_len == 0) return {it.key, it.key_len};
    return {pool_.vec().data() + it.key_off, it.key_len};
  }

  // Each object's members in storage order (length, then bytes), the last
  // of equal keys kept and the others dropped.
  auto order_members() -> Outcome {
    auto& items = items_.vec();
    for (std::uint32_t i = 0; i < items.size(); ++i) {
      if (items[i].kind != kTagObject) continue;
      auto first = order_.vec().size();
      for (auto j = i + 1; j < items[i].end; j = items[j].end) {
        if (!order_.push(j)) return refused();
      }
      auto& order = order_.vec();
      auto b = order.begin() + static_cast<std::ptrdiff_t>(first);
      auto e = order.end();
      auto less = [&](std::uint32_t x, std::uint32_t y) {
        auto kx = key_bytes(items[x]);
        auto ky = key_bytes(items[y]);
        if (kx.size() != ky.size()) return kx.size() < ky.size();
        auto c = kx.empty() ? 0 : std::memcmp(kx.data(), ky.data(), kx.size());
        return c != 0 ? c < 0 : x < y;
      };
      std::sort(b, e, less);
      auto same = [&](std::uint32_t x, std::uint32_t y) {
        auto kx = key_bytes(items[x]);
        auto ky = key_bytes(items[y]);
        return kx.size() == ky.size() && (kx.empty() || std::memcmp(kx.data(), ky.data(), kx.size()) == 0);
      };
      auto out = b;
      for (auto it = b; it != e; ++it) {
        auto next = it + 1;
        if (next != e && same(*next, *it)) {
          items[*it].dropped = true;
          continue;
        }
        *out++ = *it;
      }
      items[i].first = static_cast<std::uint32_t>(first);
      items[i].kept = static_cast<std::uint32_t>(out - b);
      order.resize(first + items[i].kept);
    }
    return {};
  }

  Memory mem_;
  Scratch<Item> items_;
  Scratch<std::uint32_t> open_;
  Scratch<std::uint32_t> order_;
  Scratch<std::byte> pool_;
  Key pending_;
  bool has_key_{false};
};

// Children [from, to) of a container into the builder, `skip` left out.
auto copy_children(Builder& b, ByteView v, const Container& c, std::uint32_t from, std::uint32_t to,
                   std::uint32_t skip = std::numeric_limits<std::uint32_t>::max()) -> Outcome {
  for (auto i = from; i < to; ++i) {
    if (i == skip) continue;
    ByteView k;
    ByteView val;
    if (auto r = child_at(v, c, i, &k, val); !r.ok()) return r;
    if (c.object) b.key(k);
    if (auto r = b.leaf(val, false); !r.ok()) return r;
  }
  return {};
}

auto invalid(std::string message, std::string hint = {}) -> Outcome {
  return data({kInvalidParameterValue, std::move(message), "", std::move(hint), ""});
}
auto wrong_subscripts() -> Outcome { return data({kArraySubscriptError, "wrong number of array subscripts", "", "", ""}); }

// An open container's header, for a node known to be one.
auto container_of(ByteView node, Type& t, Container& c) -> Outcome {
  if (auto r = type_of(node, t); !r.ok()) return r;
  if (t != Type::Array && t != Type::Object) return {};
  return open_container(node, c);
}

// One open pair of containers of a containment walk; a raw scalar is an
// array of itself.
struct Contain {
  ByteView l;
  ByteView r;
  Container lc;
  Container rc;
  std::uint32_t ln{0};
  std::uint32_t rn{0};
  std::uint32_t i{0};  // the right side's member or element being matched
  std::uint32_t j{0};  // the left side's candidate container for it
  bool object{false};
  bool lraw{false};
  bool rraw{false};
};

auto contain_elem(ByteView v, const Container& c, bool raw, std::uint32_t i, ByteView& out) -> Outcome {
  if (raw) {
    out = v;
    return {};
  }
  return child(v, c, c.table, c.area, i, out);
}

// A frame for l and r, both containers of the same kind (or raw scalars
// in an array frame).
auto contain_frame(ByteView l, ByteView r, bool object, bool lraw, bool rraw, Contain& f) -> Outcome {
  f = {};
  f.l = l;
  f.r = r;
  f.object = object;
  f.lraw = lraw;
  f.rraw = rraw;
  if (!lraw) {
    if (auto x = open_container(l, f.lc); !x.ok()) return x;
  }
  if (!rraw) {
    if (auto x = open_container(r, f.rc); !x.ok()) return x;
  }
  f.ln = lraw ? 1 : f.lc.n;
  f.rn = rraw ? 1 : f.rc.n;
  return {};
}

// The kind of a path operation, as setPath's op_type.
enum class PathOp : std::uint8_t { Create, Replace, Delete, InsertBefore, InsertAfter };

auto creates(PathOp op) -> bool { return op == PathOp::Create || op == PathOp::InsertBefore || op == PathOp::InsertAfter; }

// One container on the way down a path, and the child the path takes.
struct Step {
  ByteView node;
  Container c;
  std::uint32_t target{0};
};

// setPath, walked down iteratively: the containers on the path rebuilt,
// everything else copied. `result` is `node` when nothing changes.
auto modify_path(ByteView node, ByteView path, std::optional<ByteView> value, PathOp op, Memory mem, Alloc out, ByteView& result)
    -> Outcome {
  result = node;
  auto n = path_size(path);
  if (!n) return corrupt("a path that does not decode");
  Scratch<Step> steps{mem, Site::Stack};
  PathCursor cur{path};
  auto here = node;
  // The last level's change.
  enum class Change : std::uint8_t { None, ReplaceAt, DropAt, BeforeAt, AfterAt, Prepend, Append, AddKey };
  auto change = Change::None;
  std::uint32_t at = 0;
  std::string_view new_key;
  Container last_c;
  for (std::uint32_t level = 0; level < *n; ++level) {
    auto elem = cur.next();
    if (!elem) return data({kNullValueNotAllowed, "path element at position " + std::to_string(level + 1) + " is null", "", "", ""});
    Type t{};
    Container c;
    if (auto r = container_of(here, t, c); !r.ok()) return r;
    if (t != Type::Array && t != Type::Object) return {};  // a scalar: pushed as it is
    auto last = level + 1 == *n;
    if (t == Type::Object) {
      std::optional<std::uint32_t> idx;
      if (auto r = find_key(here, c, bytes_of(*elem), idx); !r.ok()) return r;
      if (!last) {
        if (!idx) return {};
        if (!steps.push({here, c, *idx})) return refused();
        ByteView v;
        if (auto r = child_at(here, c, *idx, nullptr, v); !r.ok()) return r;
        here = v;
        continue;
      }
      if (idx) {
        if (op == PathOp::InsertBefore || op == PathOp::InsertAfter) {
          return invalid("cannot replace existing key", "Try using the function jsonb_set to replace key value.");
        }
        change = op == PathOp::Delete ? Change::DropAt : Change::ReplaceAt;
        at = *idx;
      } else if (creates(op)) {
        change = Change::AddKey;
        new_key = *elem;
      } else {
        return {};
      }
      last_c = c;
      break;
    }
    auto index = path_index(*elem);
    if (!index) {
      return data({kInvalidTextRepresentation,
                   "path element at position " + std::to_string(level + 1) + " is not an integer: \"" + std::string{*elem} + "\"", "",
                   "", ""});
    }
    auto count = std::int64_t{c.n};
    auto idx = std::int64_t{*index};
    auto before_all = false;
    if (idx < 0) {
      if (-idx > count) {
        before_all = true;
      } else {
        idx += count;
      }
    }
    if (idx > count) idx = count;
    if (!last) {
      if (before_all || idx >= count) return {};
      if (!steps.push({here, c, static_cast<std::uint32_t>(idx)})) return refused();
      ByteView v;
      if (auto r = child_at(here, c, static_cast<std::uint32_t>(idx), nullptr, v); !r.ok()) return r;
      here = v;
      continue;
    }
    if ((before_all || count == 0) && creates(op)) {
      change = Change::Prepend;
    } else if (!before_all && idx < count) {
      at = static_cast<std::uint32_t>(idx);
      switch (op) {
        case PathOp::Create:
        case PathOp::Replace: change = Change::ReplaceAt; break;
        case PathOp::Delete: change = Change::DropAt; break;
        case PathOp::InsertBefore: change = Change::BeforeAt; break;
        case PathOp::InsertAfter: change = Change::AfterAt; break;
      }
    } else if (creates(op)) {
      change = Change::Append;
    } else {
      return {};
    }
    last_c = c;
    break;
  }
  if (change == Change::None) return {};
  Builder b{mem};
  auto& path_steps = steps.vec();
  for (const auto& s : path_steps) {
    if (auto r = b.open(s.c.object); !r.ok()) return r;
    if (s.c.object) {
      if (auto r = copy_children(b, s.node, s.c, 0, s.c.n, s.target); !r.ok()) return r;
      ByteView k;
      if (auto r = key_at(s.node, s.c, s.target, k); !r.ok()) return r;
      b.key(k);
    } else if (auto r = copy_children(b, s.node, s.c, 0, s.target); !r.ok()) {
      return r;
    }
  }
  auto put_value = [&]() -> Outcome { return b.leaf(*value, true); };
  if (auto r = b.open(last_c.object); !r.ok()) return r;
  switch (change) {
    case Change::None: break;
    case Change::DropAt:
      if (auto r = copy_children(b, here, last_c, 0, last_c.n, at); !r.ok()) return r;
      break;
    case Change::ReplaceAt:
      if (auto r = copy_children(b, here, last_c, 0, at); !r.ok()) return r;
      if (last_c.object) {
        ByteView k;
        if (auto r = key_at(here, last_c, at, k); !r.ok()) return r;
        b.key(k);
      }
      if (auto r = put_value(); !r.ok()) return r;
      if (auto r = copy_children(b, here, last_c, at + 1, last_c.n); !r.ok()) return r;
      break;
    case Change::BeforeAt:
    case Change::AfterAt: {
      if (auto r = copy_children(b, here, last_c, 0, at); !r.ok()) return r;
      if (change == Change::BeforeAt) {
        if (auto r = put_value(); !r.ok()) return r;
      }
      if (auto r = copy_children(b, here, last_c, at, at + 1); !r.ok()) return r;
      if (change == Change::AfterAt) {
        if (auto r = put_value(); !r.ok()) return r;
      }
      if (auto r = copy_children(b, here, last_c, at + 1, last_c.n); !r.ok()) return r;
      break;
    }
    case Change::Prepend:
      if (auto r = put_value(); !r.ok()) return r;
      if (auto r = copy_children(b, here, last_c, 0, last_c.n); !r.ok()) return r;
      break;
    case Change::Append:
      if (auto r = copy_children(b, here, last_c, 0, last_c.n); !r.ok()) return r;
      if (auto r = put_value(); !r.ok()) return r;
      break;
    case Change::AddKey:
      if (auto r = copy_children(b, here, last_c, 0, last_c.n); !r.ok()) return r;
      b.key(bytes_of(new_key));
      if (auto r = put_value(); !r.ok()) return r;
      break;
  }
  b.close();
  for (auto k = path_steps.size(); k-- > 0;) {
    const auto& s = path_steps[k];
    if (!s.c.object) {
      if (auto r = copy_children(b, s.node, s.c, s.target + 1, s.c.n); !r.ok()) return r;
    }
    b.close();
  }
  return b.finish(out, result);
}

// The common head of jsonb_set, jsonb_insert and #-: a path of one
// dimension, a container.
auto path_target(ByteView node, ByteView path, const char* scalar_message, Type& t, Container& c) -> Outcome {
  if (!path_size(path)) return corrupt("a path that does not decode");
  if (path_multidimensional(path)) return wrong_subscripts();
  if (auto r = container_of(node, t, c); !r.ok()) return r;
  if (t != Type::Array && t != Type::Object) return invalid(scalar_message);
  return {};
}

}  // namespace

auto path_multidimensional(ByteView path) -> bool { return path.size() >= 4 && (rd32(path, 0) & kPathMulti) != 0; }

auto contains(ByteView a, ByteView b, Memory mem, bool& result) -> Outcome {
  result = false;
  Type ta{};
  Type tb{};
  if (auto r = type_of(a, ta); !r.ok()) return r;
  if (auto r = type_of(b, tb); !r.ok()) return r;
  if ((ta == Type::Object) != (tb == Type::Object)) return {};
  auto lraw = ta != Type::Object && ta != Type::Array;
  auto rraw = tb != Type::Object && tb != Type::Array;
  if (lraw && !rraw) return {};
  Scratch<Contain> stack{mem, Site::Stack};
  Contain top;
  if (auto r = contain_frame(a, b, ta == Type::Object, lraw, rraw, top); !r.ok()) return r;
  if (!stack.push(top)) return refused();
  auto& frames = stack.vec();
  auto last = true;      // the result of the frame last popped
  auto returned = false;  // whether a frame was just popped onto the one below
  auto finish = [&](bool v) {
    last = v;
    returned = true;
    frames.pop_back();
  };
  while (!frames.empty()) {
    auto& f = frames.back();
    if (returned) {
      returned = false;
      if (f.object) {
        if (!last) {
          finish(false);
          continue;
        }
        ++f.i;
      } else if (last) {
        ++f.i;
        f.j = 0;
      } else {
        ++f.j;
      }
    }
    if (f.i == f.rn) {
      finish(true);
      continue;
    }
    if (f.object) {
      if (f.ln < f.rn) {
        finish(false);
        continue;
      }
      ByteView k;
      ByteView rv;
      if (auto r = child_at(f.r, f.rc, f.i, &k, rv); !r.ok()) return r;
      std::optional<std::uint32_t> idx;
      if (auto r = find_key(f.l, f.lc, k, idx); !r.ok()) return r;
      if (!idx) {
        finish(false);
        continue;
      }
      ByteView lv;
      if (auto r = child_at(f.l, f.lc, *idx, nullptr, lv); !r.ok()) return r;
      auto lt = tag_of(lv);
      auto rt = tag_of(rv);
      if (!lt || !rt) return corrupt("an empty node");
      if (!is_container_tag(*lt) || !is_container_tag(*rt)) {
        auto eq = false;
        if (is_container_tag(*lt) != is_container_tag(*rt)) {
          finish(false);
          continue;
        }
        if (auto r = scalar_equal(lv, rv, eq); !r.ok()) return r;
        if (!eq) {
          finish(false);
          continue;
        }
        ++f.i;
        continue;
      }
      if (*lt != *rt) {
        finish(false);
        continue;
      }
      Contain next;
      if (auto r = contain_frame(lv, rv, *lt == kTagObject, false, false, next); !r.ok()) return r;
      if (!stack.push(next)) return refused();
      continue;
    }
    ByteView re;
    if (auto r = contain_elem(f.r, f.rc, f.rraw, f.i, re); !r.ok()) return r;
    auto rt = tag_of(re);
    if (!rt) return corrupt("an empty node");
    if (!is_container_tag(*rt)) {
      auto found = false;
      for (std::uint32_t j = 0; j < f.ln && !found; ++j) {
        ByteView le;
        if (auto r = contain_elem(f.l, f.lc, f.lraw, j, le); !r.ok()) return r;
        if (auto r = scalar_equal(le, re, found); !r.ok()) return r;
      }
      if (!found) {
        finish(false);
        continue;
      }
      ++f.i;
      continue;
    }
    // A container: the next of the left's containers of its kind to try.
    ByteView candidate;
    auto have = false;
    for (; f.j < f.ln; ++f.j) {
      if (auto r = contain_elem(f.l, f.lc, f.lraw, f.j, candidate); !r.ok()) return r;
      auto lt = tag_of(candidate);
      if (!lt) return corrupt("an empty node");
      if (*lt == *rt) {
        have = true;
        break;
      }
    }
    if (!have) {
      finish(false);
      continue;
    }
    Contain next;
    if (auto r = contain_frame(candidate, re, *rt == kTagObject, false, false, next); !r.ok()) return r;
    if (!stack.push(next)) return refused();
  }
  result = last;
  return {};
}

auto exists(ByteView node, std::string_view key, bool& result) -> Outcome {
  result = false;
  Type t{};
  Container c;
  if (auto r = container_of(node, t, c); !r.ok()) return r;
  auto want = bytes_of(key);
  auto string_equal = [&](ByteView v) {
    return v.size() == want.size() + 1 && std::to_integer<std::uint8_t>(v[0]) == kTagString &&
           (want.empty() || std::memcmp(v.data() + 1, want.data(), want.size()) == 0);
  };
  switch (t) {
    case Type::Object: {
      std::optional<std::uint32_t> idx;
      if (auto r = find_key(node, c, want, idx); !r.ok()) return r;
      result = idx.has_value();
      return {};
    }
    case Type::Array:
      for (std::uint32_t i = 0; i < c.n && !result; ++i) {
        ByteView v;
        if (auto r = child_at(node, c, i, nullptr, v); !r.ok()) return r;
        result = string_equal(v);
      }
      return {};
    case Type::String: result = string_equal(node); return {};
    case Type::Null:
    case Type::Boolean:
    case Type::Number: return {};
  }
  return {};
}

auto exists_some(ByteView node, ByteView path, bool all, bool& result) -> Outcome {
  auto n = path_size(path);
  if (!n) return corrupt("a path that does not decode");
  result = all;
  PathCursor cur{path};
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto key = cur.next();
    if (!key) continue;
    auto found = false;
    if (auto r = exists(node, *key, found); !r.ok()) return r;
    if (found != all) {
      result = found;
      return {};
    }
  }
  return {};
}

auto concat(ByteView a, ByteView b, Memory mem, Alloc out, ByteView& result) -> Outcome {
  Type ta{};
  Type tb{};
  Container ca;
  Container cb;
  if (auto r = container_of(a, ta, ca); !r.ok()) return r;
  if (auto r = container_of(b, tb, cb); !r.ok()) return r;
  Builder bd{mem};
  auto objects = ta == Type::Object && tb == Type::Object;
  if (auto r = bd.open(objects); !r.ok()) return r;
  auto side = [&](ByteView v, Type t, const Container& c) -> Outcome {
    if (objects || t == Type::Array) return copy_children(bd, v, c, 0, c.n);
    return bd.leaf(v, t == Type::Object);  // an object nested, or a scalar as an element of its own
  };
  if (auto r = side(a, ta, ca); !r.ok()) return r;
  if (auto r = side(b, tb, cb); !r.ok()) return r;
  bd.close();
  return bd.finish(out, result);
}

auto delete_key(ByteView node, std::string_view key, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container c;
  if (auto r = container_of(node, t, c); !r.ok()) return r;
  if (t != Type::Array && t != Type::Object) return invalid("cannot delete from scalar");
  if (c.n == 0) return {};
  Builder b{mem};
  if (auto r = b.open(c.object); !r.ok()) return r;
  if (c.object) {
    std::optional<std::uint32_t> idx;
    if (auto r = find_key(node, c, bytes_of(key), idx); !r.ok()) return r;
    if (!idx) return {};
    if (auto r = copy_children(b, node, c, 0, c.n, *idx); !r.ok()) return r;
  } else {
    for (std::uint32_t i = 0; i < c.n; ++i) {
      ByteView v;
      if (auto r = child_at(node, c, i, nullptr, v); !r.ok()) return r;
      auto drop = v.size() == key.size() + 1 && std::to_integer<std::uint8_t>(v[0]) == kTagString &&
                  (key.empty() || std::memcmp(v.data() + 1, key.data(), key.size()) == 0);
      if (drop) continue;
      if (auto r = b.leaf(v, false); !r.ok()) return r;
    }
  }
  b.close();
  return b.finish(out, result);
}

auto delete_keys(ByteView node, ByteView path, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  auto n = path_size(path);
  if (!n) return corrupt("a path that does not decode");
  if (path_multidimensional(path)) return wrong_subscripts();
  Type t{};
  Container c;
  if (auto r = container_of(node, t, c); !r.ok()) return r;
  if (t != Type::Array && t != Type::Object) return invalid("cannot delete from scalar");
  if (c.n == 0 || *n == 0) return {};
  // The keys, sorted for a binary search per member.
  Scratch<std::string_view> keys{mem, Site::Keys};
  PathCursor cur{path};
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto k = cur.next();
    if (k && !keys.push(*k)) return refused();
  }
  auto& sorted = keys.vec();
  std::ranges::sort(sorted);
  auto listed = [&](ByteView bytes) {
    return std::ranges::binary_search(sorted, std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
  };
  Builder b{mem};
  if (auto r = b.open(c.object); !r.ok()) return r;
  for (std::uint32_t i = 0; i < c.n; ++i) {
    ByteView k;
    ByteView v;
    if (auto r = child_at(node, c, i, &k, v); !r.ok()) return r;
    if (c.object) {
      if (listed(k)) continue;
      b.key(k);
    } else if (std::to_integer<std::uint8_t>(v[0]) == kTagString && listed(v.subspan(1))) {
      continue;
    }
    if (auto r = b.leaf(v, false); !r.ok()) return r;
  }
  b.close();
  return b.finish(out, result);
}

auto delete_index(ByteView node, std::int64_t i, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container c;
  if (auto r = container_of(node, t, c); !r.ok()) return r;
  if (t == Type::Object) return invalid("cannot delete from object using integer index");
  if (t != Type::Array) return invalid("cannot delete from scalar");
  auto n = std::int64_t{c.n};
  if (n == 0) return {};
  if (i < 0) {
    if (i < -n) return {};
    i += n;
  }
  if (i >= n) return {};
  Builder b{mem};
  if (auto r = b.open(false); !r.ok()) return r;
  if (auto r = copy_children(b, node, c, 0, c.n, static_cast<std::uint32_t>(i)); !r.ok()) return r;
  b.close();
  return b.finish(out, result);
}

auto delete_path(ByteView node, ByteView path, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container c;
  if (auto r = path_target(node, path, "cannot delete path in scalar", t, c); !r.ok()) return r;
  if (c.n == 0) return {};
  return modify_path(node, path, std::nullopt, PathOp::Delete, mem, out, result);
}

auto set(ByteView node, ByteView path, ByteView value, bool create, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container c;
  if (auto r = path_target(node, path, "cannot set path in scalar", t, c); !r.ok()) return r;
  if (c.n == 0 && !create) return {};
  return modify_path(node, path, value, create ? PathOp::Create : PathOp::Replace, mem, out, result);
}

auto insert(ByteView node, ByteView path, ByteView value, bool after, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container c;
  if (auto r = path_target(node, path, "cannot set path in scalar", t, c); !r.ok()) return r;
  return modify_path(node, path, value, after ? PathOp::InsertAfter : PathOp::InsertBefore, mem, out, result);
}

auto null_treatment(std::optional<std::string_view> text, NullTreatment& t) -> Outcome {
  if (text == "raise_exception") {
    t = NullTreatment::RaiseException;
  } else if (text == "use_json_null") {
    t = NullTreatment::UseJsonNull;
  } else if (text == "delete_key") {
    t = NullTreatment::DeleteKey;
  } else if (text == "return_target") {
    t = NullTreatment::ReturnTarget;
  } else {
    return invalid(R"(null_value_treatment must be "delete_key", "return_target", "use_json_null", or "raise_exception")");
  }
  return {};
}

auto null_value_error() -> Error {
  return {kNullValueNotAllowed, "JSON value must not be null",
          R"(Exception was raised because null_value_treatment is "raise_exception".)",
          "To avoid, either change the null_value_treatment argument or ensure that an SQL NULL is not passed.", ""};
}

auto strip_nulls(ByteView node, Memory mem, Alloc out, ByteView& result) -> Outcome {
  result = node;
  Type t{};
  Container root;
  if (auto r = container_of(node, t, root); !r.ok()) return r;
  if (t != Type::Array && t != Type::Object) return {};
  struct Open {
    ByteView v;
    Container c;
    std::uint32_t i{0};
  };
  Scratch<Open> stack{mem, Site::Stack};
  Builder b{mem};
  if (auto r = b.open(root.object); !r.ok()) return r;
  if (!stack.push({node, root, 0})) return refused();
  auto& frames = stack.vec();
  while (!frames.empty()) {
    auto& f = frames.back();
    if (f.i == f.c.n) {
      b.close();
      frames.pop_back();
      continue;
    }
    ByteView k;
    ByteView v;
    if (auto r = child_at(f.v, f.c, f.i++, &k, v); !r.ok()) return r;
    auto tag = tag_of(v);
    if (!tag) return corrupt("an empty node");
    if (f.c.object) {
      if (*tag == kTagNull) continue;
      b.key(k);
    }
    if (!is_container_tag(*tag)) {
      if (auto r = b.leaf(v, false); !r.ok()) return r;
      continue;
    }
    Container c;
    if (auto r = open_container(v, c); !r.ok()) return r;
    if (auto r = b.open(c.object); !r.ok()) return r;
    // f is not used after the push.
    if (!stack.push({v, c, 0})) return refused();
  }
  return b.finish(out, result);
}

auto pretty(ByteView node, Memory mem, Alloc out, std::span<char>& text) -> Outcome {
  CountSink count;
  if (auto r = walk<true>(node, mem, count); !r.ok()) return r;
  if (count.n > kMaxText) {
    return data({kProgramLimitExceeded, "the text of a jsonb value would exceed 1073741823 bytes", "", "", ""});
  }
  auto* p = out.fn(out.self, static_cast<std::size_t>(count.n));
  if (p == nullptr) return refused();
  text = {reinterpret_cast<char*>(p), static_cast<std::size_t>(count.n)};
  WriteSink sink{text};
  auto r = walk<true>(node, mem, sink);
  if (r.ok() && sink.at != text.size()) return corrupt("text shorter than its length");
  return r;
}

auto scalar(Scalar kind, std::string_view text, Memory mem, Alloc out, ByteView& node) -> Outcome {
  auto one = [&](std::uint8_t tag) -> Outcome {
    auto* p = out.fn(out.self, 1);
    if (p == nullptr) return refused();
    p[0] = std::byte{tag};
    node = {p, 1};
    return {};
  };
  switch (kind) {
    case Scalar::True: return one(kTagTrue);
    case Scalar::False: return one(kTagFalse);
    case Scalar::Number: {
      // numeric_to_jsonb: a number when the text is a JSON number (numeric
      // and float output always is, but for NaN and the infinities).
      std::span<std::byte> value;
      auto r = parse(text, mem, out, value);
      if (r.ok()) {
        auto n = node_of(value);
        if (n && std::to_integer<std::uint8_t>((*n)[0]) == kTagNumber) {
          node = *n;
          return {};
        }
      } else if (r.status != Status::Data || r.error->state.view() != kInvalidTextRepresentation.view()) {
        return r;
      }
      break;
    }
    case Scalar::String: break;
  }
  if (!utf8_valid(reinterpret_cast<const unsigned char*>(text.data()), text.size())) {
    return data({kCharacterNotInRepertoire, "invalid byte sequence for encoding \"UTF8\"", "", "", ""});
  }
  if (text.size() >= kMaxBytes) return data(size_error(kTagString));
  auto* p = out.fn(out.self, text.size() + 1);
  if (p == nullptr) return refused();
  p[0] = std::byte{kTagString};
  if (!text.empty()) std::memcpy(p + 1, text.data(), text.size());
  node = {p, text.size() + 1};
  return {};
}

auto build_array(std::span<const BuildArg> args, Memory mem, Alloc out, ByteView& result) -> Outcome {
  Builder b{mem};
  if (auto r = b.open(false); !r.ok()) return r;
  for (const auto& a : args) {
    if (auto r = a.null ? b.leaf(kNullNode, false) : b.leaf(a.node, true); !r.ok()) return r;
  }
  b.close();
  return b.finish(out, result);
}

auto build_object(std::span<const BuildArg> args, Memory mem, Alloc out, ByteView& result) -> Outcome {
  if (args.size() % 2 != 0) {
    return invalid("argument list must have even number of elements",
                   "The arguments of jsonb_build_object() must consist of alternating keys and values.");
  }
  Builder b{mem};
  if (auto r = b.open(true); !r.ok()) return r;
  for (std::size_t i = 0; i < args.size(); i += 2) {
    const auto& k = args[i];
    if (k.null) return invalid("argument " + std::to_string(i + 1) + ": key must not be null");
    if (k.composite) return invalid("key value must be scalar, not array, composite, or json");
    auto tag = tag_of(k.node);
    if (!tag) return corrupt("an empty node");
    if (is_container_tag(*tag) || *tag == kTagNull) return invalid("key value must be scalar, not array, composite, or json");
    switch (*tag) {
      case kTagString: b.key(k.node.subspan(1)); break;
      case kTagTrue: b.key(bytes_of("true")); break;
      case kTagFalse: b.key(bytes_of("false")); break;
      case kTagNumber:
        if (auto r = b.key_number(k.node); !r.ok()) return r;
        break;
      default: return corrupt("an unknown tag");
    }
    const auto& v = args[i + 1];
    if (auto r = v.null ? b.leaf(kNullNode, false) : b.leaf(v.node, true); !r.ok()) return r;
  }
  b.close();
  return b.finish(out, result);
}

// ------------------------------------------- comparison and order (stage 2)

namespace {

// The sort key's tags: the type order, the top-level empty array first.
constexpr std::uint8_t kKeyEmptyTop = 0x01;
constexpr std::uint8_t kKeyNull = 0x02;
constexpr std::uint8_t kKeyString = 0x03;
constexpr std::uint8_t kKeyNumber = 0x04;
constexpr std::uint8_t kKeyBool = 0x05;
constexpr std::uint8_t kKeyArray = 0x06;
constexpr std::uint8_t kKeyObject = 0x07;

// Writes a sort key into its buffer: false past its end, never a write past it.
struct KeyWriter {
  std::span<std::byte> out;
  std::size_t at{0};

  auto put(std::uint8_t b) -> bool {
    if (at >= out.size()) return false;
    out[at++] = std::byte{b};
    return true;
  }
  // Big-endian, so memcmp orders it as a number.
  auto put_u32(std::uint32_t v) -> bool {
    for (int shift = 24; shift >= 0; shift -= 8) {
      if (!put(static_cast<std::uint8_t>(v >> shift))) return false;
    }
    return true;
  }
  // A string's or key's bytes, then 0x00: prefix-free because the bytes hold
  // no NUL, checked here (a damaged value is Corrupt, never a shared prefix).
  auto put_text(ByteView s) -> Outcome {
    if (std::ranges::find(s, std::byte{0}) != s.end()) return corrupt("a string that is not UTF-8 or holds a NUL byte");
    if (out.size() - at < s.size() + 1) return corrupt("a sort key past its bound");
    std::memcpy(out.data() + at, s.data(), s.size());
    at += s.size();
    out[at++] = std::byte{0};
    return {};
  }
};

// A scalar node's sort key. A number: its sign class (negative, zero,
// positive), then for a non-zero one its weight (big-endian, sign bit
// flipped) and its significant digits ended by 0x00, every byte after the
// sign class inverted when negative. Scale is not in it: 1 = 1.0.
auto key_scalar(ByteView v, KeyWriter& w) -> Outcome {
  auto past = [] { return corrupt("a sort key past its bound"); };
  auto tag = tag_of(v);
  if (!tag) return corrupt("an empty node");
  switch (*tag) {
    case kTagNull:
    case kTagFalse:
    case kTagTrue:
      if (v.size() != 1) return corrupt("a null or boolean longer than its tag");
      if (*tag == kTagNull) return w.put(kKeyNull) ? Outcome{} : past();
      return w.put(kKeyBool) && w.put(*tag == kTagTrue ? 1 : 0) ? Outcome{} : past();
    case kTagString:
      if (!w.put(kKeyString)) return past();
      return w.put_text(v.subspan(1));
    case kTagNumber: {
      Number n;
      if (auto r = read_number(v, 0, v.size(), n); !r.ok()) return r;
      if (!w.put(kKeyNumber)) return past();
      if (n.sign == 0) return w.put(0x02) ? Outcome{} : past();
      auto negative = n.sign == 2;
      if (!w.put(negative ? 0x01 : 0x03)) return past();
      auto from = w.at;
      if (!w.put_u32(static_cast<std::uint32_t>(n.weight) ^ 0x80000000U)) return past();
      if (w.out.size() - w.at < n.k + 1) return past();
      std::memcpy(w.out.data() + w.at, n.digits, n.k);
      w.at += n.k;
      w.out[w.at++] = std::byte{0};
      if (negative) {
        for (auto& b : w.out.subspan(from, w.at - from)) b = ~b;
      }
      return {};
    }
    default: return corrupt("an unknown tag");
  }
}

}  // namespace

auto sort_key(ByteView node, Memory mem, std::span<std::byte> out, std::size_t& length) -> Outcome {
  length = 0;
  KeyWriter w{out};
  auto past = [] { return corrupt("a sort key past its bound"); };
  auto tag = tag_of(node);
  if (!tag) return corrupt("an empty node");
  if (*tag == kTagArray) {
    // PostgreSQL 17 compares a top-level array's count before its raw-scalar
    // flag, so the empty one sorts below every scalar (compareJsonbContainers).
    Container c;
    if (auto r = open_container(node, c); !r.ok()) return r;
    if (c.n == 0) {
      if (!w.put(kKeyEmptyTop)) return past();
      length = w.at;
      return {};
    }
  }
  struct Open {
    ByteView v;
    Container c;
    std::uint32_t i{0};
  };
  Scratch<Open> stack{mem, Site::Stack};
  // A scalar is written whole; a container writes its tag and count, its
  // children following in storage order (an object's key before its value).
  auto visit = [&](ByteView v) -> Outcome {
    auto t = tag_of(v);
    if (!t) return corrupt("an empty node");
    if (!is_container_tag(*t)) return key_scalar(v, w);
    if (stack.vec().size() >= kMaxDepth) return corrupt("a value nested past 10000 containers");
    Container c;
    if (auto r = open_container(v, c); !r.ok()) return r;
    if (!w.put(c.object ? kKeyObject : kKeyArray) || !w.put_u32(c.n)) return past();
    if (!stack.push({v, c, 0})) return refused();
    return {};
  };
  if (auto r = visit(node); !r.ok()) return r;
  auto& frames = stack.vec();
  while (!frames.empty()) {
    auto& f = frames.back();
    if (f.i == f.c.n) {
      frames.pop_back();
      continue;
    }
    ByteView k;
    ByteView v;
    if (auto r = child_at(f.v, f.c, f.i++, f.c.object ? &k : nullptr, v); !r.ok()) return r;
    if (f.c.object) {
      if (auto r = w.put_text(k); !r.ok()) return r;
    }
    // visit may push a frame: f is not used after it.
    if (auto r = visit(v); !r.ok()) return r;
  }
  length = w.at;
  return {};
}

auto sort_key_value(ByteView node, std::vector<std::byte>& out) -> Outcome {
  out.resize(sort_key_bound(node));
  std::size_t length = 0;
  auto r = sort_key(node, Memory{}, out, length);
  out.resize(r.ok() ? length : 0);
  return r;
}

}  // namespace bytecask::sql::jsonb
