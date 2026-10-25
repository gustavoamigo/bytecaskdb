// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// pg_session.cppm — PostgreSQL wire-protocol session state machine and worker
// pool (docs/server_design.md, docs/session_design.md).
//
// A Session is a plain data structure with no thread and no lock: it holds the
// startup parameters, the block status (I, T, E), the open Transaction handle,
// the prepared statements and portals, and the pending implicit block. A
// worker in Pool runs one session's queued messages up to the next
// ReadyForQuery, then puts the session behind the others.

module;
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

// SetEncoding is SET client_encoding, the one SET taken: libpq's
// PQsetClientEncoding sends it, and pgtcl's pg_connect calls that.
enum class Control { Begin, Commit, Rollback, SetEncoding };

// A statement's text as the session takes it: nothing, a transaction
// control word, or a statement for the layer.
struct Classified {
  enum class Kind { Empty, Control, Sql } kind{Kind::Empty};
  Control control{Control::Begin};
  std::string sql;       // Sql: the one statement, without its trailing ';'
  std::string encoding;  // SetEncoding: PostgreSQL's name for it
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
    return {Classified::Kind::Control, Control::Begin, {}, {}};
  }
  if (w == "START") {
    if (words.size() < 2 || words[1] != "TRANSACTION") throw SessionError("syntax error at or near \"START\"", "42601");
    if (words.size() > 2) throw SessionError("START TRANSACTION with options is not supported", "0A000");
    return {Classified::Kind::Control, Control::Begin, {}, {}};
  }
  if (w == "COMMIT" || w == "END") {
    only(1);
    return {Classified::Kind::Control, Control::Commit, {}, {}};
  }
  if (w == "ROLLBACK" || w == "ABORT") {
    only(1);
    return {Classified::Kind::Control, Control::Rollback, {}, {}};
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
  std::string encoding;            // SetEncoding
  const Statement* stmt{nullptr};  // Sql: from Database::prepare, valid until close
};

struct Portal {
  Prepared prepared;
  std::vector<Value> params;
};

}  // namespace

export class Session {
public:
  Session(Database& db, const SessionConfig& cfg, std::function<void(std::string)> on_flush = {})
      : db_{db}, cfg_{cfg}, on_flush_{std::move(on_flush)} {}

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() { close(); }

  void push(Input in) {
    if (ended_) return;
    waiting_for_lookahead_ = false;
    queue_.push_back(std::move(in));
  }

  [[nodiscard]] auto runnable() const noexcept -> bool {
    return !ended_ && !queue_.empty() && !waiting_for_lookahead_;
  }

  [[nodiscard]] auto ended() const noexcept -> bool { return ended_; }

  // Runs queued messages up to the next ReadyForQuery (Startup, Query or Sync),
  // or until the queue is empty, an Execute needs lookahead, or the session ends.
  void step() {
    while (!ended_ && !queue_.empty()) {
      auto in = std::move(queue_.front());
      queue_.pop_front();
      if (!started_) {
        switch (in.kind) {
          case Input::Kind::BadStartupLength:
            fatal("08P01", "invalid length of startup packet");
            return;
          case Input::Kind::EndOfStream:
            close();
            return;
          case Input::Kind::Startup:
            if (startup_packet(in.body)) {
              started_ = true;
              return;
            }
            if (ended_) return;
            break;
          case Input::Kind::Message:
          case Input::Kind::BadMessageLength:
            fatal("08P01", "invalid length of startup packet");
            return;
        }
        continue;
      }

      switch (in.kind) {
        case Input::Kind::BadMessageLength:
        case Input::Kind::BadStartupLength:
          fatal("08P01", "invalid message length");
          return;
        case Input::Kind::EndOfStream:
          close();
          return;
        case Input::Kind::Startup:
          fatal("08P01", "invalid frontend message type 0");
          return;
        case Input::Kind::Message:
          break;
      }

      if (in.type == 'X') {
        close();
        return;
      }
      if (skip_to_sync_ && in.type != 'S') continue;
      Body body{in.body};
      switch (in.type) {
        case 'Q':
          simple(body);
          return;
        case 'P':
        case 'B':
        case 'D':
        case 'C':
          try {
            extended(in.type, body);
          } catch (...) {
            report(last_text_);
            fail_block();
            skip_to_sync_ = true;
          }
          break;
        case 'E':
          try {
            if (!execute(body)) {
              queue_.push_front(std::move(in));
              waiting_for_lookahead_ = true;
              return;
            }
          } catch (...) {
            if (ended_) return;
            report(last_text_);
            fail_block();
            skip_to_sync_ = true;
          }
          break;
        case 'S':
          sync();
          return;
        case 'H':
          flush();
          break;
        case 'F':
          error("0A000", "the function call protocol is not supported");
          skip_to_sync_ = true;
          break;
        default:
          fatal("08P01", std::string{"invalid frontend message type "} + std::to_string(in.type));
          return;
      }
    }
  }

