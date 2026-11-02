// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Tests for the decoupled PostgreSQL session and worker pool
// (docs/session_design.md, *Tests*):
// - Sessions move between worker threads across turns while preserving
//   per-session message order, including across open transactions.
// - A multi-turn pipeline yields the worker at ReadyForQuery so another
//   session runs after one turn rather than behind the whole pipeline.
// - An Execute outside BEGIN with no queued message after it parks without
//   blocking a worker until the next message arrives.
// - A session whose client has not taken max_unsent_bytes of its replies
//   runs no further statement until the client takes some, and a drop
//   still ends it.
// - A cancel taken while a session is idle is dropped; one taken after its
//   command's first message cancels the command's statement.
// - A cancel-only session (past --max-connections) refuses a startup with
//   53300 and takes a CancelRequest.
// - An idle timeout's timer that fires before its deadline ends nothing.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.pg_session;
import bytecask.sql.pg_wire;
import bytecask.sql.tests.util;

namespace pg = bytecask::sql::pg;

namespace {

auto startup_bytes(std::string_view db_name) -> std::string {
  pg::Out payload;
  payload.i32(pg::kProtocol30);
  payload.cstr("user");
  payload.cstr("tester");
  payload.cstr("database");
  payload.cstr(db_name);
  payload.raw('\0');

  pg::Out pkt;
  pkt.i32(static_cast<std::int32_t>(payload.size() + 4));
  pkt.bytes(payload.data());
  return pkt.take();
}

auto query_msg(std::string_view sql) -> std::string {
  pg::Out out;
  out.begin('Q');
  out.cstr(sql);
  out.end();
  return out.take();
}

auto parse_msg(std::string_view sql, std::string_view name = "") -> std::string {
  pg::Out out;
  out.begin('P');
  out.cstr(name);
  out.cstr(sql);
  out.i16(0);
  out.end();
  return out.take();
}

auto bind_msg(std::vector<std::string> params, std::string_view stmt = "", std::string_view portal = "") -> std::string {
  pg::Out out;
  out.begin('B');
  out.cstr(portal);
  out.cstr(stmt);
  out.i16(0);
  out.i16(static_cast<std::int16_t>(params.size()));
  for (const auto& p : params) {
    out.i32(static_cast<std::int32_t>(p.size()));
    out.bytes(p);
  }
  out.i16(0);
  out.end();
  return out.take();
}

auto execute_msg(std::string_view portal = "") -> std::string {
  pg::Out out;
  out.begin('E');
  out.cstr(portal);
  out.i32(0);
  out.end();
  return out.take();
}

auto sync_msg() -> std::string {
  pg::Out out;
  out.begin('S');
  out.end();
  return out.take();
}

struct ParsedReply {
  char type{'\0'};
  std::string body;
};

// Extracts complete backend messages from `raw`, leaving any unread suffix.
auto parse_replies(std::string& raw) -> std::vector<ParsedReply> {
  std::vector<ParsedReply> out;
  std::size_t pos = 0;
  while (raw.size() - pos >= 5) {
    auto type = raw[pos];
    pg::Body hdr{std::string_view{raw}.substr(pos + 1, 4)};
    auto len = static_cast<std::size_t>(hdr.i32());
    if (raw.size() - pos < 1 + len) break;
    out.push_back({type, raw.substr(pos + 5, len - 4)});
    pos += 1 + len;
  }
  raw.erase(0, pos);
  return out;
}

auto count_ready(const std::vector<ParsedReply>& msgs) -> std::size_t {
  std::size_t n = 0;
  for (const auto& m : msgs) {
    if (m.type == 'Z') ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("pg_session: sessions move between worker threads and keep message order", "[sql][server]") {
  sql_test::TempDir dir{"pg_session_pool"};
  auto db = bytecask::sql::Database::open(dir.path());
  pg::SessionConfig cfg{"testdb"};

  std::mutex wake_mu;
  std::condition_variable wake_cv;
  std::uint64_t wakes = 0;

  pg::Pool pool{*db, cfg, 2, [&] {
                  {
                    std::lock_guard<std::mutex> lock{wake_mu};
                    ++wakes;
                  }
                  wake_cv.notify_all();
                }};

  constexpr std::size_t kSessions = 12;
  constexpr std::size_t kRounds = 20;

  struct ClientState {
    pg::Framer framer{};
    std::shared_ptr<pg::Pool::Slot> slot;
    std::string raw_out;
    std::vector<ParsedReply> replies;
  };

  std::vector<ClientState> clients(kSessions);
  for (std::size_t i = 0; i < kSessions; ++i) {
    clients[i].slot = pool.open();
    std::vector<pg::Input> in;
    clients[i].framer.push(startup_bytes("testdb"), in);
    pool.submit(clients[i].slot, in);
  }

  auto wait_for_ready = [&](std::size_t idx, std::size_t target_ready) {
    std::unique_lock<std::mutex> lock{wake_mu};
    for (;;) {
      auto seen = wakes;
      lock.unlock();
      (void)pool.drain(clients[idx].slot, clients[idx].raw_out);
      auto more = parse_replies(clients[idx].raw_out);
      clients[idx].replies.insert(clients[idx].replies.end(), more.begin(), more.end());
      if (count_ready(clients[idx].replies) >= target_ready) return;
      lock.lock();
      wake_cv.wait(lock, [&] { return wakes != seen; });
    }
  };

  for (std::size_t i = 0; i < kSessions; ++i) {
    wait_for_ready(i, 1);
    REQUIRE(!clients[i].replies.empty());
    CHECK(clients[i].replies.back().type == 'Z');
    CHECK(clients[i].replies.back().body == "I");
  }

  // Create the table on session 0.
  {
    std::vector<pg::Input> in;
    clients[0].framer.push(query_msg("CREATE TABLE s (id BIGINT PRIMARY KEY, seq BIGINT)"), in);
    pool.submit(clients[0].slot, in);
    wait_for_ready(0, 2);
  }

  // Run kRounds explicit transactions (BEGIN, INSERT/UPDATE, COMMIT -> 3 turns
  // per round) across all sessions concurrently so open Transaction handles
  // migrate across worker threads.
  for (std::size_t r = 0; r < kRounds; ++r) {
    for (std::size_t i = 0; i < kSessions; ++i) {
      std::string wire;
      wire.append(query_msg("BEGIN"));
      if (r == 0) {
        wire.append(parse_msg("INSERT INTO s VALUES ($1, $2)", "ins"));
        wire.append(bind_msg({std::to_string(i), "1"}, "ins"));
      } else {
        if (r == 1) wire.append(parse_msg("UPDATE s SET seq = $2 WHERE id = $1", "upd"));
        wire.append(bind_msg({std::to_string(i), std::to_string(r + 1)}, "upd"));
      }
      wire.append(execute_msg());
      wire.append(sync_msg());
      wire.append(query_msg("COMMIT"));

      std::vector<pg::Input> in;
      clients[i].framer.push(wire, in);
      pool.submit(clients[i].slot, in);
    }
    for (std::size_t i = 0; i < kSessions; ++i) {
      auto expected_ready = (i == 0 ? 2u : 1u) + (r + 1) * 3u;
      wait_for_ready(i, expected_ready);
    }
  }

  std::size_t total_migrations = 0;
  for (std::size_t i = 0; i < kSessions; ++i) {
    std::string dummy;
    auto st = pool.drain(clients[i].slot, dummy);
    total_migrations += st.worker_migrations;

    // Every ReadyForQuery sequence for each round is 'T' (BEGIN), 'T' (Sync), 'I' (COMMIT).
    std::vector<char> z_states;
    for (const auto& m : clients[i].replies) {
      if (m.type == 'Z' && !m.body.empty()) z_states.push_back(m.body[0]);
    }
    std::size_t base = (i == 0 ? 2u : 1u);
    REQUIRE(z_states.size() == base + kRounds * 3u);
    for (std::size_t r = 0; r < kRounds; ++r) {
      CHECK(z_states[base + r * 3 + 0] == 'T');
      CHECK(z_states[base + r * 3 + 1] == 'T');
      CHECK(z_states[base + r * 3 + 2] == 'I');
    }
  }
  CHECK(total_migrations > 0);

  auto rows = db->execute("SELECT id, seq FROM s ORDER BY id");
  REQUIRE(rows.rows.size() == kSessions);
  for (std::size_t i = 0; i < kSessions; ++i) {
    CHECK(std::get<std::int64_t>(rows.rows[i][0]) == static_cast<std::int64_t>(i));
    CHECK(std::get<std::int64_t>(rows.rows[i][1]) == static_cast<std::int64_t>(kRounds));
  }

  pool.shutdown();
  db->close();
}

TEST_CASE("pg_session: Execute without following message parks until Sync arrives, and pipelines yield", "[sql][server]") {
  sql_test::TempDir dir{"pg_session_fairness"};
  auto db = bytecask::sql::Database::open(dir.path());
  pg::SessionConfig cfg{"testdb"};

  // Verify lookahead parking directly on Session: an Execute of DDL outside
  // BEGIN waits for Sync before running so it executes as autocommit.
  {
    pg::Framer framer;
    pg::Session s{*db, cfg};
    std::vector<pg::Input> in;
    framer.push(startup_bytes("testdb"), in);
    for (auto& item : in) s.push(std::move(item));
    s.step();
    (void)s.take_output();

    in.clear();
    framer.push(parse_msg("CREATE TABLE parked (k BIGINT PRIMARY KEY)") + bind_msg({}) + execute_msg(), in);
    for (auto& item : in) s.push(std::move(item));
    s.step();
    // Parse ('1') and Bind ('2') ran, but Execute parked waiting for lookahead.
    CHECK(!s.runnable());
    auto partial = s.take_output();
    auto msgs = parse_replies(partial);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].type == '1');
    CHECK(msgs[1].type == '2');

    // Now Sync arrives: Execute runs as autocommit DDL and Sync finishes with ReadyForQuery.
    in.clear();
    framer.push(sync_msg(), in);
    for (auto& item : in) s.push(std::move(item));
    CHECK(s.runnable());
    s.step();
    auto rest = s.take_output();
    auto finish = parse_replies(rest);
    REQUIRE(finish.size() == 2);
    CHECK(finish[0].type == 'C');
    CHECK(finish[1].type == 'Z');
  }

  // With 1 worker, a session with several queued turns yields at ReadyForQuery
  // and goes behind another queued session.
  db->execute("CREATE TABLE log (seq BIGINT PRIMARY KEY, who BIGINT)");
  std::mutex order_mu;
  std::condition_variable order_cv;
  std::size_t wakes = 0;
  pg::Pool pool{*db, cfg, 1, [&] {
                  {
                    std::lock_guard<std::mutex> lock{order_mu};
                    ++wakes;
                  }
                  order_cv.notify_all();
                }};
  auto a = pool.open();
  auto b = pool.open();
  pg::Framer fa;
  pg::Framer fb;
  {
    std::vector<pg::Input> ia;
    std::vector<pg::Input> ib;
    fa.push(startup_bytes("testdb"), ia);
    fb.push(startup_bytes("testdb"), ib);
    pool.submit(a, ia);
    pool.submit(b, ib);
  }
  auto wait_turns = [&](const std::shared_ptr<pg::Pool::Slot>& slot, std::size_t target) {
    std::unique_lock<std::mutex> lock{order_mu};
    for (;;) {
      auto seen = wakes;
      lock.unlock();
      std::string dummy;
      auto st = pool.drain(slot, dummy);
      if (st.turns >= target) return;
      lock.lock();
      order_cv.wait(lock, [&] { return wakes != seen; });
    }
  };
  wait_turns(a, 1);
  wait_turns(b, 1);

  // Submit 4 back-to-back extended-query turns on A and 1 on B: because the
  // worker yields A after its first ReadyForQuery, B runs before A finishes all 4.
  {
    std::string wire_a = parse_msg("INSERT INTO log VALUES ($1, $2)", "ins");
    for (int k = 1; k <= 4; ++k) {
      wire_a.append(bind_msg({std::to_string(k), "1"}, "ins"));
      wire_a.append(execute_msg());
      wire_a.append(sync_msg());
    }
    std::string wire_b = parse_msg("INSERT INTO log VALUES ($1, $2)", "ins") +
                         bind_msg({"100", "2"}, "ins") + execute_msg() + sync_msg();
    std::vector<pg::Input> ia;
    std::vector<pg::Input> ib;
    fa.push(wire_a, ia);
    fb.push(wire_b, ib);
    pool.submit(a, ia);
    pool.submit(b, ib);
  }
  wait_turns(a, 5);
  wait_turns(b, 2);
  CHECK(db->execute("SELECT seq FROM log ORDER BY seq").rows.size() == 5);

  pool.shutdown();
  db->close();
}

TEST_CASE("pg_session: past max_unsent_bytes a session runs no further statement until its client takes replies",
          "[sql][server]") {
  sql_test::TempDir dir{"pg_session_unsent"};
  auto db = bytecask::sql::Database::open(dir.path());
  db->execute("CREATE TABLE big (id BIGINT PRIMARY KEY, pad VARCHAR(1000) NOT NULL)");
  for (std::int64_t id = 0; id < 10; ++id) {
    db->execute("INSERT INTO big VALUES ($1, $2)", {id, sql_test::bytes(std::string(1000, 'p'))});
  }
  constexpr std::size_t kLimit = 64u << 10;
  constexpr std::size_t kStatements = 40;  // each reply about 10 KB: 400 KB in all
  pg::SessionConfig cfg{"testdb"};
  cfg.max_unsent_bytes = kLimit;

  std::mutex mu;
  std::condition_variable cv;
  std::uint64_t wakes = 0;
  pg::Pool pool{*db, cfg, 2, [&] {
                  {
                    std::lock_guard<std::mutex> lock{mu};
                    ++wakes;
                  }
                  cv.notify_all();
                }};
  auto slot = pool.open();  // no socket: every reply waits for the test, as the loop's unsent bytes
  pg::Framer framer;
  auto send = [&](const std::string& bytes) {
    std::vector<pg::Input> in;
    framer.push(bytes, in);
    pool.submit(slot, in);
  };
  // Drains replies into `unsent`, as the loop does, without taking them:
  // returns once nothing new has come for 300 ms.
  std::string unsent;
  auto settle = [&] {
    pg::Pool::Status st;
    for (;;) {
      std::unique_lock<std::mutex> lock{mu};
      auto seen = wakes;
      lock.unlock();
      st = pool.drain(slot, unsent);
      lock.lock();
      if (!cv.wait_for(lock, std::chrono::milliseconds{300}, [&] { return wakes != seen; })) return st;
    }
  };
  std::size_t ready = 0;
  auto take = [&] {
    ready += count_ready(parse_replies(unsent));
    unsent.clear();
  };

  send(startup_bytes("testdb"));
  (void)settle();
  take();
  REQUIRE(ready == 1);

  std::string pipeline;
  for (std::size_t k = 0; k < kStatements; ++k) pipeline += query_msg("SELECT id, pad FROM big");
  send(pipeline);
  auto st = settle();
  // Held back: past the bound by at most one statement's reply, with
  // statements left, and not idle, so the loop would read nothing more.
  CHECK(unsent.size() > kLimit);
  CHECK(unsent.size() < kLimit + (16u << 10));
  CHECK_FALSE(st.idle);
  auto copy = unsent;
  auto first = count_ready(parse_replies(copy));
  CHECK(first < kStatements);

  // Each time the client takes its replies, the session runs on, until
  // every statement has run and it is idle.
  for (int round = 0; round < 100 && ready < 1 + kStatements; ++round) {
    take();
    st = settle();
  }
  take();
  CHECK(ready == 1 + kStatements);
  CHECK(pool.drain(slot, unsent).idle);

  // Held back again, a drop still ends it.
  send(pipeline);
  (void)settle();
  CHECK(unsent.size() > kLimit);
  pool.drop(slot);
  CHECK(settle().ended);

  pool.shutdown();
  db->close();
}

namespace {

// Feeds `bytes` to the session and runs it to its next stop; returns the
// reply messages.
auto feed(pg::Session& s, pg::Framer& framer, const std::string& bytes) -> std::vector<ParsedReply> {
  std::vector<pg::Input> in;
  framer.push(bytes, in);
  for (auto& item : in) s.push(std::move(item));
  s.step();
  auto raw = s.take_output();
  return parse_replies(raw);
}

auto error_codes(const std::vector<ParsedReply>& msgs) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& m : msgs) {
    if (m.type != 'E') continue;
    for (std::size_t pos = 0; pos < m.body.size() && m.body[pos] != '\0';) {
      auto end = m.body.find('\0', pos + 1);
      if (m.body[pos] == 'C') out.push_back(m.body.substr(pos + 1, end - pos - 1));
      pos = end + 1;
    }
  }
  return out;
}

