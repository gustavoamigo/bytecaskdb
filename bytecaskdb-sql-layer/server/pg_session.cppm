// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// pg_session.cppm — one PostgreSQL wire-protocol connection, from startup to
// its end, over the embedded API (docs/server_design.md). A session runs on
// its connection's own thread and shares nothing with another but the
// Database. It blocks only on its socket and on the API's calls, which never
// wait for another caller.

module;
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
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
// a read: the connection is then sent FATAL 57P01. An open transaction is
// rolled back either way. Does not close `fd`.
void serve(Database& db, int fd, const SessionConfig& cfg, const std::atomic<bool>& stopping);

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

// SetEncoding is SET client_encoding to UTF8, the one SET taken: libpq's
// PQsetClientEncoding sends it, and pgtcl's pg_connect calls that.
enum class Control { Begin, Commit, Rollback, SetEncoding };

// A statement's text as the session takes it: nothing, a transaction
// control word, or a statement for the layer.
struct Classified {
  enum class Kind { Empty, Control, Sql } kind{Kind::Empty};
  Control control{Control::Begin};
  std::string sql;  // Sql: the one statement, without its trailing ';'
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
    only(1);
    return {Classified::Kind::Control, Control::Begin, {}};
  }
  if (w == "START") {
    if (words.size() < 2 || words[1] != "TRANSACTION") throw SessionError("syntax error at or near \"START\"", "42601");
    if (words.size() > 2) throw SessionError("START TRANSACTION with options is not supported", "0A000");
    return {Classified::Kind::Control, Control::Begin, {}};
  }
  if (w == "COMMIT" || w == "END") {
    only(1);
    return {Classified::Kind::Control, Control::Commit, {}};
  }
  if (w == "ROLLBACK" || w == "ABORT") {
    only(1);
    return {Classified::Kind::Control, Control::Rollback, {}};
  }
  if (w == "SET") {
    // SET [SESSION] client_encoding {TO | =} 'UTF8': the server speaks UTF8
    // and nothing else, so only that value is taken.
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
      std::string norm;
      for (auto ch : set[at + 2]) {
        if (ch != '-' && ch != '_') norm.push_back(ch);
      }
      if (norm != "UTF8") {
        throw SessionError("client_encoding \"" + set[at + 2] + "\" is not supported: the server speaks UTF8", "22023");
      }
      return {Classified::Kind::Control, Control::SetEncoding, {}};
    }
  }
  for (const auto* word : {"SET", "SHOW", "RESET", "PREPARE", "EXECUTE", "DEALLOCATE", "SAVEPOINT", "RELEASE", "COPY",
                           "DISCARD", "LISTEN", "UNLISTEN", "NOTIFY", "LOCK"}) {
    if (w == word) throw SessionError(w + " is not supported", "0A000");
  }
  out.kind = Classified::Kind::Sql;
  out.sql = std::string{sql};
  return out;
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
  const Statement* stmt{nullptr};  // Sql: from Database::prepare, valid until close
};

struct Portal {
  Prepared prepared;
  std::vector<Value> params;
};

class Session {
public:
  Session(Database& db, int fd, const SessionConfig& cfg, const std::atomic<bool>& stopping)
      : db_{db}, fd_{fd}, cfg_{cfg}, stopping_{stopping} {}

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
  // end of the stream.
  auto next() -> std::optional<Message> {
    if (pending_) return std::exchange(pending_, std::nullopt);
    std::string type;
    if (!read_exact(1, type)) return std::nullopt;
    std::uint32_t len = 0;
    if (!read_length(len)) return std::nullopt;
    Message m{type[0], {}};
    if (!read_exact(len - 4, m.body)) return std::nullopt;
    return m;
  }

  // The type of the message after this one, read and held for next(); 0 at
  // the end of the stream.
  auto peek_type() -> char {
    if (!pending_) pending_ = next();
    return pending_ ? pending_->type : '\0';
  }

