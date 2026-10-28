// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// pg_session.cppm — one PostgreSQL wire-protocol connection, from startup to
// its end, over the embedded API (docs/server_design.md). A session runs on
// its connection's own thread and shares nothing with another but the
// Database. It blocks only on its socket and on the API's calls, which never
// wait for another caller.

module;
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.pg_session;

import bytecask.sql.api;
import bytecask.sql.parser;
import bytecask.sql.pg_wire;
import bytecask.sql.pg_text;

export namespace bytecask::sql::pg {

struct SessionConfig {
  std::string database;  // the one name a client may connect to
};

// Serves one connection until the client terminates or drops it, or until
// `stopping` is set and the read side of `fd` has been shut down, which wakes
// a read: the connection is then sent FATAL 57P01. A send checks `stopping`
// at least once a second. An open transaction is rolled back either way.
// Does not close `fd`.
void serve(Database& db, int fd, const SessionConfig& cfg, const std::atomic<bool>& stopping);

// Once a stop has begun, how long a send waits on a client that takes no
// bytes before the connection is dropped. A client still reading gets its
// reply and the 57P01, however long its result takes.
inline constexpr std::chrono::seconds kStopSendTimeout{10};

// The FATAL 53300 a connection past the limit gets before it is closed.
[[nodiscard]] auto too_many_connections() -> std::string;

}  // namespace bytecask::sql::pg

