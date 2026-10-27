// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_server — the SQL layer over PostgreSQL's wire protocol
// (docs/server_design.md, docs/session_design.md).
//
//   sql_layer_server --dir D [--listen 127.0.0.1:5432] [--database NAME] [--max-connections 100]
//                     [--workers N] [--thread-affinity on|off]
//                     [--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION] [--sync-interval-ms 1000]
//                     [--io-backend pread|mmap|buffer_pool] [--buffer-pool-size BYTES] [--buffer-pool-direct-io on|off]
//                     [--statement-memory-bytes 67108864]
//                     [--idle-in-transaction-timeout-ms 60000] [--statement-timeout-ms 0]
//                     [--transaction-timeout-ms 0] [--idle-session-timeout-ms 0]
//
// Serves one database directory under one name, by default the directory's
// last component. Trust authentication only: anyone who reaches the port is
// in, so it listens on loopback unless told otherwise. Non-blocking I/O on
// libuv event loops (one per core) schedules sessions onto a fixed worker
// pool. SIGINT or SIGTERM stops it: running statements finish, queued messages
// and idle sessions get FATAL 57P01, a client that takes no reply bytes for
// ten seconds is dropped, open transactions roll back, and the database is
// closed. Prints "ready on HOST:PORT" once it accepts connections.

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
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
#include <mutex>
#include <set>
#include <span>
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
    if (k != "dir" && k != "listen" && k != "database" && k != "max-connections" && k != "workers" &&
        k != "thread-affinity" && k != "sync" && k != "sync-interval-ms" && k != "io-backend" &&
        k != "buffer-pool-size" && k != "buffer-pool-direct-io" && k != "statement-memory-bytes" &&
        k != "idle-in-transaction-timeout-ms" && k != "statement-timeout-ms" && k != "transaction-timeout-ms" &&
        k != "idle-session-timeout-ms") {
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

auto listen_on(const std::string& host, const std::string& port) -> int {
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
    if (::bind(fd, a->ai_addr, a->ai_addrlen) == 0 && ::listen(fd, 128) == 0) return fd;
    ::close(fd);
  }
  throw system_error(("cannot listen on " + host + ":" + port).c_str());
}

auto loopback(const std::string& host) -> bool {
  return host == "127.0.0.1" || host == "::1" || host == "localhost" || host.starts_with("127.");
}

auto available_cpus() -> std::vector<int> {
  std::vector<int> out;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (pthread_getaffinity_np(pthread_self(), sizeof mask, &mask) == 0) {
    for (int i = 0; i < CPU_SETSIZE; ++i) {
      if (CPU_ISSET(static_cast<std::size_t>(i), &mask)) out.push_back(i);
    }
  }
  if (out.empty()) out.push_back(0);
  return out;
}

void pin_current_thread(int cpu) {
  if (cpu < 0) return;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(static_cast<std::size_t>(cpu), &mask);
  (void)pthread_setaffinity_np(pthread_self(), sizeof mask, &mask);
}

struct Server;
struct LoopShard;

// One client connection owned by its LoopShard's libuv loop
// (docs/session_design.md, *The model* and *The I/O loop*). Freed in its
// uv_close callback once no worker holds its session.
struct Connection {
  explicit Connection(LoopShard* sh) : shard{sh} {
    tcp.data = this;
    write_req.data = this;
    idle_timer.data = this;
  }

  uv_tcp_t tcp{};
  uv_write_t write_req{};
  // Armed while the session is idle and a timeout applies to it
  // (docs/session_design.md, *Timeouts*); fires Pool::expire.
  uv_timer_t idle_timer{};
  LoopShard* shard{nullptr};
  std::shared_ptr<pg::Pool::Slot> slot;
  std::string write_queue;
  std::string write_active;
  std::vector<char> read_buf;
  std::chrono::steady_clock::time_point last_progress{std::chrono::steady_clock::now()};
  std::chrono::steady_clock::time_point idle_at{};  // from the last drain
  pg::Timeout idle_kind{pg::Timeout::None};
  int open_handles{2};  // tcp and idle_timer: freed when both have closed
  bool counted{false};
  bool reading{false};
  bool write_in_flight{false};
  bool write_failed{false};
  bool session_ended{false};
  bool worker_idle{true};
  bool closing{false};
};

