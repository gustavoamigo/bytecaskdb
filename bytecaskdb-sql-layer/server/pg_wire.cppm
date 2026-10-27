// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// pg_wire.cppm — PostgreSQL's wire protocol, version 3.0, as bytes: framing
// complete messages from a stream, reading the fields of a message's body,
// and building a message to send. No I/O and no policy
// (docs/server_design.md, docs/session_design.md).

module;
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bytecask.sql.pg_wire;

export namespace bytecask::sql::pg {

// The startup packet's codes: the protocol version, and the requests that
// take its place.
inline constexpr std::int32_t kProtocol30 = 196'608;  // 3.0
inline constexpr std::int32_t kSslRequest = 80'877'103;
inline constexpr std::int32_t kGssEncRequest = 80'877'104;
inline constexpr std::int32_t kCancelRequest = 80'877'102;

// The most a message may hold, its length field included: past it the
// connection is closed with 08P01 (docs/server_design.md, *I/O*).
inline constexpr std::uint32_t kMaxMessage = 64u << 20;

// A message whose body does not hold the fields its type has.
class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  ProtocolError(const ProtocolError&) = default;
  ProtocolError& operator=(const ProtocolError&) = default;
  ~ProtocolError() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// The fields of a received message's body, read in order. Integers are
// big-endian; a string ends at its NUL. Every read past the end throws
// ProtocolError. The body must outlive the reader and what it returns.
class Body {
public:
  explicit Body(std::string_view body) : s_{body} {}
  [[nodiscard]] auto byte() -> char;
  [[nodiscard]] auto i16() -> std::int16_t;
  [[nodiscard]] auto i32() -> std::int32_t;
  [[nodiscard]] auto cstr() -> std::string_view;
  [[nodiscard]] auto bytes(std::size_t n) -> std::string_view;
  [[nodiscard]] auto empty() const noexcept -> bool { return s_.empty(); }

private:
  void need(std::size_t n) const;
  std::string_view s_;
};

// Messages to send, appended to one buffer: begin() a message of a type,
// add its fields, end() it, which writes its length.
class Out {
public:
  void begin(char type);
  void i16(std::int16_t v);
  void i32(std::int32_t v);
  void cstr(std::string_view s);  // the bytes, then NUL
  void bytes(std::string_view s);
  void end();
  // A message of one byte and no body ('N' answering SSLRequest).
  void raw(char c) { buf_.push_back(c); }
  [[nodiscard]] auto data() const noexcept -> std::string_view { return buf_; }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return buf_.size(); }
  void clear() noexcept { buf_.clear(); }
  [[nodiscard]] auto take() noexcept -> std::string {
    std::string out = std::move(buf_);
    buf_.clear();
    return out;
  }

private:
  std::string buf_;
  std::size_t start_{0};  // where the open message's length goes
};

// One complete unit cut from a connection's byte stream, or EndOfStream when
// the read side closes.
struct Input {
  enum class Kind {
    Startup,           // startup packet body (after its 4-byte length)
    Message,           // regular frontend message (type + body)
    BadStartupLength,  // startup packet length < 8 or > 10'000
    BadMessageLength,  // message length < 4 or > kMaxMessage
    EndOfStream,       // client EOF
  } kind{Kind::Message};
  char type{'\0'};
  std::string body;
};

// Cuts complete startup packets and frontend messages from a byte stream.
// Owned by the connection on the I/O loop: no I/O and no lock.
class Framer {
public:
  void push(std::string_view bytes, std::vector<Input>& out);

private:
  std::string in_;
  std::size_t pos_{0};
  bool startup_{true};
  bool ended_{false};
};

}  // namespace bytecask::sql::pg