namespace bytecask::sql::pg {

namespace {

constexpr std::size_t kFlushAt = 64u << 10;

// An error the session raises itself, with its SQLSTATE.
class SessionError : public std::runtime_error {
public:
  SessionError(const std::string& what, const char* sqlstate) : std::runtime_error{what}, sqlstate_{sqlstate} {}
  SessionError(const SessionError&) = default;
  SessionError& operator=(const SessionError&) = default;
  ~SessionError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto sqlstate() const noexcept -> const char* { return sqlstate_; }

private:
  const char* sqlstate_;
};
SessionError::~SessionError() = default;

// The socket closed or failed: nothing more can be sent.
struct ConnectionGone {};

struct Message {
  char type;
  std::string body;
};

// SetEncoding is SET client_encoding, the one SET taken: libpq's
// PQsetClientEncoding sends it, and pgtcl's pg_connect calls that.
enum class Control { Begin, Commit, Rollback, SetEncoding, SetTransaction };

// SET TRANSACTION's modes. SERIALIZABLE is refused when the statement is read,
// so an isolation level that gets here is one the snapshot already gives.
struct TransactionModes {
  bool isolation{false};           // an ISOLATION LEVEL was given
  std::optional<bool> read_only;   // READ ONLY (true) or READ WRITE (false)
};

// A statement's text as the session takes it: nothing, a transaction
// control word, or a statement for the layer.
struct Classified {
  enum class Kind { Empty, Control, Sql, Cursor } kind{Kind::Empty};
  Control control{Control::Begin};
  std::string sql;       // Sql: the one statement, without its trailing ';'
  std::string encoding;  // SetEncoding: PostgreSQL's name for it
  TransactionModes modes;  // SetTransaction
  std::optional<ast::CursorStatement> cursor;  // Cursor: DECLARE, FETCH, MOVE or CLOSE
};

auto upper_words(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string w;
  for (auto c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!w.empty()) out.push_back(std::exchange(w, {}));
    } else {
      w.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

// Where the first statement ends: the first ';' outside a string, a quoted
// name and a comment, or the end. `rest_blank` says whether only white
// space, ';' and comments follow it.
auto first_statement(std::string_view s, bool& rest_blank) -> std::size_t {
  std::size_t end = s.size();
  std::size_t i = 0;
  auto skip = [&](std::size_t& k) -> bool {  // a comment or quoted text from k; true if one was skipped
    if (s.compare(k, 2, "--") == 0) {
      auto nl = s.find('\n', k);
      k = nl == std::string_view::npos ? s.size() : nl + 1;
      return true;
    }
    if (s.compare(k, 2, "/*") == 0) {
      auto close = s.find("*/", k + 2);
      k = close == std::string_view::npos ? s.size() : close + 2;
      return true;
    }
    if (s[k] == '\'' || s[k] == '"') {
      auto q = s[k];
      ++k;
      while (k < s.size()) {
        if (s[k] == q) {
          if (k + 1 < s.size() && s[k + 1] == q) {
            k += 2;  // '' or "" inside
            continue;
          }
          ++k;
          break;
        }
        ++k;
      }
      return true;
    }
    return false;
  };
  while (i < s.size()) {
    if (skip(i)) continue;
    if (s[i] == ';') {
      end = i;
      break;
    }
    ++i;
  }
  rest_blank = true;
  for (auto k = end; k < s.size();) {
    if (s[k] == ';' || std::isspace(static_cast<unsigned char>(s[k])) != 0) {
      ++k;
      continue;
    }
    if (s.compare(k, 2, "--") == 0 || s.compare(k, 2, "/*") == 0) {
      (void)skip(k);
      continue;
    }
    rest_blank = false;
    break;
  }
  return end;
}

// The client encodings taken, by PostgreSQL's name for them: UTF8, with its
// alias UNICODE, and SQL_ASCII, which PostgreSQL converts nothing for.
// Matched as PostgreSQL matches a name: any case, punctuation ignored.
auto client_encoding(std::string_view value) -> std::optional<std::string_view> {
  std::string norm;
  for (auto c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      norm.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  if (norm == "utf8" || norm == "unicode") return "UTF8";
  if (norm == "sqlascii") return "SQL_ASCII";
  return std::nullopt;
}

[[noreturn]] void unsupported_encoding(std::string_view value) {
  throw SessionError("client_encoding \"" + std::string{value} + "\" is not supported: the server speaks UTF8",
                     "22023");
}

// The mode list of SET TRANSACTION, BEGIN and START TRANSACTION: ISOLATION LEVEL …, READ ONLY, READ WRITE,
// [NOT] DEFERRABLE, with or without commas. The snapshot every transaction
// reads is stronger than READ UNCOMMITTED, READ COMMITTED and REPEATABLE READ,
// so those are taken and change nothing; SERIALIZABLE is not provided (the
// guards it needs are not built: docs/transaction_design.md, *Isolation*).
auto transaction_modes(std::string_view sql, std::size_t first) -> TransactionModes {
  std::string spaced;
  for (auto ch : sql) {
    if (ch == ',') {
      spaced += " ";
    } else {
      spaced.push_back(ch);
    }
  }
  auto words = upper_words(spaced);
  TransactionModes modes;
  for (auto i = first; i < words.size();) {
    auto at = [&](std::size_t k) -> std::string_view { return i + k < words.size() ? words[i + k] : std::string_view{}; };
    if (at(0) == "ISOLATION" && at(1) == "LEVEL") {
      if (at(2) == "SERIALIZABLE") {
        throw SessionError("ISOLATION LEVEL SERIALIZABLE is not supported: transactions run at snapshot isolation", "0A000");
      }
      auto level = (at(2) == "REPEATABLE" && at(3) == "READ") ||
                   (at(2) == "READ" && (at(3) == "COMMITTED" || at(3) == "UNCOMMITTED"));
      if (!level) throw SessionError("syntax error at or near \"" + std::string{at(2)} + "\"", "42601");
      modes.isolation = true;
      i += 4;
    } else if (at(0) == "READ" && (at(1) == "ONLY" || at(1) == "WRITE")) {
      modes.read_only = at(1) == "ONLY";
      i += 2;
    } else if (at(0) == "DEFERRABLE") {
      i += 1;
    } else if (at(0) == "NOT" && at(1) == "DEFERRABLE") {
      i += 2;
    } else {
      throw SessionError("syntax error at or near \"" + words[i] + "\"", "42601");
    }
  }
  if (words.size() <= first) throw SessionError("syntax error at end of input", "42601");
  return modes;
}

auto classify(std::string_view text, const char* several_state, std::string_view several_message) -> Classified {
  auto rest_blank = true;
  auto end = first_statement(text, rest_blank);
  if (!rest_blank) throw SessionError(std::string{several_message}, several_state);
  auto sql = text.substr(0, end);
  while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back())) != 0) sql.remove_suffix(1);
  auto words = upper_words(sql);
  Classified out;
  if (words.empty()) return out;
  const auto& w = words[0];
  auto only = [&](std::size_t n) {
    // Words past the first n, bar one WORK or TRANSACTION, are a mode or a
    // clause the session does not take.
    auto extra = words.size() > n && (words[n] == "WORK" || words[n] == "TRANSACTION") ? n + 1 : n;
    if (words.size() > extra) throw SessionError(w + " with options is not supported", "0A000");
  };
  if (w == "BEGIN") {
    auto first = words.size() > 1 && (words[1] == "WORK" || words[1] == "TRANSACTION") ? std::size_t{2} : std::size_t{1};
    auto modes = words.size() > first ? transaction_modes(sql, first) : TransactionModes{};
    return {Classified::Kind::Control, Control::Begin, {}, {}, modes};
  }
  if (w == "START") {
    if (words.size() < 2 || words[1] != "TRANSACTION") throw SessionError("syntax error at or near \"START\"", "42601");
    auto modes = words.size() > 2 ? transaction_modes(sql, 2) : TransactionModes{};
    return {Classified::Kind::Control, Control::Begin, {}, {}, modes};
  }
  if (w == "COMMIT" || w == "END") {
    only(1);
    return {Classified::Kind::Control, Control::Commit, {}};
  }
  if (w == "ROLLBACK" || w == "ABORT") {
    only(1);
    return {Classified::Kind::Control, Control::Rollback, {}};
  }
  if (w == "SET" && words.size() > 1 && words[1] == "TRANSACTION") {
    auto modes = transaction_modes(sql, 2);
    return {Classified::Kind::Control, Control::SetTransaction, {}, {}, modes};
  }
  if (w == "SET") {
    // SET [SESSION] client_encoding {TO | =} 'UTF8': the server speaks UTF8,
    // so only the encodings client_encoding() takes are.
    std::string spaced;
    for (auto ch : sql) {
      if (ch == '=') {
        spaced += " = ";
      } else if (ch != '\'' && ch != '"') {
        spaced.push_back(ch);
      }
    }
    auto set = upper_words(spaced);
    std::size_t at = set.size() > 1 && set[1] == "SESSION" ? 2 : 1;
    if (set.size() == at + 3 && set[at] == "CLIENT_ENCODING" && (set[at + 1] == "TO" || set[at + 1] == "=")) {
      auto encoding = client_encoding(set[at + 2]);
      if (!encoding) unsupported_encoding(set[at + 2]);
      return {Classified::Kind::Control, Control::SetEncoding, {}, std::string{*encoding}};
    }
  }
  if (w == "DECLARE" || w == "FETCH" || w == "MOVE" || w == "CLOSE") {
    out.kind = Classified::Kind::Cursor;
    out.cursor = ast::parse_cursor(sql);
    return out;
  }
  for (const auto* word : {"SET", "SHOW", "RESET", "PREPARE", "EXECUTE", "DEALLOCATE", "SAVEPOINT", "RELEASE", "COPY",
                           "DISCARD", "LISTEN", "UNLISTEN", "NOTIFY", "LOCK"}) {
    if (w == word) throw SessionError(w + " is not supported", "0A000");
  }
  out.kind = Classified::Kind::Sql;
  out.sql = std::string{sql};
  return out;
}

auto command_name(Command c) -> std::string_view {
  switch (c) {
    case Command::Select: return "SELECT";
    case Command::Insert: return "INSERT";
    case Command::Update: return "UPDATE";
    case Command::Delete: return "DELETE";
    case Command::CreateTable: return "CREATE TABLE";
    case Command::CreateIndex: return "CREATE INDEX";
    case Command::DropTable: return "DROP TABLE";
    case Command::DropIndex: return "DROP INDEX";
    case Command::AlterTable: return "ALTER TABLE";
    case Command::CreateView: return "CREATE VIEW";
    case Command::DropView: return "DROP VIEW";
  }
  return {};
}

auto command_tag(Command c, const Rows& r) -> std::string {
  switch (c) {
    case Command::Select: return "SELECT " + std::to_string(r.rows.size());
    case Command::Insert: return "INSERT 0 " + std::to_string(r.affected);
    case Command::Update: return "UPDATE " + std::to_string(r.affected);
    case Command::Delete: return "DELETE " + std::to_string(r.affected);
    case Command::CreateTable: return "CREATE TABLE";
    case Command::CreateIndex: return "CREATE INDEX";
    case Command::DropTable: return "DROP TABLE";
    case Command::DropIndex: return "DROP INDEX";
    case Command::AlterTable: return "ALTER TABLE";
    case Command::CreateView: return "CREATE VIEW";
    case Command::DropView: return "DROP VIEW";
  }
  return {};
}

// A byte offset into `text` as PostgreSQL's position: one-based, in
// characters (UTF-8 lead bytes).
auto char_position(std::string_view text, std::int32_t byte) -> std::int32_t {
  std::int32_t n = 1;
  for (std::size_t i = 0; i < text.size() && std::cmp_less(i, byte); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++n;
  }
  return n;
}

struct Prepared {
  Classified::Kind kind{Classified::Kind::Empty};
  Control control{Control::Begin};
  std::string encoding;            // SetEncoding
  TransactionModes modes;          // SetTransaction
  const Statement* stmt{nullptr};  // Sql, and a DECLARE's query: from Database::prepare, valid until close
  std::optional<ast::CursorStatement> cursor;  // Cursor
};

// A session's cursor, and whether DECLARE said NO SCROLL. A hold cursor
// (WITH HOLD, or any DECLARE outside a block) outlives the transaction it was
// declared in; the others are the transaction's.
struct SessionCursor {
  Cursor cursor;
  bool no_scroll{false};
  bool hold{false};
};

// What a cursor statement answers: rows (FETCH) and the command tag.
struct CursorReply {
  Rows rows;
  bool returns_rows{false};
  std::string tag;
};

struct Portal {
  Prepared prepared;
  std::vector<Value> params;
};

class Session {
public:
  Session(Database& db, int fd, const SessionConfig& cfg, const std::atomic<bool>& stopping)
      : db_{db}, fd_{fd}, cfg_{cfg}, stopping_{stopping} {}
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() { close_cursors(); }