auto backend_key(const std::vector<ParsedReply>& msgs) -> std::pair<std::int32_t, std::int32_t> {
  for (const auto& m : msgs) {
    if (m.type != 'K') continue;
    pg::Body b{m.body};
    auto pid = b.i32();
    return {pid, b.i32()};
  }
  FAIL("no BackendKeyData");
  return {};
}

auto cancel_request_bytes(std::int32_t pid, std::int32_t secret) -> std::string {
  pg::Out pkt;
  pkt.i32(16);
  pkt.i32(pg::kCancelRequest);
  pkt.i32(pid);
  pkt.i32(secret);
  return pkt.take();
}

// A table a scan of reads past the 64 reads at which a cancel is checked.
void counted_table(bytecask::sql::Database& db) {
  db.execute("CREATE TABLE many (id BIGINT PRIMARY KEY)");
  for (std::int64_t id = 0; id < 1000; ++id) db.execute("INSERT INTO many VALUES ($1)", {id});
}

}  // namespace

TEST_CASE("pg_session: a cancel taken after a command's first message cancels its statement, one before it nothing",
          "[sql][server]") {
  sql_test::TempDir dir{"pg_session_cancel"};
  auto db = bytecask::sql::Database::open(dir.path());
  counted_table(*db);
  pg::CancelRegistry cancels;
  pg::SessionConfig cfg{"testdb"};
  cfg.cancels = &cancels;
  pg::Framer framer;
  pg::Session s{*db, cfg};
  auto [pid, secret] = backend_key(feed(s, framer, startup_bytes("testdb")));

  // Idle, after ReadyForQuery: dropped when the next command's first
  // message is taken, so the statement runs.
  cancels.cancel(pid, secret);
  auto out = feed(s, framer, parse_msg("SELECT id FROM many") + bind_msg({}) + execute_msg() + sync_msg());
  CHECK(error_codes(out) == std::vector<std::string>{});
  CHECK(count_ready(out) == 1);

  // After Parse and Bind were taken (as while a statement compiles): kept
  // for the Execute, which fails with 57014.
  out = feed(s, framer, parse_msg("SELECT id FROM many") + bind_msg({}));
  CHECK(error_codes(out) == std::vector<std::string>{});
  cancels.cancel(pid, secret);
  out = feed(s, framer, execute_msg() + sync_msg());
  CHECK(error_codes(out) == std::vector<std::string>{"57014"});
  CHECK(count_ready(out) == 1);

  // The next command starts clean.
  out = feed(s, framer, query_msg("SELECT id FROM many"));
  CHECK(error_codes(out) == std::vector<std::string>{});
  s.close();
  db->close();
}

