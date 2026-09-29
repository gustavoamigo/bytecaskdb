// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Fault injection infrastructure for correctness validation.
// Compiled only when BYTECASK_TESTING is defined.
// Never included directly — guarded by #ifdef BYTECASK_TESTING
// at every call site.

#pragma once

#ifdef BYTECASK_TESTING

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bytecask::testing {

// ---------------------------------------------------------------------------
// PostWriteMode
//
// Controls what happens at a post-write checkpoint (after writev succeeds).
//
//   none        — no injection (default)
//   short_write — ftruncate to simulate partial writev, then throw
//   throw_after — throw without truncating (full entry on disk)
// ---------------------------------------------------------------------------
enum class PostWriteMode { none, short_write, throw_after };

// ---------------------------------------------------------------------------
// FaultInjector
//
// Controls when and where a fault is injected. Two injection modes:
//
//   By name  — fail at the first checkpoint matching fail_at_name
//   By count — fail when call_count exceeds fail_at
//
// Name-based injection takes priority if both are set.
//
// Post-write mode (short_write/throw_after) fires at FAULT_INJECTION_POST_WRITE
// checkpoints matching fail_at_name. It does not use count-based triggering.
//
// Thread-local: each thread has its own active injector so concurrent
// tests do not interfere with each other.
// ---------------------------------------------------------------------------
struct FaultInjector {
  std::string fail_at_name;    // fail at this named checkpoint
  int         fail_at    = -1; // fail after this many checkpoints (-1 = never)
  int         call_count = 0;  // number of checkpoints passed so far
  std::string last_checkpoint; // name of the last checkpoint that fired

  // Count-based skip set — checkpoints matching these names pass even
  // when call_count exceeds fail_at. Allows testing scenarios where some
  // operations in a cascade succeed while others fail.
  std::vector<std::string> skip_names;

  std::error_code error = std::make_error_code(std::errc::io_error);

  PostWriteMode post_write_mode{PostWriteMode::none};
  ssize_t       short_write_bytes{0};   // bytes to keep for short_write mode

  void checkpoint(const char* name) {
    last_checkpoint = name;
    ++call_count;

    // Name-based — targets a specific fault point explicitly
    if (!fail_at_name.empty() && fail_at_name == name) {
      throw std::system_error{error,
          std::string{"fault injection at: "} + name};
    }

    // Count-based — targets the Nth checkpoint in sequence
    if (fail_at >= 0 && call_count > fail_at
        && std::find(skip_names.begin(), skip_names.end(), name)
               == skip_names.end()) {
      throw std::system_error{error,
          std::string{"fault injection at: "} + name};
    }
  }
};

// Thread-local active injector.
// Set to non-null before a test scenario, reset to null after.
// Use ScopedFaultInjector to ensure cleanup even if the test throws.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunique-object-duplication"
inline thread_local FaultInjector* active_injector = nullptr;
#pragma clang diagnostic pop

// Called at every IO boundary via the FAULT_INJECTION macro.
inline void io_checkpoint(const char* name) {
  if (active_injector) active_injector->checkpoint(name);
}

// Called at post-write IO boundaries via FAULT_INJECTION_POST_WRITE.
// Does NOT increment call_count — the pre-write checkpoint already did.
// Fires only when post_write_mode != none AND fail_at_name matches.
inline void io_post_write_checkpoint(const char* name, int fd,
                                      std::uint64_t offset,
                                      std::size_t /*total*/) {
  if (!active_injector) return;
  auto& inj = *active_injector;
  inj.last_checkpoint = name;

  if (inj.post_write_mode == PostWriteMode::none) return;
  if (inj.fail_at_name != name) return;

  if (inj.post_write_mode == PostWriteMode::short_write) {
    // Truncate to simulate partial writev return. The actual writev
    // already succeeded; we rewrite history to look like a short write.
    (void)::ftruncate(fd, static_cast<off_t>(offset + static_cast<std::uint64_t>(inj.short_write_bytes)));
  }
  // For throw_after: data stays fully on disk — simulates writev
  // success followed by a failure before offset_ advances.
  throw std::system_error{inj.error,
      std::string{"fault injection (post-write) at: "} + name};
}

// ---------------------------------------------------------------------------
// ScopedFaultInjector
//
// RAII guard that activates an injector and resets active_injector
// on destruction — even if the test body throws.
// ---------------------------------------------------------------------------
struct ScopedFaultInjector {
  FaultInjector inj;

  // Name-based injection
  explicit ScopedFaultInjector(std::string name) {
    inj.fail_at_name = std::move(name);
    active_injector = &inj;
  }

  // Count-based injection
  explicit ScopedFaultInjector(int fail_at) {
    inj.fail_at = fail_at;
    active_injector = &inj;
  }

  // Count-based injection with skip set — cascade failures but let
  // named checkpoints in the skip set pass through.
  ScopedFaultInjector(int fail_at, std::initializer_list<std::string> skip) {
    inj.fail_at = fail_at;
    inj.skip_names = {skip.begin(), skip.end()};
    active_injector = &inj;
  }

  // Name-based with post-write mode
  ScopedFaultInjector(std::string name, PostWriteMode mode,
                      ssize_t short_bytes = 0) {
    inj.fail_at_name = std::move(name);
    inj.post_write_mode = mode;
    inj.short_write_bytes = short_bytes;
    active_injector = &inj;
  }

  ~ScopedFaultInjector() {
    active_injector = nullptr;
  }

  ScopedFaultInjector(const ScopedFaultInjector&) = delete;
  ScopedFaultInjector& operator=(const ScopedFaultInjector&) = delete;
  ScopedFaultInjector(ScopedFaultInjector&&) = delete;
  ScopedFaultInjector& operator=(ScopedFaultInjector&&) = delete;
};

// ---------------------------------------------------------------------------
// PageCacheModel
//
// Models which bytes of a data file the device holds, so a test can cut the
// power after a failed fdatasync with Linux semantics (fsyncgate): the failed
// flush marks the pages it covered clean without writing them, and a later
// fdatasync, finding nothing dirty, returns 0 without writing them either.
//
// Per 4 KiB page it keeps the device's bytes while the page cache holds
// others. A write to a page records the page as it reads before the write,
// unless an image is already kept, and marks it dirty. A successful sync
// forgets the images of the dirty pages; a failed one only clears their dirty
// mark, so a sync after it keeps them too. Only a new write makes the page
// dirty again. Hint files are not modelled: they are written durably.
//
// Process-wide, not thread-local: the flush leader and the background hint
// worker are not always the test's thread.
// ---------------------------------------------------------------------------
class PageCacheModel {
public:
  static constexpr std::uint64_t kPage = 4096;

  void write(int fd, std::uint64_t offset, std::size_t len) {
    if (len == 0) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    for (auto p = offset / kPage; p <= (offset + len - 1) / kPage; ++p) {
      if (!f.image.contains(p)) {
        std::vector<std::byte> page(kPage, std::byte{0});
        (void)::pread(fd, page.data(), kPage, static_cast<off_t>(p * kPage));
        f.image.emplace(p, std::move(page));
      }
      f.dirty.insert(p);
    }
  }

  void synced(int fd) {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = files_.find(key_of(fd));
    if (it == files_.end()) return;
    for (const auto p : it->second.dirty) it->second.image.erase(p);
    it->second.dirty.clear();
  }

  void sync_failed(int fd) {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = files_.find(key_of(fd));
    if (it != files_.end()) it->second.dirty.clear();
  }

  // Treats every byte of path as written but never synced, over a device
  // that holds zeros: a process that appended with sync=false and was
  // killed, its pages still in the cache.
  void mark_unsynced(const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);
    const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) throw std::system_error{errno, std::generic_category()};
    std::lock_guard<std::mutex> lk{mu_};
    auto &f = files_[key_of(fd)];
    ::close(fd);
    for (std::uint64_t p = 0; p * kPage < size; ++p)
      f.image[p] = std::vector<std::byte>(kPage, std::byte{0});
  }

  // Returns the file what the device holds: after a power loss, or once the
  // kernel has evicted pages that a failed fdatasync marked clean. Every
  // image goes back, clamped to the file's current size, and is forgotten.
  // Call it with no engine writing.
  void restore_device(const std::filesystem::path &dir) {
    std::lock_guard<std::mutex> lk{mu_};
    for (const auto &e : std::filesystem::directory_iterator{dir}) {
      if (!e.is_regular_file()) continue;
      const auto fd = ::open(e.path().c_str(), O_WRONLY | O_CLOEXEC);
      if (fd == -1) continue;
      auto it = files_.find(key_of(fd));
      if (it != files_.end()) {
        const auto size = static_cast<std::uint64_t>(
            std::filesystem::file_size(e.path()));
        for (const auto &[p, bytes] : it->second.image) {
          const auto at = p * kPage;
          if (at >= size) continue;
          const auto n = std::min<std::uint64_t>(kPage, size - at);
          (void)::pwrite(fd, bytes.data(), n, static_cast<off_t>(at));
        }
        (void)::fdatasync(fd);
        files_.erase(it);
      }
      ::close(fd);
    }
  }

  // Pages whose bytes the device does not hold, over every file.
  [[nodiscard]] auto undurable_pages() -> std::size_t {
    std::lock_guard<std::mutex> lk{mu_};
    std::size_t n = 0;
    for (const auto &[k, f] : files_) n += f.image.size();
    return n;
  }

