// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// A model of what the kernel's page cache makes durable, for tests that cut
// power. Compiled only when BYTECASK_TESTING is defined, and inert until a
// ScopedPageCacheModel enables it.
//
// A real test run never loses power, and an injected fdatasync failure fails
// before the syscall, so the bytes stay dirty and the next sync persists them.
// The model tracks what a power cut would have taken instead:
//
//   - Bytes appended since a file's last successful fdatasync are dirty; a
//     power cut loses them, and any later successful fdatasync persists them.
//   - A failed fdatasync makes the dirty bytes *lost*: Linux (since 4.13)
//     marks the pages it failed to write clean, so later fdatasyncs return 0
//     without writing them ("fsyncgate"). Only writing the range again and
//     then syncing it successfully makes it durable.
//
// power_cut() applies the model to a directory: it zeroes every lost or dirty
// range of every data file the model tracks. Files are keyed by inode, so a
// file reopened through another descriptor or path is the same file.

#pragma once

#ifdef BYTECASK_TESTING

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bytecask::testing {

class PageCacheModel {
public:
  using Offset = std::uint64_t;

  [[nodiscard]] auto enabled() const noexcept -> bool {
    return enabled_.load(std::memory_order_acquire);
  }

  void enable() {
    std::lock_guard<std::mutex> lk{mu_};
    files_.clear();
    enabled_.store(true, std::memory_order_release);
  }

  void disable() {
    std::lock_guard<std::mutex> lk{mu_};
    enabled_.store(false, std::memory_order_release);
    files_.clear();
  }

  // fd was opened for writing with size bytes in it; they count as durable.
  // Resets whatever an earlier file with the same inode left behind.
  void opened_for_write(int fd, Offset size) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    f = File{};
    f.synced_end = size;
  }

  // fdatasync on fd returned 0 with the file's bytes ending at end.
  void sync_ok(int fd, Offset end) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    for (const auto &[from, to] : f.rewritten) subtract(f.lost, from, to);
    f.rewritten.clear();
    f.synced_end = end;
  }

  // fdatasync on fd failed with the file's bytes ending at end.
  void sync_failed(int fd, Offset end) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    if (end > f.synced_end) f.lost.emplace_back(f.synced_end, end);
    f.rewritten.clear();
    f.synced_end = end;
  }

  // [from, to) was written again; the next successful sync makes it durable.
  void rewritten(int fd, Offset from, Offset to) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk{mu_};
    files_[key_of(fd)].rewritten.emplace_back(from, to);
  }

  void truncated(int fd, Offset size) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    subtract(f.lost, size, UINT64_MAX);
    subtract(f.rewritten, size, UINT64_MAX);
    f.synced_end = std::min(f.synced_end, size);
  }

  // What a process crash leaves: a copy of dir as the page cache holds it,
  // with the model's view of each data file carried over to its copy. The
  // hint worker may rename a file away mid-copy; a crash can land on either
  // side of a rename, so a file that is gone is left out.
  void crash_copy(const std::filesystem::path &dir,
                  const std::filesystem::path &copy) {
    std::filesystem::create_directories(copy);
    std::lock_guard<std::mutex> lk{mu_};
    for (const auto &e : std::filesystem::directory_iterator{dir}) {
      std::error_code ec;
      if (!e.is_regular_file(ec)) continue;
      const auto to = copy / e.path().filename();
      std::filesystem::copy_file(e.path(), to, ec);
      if (ec == std::errc::no_such_file_or_directory) continue;
      if (ec) throw std::filesystem::filesystem_error{"crash_copy", e.path(), ec};
      if (e.path().extension() != ".data") continue;
      const auto it = files_.find(key_of(e.path()));
      if (it != files_.end()) files_[key_of(to)] = it->second;
    }
  }

  // Memory pressure evicts the clean pages a failed fdatasync left: reads of
  // every lost range in dir return the device's bytes. The ranges stay lost
  // — what a read now returns is what a power cut would leave anyway.
  void evict_lost(const std::filesystem::path &dir) {
    std::lock_guard<std::mutex> lk{mu_};
    for (const auto &e : std::filesystem::directory_iterator{dir}) {
      if (e.path().extension() != ".data") continue;
      const auto it = files_.find(key_of(e.path()));
      if (it != files_.end()) zero(e.path(), it->second.lost);
    }
  }

  // Zeroes what a power cut would take from every data file in dir.
  void power_cut(const std::filesystem::path &dir) {
    std::lock_guard<std::mutex> lk{mu_};
    for (const auto &e : std::filesystem::directory_iterator{dir}) {
      if (e.path().extension() != ".data") continue;
      const auto it = files_.find(key_of(e.path()));
      if (it == files_.end()) continue;
      auto ranges = it->second.lost;
      ranges.emplace_back(it->second.synced_end, UINT64_MAX);
      zero(e.path(), ranges);
      files_.erase(it);
    }
  }