TEST_CASE("pg_session: a cancel-only session refuses a startup with 53300 and still takes a CancelRequest",
          "[sql][server]") {
  sql_test::TempDir dir{"pg_session_cancel_only"};
  auto db = bytecask::sql::Database::open(dir.path());
  counted_table(*db);
  pg::CancelRegistry cancels;
  pg::SessionConfig cfg{"testdb"};
  cfg.cancels = &cancels;

  pg::Framer framer;
  pg::Session running{*db, cfg};
  auto [pid, secret] = backend_key(feed(running, framer, startup_bytes("testdb")));
  (void)feed(running, framer, parse_msg("SELECT id FROM many") + bind_msg({}));  // inside a command

  {
    pg::Framer f;
    pg::Session refused{*db, cfg};
    refused.admit_cancel_only();
    auto out = feed(refused, f, startup_bytes("testdb"));
    CHECK(error_codes(out) == std::vector<std::string>{"53300"});
    CHECK(refused.ended());
  }
  {
    pg::Framer f;
    pg::Session canceller{*db, cfg};
    canceller.admit_cancel_only();
    auto out = feed(canceller, f, cancel_request_bytes(pid, secret));
    CHECK(out.empty());  // no reply to a CancelRequest
    CHECK(canceller.ended());
  }
  CHECK(error_codes(feed(running, framer, execute_msg() + sync_msg())) == std::vector<std::string>{"57014"});
  running.close();
  db->close();
}

