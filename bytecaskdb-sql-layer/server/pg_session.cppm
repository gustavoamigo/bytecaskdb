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
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.pg_session;

import bytecask.sql.api;
import bytecask.sql.parser;
import bytecask.sql.pg_wire;
import bytecask.sql.pg_text;

export namespace bytecask::sql::pg {

// The timeouts that end an idle session (docs/session_design.md, *Timeouts*).
enum class Timeout : std::uint8_t { None, IdleInTransaction, Transaction, IdleSession };

// Every session's cancel key, as BackendKeyData gave it to its client: a
// CancelRequest naming one cancels that session's running statement. Shared
// by every session of a server; one mutex, held only to add, remove or cancel.
class CancelRegistry {
public:
  struct Key {
    std::int32_t pid;
    std::int32_t secret;
  };

  // Registers the flag under a process id no other session holds and a
  // random secret.
  auto add(CancelFlag& flag) -> Key {
    std::lock_guard<std::mutex> lock{mu_};
    for (;;) {
      auto pid = next_pid_;
      next_pid_ = next_pid_ == std::numeric_limits<std::int32_t>::max() ? 1 : next_pid_ + 1;
      if (entries_.contains(pid)) continue;
      auto secret = static_cast<std::int32_t>(random_());
      entries_.emplace(pid, Entry{secret, &flag});
      return {pid, secret};
    }
  }

  void remove(std::int32_t pid) noexcept {
    std::lock_guard<std::mutex> lock{mu_};
    entries_.erase(pid);
  }

  // Sets the flag of the session the key names; does nothing for a key no
  // session holds, or a wrong secret.
  void cancel(std::int32_t pid, std::int32_t secret) noexcept {
    std::lock_guard<std::mutex> lock{mu_};
    if (auto it = entries_.find(pid); it != entries_.end() && it->second.secret == secret) it->second.flag->cancel();
  }

private:
  struct Entry {
    std::int32_t secret;
    CancelFlag* flag;
  };
  std::mutex mu_;
  std::unordered_map<std::int32_t, Entry> entries_;
  std::int32_t next_pid_{1};
  std::random_device random_;
};

struct SessionConfig {
  std::string database;  // the one name a client may connect to
  // docs/session_design.md, *Timeouts*; 0 turns one off.
  std::chrono::milliseconds idle_in_transaction_timeout{60'000};  // FATAL 25P03
  std::chrono::milliseconds statement_timeout{0};                 // 57014
  std::chrono::milliseconds transaction_timeout{0};               // FATAL 25P04
  std::chrono::milliseconds idle_session_timeout{0};              // FATAL 57P05
  CancelRegistry* cancels{nullptr};  // null: no BackendKeyData, and a CancelRequest cancels nothing
  // docs/session_design.md, *Bounds*: past this many reply bytes the client
  // has not taken, the session runs no further statement until it takes
  // some. Checked between statements, so one statement's reply may pass it.
  // 0: no bound.
  std::size_t max_unsent_bytes{64u << 20};
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
  Session(Database& db, const SessionConfig& cfg, std::function<void(std::string_view)> on_flush = {})
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

  // Ends an idle session on the timeout its idle_deadline() named, as
  // PostgreSQL does: FATAL, its transaction rolled back.
  void expire(Timeout kind) {
    switch (kind) {
      case Timeout::IdleInTransaction:
        fatal("25P03", "terminating connection due to idle-in-transaction timeout");
        break;
      case Timeout::Transaction: fatal("25P04", "terminating connection due to transaction timeout"); break;
      case Timeout::IdleSession: fatal("57P05", "terminating connection due to idle-session timeout"); break;
      case Timeout::None: break;
    }
  }

  // When the session, idle from `now` on, is to be ended, and on which
  // timeout: inside a transaction (explicit, failed or implicit) the
  // earlier of idle-in-transaction and the transaction's own; outside one,
  // idle-session. Timeout::None while none applies.
  [[nodiscard]] auto idle_deadline(std::chrono::steady_clock::time_point now) const
      -> std::pair<std::chrono::steady_clock::time_point, Timeout> {
    if (!started_ || ended_) return {{}, Timeout::None};
    if (!in_block_ && !implicit_) {
      if (cfg_.idle_session_timeout.count() == 0) return {{}, Timeout::None};
      return {now + cfg_.idle_session_timeout, Timeout::IdleSession};
    }
    std::pair<std::chrono::steady_clock::time_point, Timeout> at{{}, Timeout::None};
    if (cfg_.idle_in_transaction_timeout.count() > 0) at = {now + cfg_.idle_in_transaction_timeout, Timeout::IdleInTransaction};
    if (cfg_.transaction_timeout.count() > 0) {
      auto t = txn_started_ + cfg_.transaction_timeout;
      if (at.second == Timeout::None || t < at.first) at = {t, Timeout::Transaction};
    }
    return at;
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
    if (key_pid_ != 0 && cfg_.cancels != nullptr) cfg_.cancels->remove(key_pid_);  // before cancel_ goes
    key_pid_ = 0;
  }

  [[nodiscard]] auto output() const noexcept -> std::string_view { return out_.data(); }
  void clear_output() noexcept { out_.clear(); }
  [[nodiscard]] auto take_output() noexcept -> std::string { return out_.take(); }

private:
  void flush() {
    if (on_flush_ && out_.size() > 0) {
      on_flush_(out_.data());
      out_.clear();
    }
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
      // A statement stopped by its transaction's deadline ends the session,
      // as PostgreSQL's transaction_timeout does; otherwise 57014 is a
      // cancel or the statement timeout, and the session goes on.
      if (sqlstate(e) == "57014" && txn_deadline_passed()) {
        fatal("25P04", "terminating connection due to transaction timeout");
        return;
      }
      error(sqlstate(e), e.what());
    }
  }

