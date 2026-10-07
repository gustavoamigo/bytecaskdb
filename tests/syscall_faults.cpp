// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The --wrap interposers behind syscall_faults.h. The linker turns every
// reference to `open` in this binary's objects into `__wrap_open`, and
// `__real_open` into the real symbol — under a sanitizer, its interceptor.
// The C++ standard library is linked statically (xmake.lua), so its objects
// are among them: std::filesystem's rename, remove, stat, … come here too.
// A libc function that calls another inside libc (remove calls unlink,
// realpath lstat, fopen open) does so through an internal alias --wrap
// cannot see, so the public function has an interposer of its own.

#include "syscall_faults.h"

#include "fault_injector.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/sendfile.h>
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
int __real_openat(int dir, const char *path, int flags, ...);
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __real_writev(int fd, const struct iovec *iov, int cnt);
ssize_t __real_sendfile(int out, int in, off_t *off, size_t n);
ssize_t __real_copy_file_range(int in, off_t *in_off, int out, off_t *out_off,
                               size_t n, unsigned flags);
int __real_truncate(const char *path, off_t len);
int __real_lstat(const char *path, struct stat *st);
int __real_rename(const char *from, const char *to);
int __real_remove(const char *path);
int __real_unlinkat(int dir, const char *path, int flags);
int __real_mkdir(const char *path, mode_t mode);
int __real_symlink(const char *target, const char *path);
int __real_utimensat(int dir, const char *path, const struct timespec *times,
                     int flags);
int __real_fchmod(int fd, mode_t mode);
int __real_fchmodat(int dir, const char *path, mode_t mode, int flags);
char *__real_realpath(const char *path, char *out);
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *d);
FILE *__real_fopen(const char *path, const char *mode);
FILE *__real_freopen(const char *path, const char *mode, FILE *f);

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
int __wrap_openat(int dir, const char *path, int flags, ...);
ssize_t __wrap_read(int fd, void *buf, size_t n);
ssize_t __wrap_writev(int fd, const struct iovec *iov, int cnt);
ssize_t __wrap_sendfile(int out, int in, off_t *off, size_t n);
ssize_t __wrap_copy_file_range(int in, off_t *in_off, int out, off_t *out_off,
                               size_t n, unsigned flags);
int __wrap_truncate(const char *path, off_t len);
int __wrap_lstat(const char *path, struct stat *st);
int __wrap_rename(const char *from, const char *to);
int __wrap_remove(const char *path);
int __wrap_unlinkat(int dir, const char *path, int flags);
int __wrap_mkdir(const char *path, mode_t mode);
int __wrap_symlink(const char *target, const char *path);
int __wrap_utimensat(int dir, const char *path, const struct timespec *times,
                     int flags);
int __wrap_fchmod(int fd, mode_t mode);
int __wrap_fchmodat(int dir, const char *path, mode_t mode, int flags);
char *__wrap_realpath(const char *path, char *out);
DIR *__wrap_opendir(const char *path);
struct dirent *__wrap_readdir(DIR *d);
FILE *__wrap_fopen(const char *path, const char *mode);
FILE *__wrap_freopen(const char *path, const char *mode, FILE *f);
}

