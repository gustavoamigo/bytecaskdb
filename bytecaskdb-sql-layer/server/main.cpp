// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_server — the SQL layer over PostgreSQL's wire protocol
// (docs/server_design.md, docs/session_design.md).
//
//   sql_layer_server --dir D [--listen 127.0.0.1:5432] [--database NAME] [--max-connections 100]
//                     [--workers N]
//                     [--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION] [--sync-interval-ms 1000]
//                     [--io-backend pread|mmap|buffer_pool] [--buffer-pool-size BYTES] [--buffer-pool-direct-io on|off]
//                     [--statement-memory-bytes 67108864]
//
// Serves one database directory under one name, by default the directory's
// last component. Trust authentication only: anyone who reaches the port is
// in, so it listens on loopback unless told otherwise. Non-blocking I/O on a
// libuv event loop schedules sessions onto a fixed worker pool. SIGINT or
// SIGTERM stops it: running statements finish, queued messages and idle
// sessions get FATAL 57P01, a client that takes no reply bytes for ten seconds
// is dropped, open transactions roll back, and the database is closed. Prints
// "ready on HOST:PORT" once it accepts connections.

#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.pg_session;
import bytecask.sql.pg_wire;

namespace pg = bytecask::sql::pg;

namespace {

constexpr std::size_t kWriteChunk = 64u << 10;

auto flags(int argc, char** argv) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  if (argc % 2 == 0) throw std::invalid_argument(std::string{"a flag without a value: "} + argv[argc - 1]);
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string_view k{argv[i]};
    if (!k.starts_with("--")) throw std::invalid_argument("expected --flag value, got " + std::string{k});
    out[std::string{k.substr(2)}] = argv[i + 1];
  }
  for (const auto& [k, v] : out) {
    if (k != "dir" && k != "listen" && k != "database" && k != "max-connections" && k != "workers" && k != "sync" &&
        k != "sync-interval-ms" && k != "io-backend" && k != "buffer-pool-size" && k != "buffer-pool-direct-io" &&
        k != "statement-memory-bytes") {
      throw std::invalid_argument("unknown flag --" + k);
    }
  }
  return out;
}

// --sync takes the MariaDB plugin's bytecaskdb_sync names, in any case.
auto sync_mode(const std::string& name) -> bytecask::sql::SyncMode {
  using bytecask::sql::SyncMode;
  std::string upper{name};
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper == "AT_EVERY_COMMIT") return SyncMode::AtEveryCommit;
  if (upper == "AT_INTERVAL") return SyncMode::AtInterval;
  if (upper == "AT_FILE_ROTATION") return SyncMode::AtFileRotation;
  throw std::invalid_argument("--sync takes AT_EVERY_COMMIT, AT_INTERVAL or AT_FILE_ROTATION");
}

auto lower(std::string v) -> std::string {
  for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v;
}

auto io_backend(const std::string& name) -> bytecask::IoBackend {
  auto v = lower(name);
  if (v == "pread") return bytecask::IoBackend::Pread;
  if (v == "mmap") return bytecask::IoBackend::Mmap;
  if (v == "buffer_pool") return bytecask::IoBackend::BufferPool;
  throw std::invalid_argument("--io-backend takes pread, mmap or buffer_pool");
}

auto on_off(const std::string& flag, const std::string& value) -> bool {
  auto v = lower(value);
  if (v == "on") return true;
  if (v == "off") return false;
  throw std::invalid_argument("--" + flag + " takes on or off");
}

// A numeric flag's value, all of it, into `out`; std::invalid_argument
// otherwise, saying what the flag takes. Ranges are the caller's to check.
template <typename T>
void number(const std::string& flag, const std::string& value, const char* takes, T& out) {
  auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
  if (ec != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument("--" + flag + " takes " + takes);
  }
}

auto system_error(const char* what) -> std::system_error { return {errno, std::generic_category(), what}; }

// A bound socket on host:port, ready for uv_tcp_open + uv_listen.
auto bind_on(const std::string& host, const std::string& port) -> int {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* found = nullptr;
  if (auto rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &found); rc != 0) {
    throw std::runtime_error("cannot resolve " + host + ":" + port + ": " + ::gai_strerror(rc));
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> list{found, &::freeaddrinfo};
  for (auto* a = found; a != nullptr; a = a->ai_next) {
    auto fd = ::socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    int on = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (::bind(fd, a->ai_addr, a->ai_addrlen) == 0) return fd;
    ::close(fd);
  }
  throw system_error(("cannot listen on " + host + ":" + port).c_str());
}