  // The options a statement runs with: the session's cancel flag, cleared
  // first, so a CancelRequest that arrived while it was idle cancels
  // nothing; and the earlier of the statement's and its transaction's
  // deadlines. A statement outside a block is its own transaction.
  auto run_options() -> ExecutionOptions {
    cancel_.reset();
    auto now = std::chrono::steady_clock::now();
    ExecutionOptions opts{.deadline = {}, .cancel = &cancel_};
    txn_deadline_ = {};
    if (cfg_.statement_timeout.count() > 0) opts.deadline = now + cfg_.statement_timeout;
    if (cfg_.transaction_timeout.count() > 0) {
      txn_deadline_ = (in_block_ || implicit_ ? txn_started_ : now) + cfg_.transaction_timeout;
      if (opts.deadline == std::chrono::steady_clock::time_point{} || txn_deadline_ < opts.deadline) {
        opts.deadline = txn_deadline_;
      }
    }
    return opts;
  }

  [[nodiscard]] auto txn_deadline_passed() const -> bool {
    return txn_deadline_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= txn_deadline_;
  }

  void ready() {
    out_.begin('Z');
    out_.raw(status());
    out_.end();
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
        // Its own connection, as the protocol sends it: no reply either way.
        auto pid = body.i32();
        auto secret = body.i32();
        if (cfg_.cancels != nullptr) cfg_.cancels->cancel(pid, secret);
        close();
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
    if (cfg_.cancels != nullptr) {
      auto key = cfg_.cancels->add(cancel_);
      key_pid_ = key.pid;
      out_.begin('K');  // BackendKeyData: what a CancelRequest names
      out_.i32(key.pid);
      out_.i32(key.secret);
      out_.end();
    }
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
          txn_started_ = std::chrono::steady_clock::now();
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
          auto opts = run_options();
          auto rows = in_block_   ? txn_->execute(stmt, {}, opts)
                      : implicit_ ? implicit_->execute(stmt, {}, opts)
                                  : db_.execute(stmt, {}, opts);
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
      if (ended_) return;  // a transaction timeout's FATAL: no ReadyForQuery after it
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
      rows = txn_->execute(*p.stmt, portal.params, run_options());
    } else if (implicit_) {
      rows = implicit_->execute(*p.stmt, portal.params, run_options());
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
          rows = db_.execute(*p.stmt, portal.params, run_options());
          break;
        case Lookahead::Other:
          // Two or more: PostgreSQL's implicit transaction, committed at Sync.
          if (p.stmt->is_ddl()) refuse_ddl_beside_others();
          implicit_.emplace(db_.begin());
          txn_started_ = std::chrono::steady_clock::now();
          rows = implicit_->execute(*p.stmt, portal.params, run_options());
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
  std::function<void(std::string_view)> on_flush_;
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
  CancelFlag cancel_;                                     // set by a CancelRequest naming key_pid_
  std::int32_t key_pid_{0};                               // in cfg_.cancels while nonzero
  std::chrono::steady_clock::time_point txn_started_{};  // of the open block, explicit or implicit
  std::chrono::steady_clock::time_point txn_deadline_{}; // the running statement's transaction's; epoch: none
};

// Fixed worker pool scheduling sessions one turn at a time
// (docs/session_design.md, *Running a session*). Each Slot has its own mutex
// for inbox/outbox handoff with the I/O loop, and each worker has its own
// queue so workers do not contend on a single global mutex. When a connection
// has no unsent backlog on the loop, the worker sends reply bytes directly
// with non-blocking MSG_DONTWAIT and only hands over to the loop on EAGAIN.
export class Pool {
public:
  static constexpr std::size_t kNoPreference = static_cast<std::size_t>(-1);