  void run() {
    try {
      if (!startup()) return;
      loop();
    } catch (const ConnectionGone&) {  // NOLINT(bugprone-empty-catch): nothing to send it to
    }
  }

private:
  // ------------------------------------------------------------ I/O

  // More bytes into in_; false at the end of the stream or on an error.
  auto fill() -> bool {
    char buf[16 << 10];
    for (;;) {
      auto n = ::read(fd_, buf, sizeof buf);
      if (n > 0) {
        in_.append(buf, static_cast<std::size_t>(n));
        return true;
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
  }

  auto read_exact(std::size_t n, std::string& out) -> bool {
    while (in_.size() - pos_ < n) {
      if (pos_ > 0) {
        in_.erase(0, pos_);
        pos_ = 0;
      }
      if (!fill()) return false;
    }
    out.assign(in_, pos_, n);
    pos_ += n;
    return true;
  }

  auto read_length(std::uint32_t& len) -> bool {
    std::string b;
    if (!read_exact(4, b)) return false;
    Body body{b};
    len = static_cast<std::uint32_t>(body.i32());
    if (len < 4 || len > kMaxMessage) {
      fatal("08P01", "invalid message length");
      return false;
    }
    return true;
  }

  // The next message, from the one held by peek_type() first; nullopt at the
  // end of the stream, and from then on: after a FATAL the bytes left are
  // never read as messages.
  auto next() -> std::optional<Message> {
    if (pending_) return std::exchange(pending_, std::nullopt);
    if (ended_) return std::nullopt;
    std::string type;
    std::uint32_t len = 0;
    Message m{'\0', {}};
    if (!read_exact(1, type) || !read_length(len) || !read_exact(len - 4, m.body)) {
      ended_ = true;
      return std::nullopt;
    }
    m.type = type[0];
    return m;
  }

  // The type of the message after this one, read and held for next(); 0 at
  // the end of the stream.
  auto peek_type() -> char {
    if (!pending_) pending_ = next();
    return pending_ ? pending_->type : '\0';
  }

  // Whether the next message's first byte is here, without waiting for it.
  auto arrived() -> bool {
    if (in_.size() > pos_) return true;
    pollfd p{fd_, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0;  // readable or at its end: next() tells which
  }

  // The stream ended: a stop says 57P01, unless a FATAL was already sent.
  void stream_ended() {
    if (stopping_.load() && !fatal_sent_) fatal("57P01", "terminating connection due to administrator command");
  }

  // Writes the buffer. It waits for a client as long as it reads, and,
  // once a stop has begun, kStopSendTimeout for one that takes nothing: a
  // send blocked in the kernel would not see the stop, so it never blocks.
  void flush() {
    auto data = out_.data();
    std::size_t done = 0;
    auto progress = std::chrono::steady_clock::now();
    while (done < data.size()) {
      auto n = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
        progress = std::chrono::steady_clock::now();
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) throw ConnectionGone{};
      if (stopping_.load() && std::chrono::steady_clock::now() - progress > kStopSendTimeout) throw ConnectionGone{};
      pollfd p{fd_, POLLOUT, 0};
      (void)::poll(&p, 1, 1'000);
    }
    out_.clear();
  }

  // ------------------------------------------------------------ replies

  void send_fields(char type, const char* severity, std::string_view state, std::string_view message,
                   std::int32_t position) {
    out_.begin(type);
    out_.raw('S');
    out_.cstr(severity);
    out_.raw('V');
    out_.cstr(severity);
    out_.raw('C');
    out_.cstr(state);
    out_.raw('M');
    out_.cstr(message);
    if (position > 0) {
      out_.raw('P');
      out_.cstr(std::to_string(position));
    }
    out_.raw('\0');
    out_.end();
  }

  void error(std::string_view state, std::string_view message, std::int32_t position = 0) {
    send_fields('E', "ERROR", state, message, position);
  }
  void warning(std::string_view state, std::string_view message) { send_fields('N', "WARNING", state, message, 0); }

  // The session ends after it: nothing more is read or run.
  void fatal(std::string_view state, std::string_view message) {
    fatal_sent_ = true;
    ended_ = true;
    send_fields('E', "FATAL", state, message, 0);
    flush();
  }

  // The exception in flight as an ErrorResponse. `text` is the statement an
  // ast::SqlError's position is into.
  void report(std::string_view text) {
    try {
      throw;
    } catch (const SessionError& e) {
      error(e.sqlstate(), e.what());
    } catch (const TextError& e) {
      error(e.sqlstate(), e.what());
    } catch (const ProtocolError& e) {
      error("08P01", e.what());
    } catch (const ast::SqlError& e) {
      error(sqlstate(e), e.what(), e.pos() < 0 ? 0 : char_position(text, e.pos()));
    } catch (const std::exception& e) {
      error(sqlstate(e), e.what());
    }
  }

  void ready() {
    out_.begin('Z');
    out_.raw(status());
    out_.end();
    flush();
  }

  void row_description(const std::vector<ResultColumn>& cols) {
    out_.begin('T');
    out_.i16(static_cast<std::int16_t>(cols.size()));
    for (const auto& c : cols) {
      auto t = type_info(c.type);
      out_.cstr(c.name);
      out_.i32(0);
      out_.i16(0);
      out_.i32(t.oid);
      out_.i16(t.length);
      out_.i32(t.modifier);
      out_.i16(0);
    }
    out_.end();
  }

  void no_data() {
    out_.begin('n');
    out_.end();
  }

  void data_rows(const Rows& r) {
    for (const auto& row : r.rows) {
      out_.begin('D');
      out_.i16(static_cast<std::int16_t>(row.size()));
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (std::holds_alternative<std::monostate>(row[i])) {
          out_.i32(-1);
          continue;
        }
        auto text = to_text(row[i], r.types.at(i));
        out_.i32(static_cast<std::int32_t>(text.size()));
        out_.bytes(text);
      }
      out_.end();
      if (out_.size() > kFlushAt) flush();
    }
  }

  void complete(std::string_view tag) {
    out_.begin('C');
    out_.cstr(tag);
    out_.end();
  }

  void simple_reply(char type) {
    out_.begin(type);
    out_.end();
  }

  // ------------------------------------------------------------ startup

  // Startup and authentication, to the first ReadyForQuery; false if the
  // connection ends here.
  auto startup() -> bool {
    for (;;) {
      std::uint32_t len = 0;
      if (!read_length(len)) return false;
      if (len < 8 || len > 10'000) {
        fatal("08P01", "invalid length of startup packet");
        return false;
      }
      std::string b;
      if (!read_exact(len - 4, b)) return false;
      Body body{b};
      auto code = body.i32();
      if (code == kSslRequest || code == kGssEncRequest) {
        out_.raw('N');  // no encryption: the client goes on in plaintext on this connection
        flush();
        continue;
      }
      if (code == kCancelRequest) return false;  // there is no cancel: nothing to do
      if ((code >> 16) != 3) {
        fatal("0A000", "unsupported frontend protocol " + std::to_string(code >> 16) + "." +
                           std::to_string(code & 0xFFFF) + ": server supports 3.0");
        return false;
      }
      return parameters(body, code & 0xFFFF);
    }
  }

  auto parameters(Body& body, std::int32_t minor) -> bool {
    std::optional<std::string> user;
    std::optional<std::string> database;
    std::vector<std::string> unknown_options;
    for (;;) {
      auto name = body.cstr();
      if (name.empty()) break;
      auto value = body.cstr();
      if (name == "user") {
        user = std::string{value};
      } else if (name == "database") {
        database = std::string{value};
      } else if (name == "application_name") {
        application_name_ = std::string{value};
      } else if (name == "client_encoding") {
        auto encoding = client_encoding(value);
        if (!encoding) {
          fatal("22023", "client_encoding \"" + std::string{value} + "\" is not supported: the server speaks UTF8");
          return false;
        }
        encoding_ = std::string{*encoding};
      } else if (name == "options") {
        if (!value.empty()) {
          fatal("0A000", "startup options are not supported");
          return false;
        }
      } else if (name.starts_with("_pq_.")) {
        unknown_options.emplace_back(name);
      } else {
        fatal("42704", "unrecognized configuration parameter \"" + std::string{name} + "\"");
        return false;
      }
    }
    if (!user || user->empty()) {
      fatal("28000", "no PostgreSQL user name specified in startup packet");
      return false;
    }
    auto db_name = database && !database->empty() ? *database : *user;
    if (db_name != cfg_.database) {
      fatal("3D000", "database \"" + db_name + "\" does not exist");
      return false;
    }
    if (minor > 0 || !unknown_options.empty()) {
      out_.begin('v');
      out_.i32(kProtocol30 & 0xFFFF);
      out_.i32(static_cast<std::int32_t>(unknown_options.size()));
      for (const auto& o : unknown_options) out_.cstr(o);
      out_.end();
    }
    out_.begin('R');  // AuthenticationOk: trust, the MVP's only method
    out_.i32(0);
    out_.end();
    auto status = [&](std::string_view k, std::string_view v) {
      out_.begin('S');
      out_.cstr(k);
      out_.cstr(v);
      out_.end();
    };
    status("server_version", "17.0");
    status("server_encoding", "UTF8");
    status("client_encoding", encoding_);
    status("DateStyle", "ISO, MDY");
    status("IntervalStyle", "postgres");
    status("TimeZone", "UTC");
    status("integer_datetimes", "on");
    status("standard_conforming_strings", "on");
    status("is_superuser", "off");
    status("session_authorization", *user);
    status("application_name", application_name_);
    ready();
    return true;
  }

  // ------------------------------------------------------------ the block

  // I outside an explicit block, T inside one, E inside one that failed. An
  // implicit block reports I: Sync always returns there.
  [[nodiscard]] auto status() const -> char { return !in_block_ ? 'I' : failed_ ? 'E' : 'T'; }

  // An error inside a block: an explicit one fails (E) until COMMIT or
  // ROLLBACK, whatever raised it; an implicit one is rolled back.
  void fail_block() noexcept {
    if (in_block_) {
      if (txn_) txn_->rollback();
      failed_ = true;
    }
    if (implicit_) {
      implicit_->rollback();
      implicit_.reset();
    }
  }

  void refuse_if_failed() const {
    if (in_block_ && failed_) {
      throw SessionError("current transaction is aborted, commands ignored until end of transaction block", "25P02");
    }
  }

  // As PostgreSQL: in a failed block a statement that does not parse reports
  // that, and one that does is 25P02, before it is bound.
  void refuse_sql_if_failed(const std::string& sql) const {
    if (in_block_ && failed_) (void)ast::parse(sql);
    refuse_if_failed();
  }

  // A transaction ended: its portals go with it, as in PostgreSQL, and so do
  // its cursors but the hold cursors that its commit kept. A hold cursor its
  // rollback dropped is closed already: it is forgotten here or when next named.
  void end_transaction() noexcept {
    portals_.clear();
    std::erase_if(cursors_, [](auto& entry) {
      auto& c = entry.second;
      if (!c.hold) c.cursor.close();
      return !c.hold || !c.cursor.is_open();
    });
  }

  // Forgets the cursors a rollback or abort closed.
  void forget_closed_cursors() noexcept {
    std::erase_if(cursors_, [](const auto& entry) { return !entry.second.cursor.is_open(); });
  }

  // The session's end, by a disconnect, Terminate or the server's shutdown:
  // a hold cursor releases its snapshot with it.
  void close_cursors() noexcept {
    for (auto& [name, c] : cursors_) c.cursor.close();
    cursors_.clear();
  }

  // BEGIN, COMMIT, ROLLBACK, SET TRANSACTION or SET client_encoding; returns the command tag.
  // Ending a block drops every portal: a caller holds none across it.
  auto run_control(Control c, const std::string& encoding, TransactionModes modes) -> std::string {
    switch (c) {
      case Control::Begin:
        if (in_block_) {
          refuse_if_failed();
          warning("25001", "there is already a transaction in progress");
          return "BEGIN";
        }
        if (implicit_) {
          txn_ = std::move(implicit_);  // BEGIN inside an implicit block makes it explicit
          implicit_.reset();
        } else {
          txn_.emplace(db_.begin());
          start_modes();
        }
        in_block_ = true;
        failed_ = false;
        if (modes.read_only) read_only_ = *modes.read_only;
        return "BEGIN";
      case Control::Commit: {
        end_transaction();
        std::optional<Transaction> t;
        if (in_block_) {
          in_block_ = false;
          if (std::exchange(failed_, false)) {
            txn_.reset();
            return "ROLLBACK";  // COMMIT of a failed block rolls it back
          }
          t = std::move(txn_);
          txn_.reset();
        } else if (implicit_) {
          t = std::move(implicit_);
          implicit_.reset();
        } else {
          warning("25P01", "there is no transaction in progress");
          return "COMMIT";
        }
        return t->commit() == CommitOutcome::Committed ? "COMMIT" : "ROLLBACK";
      }
      case Control::SetTransaction:
        refuse_if_failed();
        if (!in_block_ && !implicit_) {
          warning("25P01", "SET TRANSACTION can only be used in transaction blocks");
          return "SET";
        }
        if (modes.isolation && block_used_) {
          throw SessionError("SET TRANSACTION ISOLATION LEVEL must be called before any query", "25001");
        }
        if (modes.read_only) {
          if (!*modes.read_only && read_only_ && block_used_) {
            throw SessionError("transaction read-write mode must be set before any query", "25001");
          }
          read_only_ = *modes.read_only;
        }
        return "SET";
      case Control::SetEncoding:
        refuse_if_failed();
        encoding_ = encoding;
        out_.begin('S');
        out_.cstr("client_encoding");
        out_.cstr(encoding_);
        out_.end();
        return "SET";
      case Control::Rollback:
        end_transaction();
        if (in_block_) {
          if (txn_) txn_->rollback();
          txn_.reset();
          in_block_ = false;
          failed_ = false;
        } else if (implicit_) {
          implicit_->rollback();
          implicit_.reset();
        } else {
          warning("25P01", "there is no transaction in progress");
        }
        return "ROLLBACK";
    }
    return {};
  }

  // ------------------------------------------------------------ cursors

  // What a DECLARE refuses before its query is read, in PostgreSQL's order: a
  // failed block, then a statement outside a block.
  void check_declare(const ast::DeclareCursor& d) const {
    refuse_if_failed();
    // Outside a block it is a hold cursor (a known difference: PostgreSQL says 25P01).
    if (d.hold && d.for_update) {
      throw SessionError("DECLARE CURSOR WITH HOLD ... FOR UPDATE is not supported", "0A000");
    }
    if (d.scroll == ast::DeclareCursor::Scroll::Scroll && d.for_update) {
      throw SessionError("DECLARE SCROLL CURSOR ... FOR UPDATE is not supported", "0A000");
    }
    if (d.binary) throw SessionError("DECLARE BINARY CURSOR is not supported: binary results are not", "0A000");
  }

  // The statement a cursor statement prepares: a DECLARE's query, else none.
  auto prepare_cursor(const ast::CursorStatement& c) -> const Statement* {
    const auto* d = std::get_if<ast::DeclareCursor>(&c);
    if (d == nullptr) return nullptr;
    check_declare(*d);
    const auto& stmt = db_.prepare(d->query);
    if (stmt.command() != Command::Select) throw SessionError("a cursor needs a SELECT", "42601");
    return &stmt;
  }

  auto find_cursor(const std::string& name) -> SessionCursor& {
    forget_closed_cursors();
    auto it = cursors_.find(name);
    if (it == cursors_.end()) throw SessionError("cursor \"" + name + "\" does not exist", "34000");
    return it->second;
  }

  // The cursor a DECLARE opens: in the block's transaction, or, outside one,
  // in a transaction of its own that commits at once. Outside a block it is
  // always a hold cursor, taken on the snapshot of that moment.
  auto declare_cursor(const ast::DeclareCursor& d, const Statement& stmt, const std::vector<Value>& params) -> Cursor {
    auto kind = d.hold || !in_block_ ? Holdability::Hold : Holdability::Plain;
    if (in_block_) return txn_->open_cursor(stmt, params, kind);
    if (implicit_) return implicit_->open_cursor(stmt, params, kind);
    auto t = db_.begin();
    auto cursor = t.open_cursor(stmt, params, kind);
    (void)t.commit();
    return cursor;
  }

  // The rows a FETCH of `f` takes from `c`: how many, for a forward fetch.
  // Backward and absolute fetches are phase 5 (docs/cursor_design.md).
  static auto forward_count(const ast::FetchCursor& f, const SessionCursor& c) -> std::uint64_t {
    using Direction = ast::FetchCursor::Direction;
    auto verb = std::string{f.move ? "MOVE" : "FETCH"};
    auto backward = f.direction == Direction::Backward || (f.direction == Direction::Forward && f.count < 0);
    if (backward && c.no_scroll) throw SessionError("cursor can only scan forward", "55000");
    if (f.direction == Direction::Absolute || f.direction == Direction::Relative || backward ||
        (f.direction == Direction::Forward && f.count == 0)) {
      throw SessionError(verb + " in this direction is not supported yet (phase 5 of docs/cursor_design.md); only NEXT, a count, FORWARD n and ALL are",
                         "0A000");
    }
    return static_cast<std::uint64_t>(f.count);
  }

  // DECLARE, FETCH, MOVE or CLOSE inside the block. A failure here fails the
  // block through the caller, which closes the cursors with the transaction.
  auto run_cursor(const ast::CursorStatement& c, const Statement* stmt, const std::vector<Value>& params)
      -> CursorReply {
    refuse_if_failed();
    CursorReply out;
    if (const auto* d = std::get_if<ast::DeclareCursor>(&c)) {
      check_declare(*d);
      forget_closed_cursors();
      if (cursors_.contains(d->name) || portals_.contains(d->name)) {
        throw SessionError("cursor \"" + d->name + "\" already exists", "42P03");
      }
      enter_statement(*stmt);
      cursors_.emplace(d->name, SessionCursor{declare_cursor(*d, *stmt, params),
                                              d->scroll == ast::DeclareCursor::Scroll::NoScroll,
                                              d->hold || !in_block_});
      out.tag = "DECLARE CURSOR";
    } else if (const auto* f = std::get_if<ast::FetchCursor>(&c)) {
      auto& cursor = find_cursor(f->name);
      auto k = forward_count(*f, cursor);
      out.rows = cursor.cursor.fetch(k);
      out.returns_rows = !f->move;
      out.tag = (f->move ? "MOVE " : "FETCH ") + std::to_string(out.rows.rows.size());
      if (f->move) out.rows.rows.clear();
    } else {
      const auto& x = std::get<ast::CloseCursor>(c);
      if (x.all) {
        for (auto& [name, sc] : cursors_) sc.cursor.close();
        cursors_.clear();
        out.tag = "CLOSE CURSOR ALL";
      } else {
        auto& cursor = find_cursor(x.name);
        cursor.cursor.close();
        cursors_.erase(x.name);
        out.tag = "CLOSE CURSOR";
      }
    }
    return out;
  }

  // The columns a FETCH of `name` would describe; none if there is no such cursor yet.
  auto cursor_columns(const std::string& name) -> std::optional<std::vector<ResultColumn>> {
    auto it = cursors_.find(name);
    if (it == cursors_.end() || !it->second.cursor.is_open()) return std::nullopt;
    return result_columns(it->second.cursor.fetch(0));
  }

  static auto result_columns(const Rows& rows) -> std::vector<ResultColumn> {
    std::vector<ResultColumn> cols;
    for (std::size_t i = 0; i < rows.columns.size(); ++i) cols.push_back({rows.columns[i], rows.types.at(i)});
    return cols;
  }

  // ------------------------------------------------------------ messages

  void loop() {
    for (;;) {
      auto m = next();
      if (!m) {
        stream_ended();
        return;
      }
      if (m->type == 'X') return;
      if (skip_to_sync_ && m->type != 'S') continue;
      Body body{m->body};
      switch (m->type) {
        case 'Q': simple(body); break;
        case 'P':
        case 'B':
        case 'D':
        case 'E':
        case 'C':
          try {
            extended(m->type, body);
          } catch (const ConnectionGone&) {
            throw;
          } catch (...) {
            report(last_text_);
            fail_block();
            skip_to_sync_ = true;
          }
          break;
        case 'S': sync(); break;
        case 'H': flush(); break;
        case 'F':
          error("0A000", "the function call protocol is not supported");
          skip_to_sync_ = true;
          break;
        default:
          fatal("08P01", std::string{"invalid frontend message type "} + std::to_string(m->type));
          return;
      }
    }
  }

  // A Query inside an extended block, before its Sync, joins the block's
  // implicit transaction, which commits when the Query ends, as in
  // PostgreSQL.
  void simple(Body& body) {
    auto text = std::string{body.cstr()};
    try {
      last_text_ = text;  // a cursor statement's parse error is a position into it
      auto c = classify(text, "0A000", "several statements in one query are not supported");
      switch (c.kind) {
        case Classified::Kind::Empty: simple_reply('I'); break;
        case Classified::Kind::Cursor: {
          const auto* stmt = prepare_cursor(*c.cursor);
          if (stmt != nullptr && !stmt->params().empty()) throw SessionError("there is no parameter $1", "42P02");
          auto reply = run_cursor(*c.cursor, stmt, {});
          if (reply.returns_rows) {
            row_description(result_columns(reply.rows));
            data_rows(reply.rows);
          }
          complete(reply.tag);
          break;
        }
        case Classified::Kind::Control: complete(run_control(c.control, c.encoding, c.modes)); break;
        case Classified::Kind::Sql: {
          last_text_ = c.sql;
          refuse_sql_if_failed(c.sql);
          const auto& stmt = db_.prepare(c.sql);
          if (!stmt.params().empty()) throw SessionError("there is no parameter $1", "42P02");
          if (implicit_ && stmt.is_ddl()) refuse_ddl_beside_others();
          enter_statement(stmt);
          auto rows = in_block_   ? txn_->execute(stmt, {})
                      : implicit_ ? implicit_->execute(stmt, {})
                                  : db_.execute(stmt, {});
          if (stmt.command() == Command::Select) {
            std::vector<ResultColumn> cols;
            for (std::size_t i = 0; i < rows.columns.size(); ++i) cols.push_back({rows.columns[i], rows.types.at(i)});
            row_description(cols);
            data_rows(rows);
          }
          complete(command_tag(stmt.command(), rows));
          break;
        }
      }
      if (implicit_) {
        auto t = std::move(*implicit_);
        implicit_.reset();
        (void)t.commit();
      }
    } catch (const ConnectionGone&) {
      throw;
    } catch (...) {
      report(last_text_);
      fail_block();
    }
    if (!in_block_) end_transaction();
    last_text_.clear();
    ready();
  }

  void sync() {
    skip_to_sync_ = false;
    if (implicit_) {
      // The block's statements commit together at Sync.
      auto t = std::move(*implicit_);
      implicit_.reset();
      try {
        (void)t.commit();
      } catch (...) {
        report({});
      }
    }
    if (!in_block_) end_transaction();  // outside BEGIN, Sync ends the block's transaction
    ready();
  }

  void extended(char type, Body& body) {
    switch (type) {
      case 'P': parse(body); break;
      case 'B': bind(body); break;
      case 'D': describe(body); break;
      case 'E': execute(body); break;
      case 'C': close(body); break;
      default: break;
    }
  }

  void parse(Body& body) {
    auto name = std::string{body.cstr()};
    auto text = std::string{body.cstr()};
    auto n = body.i16();
    for (std::int16_t i = 0; i < n; ++i) (void)body.i32();  // the client's types: not used (docs/server_design.md)
    if (!name.empty() && statements_.contains(name)) {
      throw SessionError("prepared statement \"" + name + "\" already exists", "42P05");
    }
    last_text_ = text;
    auto c = classify(text, "42601", "cannot insert multiple commands into a prepared statement");
    Prepared p{c.kind, c.control, c.encoding, c.modes, nullptr, std::move(c.cursor)};
    if (c.kind == Classified::Kind::Cursor) {
      refuse_if_failed();
      p.stmt = prepare_cursor(*p.cursor);
      if (p.stmt != nullptr) db_.refit(*p.stmt);
    }
    if (c.kind == Classified::Kind::Sql) {
      last_text_ = c.sql;
      refuse_sql_if_failed(c.sql);
      p.stmt = &db_.prepare(c.sql);
      db_.refit(*p.stmt);  // a cached plan a DDL since made stale describes the old types
    }
    statements_[name] = p;
    simple_reply('1');
  }

  auto find_statement(const std::string& name) -> const Prepared& {
    auto it = statements_.find(name);
    if (it == statements_.end()) throw SessionError("prepared statement \"" + name + "\" does not exist", "26000");
    return it->second;
  }

  auto find_portal(const std::string& name) -> const Portal& {
    auto it = portals_.find(name);
    if (it == portals_.end()) throw SessionError("portal \"" + name + "\" does not exist", "34000");
    return it->second;
  }

  void bind(Body& body) {
    auto portal = std::string{body.cstr()};
    auto statement = std::string{body.cstr()};
    const auto& p = find_statement(statement);
    auto formats = body.i16();
    for (std::int16_t i = 0; i < formats; ++i) {
      if (body.i16() != 0) throw SessionError("binary format is not supported", "0A000");
    }
    if (p.stmt != nullptr) db_.refit(*p.stmt);  // a DDL since Parse
    auto infos = p.stmt != nullptr ? p.stmt->params() : std::vector<ParamInfo>{};
    auto n = body.i16();
    if (n < 0 || static_cast<std::size_t>(n) != infos.size()) {
      throw SessionError("bind message supplies " + std::to_string(n) + " parameters, but prepared statement \"" +
                             statement + "\" requires " + std::to_string(infos.size()),
                         "08P01");
    }
    if (p.kind == Classified::Kind::Sql || p.kind == Classified::Kind::Cursor) refuse_if_failed();
    Portal out{p, {}};
    for (std::int16_t i = 0; i < n; ++i) {
      auto len = body.i32();
      if (len < 0) {
        out.params.emplace_back(std::monostate{});
        continue;
      }
      out.params.push_back(from_text(body.bytes(static_cast<std::size_t>(len)), infos[static_cast<std::size_t>(i)]));
    }
    auto results = body.i16();
    for (std::int16_t i = 0; i < results; ++i) {
      if (body.i16() != 0) throw SessionError("binary format is not supported", "0A000");
    }
    forget_closed_cursors();
    if (!portal.empty() && (portals_.contains(portal) || cursors_.contains(portal))) {
      throw SessionError("portal \"" + portal + "\" already exists", "42P03");
    }
    portals_[portal] = std::move(out);
    simple_reply('2');
  }

  void describe_result(const Prepared& p) {
    if (p.kind == Classified::Kind::Cursor) {
      // A FETCH describes its cursor's columns once the cursor exists; a MOVE, DECLARE and CLOSE return no rows.
      const auto* f = std::get_if<ast::FetchCursor>(&*p.cursor);
      auto cols = f != nullptr && !f->move ? cursor_columns(f->name) : std::nullopt;
      if (cols) {
        row_description(*cols);
      } else {
        no_data();
      }
      return;
    }
    if (p.stmt != nullptr && p.stmt->command() == Command::Select) {
      row_description(p.stmt->columns());
    } else {
      no_data();
    }
  }

  void describe(Body& body) {
    auto what = body.byte();
    auto name = std::string{body.cstr()};
    if (what == 'S') {
      const auto& p = find_statement(name);
      if (p.stmt != nullptr) db_.refit(*p.stmt);
      auto infos = p.stmt != nullptr ? p.stmt->params() : std::vector<ParamInfo>{};
      out_.begin('t');
      out_.i16(static_cast<std::int16_t>(infos.size()));
      for (const auto& i : infos) out_.i32(type_info(i.type).oid);
      out_.end();
      describe_result(p);
    } else if (what == 'P') {
      describe_result(find_portal(name).prepared);
    } else {
      throw SessionError("invalid DESCRIBE message subtype", "08P01");
    }
  }

  void execute(Body& body) {
    auto name = std::string{body.cstr()};
    if (body.i32() > 0) throw SessionError("Execute with a row limit is not supported", "0A000");
    const auto& portal = find_portal(name);
    const auto& p = portal.prepared;
    if (p.kind == Classified::Kind::Empty) {
      simple_reply('I');
      return;
    }
    if (p.kind == Classified::Kind::Control) {
      auto [control, encoding, modes] = std::tuple{p.control, p.encoding, p.modes};  // run_control may drop the portal
      complete(run_control(control, encoding, modes));
      return;
    }
    if (p.kind == Classified::Kind::Cursor) {
      last_text_ = p.stmt != nullptr ? p.stmt->sql() : std::string{};
      auto reply = run_cursor(*p.cursor, p.stmt, portal.params);
      if (reply.returns_rows) data_rows(reply.rows);
      complete(reply.tag);
      return;
    }
    last_text_ = p.stmt->sql();
    refuse_if_failed();
    Rows rows;
    enter_statement(*p.stmt);
    if (in_block_) {
      rows = txn_->execute(*p.stmt, portal.params);
    } else if (implicit_) {
      rows = implicit_->execute(*p.stmt, portal.params);
    } else if (auto flush_after = false; next_is_sync(flush_after)) {
      // The block's one statement, as PQexecParams sends it: autocommit,
      // with its conflict retry, as a simple Query runs it.
      rows = db_.execute(*p.stmt, portal.params);
    } else {
      // Two or more: PostgreSQL's implicit transaction, committed at Sync.
      if (p.stmt->is_ddl()) refuse_ddl_beside_others();
      implicit_.emplace(db_.begin());
      start_modes();
      block_used_ = true;
      rows = implicit_->execute(*p.stmt, portal.params);
      reply(*p.stmt, rows);
      if (flush_after) flush();
      return;
    }
    reply(*p.stmt, rows);
  }

  void reply(const Statement& stmt, const Rows& rows) {
    data_rows(rows);
    complete(command_tag(stmt.command(), rows));
  }

  // A new transaction starts with PostgreSQL's defaults: read write, no query yet.
  void start_modes() {
    read_only_ = false;
    block_used_ = false;
  }

  // A statement about to run in a block: a read-only one takes only a SELECT
  // (25006), as PostgreSQL does; SELECT … FOR UPDATE is not told apart from it.
  void enter_statement(const Statement& stmt) {
    if (!in_block_ && !implicit_) return;
    if (read_only_ && stmt.command() != Command::Select) {
      throw SessionError("cannot execute " + std::string{command_name(stmt.command())} + " in a read-only transaction",
                         "25006");
    }
    block_used_ = true;
  }

  [[noreturn]] static void refuse_ddl_beside_others() {
    throw SessionError("DDL in a block of several statements is not supported: send it alone", "0A000");
  }

  // Whether the message after this Execute, past any Flush, is Sync. A Flush
  // is looked past only when the message after it is already here: a client
  // that sent Flush may be waiting for this Execute's reply, which is then
  // sent at once (`flush_after`), and the statement runs in the implicit
  // transaction. The stream's end runs nothing: no Sync would commit it.
  auto next_is_sync(bool& flush_after) -> bool {
    for (;;) {
      auto t = peek_type();
      if (t == '\0') {
        stream_ended();
        throw ConnectionGone{};
      }
      if (t != 'H') return t == 'S';
      if (!arrived()) {
        flush_after = true;
        return false;
      }
      pending_.reset();  // this Flush: Sync, or the flush after the reply, covers it
      flush_after = true;
    }
  }

  void close(Body& body) {
    auto what = body.byte();
    auto name = std::string{body.cstr()};
    if (what == 'S') {
      statements_.erase(name);
    } else if (what == 'P') {
      portals_.erase(name);
    } else {
      throw SessionError("invalid CLOSE message subtype", "08P01");
    }
    simple_reply('3');
  }

  Database& db_;
  int fd_;
  const SessionConfig& cfg_;
  const std::atomic<bool>& stopping_;
  std::string in_;
  std::size_t pos_{0};
  std::optional<Message> pending_;
  Out out_;
  std::string application_name_;
  std::string encoding_{"UTF8"};  // client_encoding, as ParameterStatus reports it
  std::string last_text_;  // the statement an error's position is into
  std::map<std::string, Prepared> statements_;
  std::map<std::string, Portal> portals_;
  std::map<std::string, SessionCursor> cursors_;  // the block's cursors; they share the portals' namespace
  bool read_only_{false};   // SET TRANSACTION READ ONLY, for the block that is open
  bool block_used_{false};  // the open block has run a statement
  std::optional<Transaction> txn_;       // the explicit block's, from BEGIN
  std::optional<Transaction> implicit_;  // an extended block of several statements, outside BEGIN
  bool in_block_{false};                 // inside BEGIN
  bool failed_{false};                   // the explicit block failed: E until COMMIT or ROLLBACK
  bool skip_to_sync_{false};
  bool ended_{false};       // the stream ended, or a FATAL was sent: nothing more is read
  bool fatal_sent_{false};
};

}  // namespace

void serve(Database& db, int fd, const SessionConfig& cfg, const std::atomic<bool>& stopping) {
  Session s{db, fd, cfg, stopping};
  s.run();
}

auto too_many_connections() -> std::string {
  Out out;
  out.begin('E');
  for (auto [code, value] : {std::pair{'S', "FATAL"}, std::pair{'V', "FATAL"}, std::pair{'C', "53300"},
                             std::pair{'M', "sorry, too many clients already"}}) {
    out.raw(code);
    out.cstr(value);
  }
  out.raw('\0');
  out.end();
  return std::string{out.data()};
}

}  // namespace bytecask::sql::pg