TEST_CASE("pg_session: an idle timeout's timer that fires before its deadline ends nothing", "[sql][server]") {
  sql_test::TempDir dir{"pg_session_expire"};
  auto db = bytecask::sql::Database::open(dir.path());
  pg::SessionConfig cfg{"testdb"};
  cfg.idle_in_transaction_timeout = std::chrono::milliseconds{300};

  std::mutex mu;
  std::condition_variable cv;
  std::uint64_t wakes = 0;
  pg::Pool pool{*db, cfg, 2, [&] {
                  {
                    std::lock_guard<std::mutex> lock{mu};
                    ++wakes;
                  }
                  cv.notify_all();
                }};
  db->execute("CREATE TABLE one (id BIGINT PRIMARY KEY)");
  auto slot = pool.open();
  pg::Framer framer;
  std::string unsent;
  auto send = [&](const std::string& bytes) {
    std::vector<pg::Input> in;
    framer.push(bytes, in);
    pool.submit(slot, in);
  };
  // Waits until the session is idle again, taking its replies.
  auto idle = [&] {
    for (int i = 0; i < 500; ++i) {
      auto st = pool.drain(slot, unsent);
      if (st.idle || st.ended) return st;
      std::unique_lock<std::mutex> lock{mu};
      cv.wait_for(lock, std::chrono::milliseconds{10});
    }
    FAIL("the session did not go idle");
    return pg::Pool::Status{};
  };

  send(startup_bytes("testdb"));
  (void)idle();
  send(query_msg("BEGIN"));
  auto st = idle();
  REQUIRE(st.idle_kind == pg::Timeout::IdleInTransaction);

  // A timer for this kind, fired early (or armed for an earlier idle
  // period): refused, and the session goes on.
  CHECK_FALSE(pool.expire(slot, pg::Timeout::IdleInTransaction));
  CHECK_FALSE(pool.drain(slot, unsent).ended);
  unsent.clear();
  send(query_msg("SELECT id FROM one"));
  st = idle();
  CHECK_FALSE(st.ended);
  auto replies = parse_replies(unsent);
  CHECK(error_codes(replies) == std::vector<std::string>{});
  CHECK(count_ready(replies) == 1);

  // Past the deadline it ends the session with 25P03.
  std::this_thread::sleep_until(st.idle_at + std::chrono::milliseconds{5});
  CHECK(pool.expire(slot, pg::Timeout::IdleInTransaction));
  st = idle();
  CHECK(st.ended);
  CHECK(error_codes(parse_replies(unsent)) == std::vector<std::string>{"25P03"});

  pool.shutdown();
  db->close();
}
