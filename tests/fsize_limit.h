// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// RLIMIT_FSIZE for the scope of a write, which is how a test gets a real
// short write from a regular file: the write that crosses the limit returns
// the bytes below it and sets no errno — the shape of a disk filling up
// mid-write (#221).

#pragma once

#include <cerrno>
#include <csignal>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <system_error>

namespace bytecask_test {

struct WriteError {
  int code{0};  // 0: fn threw no std::system_error
  std::string what;

  // EIO naming the short write, not a stale errno.
  [[nodiscard]] auto is_short_write() const -> bool {
    return code == static_cast<int>(std::errc::io_error) &&
           what.find("short write") != std::string::npos;
  }
};

// Runs fn with the file size limit set to limit bytes and errno primed with
// EAGAIN, so an error that reports a stale errno is told from one that
// reports what happened. Returns the std::system_error fn threw. Runs
// in-process, so coverage sees the write paths; the limit and SIGXFSZ's
// disposition are restored before it returns, and fn must not report to
// Catch2 (its output may be a file past the limit).
inline auto write_error_under_fsize_limit(rlim_t limit,
                                          const std::function<void()> &fn)
    -> WriteError {
  rlimit saved{};
  if (::getrlimit(RLIMIT_FSIZE, &saved) != 0) return {-1, "getrlimit"};
  // SIGXFSZ would kill the process at the limit; ignored, the write that
  // crosses it returns short instead.
  struct Restore {
    rlimit limit;
    void (*handler)(int);
    ~Restore() {
      ::setrlimit(RLIMIT_FSIZE, &limit);
      std::signal(SIGXFSZ, handler);
    }
  } const restore{saved, std::signal(SIGXFSZ, SIG_IGN)};
  auto limited = saved;
  limited.rlim_cur = limit;
  if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) return {-1, "setrlimit"};
  errno = EAGAIN;
  try {
    fn();
  } catch (const std::system_error &e) {
    return {e.code().value(), e.what()};
  }
  return {};
}

} // namespace bytecask_test