  struct Slot {
    enum class State { Idle, Queued, Running };

    Slot(Database& db, const SessionConfig& cfg, Pool& pool, int sock_fd, void* user_tag, std::size_t pref,
         std::function<void(void*)> wake_cb)
        : session{db, cfg, [this, &pool](std::string_view bytes) { pool.write_or_queue(*this, bytes); }},
          fd{sock_fd},
          tag{user_tag},
          preferred_worker{pref},
          on_wake{std::move(wake_cb)} {}

    Session session;  // owned by the running worker (State::Running), or idle
    Framer framer{};  // owned by the running worker (State::Running), or idle
    std::mutex mu;
    State state{State::Idle};
    std::string raw_in;
    std::deque<Input> inbox;
    std::string outbox;
    int fd{-1};
    void* tag{nullptr};
    std::size_t preferred_worker{kNoPreference};
    std::function<void(void*)> on_wake;
    bool loop_writing{false};
    // The reply bytes the loop holds, queued and in flight, as of its last
    // drain(); with outbox, what the client has not taken.
    std::size_t loop_unsent{0};
    // Runnable, but held back until the client takes enough of its reply
    // (SessionConfig::max_unsent_bytes); a drain() under the bound queues it.
    bool output_blocked{false};
    bool in_woken{false};
    bool stopping{false};
    bool dropped{false};
    bool session_ended{false};
    std::size_t turns{0};
    std::size_t worker_migrations{0};
    std::thread::id last_worker{};
    std::size_t running_on{kNoPreference};  // the worker running it (State::Running)
    Timeout expired{Timeout::None};          // set by expire(): the next turn ends the session on it
    // Set by the worker at the end of each turn, read by the loop through
    // drain(): when the session, if it stays idle, is to be ended, and why.
    std::chrono::steady_clock::time_point idle_at{};
    Timeout idle_kind{Timeout::None};
  };

  struct Status {
    bool ended{false};
    bool idle{false};
    std::size_t turns{0};
    std::size_t worker_migrations{0};
    std::chrono::steady_clock::time_point idle_at{};
    Timeout idle_kind{Timeout::None};
  };

  Pool(Database& db, const SessionConfig& cfg, std::size_t workers, std::function<void()> on_wake = {},
       std::span<const int> cpus = {})
      : db_{db}, cfg_{cfg}, on_wake_{std::move(on_wake)} {
    auto n = workers == 0 ? 1 : workers;
    wq_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      wq_.push_back(std::make_unique<WorkerQueue>());
      wq_.back()->wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (wq_.back()->wake_fd < 0) throw std::system_error{errno, std::generic_category(), "eventfd"};
    }
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      int cpu = cpus.empty() ? -1 : cpus[i % cpus.size()];
      workers_.emplace_back([this, i, cpu] {
        if (cpu >= 0) {
          cpu_set_t set;
          CPU_ZERO(&set);
          CPU_SET(static_cast<std::size_t>(cpu), &set);
          (void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
        }
        worker_loop(i);
      });
    }
  }

  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  ~Pool() { shutdown(); }