namespace {

using bytecask::testing::SyscallFault;

// Which modes a call takes part in.
enum class Kind {
  read,    // pread, read: before, short_io
  write,   // pwrite, pwritev, write, writev, sendfile, copy_file_range:
           // before, after, short_io
  change,  // fdatasync, fsync, ftruncate, truncate, renameat2, rename, link,
           // unlink, unlinkat, remove, mkdir, symlink, utimensat, fchmod,
           // fchmodat: before, after
  other,   // open, openat, fopen, freopen, fstat, stat, lstat, realpath,
           // opendir, readdir, mmap: before
};

enum class Verdict { pass, fail };

// What decide() ruled for one call: whether to fail it, in which mode, and
// with which errno (0: the call's own default).
struct Ruling {
  Verdict verdict{Verdict::pass};
  SyscallFault mode{SyscallFault::none};
  int err{0};
};

struct State {
  std::mutex mu;
  // The directory as the test named it and as the kernel names it; a path
  // argument is matched against both, a descriptor's link against the second.
  std::string dir;
  std::string real_dir;
  SyscallFault mode{SyscallFault::none};
  int nth{0};
  bool cascade{false};
  int err{0};
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
// is to fail. The call names its file by `path`, by the descriptor `fd`
// (`path` null), or by `path` relative to the directory descriptor `fd`
// (the *at calls; AT_FDCWD or -1 for none).
auto decide(const char *call, Kind kind, const char *path, int fd)
    -> Ruling {
  if (!armed.load(std::memory_order_acquire) ||
      bytecask::testing::syscall_faults_suspended > 0) {
    return {};
  }
  auto &s = state();
  const std::lock_guard<std::mutex> lk{s.mu};
  if (!armed.load(std::memory_order_relaxed) || !counts(kind, s.mode)) {
    return {};
  }
  // A path the caller wrote is matched against the directory as the test
  // named it too; one built from a descriptor's link is the kernel's.
  const auto as_written = path != nullptr && (path[0] == '/' || fd < 0);
  const auto target = path == nullptr ? fd_path(fd)
                      : as_written    ? std::string{path}
                                      : fd_path(fd) + "/" + path;
  const auto ours = under(target, s.real_dir) ||
                    (as_written && under(target, s.dir));
  if (!ours) return {};

  ++s.calls;
  if (s.mode == SyscallFault::none) return {};
  if (s.calls != s.nth && !(s.cascade && s.calls > s.nth)) {
    return {};
  }
  if (!s.fired) {
    s.fired = true;
    s.what = std::string{call} + "(" + target + ")";
  }
  return {Verdict::fail, s.mode, s.err};
}

auto halve(std::size_t n) -> std::size_t { return n > 1 ? n / 2 : n; }

// The first half of a vector's bytes, cut wherever that falls in it.
auto halve(const struct iovec *iov, int cnt) -> std::vector<struct iovec> {
  std::size_t total = 0;
  for (int i = 0; i < cnt; ++i) total += iov[i].iov_len;
  auto keep = halve(total);
  std::vector<struct iovec> cut;
  for (int i = 0; i < cnt && keep > 0; ++i) {
    const auto len = std::min(keep, iov[i].iov_len);
    cut.push_back({iov[i].iov_base, len});
    keep -= len;
  }
  return cut;
}

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
auto fail_or_run(Ruling d, R failure, int err, Real real) -> R {
  if (d.verdict == Verdict::pass) return real();
  if (d.mode == SyscallFault::after) (void)real();
  errno = d.err != 0 ? d.err : err;
  return failure;
}

}  // namespace

namespace bytecask::testing {

ScopedSyscallFaults::ScopedSyscallFaults(const std::filesystem::path &dir,
                                         SyscallFault mode, int nth,
                                         bool cascade, int err) {
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
  s.err = err;
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
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_pread(fd, buf, halve(n), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pread(fd, buf, n, off); });
}

ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off) {
  const auto d = decide("pwrite", Kind::write, nullptr, fd);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_pwrite(fd, buf, halve(n), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pwrite(fd, buf, n, off); });
}

ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off) {
  const auto d = decide("pwritev", Kind::write, nullptr, fd);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    const auto cut = halve(iov, cnt);
    return __real_pwritev(fd, cut.data(), static_cast<int>(cut.size()), off);
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_pwritev(fd, iov, cnt, off); });
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
  const auto d = decide("write", Kind::write, nullptr, fd);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_write(fd, buf, halve(n));
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_write(fd, buf, n); });
}

int __wrap_fdatasync(int fd) {
  const auto d = decide("fdatasync", Kind::change, nullptr, fd);
  if (d.verdict == Verdict::fail) model_sync(fd, d.mode == SyscallFault::after);
  return fail_or_run(d, -1, EIO, [&] { return __real_fdatasync(fd); });
}

int __wrap_fsync(int fd) {
  const auto d = decide("fsync", Kind::change, nullptr, fd);
  if (d.verdict == Verdict::fail) model_sync(fd, d.mode == SyscallFault::after);
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

int __wrap_openat(int dir, const char *path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = static_cast<mode_t>(va_arg(ap, int));
    va_end(ap);
  }
  return fail_or_run(decide("openat", Kind::other, path, dir), -1, EIO,
                     [&] { return __real_openat(dir, path, flags, mode); });
}

ssize_t __wrap_read(int fd, void *buf, size_t n) {
  const auto d = decide("read", Kind::read, nullptr, fd);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_read(fd, buf, halve(n));
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_read(fd, buf, n); });
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int cnt) {
  const auto d = decide("writev", Kind::write, nullptr, fd);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    const auto cut = halve(iov, cnt);
    return __real_writev(fd, cut.data(), static_cast<int>(cut.size()));
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_writev(fd, iov, cnt); });
}