  void flush() {
    auto data = out_.data();
    std::size_t done = 0;
    while (done < data.size()) {
      auto n = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw ConnectionGone{};
      done += static_cast<std::size_t>(n);
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

  void fatal(std::string_view state, std::string_view message) {
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
        std::string norm;
        for (auto c : value) {
          if (c != '-' && c != '_') norm.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (norm != "utf8") {
          fatal("22023", "client_encoding \"" + std::string{value} + "\" is not supported: the server speaks UTF8");
          return false;
        }
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
    status("client_encoding", "UTF8");
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

  // BEGIN, COMMIT or ROLLBACK; returns the command tag.
  auto run_control(Control c) -> std::string {
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
        }
        in_block_ = true;
        failed_ = false;
        return "BEGIN";
      case Control::Commit: {
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
      case Control::SetEncoding:
        refuse_if_failed();
        out_.begin('S');
        out_.cstr("client_encoding");
        out_.cstr("UTF8");
        out_.end();
        return "SET";
      case Control::Rollback:
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

  // ------------------------------------------------------------ messages

  void loop() {
    for (;;) {
      auto m = next();
      if (!m) {
        if (stopping_.load()) fatal("57P01", "terminating connection due to administrator command");
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

  void simple(Body& body) {
    auto text = std::string{body.cstr()};
    if (implicit_) {  // a Query inside an extended block, before its Sync
      implicit_->rollback();
      implicit_.reset();
    }
    try {
      auto c = classify(text, "0A000", "several statements in one query are not supported");
      switch (c.kind) {
        case Classified::Kind::Empty: simple_reply('I'); break;
        case Classified::Kind::Control: complete(run_control(c.control)); break;
        case Classified::Kind::Sql: {
          last_text_ = c.sql;
          refuse_sql_if_failed(c.sql);
          const auto& stmt = db_.prepare(c.sql);
          if (!stmt.params().empty()) throw SessionError("there is no parameter $1", "42P02");
          auto rows = in_block_ ? txn_->execute(stmt, {}) : db_.execute(stmt, {});
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
    } catch (const ConnectionGone&) {
      throw;
    } catch (...) {
      report(last_text_);
      fail_block();
    }
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
    Prepared p{c.kind, c.control, nullptr};
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
    if (p.kind == Classified::Kind::Sql) refuse_if_failed();
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
    if (!portal.empty() && portals_.contains(portal)) {
      throw SessionError("portal \"" + portal + "\" already exists", "42P03");
    }
    portals_[portal] = std::move(out);
    simple_reply('2');
  }

  void describe_result(const Prepared& p) {
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
      complete(run_control(p.control));
      return;
    }
    last_text_ = p.stmt->sql();
    refuse_if_failed();
    Rows rows;
    if (in_block_) {
      rows = txn_->execute(*p.stmt, portal.params);
    } else if (implicit_) {
      rows = implicit_->execute(*p.stmt, portal.params);
    } else if (peek_type() == 'S') {
      // The block's one statement, as PQexecParams sends it: autocommit,
      // with its conflict retry, as a simple Query runs it.
      rows = db_.execute(*p.stmt, portal.params);
    } else {
      // Two or more: PostgreSQL's implicit transaction, committed at Sync.
      if (p.stmt->is_ddl()) {
        throw SessionError("DDL in a block of several statements is not supported: send it alone", "0A000");
      }
      implicit_.emplace(db_.begin());
      rows = implicit_->execute(*p.stmt, portal.params);
    }
    data_rows(rows);
    complete(command_tag(p.stmt->command(), rows));
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
  std::string last_text_;  // the statement an error's position is into
  std::map<std::string, Prepared> statements_;
  std::map<std::string, Portal> portals_;
  std::optional<Transaction> txn_;       // the explicit block's, from BEGIN
  std::optional<Transaction> implicit_;  // an extended block of several statements, outside BEGIN
  bool in_block_{false};                 // inside BEGIN
  bool failed_{false};                   // the explicit block failed: E until COMMIT or ROLLBACK
  bool skip_to_sync_{false};
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