private:
  struct FileState {
    std::map<std::uint64_t, std::vector<std::byte>> image;
    std::set<std::uint64_t> dirty;
  };
  static auto key_of(int fd) -> std::pair<dev_t, ino_t> {
    struct stat st{};
    if (::fstat(fd, &st) != 0) throw std::system_error{errno, std::generic_category()};
    return {st.st_dev, st.st_ino};
  }
  std::mutex mu_;
  std::map<std::pair<dev_t, ino_t>, FileState> files_;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunique-object-duplication"
inline PageCacheModel* active_cache_model = nullptr;
#pragma clang diagnostic pop

inline void io_cache_write(int fd, std::uint64_t offset, std::size_t len) {
  if (active_cache_model) active_cache_model->write(fd, offset, len);
}

inline void io_cache_synced(int fd) {
  if (active_cache_model) active_cache_model->synced(fd);
}

// A sync's checkpoint. An injected failure there is an fdatasync that
// failed, and the model drops fd's dirty marks as the kernel does.
inline void io_sync_checkpoint(const char* name, int fd) {
  try {
    io_checkpoint(name);
  } catch (...) {
    if (active_cache_model) active_cache_model->sync_failed(fd);
    throw;
  }
}

// RAII guard: the model sees data file writes and syncs while it lives.
struct ScopedPageCacheModel {
  PageCacheModel model;
  ScopedPageCacheModel() { active_cache_model = &model; }
  ~ScopedPageCacheModel() { active_cache_model = nullptr; }
  ScopedPageCacheModel(const ScopedPageCacheModel&) = delete;
  ScopedPageCacheModel& operator=(const ScopedPageCacheModel&) = delete;
  ScopedPageCacheModel(ScopedPageCacheModel&&) = delete;
  ScopedPageCacheModel& operator=(ScopedPageCacheModel&&) = delete;
};

} // namespace bytecask::testing

// The macro — only defined when BYTECASK_TESTING is set.
// Not defined in release builds — FAULT_INJECTION does not exist.
#define FAULT_INJECTION(name) \
    ::bytecask::testing::io_checkpoint(#name)

#define FAULT_INJECTION_POST_WRITE(name, fd, offset, total) \
    ::bytecask::testing::io_post_write_checkpoint(#name, fd, offset, total)

#define FAULT_INJECTION_SYNC(name, fd) \
    ::bytecask::testing::io_sync_checkpoint(#name, fd)

#define FAULT_CACHE_WRITE(fd, offset, len) \
    ::bytecask::testing::io_cache_write(fd, offset, len)

#define FAULT_CACHE_SYNCED(fd) \
    ::bytecask::testing::io_cache_synced(fd)

#endif // BYTECASK_TESTING