ssize_t __wrap_sendfile(int out, int in, off_t *off, size_t n) {
  const auto d = decide("sendfile", Kind::write, nullptr, out);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_sendfile(out, in, off, halve(n));
  }
  return fail_or_run(d, ssize_t{-1}, EIO,
                     [&] { return __real_sendfile(out, in, off, n); });
}

ssize_t __wrap_copy_file_range(int in, off_t *in_off, int out, off_t *out_off,
                               size_t n, unsigned flags) {
  const auto d = decide("copy_file_range", Kind::write, nullptr, out);
  if (d.verdict == Verdict::fail && d.mode == SyscallFault::short_io) {
    return __real_copy_file_range(in, in_off, out, out_off, halve(n), flags);
  }
  return fail_or_run(d, ssize_t{-1}, EIO, [&] {
    return __real_copy_file_range(in, in_off, out, out_off, n, flags);
  });
}

int __wrap_truncate(const char *path, off_t len) {
  return fail_or_run(decide("truncate", Kind::change, path, -1), -1, EIO,
                     [&] { return __real_truncate(path, len); });
}

int __wrap_lstat(const char *path, struct stat *st) {
  return fail_or_run(decide("lstat", Kind::other, path, -1), -1, EIO,
                     [&] { return __real_lstat(path, st); });
}

int __wrap_rename(const char *from, const char *to) {
  return fail_or_run(decide("rename", Kind::change, from, -1), -1, EIO,
                     [&] { return __real_rename(from, to); });
}

int __wrap_remove(const char *path) {
  return fail_or_run(decide("remove", Kind::change, path, -1), -1, EIO,
                     [&] { return __real_remove(path); });
}

int __wrap_unlinkat(int dir, const char *path, int flags) {
  return fail_or_run(decide("unlinkat", Kind::change, path, dir), -1, EIO,
                     [&] { return __real_unlinkat(dir, path, flags); });
}

int __wrap_mkdir(const char *path, mode_t mode) {
  return fail_or_run(decide("mkdir", Kind::change, path, -1), -1, EIO,
                     [&] { return __real_mkdir(path, mode); });
}

// Counted by the link it creates, not by what the link points at.
int __wrap_symlink(const char *target, const char *path) {
  return fail_or_run(decide("symlink", Kind::change, path, -1), -1, EIO,
                     [&] { return __real_symlink(target, path); });
}

// A null path names the descriptor itself.
int __wrap_utimensat(int dir, const char *path, const struct timespec *times,
                     int flags) {
  return fail_or_run(decide("utimensat", Kind::change, path, dir), -1, EIO,
                     [&] { return __real_utimensat(dir, path, times, flags); });
}

int __wrap_fchmod(int fd, mode_t mode) {
  return fail_or_run(decide("fchmod", Kind::change, nullptr, fd), -1, EIO,
                     [&] { return __real_fchmod(fd, mode); });
}

int __wrap_fchmodat(int dir, const char *path, mode_t mode, int flags) {
  return fail_or_run(decide("fchmodat", Kind::change, path, dir), -1, EIO,
                     [&] { return __real_fchmodat(dir, path, mode, flags); });
}

char *__wrap_realpath(const char *path, char *out) {
  return fail_or_run(decide("realpath", Kind::other, path, -1),
                     static_cast<char *>(nullptr), EIO,
                     [&] { return __real_realpath(path, out); });
}

DIR *__wrap_opendir(const char *path) {
  return fail_or_run(decide("opendir", Kind::other, path, -1),
                     static_cast<DIR *>(nullptr), EIO,
                     [&] { return __real_opendir(path); });
}

struct dirent *__wrap_readdir(DIR *d) {
  return fail_or_run(decide("readdir", Kind::other, nullptr, ::dirfd(d)),
                     static_cast<struct dirent *>(nullptr), EIO,
                     [&] { return __real_readdir(d); });
}

// Only the open is counted: the stream's reads and writes run inside libc.
FILE *__wrap_fopen(const char *path, const char *mode) {
  return fail_or_run(decide("fopen", Kind::other, path, -1),
                     static_cast<FILE *>(nullptr), EIO,
                     [&] { return __real_fopen(path, mode); });
}

// A null path reopens the stream's own file.
FILE *__wrap_freopen(const char *path, const char *mode, FILE *f) {
  const auto d = path != nullptr
                     ? decide("freopen", Kind::other, path, -1)
                     : decide("freopen", Kind::other, nullptr, ::fileno(f));
  return fail_or_run(d, static_cast<FILE *>(nullptr), EIO,
                     [&] { return __real_freopen(path, mode, f); });
}

}  // extern "C"
