// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_server — the SQL layer over PostgreSQL's wire protocol
// (docs/server_design.md).
//
//   sql_layer_server --dir D [--listen 127.0.0.1:5432] [--database NAME] [--max-connections 100]
//                     [--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION] [--sync-interval-ms 1000]
//                     [--io-backend pread|mmap|buffer_pool] [--buffer-pool-size BYTES] [--buffer-pool-direct-io on|off]
//
// Serves one database directory under one name, by default the directory's
// last component. Trust authentication only: anyone who reaches the port is
// in, so it listens on loopback unless told otherwise. One thread per
// connection. SIGINT or SIGTERM stops it: each connection is sent FATAL
// 57P01 between messages, or dropped if its client takes no reply bytes for
// ten seconds, open transactions roll back, and the database is closed. Prints "ready on HOST:PORT" once it accepts connections.

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.pg_session;

namespace pg = bytecask::sql::pg;

namespace {

auto flags(int argc, char** argv) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  if (argc % 2 == 0) throw std::invalid_argument(std::string{"a flag without a value: "} + argv[argc - 1]);
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string_view k{argv[i]};
    if (!k.starts_with("--")) throw std::invalid_argument("expected --flag value, got " + std::string{k});
    out[std::string{k.substr(2)}] = argv[i + 1];
  }
  for (const auto& [k, v] : out) {
    if (k != "dir" && k != "listen" && k != "database" && k != "max-connections" && k != "sync" &&
        k != "sync-interval-ms" &&
        k != "io-backend" && k != "buffer-pool-size" && k != "buffer-pool-direct-io") {
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

auto system_error(const char* what) -> std::system_error { return {errno, std::generic_category(), what}; }

// A listening socket on host:port.
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
    auto fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
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

// The connections being served, so shutdown can wake each one's read, and
// how many there are, so it can wait for them.
struct Connections {
  std::mutex mu;
  std::condition_variable none_left;
  std::set<int> fds;
};

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
      const auto& v = f["max-connections"];
      auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), max_connections);
      if (ec != std::errc{} || end != v.data() + v.size() || max_connections == 0) {
        throw std::invalid_argument("--max-connections takes a positive number");
      }
    }

    bytecask::sql::DatabaseOptions db_opts;
    if (f.contains("sync")) db_opts.sync = sync_mode(f["sync"]);
    if (f.contains("sync-interval-ms")) {
      const auto& v = f["sync-interval-ms"];
      auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), db_opts.sync_interval_ms);
      if (ec != std::errc{} || end != v.data() + v.size()) {
        throw std::invalid_argument("--sync-interval-ms takes a number");
      }
      // Its range is Database::open's to check, which refuses it before the server listens.
    }

    if (f.contains("io-backend")) db_opts.io_backend = io_backend(f["io-backend"]);
    if (f.contains("buffer-pool-size")) {
      const auto& v = f["buffer-pool-size"];
      auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), db_opts.buffer_pool.capacity_bytes);
      if (ec != std::errc{} || end != v.data() + v.size()) {
        throw std::invalid_argument("--buffer-pool-size takes a number of bytes");
      }
    }
    if (f.contains("buffer-pool-direct-io")) {
      db_opts.buffer_pool.direct_io = on_off("buffer-pool-direct-io", f["buffer-pool-direct-io"]);
    }

    // SIGINT and SIGTERM go to one thread, which wakes the accept loop
    // through a pipe; every other thread blocks them.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    ::signal(SIGPIPE, SIG_IGN);  // a dropped client is a failed send, not a signal
    int wake[2];
    if (::pipe2(wake, O_CLOEXEC) != 0) throw system_error("pipe");

    auto db = bytecask::sql::Database::open(dir, db_opts);
    auto listener = listen_on(host, port);
    if (!loopback(host)) {
      std::fprintf(stderr,
                   "sql_layer_server: warning: listening on %s with trust authentication: anyone who reaches it is "
                   "in\n",
                   host.c_str());
    }
    std::printf("sql_layer_server: ready on %s:%s, database \"%s\"\n", host.c_str(), port.c_str(),
                cfg.database.c_str());
    std::fflush(stdout);

    std::atomic<bool> stopping{false};
    std::thread{[&stopping, &stop_signals, w = wake[1]] {
      int sig = 0;
      sigwait(&stop_signals, &sig);
      stopping.store(true);
      char c = 1;
      (void)::write(w, &c, 1);
    }}.detach();  // it outlives main only to exit with the process

    Connections conns;
    for (;;) {
      pollfd fds[2]{{listener, POLLIN, 0}, {wake[0], POLLIN, 0}};
      if (::poll(fds, 2, -1) < 0) {
        if (errno == EINTR) continue;
        throw system_error("poll");
      }
      if (stopping.load() || (fds[1].revents & POLLIN) != 0) break;
      if ((fds[0].revents & POLLIN) == 0) continue;
      auto fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) continue;  // the client went away before accept, or descriptors ran out: keep serving
      int on = 1;
      (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
      {
        std::lock_guard<std::mutex> lock{conns.mu};
        if (conns.fds.size() >= max_connections) {
          auto fatal = pg::too_many_connections();
          (void)::send(fd, fatal.data(), fatal.size(), MSG_NOSIGNAL);
          ::close(fd);
          continue;
        }
        conns.fds.insert(fd);
      }
      std::thread{[&db, &cfg, &stopping, &conns, fd] {
        try {
          pg::serve(*db, fd, cfg, stopping);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "sql_layer_server: connection ended: %s\n", e.what());
        }
        std::lock_guard<std::mutex> lock{conns.mu};
        conns.fds.erase(fd);
        ::close(fd);
        conns.none_left.notify_all();
      }}.detach();
    }

    // Stopping: no new connections; each one's read wakes, it finishes what
    // it is running, says 57P01 and ends. A send stops waiting on a client
    // that takes nothing for a while (pg::kStopSendTimeout): it is dropped.
    ::close(listener);
    {
      std::unique_lock<std::mutex> lock{conns.mu};
      for (auto fd : conns.fds) (void)::shutdown(fd, SHUT_RD);
      conns.none_left.wait(lock, [&] { return conns.fds.empty(); });
    }
    db->close();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sql_layer_server: %s\n", e.what());
    return 1;
  }
}