auto loopback(const std::string& host) -> bool {
  return host == "127.0.0.1" || host == "::1" || host == "localhost" || host.starts_with("127.");
}

struct Server;

// One client connection owned by the libuv loop (docs/session_design.md,
// *The model* and *The I/O loop*). Freed in its uv_close callback once no
// worker holds its session.
struct Connection {
  explicit Connection(Server* s) : server{s} {
    tcp.data = this;
    write_req.data = this;
  }

  uv_tcp_t tcp{};
  uv_write_t write_req{};
  Server* server{nullptr};
  pg::Framer framer{};
  std::shared_ptr<pg::Pool::Slot> slot;
  std::string write_queue;
  std::string write_active;
  std::vector<char> read_buf;
  std::chrono::steady_clock::time_point last_progress{std::chrono::steady_clock::now()};
  bool counted{false};
  bool reading{false};
  bool write_in_flight{false};
  bool write_failed{false};
  bool session_ended{false};
  bool worker_idle{true};
  bool closing{false};
};

struct Server {
  Server(bytecask::sql::Database& db, const pg::SessionConfig& cfg, std::size_t max_conns, std::size_t workers)
      : max_connections{max_conns},
        pool{db, cfg, workers, [this] { (void)uv_async_send(&wake); }} {}

  uv_loop_t loop{};
  uv_tcp_t listener{};
  uv_async_t wake{};
  uv_signal_t sigint{};
  uv_signal_t sigterm{};
  uv_timer_t stop_timer{};
  std::size_t max_connections{100};
  std::size_t active_connections{0};
  pg::Pool pool;
  std::set<Connection*> conns;
  bool stopping{false};
  bool listener_closed{false};
  bool signals_closed{false};
  bool loop_handles_closed{false};
};

void check_shutdown_complete(Server* s);
void pump(Connection* c);

void on_conn_closed(uv_handle_t* h) {
  auto* c = static_cast<Connection*>(h->data);
  auto* s = c->server;
  if (c->counted) --s->active_connections;
  s->conns.erase(c);
  delete c;
  check_shutdown_complete(s);
}

void on_write(uv_write_t* req, int status) {
  auto* c = static_cast<Connection*>(req->data);
  c->write_in_flight = false;
  c->write_active.clear();
  if (status != 0) {
    c->write_failed = true;
    c->write_queue.clear();
    if (c->slot) c->server->pool.drop(c->slot);
  } else {
    c->last_progress = std::chrono::steady_clock::now();
  }
  pump(c);
}

void pump(Connection* c) {
  if (c->closing) return;
  auto* s = c->server;
  if (c->slot) {
    auto st = s->pool.drain(c->slot, c->write_queue);
    c->session_ended = st.ended;
    c->worker_idle = st.idle;
    if (c->session_ended && c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
  }
  if (c->write_failed) c->write_queue.clear();

  if (!c->write_in_flight && !c->write_queue.empty() && !c->write_failed) {
    auto n = std::min(c->write_queue.size(), kWriteChunk);
    c->write_active.assign(c->write_queue, 0, n);
    c->write_queue.erase(0, n);
    c->write_in_flight = true;
    c->last_progress = std::chrono::steady_clock::now();
    auto wrbuf = uv_buf_init(c->write_active.data(), static_cast<unsigned int>(c->write_active.size()));
    if (uv_write(&c->write_req, reinterpret_cast<uv_stream_t*>(&c->tcp), &wrbuf, 1, on_write) != 0) {
      c->write_in_flight = false;
      c->write_active.clear();
      c->write_failed = true;
      c->write_queue.clear();
      if (c->slot) s->pool.drop(c->slot);
    }
  }

  auto done_writing = c->write_failed || (c->session_ended && c->write_queue.empty() && !c->write_in_flight);
  if (done_writing && c->worker_idle) {
    if (c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
    c->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), on_conn_closed);
  }
}