  [[nodiscard]] auto open(int fd = -1, void* tag = nullptr, std::size_t preferred_worker = kNoPreference,
                          std::function<void(void*)> wake_cb = {}) -> std::shared_ptr<Slot> {
    return std::make_shared<Slot>(db_, cfg_, *this, fd, tag, preferred_worker, std::move(wake_cb));
  }

  void detach(const std::shared_ptr<Slot>& slot) {
    auto running_on = kNoPreference;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      slot->fd = -1;
      slot->tag = nullptr;
      slot->in_woken = false;
      if (slot->state == Slot::State::Running) running_on = slot->running_on;
    }
    wake_parked(running_on);
  }
  void submit(const std::shared_ptr<Slot>& slot, std::span<Input> inputs) {
    if (inputs.empty()) return;
    auto need_schedule = false;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      if (slot->dropped || slot->session_ended) return;
      for (auto& in : inputs) slot->inbox.push_back(std::move(in));
      if (slot->state == Slot::State::Idle && !slot->output_blocked) {
        slot->state = Slot::State::Queued;
        need_schedule = true;
      }
    }
    if (need_schedule) enqueue_slot(slot);
  }

  void submit(const std::shared_ptr<Slot>& slot, Input in) {
    submit(slot, std::span<Input>{&in, 1});
  }

  void submit_bytes(const std::shared_ptr<Slot>& slot, std::string_view bytes) {
    if (bytes.empty()) return;
    auto need_schedule = false;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      if (slot->dropped || slot->session_ended) return;
      slot->raw_in.append(bytes);
      if (slot->state == Slot::State::Idle && !slot->output_blocked) {
        slot->state = Slot::State::Queued;
        need_schedule = true;
      }
    }
    if (need_schedule) enqueue_slot(slot);
  }

  void stop(const std::shared_ptr<Slot>& slot) {
    auto need_schedule = false;
    auto running_on = kNoPreference;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      if (slot->dropped || slot->session_ended) return;
      slot->stopping = true;
      if (slot->state == Slot::State::Idle) {
        slot->state = Slot::State::Queued;
        need_schedule = true;
      } else if (slot->state == Slot::State::Running) {
        running_on = slot->running_on;
      }
    }
    if (need_schedule) enqueue_slot(slot);
    wake_parked(running_on);
  }

  // The loop's timer for `kind` fired: ends the session on it, on a worker,
  // if it is still idle on that same timeout. False, doing nothing, if the
  // session ran or received bytes meanwhile; its next idle turn sets a new
  // deadline.
  auto expire(const std::shared_ptr<Slot>& slot, Timeout kind) -> bool {
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      if (kind == Timeout::None || slot->dropped || slot->session_ended || slot->stopping || slot->output_blocked) return false;
      if (slot->state != Slot::State::Idle || !slot->raw_in.empty() || !slot->inbox.empty()) return false;
      if (slot->idle_kind != kind) return false;
      slot->expired = kind;
      slot->state = Slot::State::Queued;
    }
    enqueue_slot(slot);
    return true;
  }

  void drop(const std::shared_ptr<Slot>& slot) {
    auto need_schedule = false;
    auto running_on = kNoPreference;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      slot->dropped = true;
      slot->outbox.clear();
      if (!slot->session_ended && slot->state == Slot::State::Idle) {
        slot->state = Slot::State::Queued;
        need_schedule = true;
      } else if (slot->state == Slot::State::Running) {
        running_on = slot->running_on;
      }
    }
    if (need_schedule) enqueue_slot(slot);
    wake_parked(running_on);
  }

  // The loop takes the reply bytes queued for it into `out`, and reports the
  // session's state. `in_flight` is the bytes of its write in progress: with
  // `out`, what it holds unsent. A session held back on its output is queued
  // once that is under the bound.
  [[nodiscard]] auto drain(const std::shared_ptr<Slot>& slot, std::string& out, std::size_t in_flight = 0) -> Status {
    Status st;
    auto resume = false;
    {
      std::lock_guard<std::mutex> lock{slot->mu};
      slot->in_woken = false;
      if (!slot->outbox.empty()) {
        if (out.empty()) {
          out = std::move(slot->outbox);
        } else {
          out.append(slot->outbox);
        }
        slot->outbox.clear();
        slot->loop_writing = true;
      } else if (out.empty() && in_flight == 0) {
        slot->loop_writing = false;
      }
      slot->loop_unsent = out.size() + in_flight;
      if (slot->output_blocked && slot->loop_unsent <= cfg_.max_unsent_bytes) {
        slot->output_blocked = false;
        if (!slot->dropped && !slot->session_ended && slot->state == Slot::State::Idle) {
          slot->state = Slot::State::Queued;
          resume = true;
        }
      }
      st = {slot->session_ended, slot->state == Slot::State::Idle && !slot->output_blocked, slot->turns,
            slot->worker_migrations, slot->idle_at, slot->idle_kind};
    }
    if (resume) enqueue_slot(slot);
    return st;
  }

  void shutdown() noexcept {
    if (shutdown_.exchange(true)) return;
    for (auto& w : wq_) {
      {
        std::lock_guard<std::mutex> lock{w->mu};
        w->shutdown = true;
      }
      w->cv.notify_all();
      poke(*w);
    }
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
    for (auto& w : wq_) {
      if (w->wake_fd >= 0) ::close(w->wake_fd);
      w->wake_fd = -1;
    }
  }

