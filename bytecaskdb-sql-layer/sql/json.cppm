// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// json.cppm — the JSON libpg_query's pg_query_parse_plpgsql returns, read
// into a tree (docs/procedures_design.md, *Compilation pipeline*). Only what
// that output holds: objects, arrays, strings with escapes, numbers as
// text, true, false and null. A reader for a trusted producer, not a
// validator: anything it does not read is std::runtime_error.

module;

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module bytecask.sql.json;

export namespace bytecask::sql::json {

struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind{Kind::Null};
  bool boolean{false};
  std::string text;  // a string's value, a number's digits
  std::vector<Value> items;
  std::map<std::string, Value, std::less<>> members;

  // The member `key` of an object, or nullptr (also for a non-object).
  [[nodiscard]] auto get(std::string_view key) const -> const Value* {
    if (kind != Kind::Object) return nullptr;
    auto it = members.find(key);
    return it == members.end() ? nullptr : &it->second;
  }
  // A string member's value, or "" when absent.
  [[nodiscard]] auto str(std::string_view key) const -> std::string {
    const auto* v = get(key);
    return v != nullptr && v->kind == Kind::String ? v->text : std::string{};
  }
  // A boolean member, false when absent.
  [[nodiscard]] auto flag(std::string_view key) const -> bool {
    const auto* v = get(key);
    return v != nullptr && v->kind == Kind::Bool && v->boolean;
  }
  // An integer member, `fallback` when absent. libpg_query leaves out a
  // member whose value is 0 or false.
  [[nodiscard]] auto integer(std::string_view key, std::int64_t fallback = 0) const -> std::int64_t {
    const auto* v = get(key);
    return v != nullptr && v->kind == Kind::Number ? std::stoll(v->text) : fallback;
  }
  // An array member's items; empty when absent.
  [[nodiscard]] auto list(std::string_view key) const -> std::span<const Value> {
    const auto* v = get(key);
    return v != nullptr && v->kind == Kind::Array ? std::span<const Value>{v->items} : std::span<const Value>{};
  }
};

[[nodiscard]] auto parse(std::string_view text) -> Value;

}  // namespace bytecask::sql::json

namespace bytecask::sql::json {

namespace {

class Reader {
public:
  explicit Reader(std::string_view s) : s_{s} {}

  auto value() -> Value {
    space();
    if (i_ >= s_.size()) fail("unexpected end");
    auto c = s_[i_];
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') {
      Value v;
      v.kind = Value::Kind::String;
      v.text = string();
      return v;
    }
    if (word("true")) return boolean(true);
    if (word("false")) return boolean(false);
    if (word("null")) return {};
    return number();
  }

  void end() {
    space();
    if (i_ != s_.size()) fail("trailing text");
  }

private:
  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error("PL/pgSQL parse tree: " + what + " at " + std::to_string(i_));
  }
  void space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\t' || s_[i_] == '\r')) ++i_;
  }
  void expect(char c) {
    space();
    if (i_ >= s_.size() || s_[i_] != c) fail(std::string{"expected '"} + c + "'");
    ++i_;
  }
  auto word(std::string_view w) -> bool {
    if (s_.substr(i_, w.size()) != w) return false;
    i_ += w.size();
    return true;
  }
  static auto boolean(bool b) -> Value {
    Value v;
    v.kind = Value::Kind::Bool;
    v.boolean = b;
    return v;
  }
  auto number() -> Value {
    Value v;
    v.kind = Value::Kind::Number;
    auto start = i_;
    while (i_ < s_.size() && (std::string_view{"+-.eE0123456789"}.find(s_[i_]) != std::string_view::npos)) ++i_;
    if (i_ == start) fail("unexpected character");
    v.text = std::string{s_.substr(start, i_ - start)};
    return v;
  }
  auto hex4() -> std::uint32_t {
    if (i_ + 4 > s_.size()) fail("short \\u escape");
    std::uint32_t v = 0;
    for (auto k = 0; k < 4; ++k) {
      auto c = s_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        fail("bad \\u escape");
      }
    }
    return v;
  }
  static void utf8(std::string& out, std::uint32_t cp) {
    auto put = [&](std::uint32_t b) { out.push_back(static_cast<char>(b)); };
    if (cp < 0x80) {
      put(cp);
    } else if (cp < 0x800) {
      put(0xC0 | (cp >> 6));
      put(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      put(0xE0 | (cp >> 12));
      put(0x80 | ((cp >> 6) & 0x3F));
      put(0x80 | (cp & 0x3F));
    } else {
      put(0xF0 | (cp >> 18));
      put(0x80 | ((cp >> 12) & 0x3F));
      put(0x80 | ((cp >> 6) & 0x3F));
      put(0x80 | (cp & 0x3F));
    }
  }
  auto string() -> std::string {
    expect('"');
    std::string out;
    while (true) {
      if (i_ >= s_.size()) fail("unterminated string");
      auto c = s_[i_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size()) fail("unterminated escape");
      auto e = s_[i_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          auto cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(i_, 2) == "\\u") {
            i_ += 2;
            auto lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
  }
  auto array() -> Value {
    expect('[');
    Value v;
    v.kind = Value::Kind::Array;
    space();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return v;
    }
    while (true) {
      v.items.push_back(value());
      space();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      expect(']');
      return v;
    }
  }
  auto object() -> Value {
    expect('{');
    Value v;
    v.kind = Value::Kind::Object;
    space();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return v;
    }
    while (true) {
      space();
      auto key = string();
      expect(':');
      v.members.insert_or_assign(std::move(key), value());
      space();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      expect('}');
      return v;
    }
  }

  std::string_view s_;
  std::size_t i_{0};
};

}  // namespace

auto parse(std::string_view text) -> Value {
  Reader r{text};
  auto v = r.value();
  r.end();
  return v;
}

}  // namespace bytecask::sql::json