private:
  using Range = std::pair<Offset, Offset>;
  using Key = std::pair<std::uint64_t, std::uint64_t>;  // (st_dev, st_ino)

  struct File {
    Offset synced_end{0};
    std::vector<Range> lost;
    std::vector<Range> rewritten;
  };

  static auto key_of(int fd) -> Key {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "PageCacheModel: fstat"};
    }
    return {st.st_dev, st.st_ino};
  }

  static auto key_of(const std::filesystem::path &p) -> Key {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "PageCacheModel: stat"};
    }
    return {st.st_dev, st.st_ino};
  }

  // Removes [from, to) from every range in rs.
  static void subtract(std::vector<Range> &rs, Offset from, Offset to) {
    std::vector<Range> out;
    for (const auto &[a, b] : rs) {
      if (b <= from || a >= to) {
        out.emplace_back(a, b);
        continue;
      }
      if (a < from) out.emplace_back(a, from);
      if (b > to) out.emplace_back(to, b);
    }
    rs = std::move(out);
  }

  static void zero(const std::filesystem::path &p,
                   const std::vector<Range> &ranges) {
    const auto size = std::filesystem::file_size(p);
    const auto fd = ::open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{errno, std::generic_category(),
                              "PageCacheModel: open"};
    }
    const std::vector<char> zeros(64 * 1024, 0);
    for (const auto &[a, b] : ranges) {
      for (auto off = a; off < std::min<Offset>(b, size);) {
        const auto n = std::min<Offset>(zeros.size(), std::min(b, size) - off);
        if (::pwrite(fd, zeros.data(), n, static_cast<off_t>(off)) !=
            static_cast<ssize_t>(n)) {
          ::close(fd);
          throw std::system_error{errno, std::generic_category(),
                                  "PageCacheModel: pwrite"};
        }
        off += n;
      }
    }
    ::close(fd);
  }

  std::atomic<bool> enabled_{false};
  std::mutex mu_;
  std::map<Key, File> files_;
};

// One model per process: an fdatasync can run on any thread.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunique-object-duplication"
inline auto page_cache_model() -> PageCacheModel & {
  static auto *const model = new PageCacheModel;
  return *model;
}
#pragma clang diagnostic pop

// Enables the model for a test's scope.
struct ScopedPageCacheModel {
  ScopedPageCacheModel() { page_cache_model().enable(); }
  ~ScopedPageCacheModel() { page_cache_model().disable(); }
  ScopedPageCacheModel(const ScopedPageCacheModel &) = delete;
  ScopedPageCacheModel &operator=(const ScopedPageCacheModel &) = delete;
  ScopedPageCacheModel(ScopedPageCacheModel &&) = delete;
  ScopedPageCacheModel &operator=(ScopedPageCacheModel &&) = delete;
};

} // namespace bytecask::testing

#endif // BYTECASK_TESTING