namespace bytecask::sql::pg {

namespace {

[[nodiscard]] auto read_u32(std::string_view s) noexcept -> std::uint32_t {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<unsigned char>(s[i]);
  return v;
}

}  // namespace

ProtocolError::~ProtocolError() = default;

void Body::need(std::size_t n) const {
  if (s_.size() < n) throw ProtocolError("message too short for its fields");
}

auto Body::byte() -> char {
  need(1);
  auto c = s_.front();
  s_.remove_prefix(1);
  return c;
}

auto Body::i16() -> std::int16_t {
  need(2);
  auto v = static_cast<std::uint16_t>((static_cast<unsigned char>(s_[0]) << 8) | static_cast<unsigned char>(s_[1]));
  s_.remove_prefix(2);
  return static_cast<std::int16_t>(v);
}

auto Body::i32() -> std::int32_t {
  need(4);
  auto v = read_u32(s_);
  s_.remove_prefix(4);
  return static_cast<std::int32_t>(v);
}

auto Body::cstr() -> std::string_view {
  auto nul = s_.find('\0');
  if (nul == std::string_view::npos) throw ProtocolError("a string without its terminating NUL");
  auto out = s_.substr(0, nul);
  s_.remove_prefix(nul + 1);
  return out;
}

auto Body::bytes(std::size_t n) -> std::string_view {
  need(n);
  auto out = s_.substr(0, n);
  s_.remove_prefix(n);
  return out;
}

void Out::begin(char type) {
  buf_.push_back(type);
  start_ = buf_.size();
  buf_.append(4, '\0');
}

void Out::i16(std::int16_t v) {
  auto u = static_cast<std::uint16_t>(v);
  buf_.push_back(static_cast<char>(u >> 8));
  buf_.push_back(static_cast<char>(u & 0xFF));
}

void Out::i32(std::int32_t v) {
  auto u = static_cast<std::uint32_t>(v);
  for (int shift = 24; shift >= 0; shift -= 8) buf_.push_back(static_cast<char>((u >> shift) & 0xFF));
}

void Out::cstr(std::string_view s) {
  buf_.append(s);
  buf_.push_back('\0');
}

void Out::bytes(std::string_view s) { buf_.append(s); }

void Out::end() {
  auto len = static_cast<std::uint32_t>(buf_.size() - start_);
  for (std::size_t i = 0; i < 4; ++i) buf_[start_ + i] = static_cast<char>((len >> (24 - 8 * i)) & 0xFF);
}

void Framer::push(std::string_view bytes, std::vector<Input>& out) {
  if (ended_ || bytes.empty()) return;
  in_.append(bytes);
  for (;;) {
    auto avail = in_.size() - pos_;
    if (startup_) {
      if (avail < 4) break;
      auto len = read_u32(std::string_view{in_}.substr(pos_, 4));
      if (len < 8 || len > 10'000) {
        out.push_back({Input::Kind::BadStartupLength, '\0', {}});
        ended_ = true;
        in_.clear();
        pos_ = 0;
        return;
      }
      if (avail < len) break;
      std::string body{in_, pos_ + 4, len - 4};
      pos_ += len;
      auto code = static_cast<std::int32_t>(read_u32(body));
      if (code != kSslRequest && code != kGssEncRequest) startup_ = false;
      out.push_back({Input::Kind::Startup, '\0', std::move(body)});
    } else {
      if (avail < 5) break;
      auto type = in_[pos_];
      auto len = read_u32(std::string_view{in_}.substr(pos_ + 1, 4));
      if (len < 4 || len > kMaxMessage) {
        out.push_back({Input::Kind::BadMessageLength, '\0', {}});
        ended_ = true;
        in_.clear();
        pos_ = 0;
        return;
      }
      auto total = 1u + static_cast<std::size_t>(len);
      if (avail < total) break;
      std::string body{in_, pos_ + 5, static_cast<std::size_t>(len - 4)};
      pos_ += total;
      out.push_back({Input::Kind::Message, type, std::move(body)});
    }
  }
  if (pos_ == in_.size()) {
    in_.clear();
    pos_ = 0;
  } else if (pos_ > 0) {
    in_.erase(0, pos_);
    pos_ = 0;
  }
}

}  // namespace bytecask::sql::pg