struct LoopShard {
  LoopShard(Server* s, std::size_t idx) : server{s}, index{idx} {
    wake.data = this;
    stop_timer.data = this;
  }

  Server* server{nullptr};
  std::size_t index{0};
  uv_loop_t loop{};
  uv_async_t wake{};
  uv_timer_t stop_timer{};
  std::set<Connection*> conns;
  std::mutex mu;
  std::vector<std::pair<int, std::size_t>> incoming_fds;  // (fd, conn_seq)
  std::vector<void*> woken_conns;
  bool stopping{false};
  bool loop_handles_closed{false};
};

struct Server {
  Server(bytecask::sql::Database& db, const pg::SessionConfig& cfg, std::size_t max_conns, std::size_t n_workers,
         bool use_affinity, std::span<const int> cpus, int listen_fd)
      : max_connections{max_conns},
        workers{n_workers},
        affinity{use_affinity},
        listener_fd{listen_fd},
        pool{db, cfg, n_workers, {}, use_affinity ? cpus : std::span<const int>{}} {
    listener_poll.data = this;
    sigint.data = this;
    sigterm.data = this;
  }

  std::size_t max_connections{100};
  std::size_t workers{2};
  bool affinity{false};
  int listener_fd{-1};
  std::atomic<std::size_t> active_connections{0};
  std::atomic<std::size_t> next_conn_seq{0};
  std::atomic<bool> stopping{false};
  pg::Pool pool;
  std::vector<std::unique_ptr<LoopShard>> shards;
  uv_poll_t listener_poll{};
  uv_signal_t sigint{};
  uv_signal_t sigterm{};
  bool listener_closed{false};
  bool signals_closed{false};
};

void check_shard_shutdown_complete(LoopShard* sh);
void pump(Connection* c);

void on_alloc(uv_handle_t* h, std::size_t suggested, uv_buf_t* buf);
void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
void on_handle_closed(uv_handle_t* h) {
  auto* c = static_cast<Connection*>(h->data);
  if (--c->open_handles > 0) return;
  auto* sh = c->shard;
  auto* s = sh->server;
  if (c->slot) s->pool.detach(c->slot);
  if (c->counted) s->active_connections.fetch_sub(1, std::memory_order_acq_rel);
  sh->conns.erase(c);
  delete c;
  check_shard_shutdown_complete(sh);
}

void close_connection(Connection* c) {
  c->closing = true;
  uv_close(reinterpret_cast<uv_handle_t*>(&c->idle_timer), on_handle_closed);
  uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), on_handle_closed);
}

void on_idle_timer(uv_timer_t* t) {
  auto* c = static_cast<Connection*>(t->data);
  if (c->closing || !c->slot) return;
  // Does nothing if the session ran meanwhile: its next idle drain re-arms.
  (void)c->shard->server->pool.expire(c->slot, c->idle_kind);
}

// While the session is idle and one of its timeouts applies, the timer runs
// to that timeout's deadline; otherwise it is stopped.
void arm_idle_timer(Connection* c) {
  if (!c->worker_idle || c->session_ended || c->idle_kind == pg::Timeout::None) {
    (void)uv_timer_stop(&c->idle_timer);
    return;
  }
  auto left = std::chrono::ceil<std::chrono::milliseconds>(c->idle_at - std::chrono::steady_clock::now()).count();
  (void)uv_timer_start(&c->idle_timer, on_idle_timer, left > 0 ? static_cast<std::uint64_t>(left) : 0, 0);
}

void on_write(uv_write_t* req, int status) {
  auto* c = static_cast<Connection*>(req->data);
  c->write_in_flight = false;
  c->write_active.clear();
  if (status != 0) {
    c->write_failed = true;
    c->write_queue.clear();
    if (c->slot) c->shard->server->pool.drop(c->slot);
  } else {
    c->last_progress = std::chrono::steady_clock::now();
  }
  pump(c);
}

