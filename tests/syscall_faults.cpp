// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The --wrap interposers behind syscall_faults.h. The linker turns every
// reference to `open` in this binary's objects into `__wrap_open`, and
// `__real_open` into the real symbol — under a sanitizer, its interceptor.
// Calls made inside shared libraries (std::filesystem in libstdc++) are not
// redirected, and so are neither counted nor failed.

#include "syscall_faults.h"

#include "fault_injector.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>
#include <vector>

extern "C" {
int __real_open(const char *path, int flags, ...);
ssize_t __real_pread(int fd, void *buf, size_t n, off_t off);
ssize_t __real_pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t __real_pwritev(int fd, const struct iovec *iov, int cnt, off_t off);
ssize_t __real_write(int fd, const void *buf, size_t n);
int __real_fdatasync(int fd);
int __real_fsync(int fd);
int __real_ftruncate(int fd, off_t len);
int __real_fstat(int fd, struct stat *st);
int __real_stat(const char *path, struct stat *st);
int __real_renameat2(int olddir, const char *from, int newdir, const char *to,
                     unsigned flags);
int __real_link(const char *from, const char *to);
int __real_unlink(const char *path);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t off);

int __wrap_open(const char *path, int flags, ...);
ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off);
ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off);
ssize_t __wrap_write(int fd, const void *buf, size_t n);
int __wrap_fdatasync(int fd);
int __wrap_fsync(int fd);
int __wrap_ftruncate(int fd, off_t len);
int __wrap_fstat(int fd, struct stat *st);
int __wrap_stat(const char *path, struct stat *st);
int __wrap_renameat2(int olddir, const char *from, int newdir, const char *to,
                     unsigned flags);
int __wrap_link(const char *from, const char *to);
int __wrap_unlink(const char *path);
void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t off);
}

namespace {

using bytecask::testing::SyscallFault;

// Which modes a call takes part in.
enum class Kind {
  read,    // pread: before, short_io
  write,   // pwrite, pwritev, write: before, after, short_io
  change,  // fdatasync, fsync, ftruncate, renameat2, link, unlink: before, after
  other,   // open, fstat, stat, mmap: before
};

enum class Verdict { pass, fail };

struct State {
  std::mutex mu;
  // The directory as the test named it and as the kernel names it; a path
  // argument is matched against both, a descriptor's link against the second.
  std::string dir;
  std::string real_dir;
  SyscallFault mode{SyscallFault::none};
  int nth{0};
  bool cascade{false};
  int calls{0};
  bool fired{false};
  std::string what;
};

// Read on every wrapped call, from any thread, before main and after exit.
constinit std::atomic<bool> armed{false};

// Leaked on purpose: a static with a destructor would be torn down while a
// detached engine thread could still call through a wrapper.
auto state() -> State & {
  static auto *s = new State;
  return *s;
}

auto under(std::string_view path, std::string_view dir) -> bool {
  return !dir.empty() && path.starts_with(dir) &&
         (path.size() == dir.size() || path[dir.size()] == '/');
}

auto fd_path(int fd) -> std::string {
  if (fd < 0) return {};
  const auto link = "/proc/self/fd/" + std::to_string(fd);
  std::string out(4096, '\0');
  const auto n = ::readlink(link.c_str(), out.data(), out.size());
  out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
  return out;
}

auto counts(Kind kind, SyscallFault mode) -> bool {
  switch (mode) {
  case SyscallFault::none:
  case SyscallFault::before:
    return true;
  case SyscallFault::after:
    return kind == Kind::write || kind == Kind::change;
  case SyscallFault::short_io:
    return kind == Kind::read || kind == Kind::write;
  }
  return false;
}

// Counts the call if it is the sweep's to count, and says whether this one
// is to fail. `path` is empty for a call that takes a descriptor.
auto decide(const char *call, Kind kind, const char *path, int fd)
    -> std::pair<Verdict, SyscallFault> {
  if (!armed.load(std::memory_order_acquire) ||
      bytecask::testing::syscall_faults_suspended > 0) {
    return {Verdict::pass, SyscallFault::none};
  }
  auto &s = state();
  const std::lock_guard<std::mutex> lk{s.mu};
  if (!armed.load(std::memory_order_relaxed) || !counts(kind, s.mode)) {
    return {Verdict::pass, SyscallFault::none};
  }
  const auto target = path != nullptr ? std::string{path} : fd_path(fd);
  const auto ours = path != nullptr
                        ? under(target, s.dir) || under(target, s.real_dir)
                        : under(target, s.real_dir);
  if (!ours) return {Verdict::pass, SyscallFault::none};

  ++s.calls;
  if (s.mode == SyscallFault::none) return {Verdict::pass, SyscallFault::none};
  if (s.calls != s.nth && !(s.cascade && s.calls > s.nth)) {
    return {Verdict::pass, SyscallFault::none};
  }
  if (!s.fired) {
    s.fired = true;
    s.what = std::string{call} + "(" + target + ")";
  }
  return {Verdict::fail, s.mode};
}

auto halve(std::size_t n) -> std::size_t { return n > 1 ? n / 2 : n; }

// A sync the device never saw leaves its pages clean and unwritten, as the
// kernel does (PageCacheModel); one that failed after it ran wrote them.
void model_sync(int fd, bool reached_device) {
  const bytecask::testing::SuspendSyscallFaults uncounted;
  auto *model = bytecask::testing::active_cache_model;
  if (model == nullptr) return;
  if (reached_device) {
    model->synced(fd);
  } else {
    model->sync_failed(fd);
  }
}

template <typename R, typename Real>
auto fail_or_run(std::pair<Verdict, SyscallFault> d, R failure, int err,
                 Real real) -> R {
  if (d.first == Verdict::pass) return real();
  if (d.second == SyscallFault::after) (void)real();
  errno = err;
  return failure;
}

}  // namespace