void on_alloc(uv_handle_t* h, std::size_t suggested, uv_buf_t* buf) {
  auto* c = static_cast<Connection*>(h->data);
  if (c->read_buf.size() < suggested) c->read_buf.resize(suggested);
  buf->base = c->read_buf.data();
  buf->len = c->read_buf.size();
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
  auto* c = static_cast<Connection*>(stream->data);
  auto* s = c->server;
  if (nread < 0) {
    if (c->reading) {
      (void)uv_read_stop(stream);
      c->reading = false;
    }
    if (nread == UV_EOF) {
      if (c->slot) s->pool.submit(c->slot, pg::Input{pg::Input::Kind::EndOfStream, '\0', {}});
    } else {
      c->write_failed = true;
      c->write_queue.clear();
      if (c->slot) s->pool.drop(c->slot);
    }
    pump(c);
    return;
  }
  if (nread == 0) return;

  std::vector<pg::Input> inputs;
  c->framer.push(std::string_view{c->read_buf.data(), static_cast<std::size_t>(nread)}, inputs);
  for (const auto& in : inputs) {
    if (in.kind == pg::Input::Kind::BadStartupLength || in.kind == pg::Input::Kind::BadMessageLength) {
      if (c->reading) {
        (void)uv_read_stop(stream);
        c->reading = false;
      }
      break;
    }
  }
  if (!inputs.empty() && c->slot) s->pool.submit(c->slot, inputs);
}

void on_connect(uv_stream_t* server_stream, int status) {
  if (status < 0) return;
  auto* s = static_cast<Server*>(server_stream->data);
  if (s->stopping) return;

  auto* c = new Connection{s};
  (void)uv_tcp_init(&s->loop, &c->tcp);
  if (uv_accept(server_stream, reinterpret_cast<uv_stream_t*>(&c->tcp)) != 0) {
    c->closing = true;
    s->conns.insert(c);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), on_conn_closed);
    return;
  }
  (void)uv_tcp_nodelay(&c->tcp, 1);
  s->conns.insert(c);

  if (s->active_connections >= s->max_connections) {
    c->write_queue = pg::too_many_connections();
    c->session_ended = true;
    c->worker_idle = true;
    pump(c);
    return;
  }

  ++s->active_connections;
  c->counted = true;
  c->slot = s->pool.open();
  c->worker_idle = true;
  c->reading = true;
  (void)uv_read_start(reinterpret_cast<uv_stream_t*>(&c->tcp), on_alloc, on_read);
}

void on_wake(uv_async_t* h) {
  auto* s = static_cast<Server*>(h->data);
  std::vector<Connection*> snapshot{s->conns.begin(), s->conns.end()};
  for (auto* c : snapshot) {
    if (s->conns.contains(c)) pump(c);
  }
  check_shutdown_complete(s);
}

void on_stop_timer(uv_timer_t* h) {
  auto* s = static_cast<Server*>(h->data);
  auto now = std::chrono::steady_clock::now();
  std::vector<Connection*> snapshot{s->conns.begin(), s->conns.end()};
  for (auto* c : snapshot) {
    if (!s->conns.contains(c) || c->closing) continue;
    if (c->write_in_flight && now - c->last_progress > pg::kStopSendTimeout) {
      c->write_failed = true;
      c->write_queue.clear();
      if (c->slot) s->pool.drop(c->slot);
    }
    pump(c);
  }
  check_shutdown_complete(s);
}

void check_shutdown_complete(Server* s) {
  if (!s->stopping || !s->conns.empty() || s->loop_handles_closed) return;
  s->loop_handles_closed = true;
  (void)uv_timer_stop(&s->stop_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&s->stop_timer), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&s->wake), nullptr);
}

void begin_shutdown(Server* s) {
  if (s->stopping) return;
  s->stopping = true;
  if (!s->listener_closed) {
    s->listener_closed = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&s->listener), nullptr);
  }
  if (!s->signals_closed) {
    s->signals_closed = true;
    (void)uv_signal_stop(&s->sigint);
    (void)uv_signal_stop(&s->sigterm);
    uv_close(reinterpret_cast<uv_handle_t*>(&s->sigint), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&s->sigterm), nullptr);
  }
  auto now = std::chrono::steady_clock::now();
  std::vector<Connection*> snapshot{s->conns.begin(), s->conns.end()};
  for (auto* c : snapshot) {
    if (!s->conns.contains(c) || c->closing) continue;
    if (c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
    c->last_progress = now;
    if (c->slot) s->pool.stop(c->slot);
    pump(c);
  }
  (void)uv_timer_start(&s->stop_timer, on_stop_timer, 250, 250);
  check_shutdown_complete(s);
}

void on_signal(uv_signal_t* h, int /*signum*/) {
  begin_shutdown(static_cast<Server*>(h->data));
}

}  // namespace