private:
  // How long a worker waits on its session's socket for the client's next
  // statement before handing the session back to the loop.
  static constexpr int kBurstWaitMs = 10;

  struct alignas(64) WorkerQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Slot>> queue;
    std::atomic<std::size_t> len{0};    // queue.size(), read without the lock
    std::atomic<bool> busy{false};
    std::atomic<bool> parked{false};    // waiting on its session's socket and wake_fd
    int wake_fd{-1};                    // eventfd: ends a parked wait
    bool shutdown{false};
  };

  static void poke(WorkerQueue& w) noexcept {
    std::uint64_t one = 1;
    (void)!::write(w.wake_fd, &one, sizeof one);
  }

  void wake_parked(std::size_t idx) noexcept {
    if (idx < wq_.size() && wq_[idx]->parked.load()) poke(*wq_[idx]);
  }

  void enqueue_slot(const std::shared_ptr<Slot>& slot) {
    queued_slots_.fetch_add(1);
    auto n = wq_.size();
    auto start = slot->preferred_worker < n
                     ? slot->preferred_worker
                     : next_worker_.fetch_add(1, std::memory_order_relaxed) % n;
    // An idle worker first; else one parked on its session's socket, which
    // yields at once when poked; else the start, which may be in a commit.
    auto chosen = n;
    auto parked = n;
    for (std::size_t i = 0; i < n; ++i) {
      auto idx = (start + i) % n;
      if (!wq_[idx]->busy.load(std::memory_order_relaxed)) {
        chosen = idx;
        break;
      }
      if (parked == n && wq_[idx]->parked.load(std::memory_order_relaxed)) parked = idx;
    }
    if (chosen == n) chosen = parked < n ? parked : start;
    auto& w = *wq_[chosen];
    {
      std::lock_guard<std::mutex> lock{w.mu};
      w.queue.push_back(slot);
      w.len.fetch_add(1, std::memory_order_relaxed);
      w.busy.store(true, std::memory_order_relaxed);
    }
    w.cv.notify_one();
    // Its worker may be parked on another session's socket: end the wait so
    // the session queued behind it runs.
    if (w.parked.load()) poke(w);
  }

  auto pop_slot(std::size_t my_idx) -> std::shared_ptr<Slot> {
    auto n = wq_.size();
    auto& mine = *wq_[my_idx];
    for (;;) {
      {
        std::lock_guard<std::mutex> lock{mine.mu};
        if (!mine.queue.empty()) {
          auto s = std::move(mine.queue.front());
          mine.queue.pop_front();
          mine.len.fetch_sub(1, std::memory_order_relaxed);
          mine.busy.store(true, std::memory_order_relaxed);
          queued_slots_.fetch_sub(1, std::memory_order_relaxed);
          return s;
        }
      }
      for (std::size_t i = 1; i < n; ++i) {
        auto& other = *wq_[(my_idx + i) % n];
        if (other.len.load(std::memory_order_relaxed) == 0) continue;
        std::lock_guard<std::mutex> lock{other.mu};
        if (!other.queue.empty()) {
          auto s = std::move(other.queue.front());
          other.queue.pop_front();
          other.len.fetch_sub(1, std::memory_order_relaxed);
          mine.busy.store(true, std::memory_order_relaxed);
          queued_slots_.fetch_sub(1, std::memory_order_relaxed);
          return s;
        }
      }
      std::unique_lock<std::mutex> lock{mine.mu};
      while (!mine.shutdown && mine.queue.empty()) {
        mine.busy.store(false, std::memory_order_relaxed);
        mine.cv.wait(lock);
      }
      if (mine.shutdown && mine.queue.empty()) return nullptr;
    }
  }

  void notify_loop(Slot& slot) {
    std::function<void(void*)> cb;
    void* t = nullptr;
    {
      std::lock_guard<std::mutex> lock{slot.mu};
      if (!slot.in_woken) {
        slot.in_woken = true;
        cb = slot.on_wake;
        t = slot.tag;
      }
    }
    if (cb && t != nullptr) {
      cb(t);
    } else if (on_wake_) {
      on_wake_();
    }
  }

  void write_or_queue(Slot& slot, std::string_view bytes) {
    if (bytes.empty()) return;
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock{slot.mu};
      if (slot.dropped) return;
      if (slot.fd < 0 || slot.loop_writing || !slot.outbox.empty()) {
        slot.outbox.append(bytes);
        slot.loop_writing = true;
      } else {
        fd = slot.fd;
      }
    }
    if (fd < 0) {
      notify_loop(slot);
      return;
    }

    std::size_t done = 0;
    auto send_failed = false;
    while (done < bytes.size()) {
      auto n = ::send(fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      send_failed = true;
      break;
    }
    if (done == bytes.size() && !send_failed) return;

    {
      std::lock_guard<std::mutex> lock{slot.mu};
      if (send_failed) {
        slot.dropped = true;
        slot.outbox.clear();
      } else if (!slot.dropped) {
        slot.outbox.append(bytes.substr(done));
        slot.loop_writing = true;
      }
    }
    notify_loop(slot);
  }

  void feed_raw(Slot& slot, std::string_view raw, std::vector<Input>& scratch) {
    if (raw.empty()) return;
    scratch.clear();
    slot.framer.push(raw, scratch);
    for (auto& in : scratch) slot.session.push(std::move(in));
    scratch.clear();
  }

  void flush_session_output(Slot& slot) {
    auto rem = slot.session.output();
    if (!rem.empty()) {
      write_or_queue(slot, rem);
      slot.session.clear_output();
    }
  }

  void worker_loop(std::size_t my_idx) {
    auto tid = std::this_thread::get_id();
    std::string local_raw;
    std::vector<Input> scratch;
    char recv_buf[16 << 10];

    for (;;) {
      auto slot = pop_slot(my_idx);
      if (!slot) return;
      auto dropped = false;
      auto stopping = false;
      auto expired = Timeout::None;
      {
        std::lock_guard<std::mutex> lock{slot->mu};
        slot->state = Slot::State::Running;
        slot->running_on = my_idx;
        if (slot->turns > 0 && slot->last_worker != tid) ++slot->worker_migrations;
        slot->last_worker = tid;
        ++slot->turns;
        local_raw.swap(slot->raw_in);
        slot->raw_in.clear();
        while (!slot->inbox.empty()) {
          slot->session.push(std::move(slot->inbox.front()));
          slot->inbox.pop_front();
        }
        dropped = slot->dropped;
        stopping = slot->stopping;
        expired = std::exchange(slot->expired, Timeout::None);
      }
      feed_raw(*slot, local_raw, scratch);
      local_raw.clear();

      if (dropped) {
        slot->session.close();
      } else if (stopping) {
        slot->session.stop();
        flush_session_output(*slot);
      } else if (expired != Timeout::None) {
        slot->session.expire(expired);
        flush_session_output(*slot);
      } else {
        for (;;) {
          slot->session.step();
          flush_session_output(*slot);
          if (slot->session.ended() || wq_.size() == 1 || queued_slots_.load(std::memory_order_relaxed) > 0 ||
              shutdown_.load(std::memory_order_relaxed)) {
            break;
          }
          int fd = -1;
          {
            std::lock_guard<std::mutex> lock{slot->mu};
            if (slot->dropped || slot->stopping || slot->loop_writing) break;
            fd = slot->fd;
          }
          if (slot->session.runnable()) {
            std::lock_guard<std::mutex> lock{slot->mu};
            ++slot->turns;
            continue;
          }
          if (fd < 0) break;

          // Active-session fast path: while no other session is queued, wait
          // here for the client's next statement and run it without bouncing
          // through the libuv loop. The wait ends on the socket, on wake_fd
          // (a session queued behind this worker, stop, drop, detach or
          // shutdown), or after kBurstWaitMs.
          auto& me = *wq_[my_idx];
          auto got_bytes = false;
          auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{kBurstWaitMs};
          for (;;) {
            auto n = ::recv(fd, recv_buf, sizeof recv_buf, MSG_DONTWAIT);
            if (n > 0) {
              feed_raw(*slot, std::string_view{recv_buf, static_cast<std::size_t>(n)}, scratch);
              got_bytes = true;
              if (slot->session.runnable()) break;
              continue;
            }
            if (n == 0) {
              slot->session.push({Input::Kind::EndOfStream, '\0', {}});
              slot->session.step();
              got_bytes = false;
              break;
            }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
              std::lock_guard<std::mutex> lock{slot->mu};
              slot->dropped = true;
              got_bytes = false;
              break;
            }
            auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            // parked before the checks, so an enqueue, stop, drop or detach
            // after them sees it and pokes wake_fd.
            me.parked.store(true);
            if (queued_slots_.load() > 0 || shutdown_.load()) {
              me.parked.store(false);
              break;
            }
            {
              std::lock_guard<std::mutex> lock{slot->mu};
              if (slot->stopping || slot->dropped || slot->fd < 0) {
                me.parked.store(false);
                break;
              }
            }
            pollfd pfds[2]{{fd, POLLIN, 0}, {me.wake_fd, POLLIN, 0}};
            auto ready = ::poll(pfds, 2, static_cast<int>(left.count()));
            me.parked.store(false);
            if (ready > 0 && (pfds[1].revents & POLLIN) != 0) {
              std::uint64_t count = 0;
              (void)!::read(me.wake_fd, &count, sizeof count);
              break;
            }
          }
          if (!got_bytes || !slot->session.runnable()) break;
          {
            std::lock_guard<std::mutex> lock{slot->mu};
            if (slot->dropped || slot->stopping) break;
            ++slot->turns;
          }
        }
      }

      auto requeue = false;
      auto need_loop_wake = false;
      {
        std::unique_lock<std::mutex> lock{slot->mu};
        if (!slot->raw_in.empty()) {
          local_raw.swap(slot->raw_in);
          slot->raw_in.clear();
          lock.unlock();
          feed_raw(*slot, local_raw, scratch);
          local_raw.clear();
          lock.lock();
        }
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
          flush_session_output(*slot);
          lock.lock();
        }
        slot->session_ended = slot->session.ended();
        std::tie(slot->idle_at, slot->idle_kind) = slot->session.idle_deadline(std::chrono::steady_clock::now());
        const auto runnable = !slot->dropped && !slot->session_ended && slot->session.runnable();
        // A client that takes no reply holds the next statement back, and
        // the loop, seeing the session not idle, reads nothing more from it.
        slot->output_blocked = runnable && cfg_.max_unsent_bytes != 0 &&
                               slot->outbox.size() + slot->loop_unsent > cfg_.max_unsent_bytes;
        requeue = runnable && !slot->output_blocked;
        slot->state = requeue ? Slot::State::Queued : Slot::State::Idle;
        need_loop_wake = !requeue || !slot->outbox.empty() || slot->session_ended || slot->dropped || slot->stopping;
      }
      if (requeue) enqueue_slot(slot);
      if (need_loop_wake) notify_loop(*slot);
    }
  }

  Database& db_;
  const SessionConfig& cfg_;
  std::function<void()> on_wake_;
  std::vector<std::unique_ptr<WorkerQueue>> wq_;
  std::vector<std::thread> workers_;
  std::atomic<std::size_t> next_worker_{0};
  std::atomic<std::size_t> queued_slots_{0};
  std::atomic<bool> shutdown_{false};
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