namespace bytecask::testing {

ScopedSyscallFaults::ScopedSyscallFaults(const std::filesystem::path &dir,
                                         SyscallFault mode, int nth,
                                         bool cascade) {
  std::error_code ec;
  auto real = std::filesystem::weakly_canonical(dir, ec);
  if (ec) real = dir;
  auto &s = state();
  const std::lock_guard<std::mutex> lk{s.mu};
  s.dir = dir.string();
  s.real_dir = real.string();
  s.mode = mode;
  s.nth = nth;
  s.cascade = cascade;
  s.calls = 0;
  s.fired = false;
  s.what.clear();
  armed.store(true, std::memory_order_release);
}

ScopedSyscallFaults::~ScopedSyscallFaults() { (void)report(); }

auto ScopedSyscallFaults::report() -> SyscallFaultReport {
  auto &s = state();
  const std::lock_guard<std::mutex> lk{s.mu};
  armed.store(false, std::memory_order_release);
  return {.calls = s.calls, .fired = s.fired, .what = s.what};
}

}  // namespace bytecask::testing

extern "C" {

int __wrap_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = static_cast<mode_t>(va_arg(ap, int));
    va_end(ap);
  }
  return fail_or_run(decide("open", Kind::other, path, -1), -1, EIO,
                     [&] { return __real_open(path, flags, mode); });
}

ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off) {
  const auto d = decide("pread", Kind::read, nullptr, fd);
  if (d.first == Verdict::fail && d.second == SyscallFault::short_io) {
    return __real_pread(fd, buf, halve(n), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pread(fd, buf, n, off); });
}

ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off) {
  const auto d = decide("pwrite", Kind::write, nullptr, fd);
  if (d.first == Verdict::fail && d.second == SyscallFault::short_io) {
    return __real_pwrite(fd, buf, halve(n), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pwrite(fd, buf, n, off); });
}

ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off) {
  const auto d = decide("pwritev", Kind::write, nullptr, fd);
  if (d.first == Verdict::fail && d.second == SyscallFault::short_io) {
    // The first half of the bytes, cut wherever that falls in the vector.
    std::size_t total = 0;
    for (int i = 0; i < cnt; ++i) total += iov[i].iov_len;
    auto keep = halve(total);
    std::vector<struct iovec> cut;
    for (int i = 0; i < cnt && keep > 0; ++i) {
      const auto len = std::min(keep, iov[i].iov_len);
      cut.push_back({iov[i].iov_base, len});
      keep -= len;
    }
    return __real_pwritev(fd, cut.data(), static_cast<int>(cut.size()), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pwritev(fd, iov, cnt, off); });
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
  const auto d = decide("write", Kind::write, nullptr, fd);
  if (d.first == Verdict::fail && d.second == SyscallFault::short_io) {
    return __real_write(fd, buf, halve(n));
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_write(fd, buf, n); });
}

int __wrap_fdatasync(int fd) {
  const auto d = decide("fdatasync", Kind::change, nullptr, fd);
  if (d.first == Verdict::fail) model_sync(fd, d.second == SyscallFault::after);
  return fail_or_run(d, -1, EIO, [&] { return __real_fdatasync(fd); });
}

int __wrap_fsync(int fd) {
  const auto d = decide("fsync", Kind::change, nullptr, fd);
  if (d.first == Verdict::fail) model_sync(fd, d.second == SyscallFault::after);
  return fail_or_run(d, -1, EIO, [&] { return __real_fsync(fd); });
}

int __wrap_ftruncate(int fd, off_t len) {
  return fail_or_run(decide("ftruncate", Kind::change, nullptr, fd), -1, EIO,
                     [&] { return __real_ftruncate(fd, len); });
}

int __wrap_fstat(int fd, struct stat *st) {
  return fail_or_run(decide("fstat", Kind::other, nullptr, fd), -1, EIO,
                     [&] { return __real_fstat(fd, st); });
}

int __wrap_stat(const char *path, struct stat *st) {
  return fail_or_run(decide("stat", Kind::other, path, -1), -1, EIO,
                     [&] { return __real_stat(path, st); });
}

int __wrap_renameat2(int olddir, const char *from, int newdir, const char *to,
                     unsigned flags) {
  return fail_or_run(
      decide("renameat2", Kind::change, from, -1), -1, EIO,
      [&] { return __real_renameat2(olddir, from, newdir, to, flags); });
}

int __wrap_link(const char *from, const char *to) {
  return fail_or_run(decide("link", Kind::change, from, -1), -1, EIO,
                     [&] { return __real_link(from, to); });
}

int __wrap_unlink(const char *path) {
  return fail_or_run(decide("unlink", Kind::change, path, -1), -1, EIO,
                     [&] { return __real_unlink(path); });
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t off) {
  return fail_or_run(
      decide("mmap", Kind::other, nullptr, fd), MAP_FAILED, ENOMEM,
      [&] { return __real_mmap(addr, len, prot, flags, fd, off); });
}

}  // extern "C"