int main(int argc, char** argv) {
  try {
    auto f = flags(argc, argv);
    if (!f.contains("dir")) throw std::invalid_argument("--dir is required");
    std::filesystem::path dir{f["dir"]};
    auto listen = f.contains("listen") ? f["listen"] : std::string{"127.0.0.1:5432"};
    auto colon = listen.rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument("--listen takes HOST:PORT");
    auto host = listen.substr(0, colon);
    auto port = listen.substr(colon + 1);
    // The default name is the directory's last component, a trailing '/' or
    // a '.' notwithstanding.
    auto named = std::filesystem::absolute(dir).lexically_normal();
    if (!named.has_filename()) named = named.parent_path();
    pg::SessionConfig cfg{f.contains("database") ? f["database"] : named.filename().string()};
    std::size_t max_connections = 100;
    if (f.contains("max-connections")) {
      number("max-connections", f["max-connections"], "a positive number", max_connections);
      if (max_connections == 0) throw std::invalid_argument("--max-connections takes a positive number");
    }
    auto hw = std::thread::hardware_concurrency();
    std::size_t workers = std::max<std::size_t>(2, hw == 0 ? 2 : 2 * static_cast<std::size_t>(hw));
    if (f.contains("workers")) {
      number("workers", f["workers"], "a positive number", workers);
      if (workers == 0) throw std::invalid_argument("--workers takes a positive number");
    }

    bytecask::sql::DatabaseOptions db_opts;
    if (f.contains("sync")) db_opts.sync = sync_mode(f["sync"]);
    if (f.contains("sync-interval-ms")) {
      // Its range is Database::open's to check, which refuses it before the server listens.
      number("sync-interval-ms", f["sync-interval-ms"], "a number", db_opts.sync_interval_ms);
    }

    if (f.contains("io-backend")) db_opts.io_backend = io_backend(f["io-backend"]);
    if (f.contains("buffer-pool-size")) {
      number("buffer-pool-size", f["buffer-pool-size"], "a number of bytes", db_opts.buffer_pool.capacity_bytes);
    }
    if (f.contains("buffer-pool-direct-io")) {
      db_opts.buffer_pool.direct_io = on_off("buffer-pool-direct-io", f["buffer-pool-direct-io"]);
    }
    if (f.contains("statement-memory-bytes")) {
      // 0 is Database::open's to refuse, before the server listens.
      number("statement-memory-bytes", f["statement-memory-bytes"], "a number of bytes",
             db_opts.statement_memory_bytes);
    }

    // Block SIGINT and SIGTERM while spawning Database and worker threads so
    // only the event loop thread receives them.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    auto db = bytecask::sql::Database::open(dir, db_opts);
    auto listener_fd = bind_on(host, port);

    Server srv{*db, cfg, max_connections, workers};
    if (auto rc = uv_loop_init(&srv.loop); rc != 0) {
      ::close(listener_fd);
      throw std::runtime_error(std::string{"uv_loop_init: "} + uv_strerror(rc));
    }
    srv.listener.data = &srv;
    srv.wake.data = &srv;
    srv.sigint.data = &srv;
    srv.sigterm.data = &srv;
    srv.stop_timer.data = &srv;

    (void)uv_async_init(&srv.loop, &srv.wake, on_wake);
    (void)uv_timer_init(&srv.loop, &srv.stop_timer);
    (void)uv_signal_init(&srv.loop, &srv.sigint);
    (void)uv_signal_init(&srv.loop, &srv.sigterm);
    (void)uv_signal_start(&srv.sigint, on_signal, SIGINT);
    (void)uv_signal_start(&srv.sigterm, on_signal, SIGTERM);

    (void)uv_tcp_init(&srv.loop, &srv.listener);
    if (auto rc = uv_tcp_open(&srv.listener, listener_fd); rc != 0) {
      ::close(listener_fd);
      throw std::runtime_error("cannot listen on " + host + ":" + port + ": " + uv_strerror(rc));
    }
    if (auto rc = uv_listen(reinterpret_cast<uv_stream_t*>(&srv.listener), 128, on_connect); rc != 0) {
      throw std::runtime_error("cannot listen on " + host + ":" + port + ": " + uv_strerror(rc));
    }

    pthread_sigmask(SIG_UNBLOCK, &stop_signals, nullptr);

    if (!loopback(host)) {
      std::fprintf(stderr,
                   "sql_layer_server: warning: listening on %s with trust authentication: anyone who reaches it is "
                   "in\n",
                   host.c_str());
    }
    std::printf("sql_layer_server: ready on %s:%s, database \"%s\"\n", host.c_str(), port.c_str(),
                cfg.database.c_str());
    std::fflush(stdout);

    (void)uv_run(&srv.loop, UV_RUN_DEFAULT);
    (void)uv_loop_close(&srv.loop);
    srv.pool.shutdown();
    db->close();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sql_layer_server: %s\n", e.what());
    return 1;
  }
}
