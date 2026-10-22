// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_server — the SQL layer over PostgreSQL's wire protocol
// (docs/server_design.md).
//
//   sql_layer_server --dir D [--listen 127.0.0.1:5432] [--database NAME] [--max-connections 100]
//
// Serves one database directory under one name, by default the directory's
// last component. Trust authentication only: anyone who reaches the port is
// in, so it listens on loopback unless told otherwise. One thread per
// connection. SIGINT or SIGTERM stops it: each connection is sent FATAL
// 57P01 between messages, open transactions roll back, and the database is
// closed. Prints "ready on HOST:PORT" once it accepts connections.

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
    if (k != "dir" && k != "listen" && k != "database" && k != "max-connections") {
      throw std::invalid_argument("unknown flag --" + k);
    }
  }
  return out;
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
    pg::SessionConfig cfg{f.contains("database") ? f["database"] : std::filesystem::absolute(dir).filename().string()};
    std::size_t max_connections = 100;
    if (f.contains("max-connections")) {
      const auto& v = f["max-connections"];
      auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), max_connections);
      if (ec != std::errc{} || end != v.data() + v.size() || max_connections == 0) {
        throw std::invalid_argument("--max-connections takes a positive number");
      }
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

    auto db = bytecask::sql::Database::open(dir);
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
    // it is running, says 57P01 and ends.
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