void pump(Connection* c) {
  if (c->closing) return;
  auto* sh = c->shard;
  auto* s = sh->server;
  if (c->slot) {
    auto st = s->pool.drain(c->slot, c->write_queue, c->write_in_flight);
    c->session_ended = st.ended;
    c->worker_idle = st.idle;
    c->idle_at = st.idle_at;
    c->idle_kind = st.idle_kind;
    if (c->session_ended && c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
  }
  if (c->write_failed) c->write_queue.clear();

  while (!c->write_in_flight && !c->write_queue.empty() && !c->write_failed) {
    auto n = std::min(c->write_queue.size(), kWriteChunk);
    auto try_buf = uv_buf_init(c->write_queue.data(), static_cast<unsigned int>(n));
    auto written = uv_try_write(reinterpret_cast<uv_stream_t*>(&c->tcp), &try_buf, 1);
    if (written > 0) {
      c->write_queue.erase(0, static_cast<std::size_t>(written));
      c->last_progress = std::chrono::steady_clock::now();
      continue;
    }
    if (written == UV_EAGAIN || written == 0) {
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
      break;
    }
    c->write_failed = true;
    c->write_queue.clear();
    if (c->slot) s->pool.drop(c->slot);
    break;
  }
  if (!c->write_in_flight && c->write_queue.empty() && c->slot && !c->write_failed) {
    auto st = s->pool.drain(c->slot, c->write_queue, false);
    c->session_ended = st.ended;
    c->worker_idle = st.idle;
    c->idle_at = st.idle_at;
    c->idle_kind = st.idle_kind;
  }

  if (c->worker_idle && !c->session_ended && !c->write_failed && !c->reading && !sh->stopping &&
      !s->stopping.load(std::memory_order_relaxed)) {
    c->reading = true;
    (void)uv_read_start(reinterpret_cast<uv_stream_t*>(&c->tcp), on_alloc, on_read);
  }
  if (c->slot) arm_idle_timer(c);

  auto done_writing = c->write_failed || (c->session_ended && c->write_queue.empty() && !c->write_in_flight);
  if (done_writing && c->worker_idle) {
    if (c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
    close_connection(c);
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
  auto* s = c->shard->server;
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

  if (c->reading) {
    (void)uv_read_stop(stream);
    c->reading = false;
  }
  c->worker_idle = false;
  if (c->slot) s->pool.submit_bytes(c->slot, std::string_view{c->read_buf.data(), static_cast<std::size_t>(nread)});
}

void accept_on_shard(LoopShard* sh, int fd, std::size_t conn_seq) {
  auto* s = sh->server;
  if (sh->stopping || s->stopping.load(std::memory_order_relaxed)) {
    ::close(fd);
    return;
  }

  auto* c = new Connection{sh};
  (void)uv_tcp_init(&sh->loop, &c->tcp);
  (void)uv_timer_init(&sh->loop, &c->idle_timer);
  if (uv_tcp_open(&c->tcp, fd) != 0) {
    ::close(fd);
    sh->conns.insert(c);
    close_connection(c);
    return;
  }
  sh->conns.insert(c);

  auto cur = s->active_connections.load(std::memory_order_relaxed);
  auto reserved = false;
  while (cur < s->max_connections) {
    if (s->active_connections.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
      reserved = true;
      break;
    }
  }
  if (!reserved) {
    c->write_queue = pg::too_many_connections();
    c->session_ended = true;
    c->worker_idle = true;
    pump(c);
    return;
  }

  c->counted = true;
  auto pref = s->affinity ? (conn_seq % s->workers) : pg::Pool::kNoPreference;
  c->slot = s->pool.open(fd, c, pref, [sh](void* tag) {
    {
      std::lock_guard<std::mutex> lock{sh->mu};
      sh->woken_conns.push_back(tag);
    }
    (void)uv_async_send(&sh->wake);
  });
  c->worker_idle = true;
  c->reading = true;
  (void)uv_read_start(reinterpret_cast<uv_stream_t*>(&c->tcp), on_alloc, on_read);
}

void on_accept(uv_poll_t* handle, int status, int /*events*/) {
  if (status < 0) return;
  auto* s = static_cast<Server*>(handle->data);
  if (s->stopping.load(std::memory_order_relaxed)) return;

  for (;;) {
    auto fd = ::accept4(s->listener_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    int on = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    auto seq = s->next_conn_seq.fetch_add(1, std::memory_order_relaxed);
    auto idx = seq % s->shards.size();
    if (idx == 0) {
      accept_on_shard(s->shards[0].get(), fd, seq);
    } else {
      auto* sh = s->shards[idx].get();
      {
        std::lock_guard<std::mutex> lock{sh->mu};
        sh->incoming_fds.emplace_back(fd, seq);
      }
      (void)uv_async_send(&sh->wake);
    }
  }
}

void on_stop_timer(uv_timer_t* h) {
  auto* sh = static_cast<LoopShard*>(h->data);
  auto* s = sh->server;
  auto now = std::chrono::steady_clock::now();
  std::vector<Connection*> snapshot{sh->conns.begin(), sh->conns.end()};
  for (auto* c : snapshot) {
    if (!sh->conns.contains(c) || c->closing) continue;
    if (c->write_in_flight && now - c->last_progress > pg::kStopSendTimeout) {
      c->write_failed = true;
      c->write_queue.clear();
      if (c->slot) s->pool.drop(c->slot);
    }
    pump(c);
  }
  check_shard_shutdown_complete(sh);
}

void check_shard_shutdown_complete(LoopShard* sh) {
  if (!sh->stopping || !sh->conns.empty() || sh->loop_handles_closed) return;
  sh->loop_handles_closed = true;
  (void)uv_timer_stop(&sh->stop_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&sh->stop_timer), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&sh->wake), nullptr);
}

void begin_shard_shutdown(LoopShard* sh) {
  if (sh->stopping) return;
  sh->stopping = true;
  auto* s = sh->server;
  std::vector< std::pair<int, std::size_t> > pending_fds;
  {
    std::lock_guard<std::mutex> lock{sh->mu};
    pending_fds.swap(sh->incoming_fds);
  }
  for (auto [fd, _] : pending_fds) ::close(fd);

  auto now = std::chrono::steady_clock::now();
  std::vector<Connection*> snapshot{sh->conns.begin(), sh->conns.end()};
  for (auto* c : snapshot) {
    if (!sh->conns.contains(c) || c->closing) continue;
    if (c->reading) {
      (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
      c->reading = false;
    }
    c->last_progress = now;
    if (c->slot) s->pool.stop(c->slot);
    pump(c);
  }
  (void)uv_timer_start(&sh->stop_timer, on_stop_timer, 250, 250);
  check_shard_shutdown_complete(sh);
}

void on_wake(uv_async_t* h) {
  auto* sh = static_cast<LoopShard*>(h->data);
  std::vector<std::pair<int, std::size_t>> fds;
  std::vector<void*> tags;
  {
    std::lock_guard<std::mutex> lock{sh->mu};
    fds.swap(sh->incoming_fds);
    tags.swap(sh->woken_conns);
  }
  for (auto [fd, seq] : fds) accept_on_shard(sh, fd, seq);
  for (auto* tag : tags) {
    auto* c = static_cast<Connection*>(tag);
    if (sh->conns.contains(c)) pump(c);
  }
  if (sh->server->stopping.load(std::memory_order_relaxed)) {
    begin_shard_shutdown(sh);
  }
  check_shard_shutdown_complete(sh);
}

void on_signal(uv_signal_t* h, int /*signum*/) {
  auto* s = static_cast<Server*>(h->data);
  if (s->stopping.exchange(true)) return;
  if (!s->listener_closed) {
    s->listener_closed = true;
    (void)uv_poll_stop(&s->listener_poll);
    uv_close(reinterpret_cast<uv_handle_t*>(&s->listener_poll), nullptr);
    (void)::shutdown(s->listener_fd, SHUT_RDWR);
  }
  if (!s->signals_closed) {
    s->signals_closed = true;
    // Further SIGINT or SIGTERM are ignored while the stop runs, as before:
    // every other thread blocks them, so blocking them here too keeps them
    // pending once closing the handles below restores their default action.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    (void)uv_signal_stop(&s->sigint);
    (void)uv_signal_stop(&s->sigterm);
    uv_close(reinterpret_cast<uv_handle_t*>(&s->sigint), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&s->sigterm), nullptr);
  }
  for (auto& sh : s->shards) {
    (void)uv_async_send(&sh->wake);
  }
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
    pg::CancelRegistry cancels;  // outlives every session: the pool is destroyed first
    pg::SessionConfig cfg{f.contains("database") ? f["database"] : named.filename().string()};
    cfg.cancels = &cancels;
    // 0 turns a timeout off (docs/session_design.md, *Timeouts*).
    for (auto [flag, out] : {std::pair{"idle-in-transaction-timeout-ms", &cfg.idle_in_transaction_timeout},
                             std::pair{"statement-timeout-ms", &cfg.statement_timeout},
                             std::pair{"transaction-timeout-ms", &cfg.transaction_timeout},
                             std::pair{"idle-session-timeout-ms", &cfg.idle_session_timeout}}) {
      if (!f.contains(flag)) continue;
      std::uint32_t ms = 0;
      number(flag, f[flag], "a number of milliseconds", ms);
      *out = std::chrono::milliseconds{ms};
    }
    std::size_t max_connections = 100;
    if (f.contains("max-connections")) {
      number("max-connections", f["max-connections"], "a positive number", max_connections);
      if (max_connections == 0) throw std::invalid_argument("--max-connections takes a positive number");
    }
    auto cpus = available_cpus();
    std::size_t workers = std::max<std::size_t>(4, 4 * cpus.size());
    if (f.contains("workers")) {
      number("workers", f["workers"], "a positive number", workers);
      if (workers == 0) throw std::invalid_argument("--workers takes a positive number");
    }
    auto affinity = false;
    if (f.contains("thread-affinity")) {
      affinity = on_off("thread-affinity", f["thread-affinity"]);
    }

    bytecask::sql::DatabaseOptions db_opts;
    if (f.contains("sync")) db_opts.sync = sync_mode(f["sync"]);
    if (f.contains("sync-interval-ms")) {
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
      number("statement-memory-bytes", f["statement-memory-bytes"], "a number of bytes",
             db_opts.statement_memory_bytes);
    }

    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    auto db = bytecask::sql::Database::open(dir, db_opts);
    auto listener_fd = listen_on(host, port);

    auto n_loops = std::min(workers, std::max<std::size_t>(1, cpus.size()));
    Server srv{*db, cfg, max_connections, workers, affinity, cpus, listener_fd};
    srv.shards.reserve(n_loops);
    for (std::size_t i = 0; i < n_loops; ++i) {
      auto sh = std::make_unique<LoopShard>(&srv, i);
      if (auto rc = uv_loop_init(&sh->loop); rc != 0) {
        if (srv.listener_fd >= 0) ::close(srv.listener_fd);
        throw std::runtime_error(std::string{"uv_loop_init: "} + uv_strerror(rc));
      }
      (void)uv_async_init(&sh->loop, &sh->wake, on_wake);
      (void)uv_timer_init(&sh->loop, &sh->stop_timer);
      srv.shards.push_back(std::move(sh));
    }

    auto& sh0 = *srv.shards[0];
    (void)uv_signal_init(&sh0.loop, &srv.sigint);
    (void)uv_signal_init(&sh0.loop, &srv.sigterm);
    (void)uv_signal_start(&srv.sigint, on_signal, SIGINT);
    (void)uv_signal_start(&srv.sigterm, on_signal, SIGTERM);
    (void)uv_poll_init(&sh0.loop, &srv.listener_poll, listener_fd);
    (void)uv_poll_start(&srv.listener_poll, UV_READABLE, on_accept);

    std::vector<std::thread> loop_threads;
    loop_threads.reserve(n_loops - 1);
    for (std::size_t i = 1; i < n_loops; ++i) {
      int cpu = affinity ? cpus[i % cpus.size()] : -1;
      auto* sh = srv.shards[i].get();
      loop_threads.emplace_back([sh, cpu] {
        pin_current_thread(cpu);
        (void)uv_run(&sh->loop, UV_RUN_DEFAULT);
      });
    }

    if (affinity) pin_current_thread(cpus[0]);
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

    (void)uv_run(&sh0.loop, UV_RUN_DEFAULT);
    for (auto& t : loop_threads) {
      if (t.joinable()) t.join();
    }
    for (auto& sh : srv.shards) {
      (void)uv_loop_close(&sh->loop);
    }
    if (srv.listener_fd >= 0) ::close(srv.listener_fd);
    srv.pool.shutdown();
    db->close();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sql_layer_server: %s\n", e.what());
    return 1;
  }
}