  // Ends the session on server shutdown: sends FATAL 57P01 unless a FATAL was
  // already sent, and rolls back any open transaction.
  void stop() {
    if (!fatal_sent_) fatal("57P01", "terminating connection due to administrator command");
    close();
  }

  void close() noexcept {
    if (txn_) txn_->rollback();
    txn_.reset();
    if (implicit_) implicit_->rollback();
    implicit_.reset();
    portals_.clear();
    queue_.clear();
    waiting_for_lookahead_ = false;
    ended_ = true;
  }

  [[nodiscard]] auto take_output() noexcept -> std::string { return out_.take(); }

private:
  void flush() {
    if (on_flush_ && out_.size() > 0) on_flush_(out_.take());
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

  // The session ends after it: nothing more is read or run, and any open
  // transaction is rolled back.
  void fatal(std::string_view state, std::string_view message) {
    fatal_sent_ = true;
    send_fields('E', "FATAL", state, message, 0);
    flush();
    close();
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

  // Handles one startup packet; true when startup reached ReadyForQuery.
  auto startup_packet(std::string_view raw) -> bool {
    try {
      Body body{raw};
      auto code = body.i32();
      if (code == kSslRequest || code == kGssEncRequest) {
        out_.raw('N');  // no encryption: the client goes on in plaintext on this connection
        flush();
        return false;
      }
      if (code == kCancelRequest) {
        close();  // there is no cancel yet (slice 2): nothing to do
        return false;
      }
      if ((code >> 16) != 3) {
        fatal("0A000", "unsupported frontend protocol " + std::to_string(code >> 16) + "." +
                           std::to_string(code & 0xFFFF) + ": server supports 3.0");
        return false;
      }
      return parameters(body, code & 0xFFFF);
    } catch (const ProtocolError& e) {
      fatal("08P01", e.what());
      return false;
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

  // A transaction ended: its portals go with it, as in PostgreSQL.
  void end_transaction() noexcept { portals_.clear(); }

  // BEGIN, COMMIT, ROLLBACK or SET client_encoding; returns the command tag.
  // Ending a block drops every portal: a caller holds none across it.
  auto run_control(Control c, const std::string& encoding) -> std::string {
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

  // ------------------------------------------------------------ messages

  // A Query inside an extended block, before its Sync, joins the block's
  // implicit transaction, which commits when the Query ends, as in
  // PostgreSQL.
  void simple(Body& body) {
    try {
      auto text = std::string{body.cstr()};
      auto c = classify(text, "0A000", "several statements in one query are not supported");
      switch (c.kind) {
        case Classified::Kind::Empty: simple_reply('I'); break;
        case Classified::Kind::Control: complete(run_control(c.control, c.encoding)); break;
        case Classified::Kind::Sql: {
          last_text_ = c.sql;
          refuse_sql_if_failed(c.sql);
          const auto& stmt = db_.prepare(c.sql);
          if (!stmt.params().empty()) throw SessionError("there is no parameter $1", "42P02");
          if (implicit_ && stmt.is_ddl()) refuse_ddl_beside_others();
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
      case 'C': close_msg(body); break;
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
    Prepared p{c.kind, c.control, c.encoding, nullptr};
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

  enum class Lookahead { Wait, Sync, Other, Ended };

  // Inspects the queued items after this Execute (docs/session_design.md,
  // *Running a session*). A Flush is consumed; if nothing is queued behind it
  // yet, the client may be waiting for this Execute's reply before sending
  // Sync, so the statement runs in the implicit transaction and flushes.
  auto lookahead(bool& flush_after) -> Lookahead {
    if (queue_.empty()) return Lookahead::Wait;
    while (!queue_.empty()) {
      const auto& next = queue_.front();
      if (next.kind == Input::Kind::BadMessageLength || next.kind == Input::Kind::BadStartupLength) {
        queue_.pop_front();
        fatal("08P01", "invalid message length");
        return Lookahead::Ended;
      }
      if (next.kind == Input::Kind::EndOfStream) {
        queue_.pop_front();
        close();
        return Lookahead::Ended;
      }
      if (next.kind != Input::Kind::Message) {
        queue_.pop_front();
        fatal("08P01", "invalid frontend message type 0");
        return Lookahead::Ended;
      }
      if (next.type != 'H') {
        return next.type == 'S' ? Lookahead::Sync : Lookahead::Other;
      }
      queue_.pop_front();
      flush_after = true;
      if (queue_.empty()) return Lookahead::Other;
    }
    return Lookahead::Other;
  }

  // Runs Execute; returns false only when no message after it has arrived yet
  // and lookahead is needed before choosing autocommit vs implicit transaction.
  auto execute(Body& body) -> bool {
    auto name = std::string{body.cstr()};
    if (body.i32() > 0) throw SessionError("Execute with a row limit is not supported", "0A000");
    const auto& portal = find_portal(name);
    const auto& p = portal.prepared;
    if (p.kind == Classified::Kind::Empty) {
      simple_reply('I');
      return true;
    }
    if (p.kind == Classified::Kind::Control) {
      auto [control, encoding] = std::pair{p.control, p.encoding};  // run_control may drop the portal
      complete(run_control(control, encoding));
      return true;
    }
    last_text_ = p.stmt->sql();
    refuse_if_failed();
    Rows rows;
    if (in_block_) {
      rows = txn_->execute(*p.stmt, portal.params);
    } else if (implicit_) {
      rows = implicit_->execute(*p.stmt, portal.params);
    } else {
      auto flush_after = false;
      switch (lookahead(flush_after)) {
        case Lookahead::Wait:
          return false;
        case Lookahead::Ended:
          return true;
        case Lookahead::Sync:
          // The block's one statement, as PQexecParams sends it: autocommit,
          // with its conflict retry, as a simple Query runs it.
          rows = db_.execute(*p.stmt, portal.params);
          break;
        case Lookahead::Other:
          // Two or more: PostgreSQL's implicit transaction, committed at Sync.
          if (p.stmt->is_ddl()) refuse_ddl_beside_others();
          implicit_.emplace(db_.begin());
          rows = implicit_->execute(*p.stmt, portal.params);
          reply(*p.stmt, rows);
          if (flush_after) flush();
          return true;
      }
    }
    reply(*p.stmt, rows);
    return true;
  }

  void reply(const Statement& stmt, const Rows& rows) {
    data_rows(rows);
    complete(command_tag(stmt.command(), rows));
  }

  [[noreturn]] static void refuse_ddl_beside_others() {
    throw SessionError("DDL in a block of several statements is not supported: send it alone", "0A000");
  }

  void close_msg(Body& body) {
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
  const SessionConfig& cfg_;
  std::function<void(std::string)> on_flush_;
  std::deque<Input> queue_;
  Out out_;
  std::string application_name_;
  std::string encoding_{"UTF8"};  // client_encoding, as ParameterStatus reports it
  std::string last_text_;         // the statement an error's position is into
  std::map<std::string, Prepared> statements_;
  std::map<std::string, Portal> portals_;
  std::optional<Transaction> txn_;       // the explicit block's, from BEGIN
  std::optional<Transaction> implicit_;  // an extended block of several statements, outside BEGIN
  bool in_block_{false};                 // inside BEGIN
  bool failed_{false};                   // the explicit block failed: E until COMMIT or ROLLBACK
  bool skip_to_sync_{false};
  bool started_{false};
  bool waiting_for_lookahead_{false};
  bool ended_{false};  // the stream ended, or a FATAL was sent: nothing more is read
  bool fatal_sent_{false};
};

// Fixed worker pool scheduling sessions one turn at a time
// (docs/session_design.md, *Running a session*). The pool's mutex is the only
// lock: taking a session from the queue gives the worker exclusive access to
// its Session.
export class Pool {
public:
  struct Slot {
    enum class State { Idle, Queued, Running };

    Slot(Database& db, const SessionConfig& cfg, Pool& pool)
        : session{db, cfg, [this, &pool](std::string bytes) { pool.flush_from_worker(*this, std::move(bytes)); }} {}

    Session session;  // owned by the running worker (State::Running), or idle
    State state{State::Idle};
    std::deque<Input> inbox;
    std::string outbox;
    bool stopping{false};
    bool dropped{false};
    bool session_ended{false};
    std::size_t turns{0};
    std::size_t worker_migrations{0};
    std::thread::id last_worker{};
  };

  struct Status {
    bool ended{false};
    bool idle{false};
    std::size_t turns{0};
    std::size_t worker_migrations{0};
  };

  Pool(Database& db, const SessionConfig& cfg, std::size_t workers, std::function<void()> on_wake = {})
      : db_{db}, cfg_{cfg}, on_wake_{std::move(on_wake)} {
    auto n = workers == 0 ? 1 : workers;
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  ~Pool() { shutdown(); }

  [[nodiscard]] auto open() -> std::shared_ptr<Slot> { return std::make_shared<Slot>(db_, cfg_, *this); }

  void submit(const std::shared_ptr<Slot>& slot, std::span<Input> inputs) {
    if (inputs.empty()) return;
    std::lock_guard<std::mutex> lock{mu_};
    if (slot->dropped || slot->session_ended) return;
    for (auto& in : inputs) slot->inbox.push_back(std::move(in));
    schedule(slot);
  }

  void submit(const std::shared_ptr<Slot>& slot, Input in) {
    submit(slot, std::span<Input>{&in, 1});
  }

  void stop(const std::shared_ptr<Slot>& slot) {
    std::lock_guard<std::mutex> lock{mu_};
    if (slot->dropped || slot->session_ended) return;
    slot->stopping = true;
    schedule(slot);
  }

  void drop(const std::shared_ptr<Slot>& slot) {
    std::lock_guard<std::mutex> lock{mu_};
    slot->dropped = true;
    slot->outbox.clear();
    if (!slot->session_ended) schedule(slot);
  }

  [[nodiscard]] auto drain(const std::shared_ptr<Slot>& slot, std::string& out) -> Status {
    std::lock_guard<std::mutex> lock{mu_};
    if (!slot->outbox.empty()) {
      if (out.empty()) {
        out = std::move(slot->outbox);
      } else {
        out.append(slot->outbox);
      }
      slot->outbox.clear();
    }
    return {slot->session_ended, slot->state == Slot::State::Idle, slot->turns, slot->worker_migrations};
  }

  void shutdown() noexcept {
    {
      std::lock_guard<std::mutex> lock{mu_};
      if (shutdown_threads_) return;
      shutdown_threads_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
  }

private:
  void schedule(const std::shared_ptr<Slot>& slot) {
    if (slot->state == Slot::State::Idle) {
      slot->state = Slot::State::Queued;
      queue_.push_back(slot);
      cv_.notify_one();
    }
  }

  void flush_from_worker(Slot& slot, std::string bytes) {
    if (bytes.empty()) return;
    {
      std::lock_guard<std::mutex> lock{mu_};
      if (!slot.dropped) slot.outbox.append(bytes);
    }
    wake();
  }

  void wake() {
    if (on_wake_) on_wake_();
  }

  void worker_loop() {
    auto tid = std::this_thread::get_id();
    for (;;) {
      std::shared_ptr<Slot> slot;
      auto dropped = false;
      auto stopping = false;
      {
        std::unique_lock<std::mutex> lock{mu_};
        cv_.wait(lock, [&] { return shutdown_threads_ || !queue_.empty(); });
        if (shutdown_threads_ && queue_.empty()) return;
        slot = std::move(queue_.front());
        queue_.pop_front();
        slot->state = Slot::State::Running;
        if (slot->turns > 0 && slot->last_worker != tid) ++slot->worker_migrations;
        slot->last_worker = tid;
        ++slot->turns;
        while (!slot->inbox.empty()) {
          slot->session.push(std::move(slot->inbox.front()));
          slot->inbox.pop_front();
        }
        dropped = slot->dropped;
        stopping = slot->stopping;
      }

      if (dropped) {
        slot->session.close();
      } else if (stopping) {
        slot->session.stop();
      } else {
        slot->session.step();
      }

      {
        std::unique_lock<std::mutex> lock{mu_};
        while (!slot->inbox.empty()) {
          slot->session.push(std::move(slot->inbox.front()));
          slot->inbox.pop_front();
        }
        if (slot->dropped && !slot->session.ended()) {
          lock.unlock();
          slot->session.close();
          lock.lock();
          slot->outbox.clear();
        } else if (slot->stopping && !slot->session.ended()) {
          lock.unlock();
          slot->session.stop();
          lock.lock();
        }
        auto rem = slot->session.take_output();
        if (!rem.empty() && !slot->dropped) slot->outbox.append(rem);
        slot->session_ended = slot->session.ended();
        if (!slot->dropped && !slot->session_ended && slot->session.runnable()) {
          slot->state = Slot::State::Queued;
          queue_.push_back(slot);
          cv_.notify_one();
        } else {
          slot->state = Slot::State::Idle;
        }
      }
      wake();
    }
  }

  Database& db_;
  const SessionConfig& cfg_;
  std::function<void()> on_wake_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Slot>> queue_;
  std::vector<std::thread> workers_;
  bool shutdown_threads_{false};
};

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
