// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Unit tests for BufferPool — the bounded frame cache behind
// IoBackend::BufferPool. See docs/buffer_pool_design.md.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

import bytecask.buffer_pool;
import bytecask.counters;

namespace {

// A scratch file of deterministic bytes, plus an fd open on it.
class ScratchFile {
public:
  explicit ScratchFile(std::size_t size) : size_{size} {
    path_ = std::filesystem::temp_directory_path() /
            ("bc_pool_" + std::to_string(::getpid()) + "_" +
             std::to_string(next_id()) + ".bin");
    bytes_.resize(size);
    std::mt19937_64 rng{0xC0FFEE};
    for (auto &b : bytes_) {
      b = static_cast<std::byte>(rng() & 0xFF);
    }
    auto *f = std::fopen(path_.c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite(bytes_.data(), 1, size, f) == size);
    REQUIRE(std::fclose(f) == 0);
    fd_ = ::open(path_.c_str(), O_RDONLY);
    REQUIRE(fd_ != -1);
    // -1 where the filesystem refuses O_DIRECT; the pool then fills buffered,
    // which is the same fallback a real sealed file takes.
    direct_fd_ = ::open(path_.c_str(), O_RDONLY | O_DIRECT);
  }

  ~ScratchFile() {
    if (direct_fd_ != -1) ::close(direct_fd_);
    if (fd_ != -1) ::close(fd_);
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ScratchFile(const ScratchFile &) = delete;
  auto operator=(const ScratchFile &) -> ScratchFile & = delete;

  [[nodiscard]] auto fd() const noexcept -> bytecask::PoolFile {
    return {.buffered = fd_, .direct = direct_fd_};
  }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  // The oracle: what a correct read of [offset, offset+len) must return.
  [[nodiscard]] auto expected(std::size_t offset, std::size_t len) const
      -> std::vector<std::byte> {
    return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + len)};
  }

private:
  static auto next_id() -> unsigned {
    static std::atomic<unsigned> counter{0};
    return counter.fetch_add(1);
  }

  std::filesystem::path path_;
  std::vector<std::byte> bytes_;
  std::size_t size_;
  int fd_{-1};
  int direct_fd_{-1};
};

// Big enough that frames are plentiful unless a test says otherwise.
constexpr std::size_t kRoomyCapacity = 4 * 1024 * 1024;

} // namespace

TEST_CASE("BufferPool: capacity below one frame is rejected",
          "[buffer_pool]") {
  bytecask::Counters counters;
  // Silently producing a zero-frame pool would be a cache that does nothing.
  CHECK_THROWS_AS(
      bytecask::BufferPool(bytecask::BufferPoolOptions{.capacity_bytes = 128},
                           counters),
      std::invalid_argument);
}

TEST_CASE("BufferPool: frame count is derived from the budget",
          "[buffer_pool]") {
  bytecask::Counters counters;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}, counters};

  // The bound is the contract: everything the pool owns — frames, per-frame
  // metadata, table share — comes out of capacity_bytes, so the frames alone
  // must be strictly under it rather than equal to it.
  CHECK(pool.frame_count() * bytecask::kPoolFrameBytes < kRoomyCapacity);
  CHECK(pool.frame_count() > 0);
  CHECK(counters.pool_frames_total ==
        static_cast<std::int64_t>(pool.frame_count()));
}

TEST_CASE("BufferPool: a repeated read is served from cache", "[buffer_pool]") {
  ScratchFile file{64 * 1024};
  bytecask::Counters counters;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}, counters};

  std::vector<std::byte> got(100);
  pool.read_at(1, file.fd(), 512, got.size(), file.size(), got.data());
  CHECK(got == file.expected(512, got.size()));
  CHECK(counters.pool_misses.load() == 1);
  CHECK(counters.pool_hits.load() == 0);

  // Same range again — must not reach the device.
  std::fill(got.begin(), got.end(), std::byte{0});
  pool.read_at(1, file.fd(), 512, got.size(), file.size(), got.data());
  CHECK(got == file.expected(512, got.size()));
  CHECK(counters.pool_hits.load() == 1);
  CHECK(counters.pool_misses.load() == 1);
}

TEST_CASE("BufferPool: file_id is part of the key", "[buffer_pool]") {
  ScratchFile a{16 * 1024};
  ScratchFile b{16 * 1024};
  bytecask::Counters counters;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}, counters};

  std::vector<std::byte> from_a(64);
  std::vector<std::byte> from_b(64);
  pool.read_at(1, a.fd(), 0, from_a.size(), a.size(), from_a.data());
  // Same offset, different file — must not be served a's frame.
  pool.read_at(2, b.fd(), 0, from_b.size(), b.size(), from_b.data());

  CHECK(from_a == a.expected(0, from_a.size()));
  CHECK(from_b == b.expected(0, from_b.size()));
  CHECK(counters.pool_misses.load() == 2);
}

TEST_CASE("BufferPool: reads spanning frames are served whole",
          "[buffer_pool]") {
  ScratchFile file{64 * 1024};
  bytecask::Counters counters;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}, counters};

  // Straddles three frames: starts mid-frame 0, ends mid-frame 2.
  const std::size_t offset = bytecask::kPoolFrameBytes - 10;
  const std::size_t len = 2 * bytecask::kPoolFrameBytes + 20;
  std::vector<std::byte> got(len);
  pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
  CHECK(got == file.expected(offset, len));
  CHECK(counters.pool_multi_frame_reads.load() == 1);

  std::fill(got.begin(), got.end(), std::byte{0});
  pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
  CHECK(got == file.expected(offset, len));
  CHECK(counters.pool_hits.load() == 1);  // every covered frame was resident
}

TEST_CASE("BufferPool: a short tail frame is correct but never admitted",
          "[buffer_pool]") {
  // Final frame is partial, so it must be served without being cached.
  ScratchFile file{bytecask::kPoolFrameBytes + 100};
  bytecask::Counters counters;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}, counters};

  const std::size_t offset = bytecask::kPoolFrameBytes + 10;
  std::vector<std::byte> got(50);
  for (int i = 0; i < 3; ++i) {
    std::fill(got.begin(), got.end(), std::byte{0});
    pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
    CHECK(got == file.expected(offset, got.size()));
  }
  // Never cached, so every read is a miss — correctness over hit ratio.
  CHECK(counters.pool_hits.load() == 0);
  CHECK(counters.pool_misses.load() == 3);
}

TEST_CASE("BufferPool: an oversize read bypasses admission", "[buffer_pool]") {
  ScratchFile file{1024 * 1024};
  bytecask::Counters counters;
  // 64 frames; the guard rejects anything over an eighth of capacity.
  const std::size_t capacity = 64 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity,
                                  .oversize_guard_divisor = 8},
      counters};

  const std::size_t len = capacity / 4;  // comfortably over capacity/8
  std::vector<std::byte> got(len);
  pool.read_at(1, file.fd(), 0, len, file.size(), got.data());
  CHECK(got == file.expected(0, len));
  CHECK(counters.pool_oversize_reads.load() == 1);

  // Admitting it would have evicted the working set to hold one value.
  pool.read_at(1, file.fd(), 0, len, file.size(), got.data());
  CHECK(counters.pool_oversize_reads.load() == 2);
  CHECK(counters.pool_hits.load() == 0);
}

TEST_CASE("BufferPool: differential against pread under constant eviction",
          "[buffer_pool]") {
  // The design's differential test: random reads compared byte-for-byte with
  // the file's true contents, at a pool far too small to hold the working set
  // so that eviction runs continuously.
  ScratchFile file{2 * 1024 * 1024};
  bytecask::Counters counters;
  const std::size_t capacity = 16 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}, counters};

  std::mt19937_64 rng{12345};
  std::vector<std::byte> got;
  for (int i = 0; i < 4000; ++i) {
    const auto len = static_cast<std::size_t>(1 + rng() % 3000);
    const auto offset =
        static_cast<std::size_t>(rng() % (file.size() - len));
    got.assign(len, std::byte{0});
    pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
    if (got != file.expected(offset, len)) {
      FAIL("mismatch at offset " << offset << " len " << len);
    }
  }
  // The point of the sizing: eviction must actually have run.
  CHECK(counters.pool_evictions.load() > 0);
}

TEST_CASE("BufferPool: concurrent readers never observe a torn frame",
          "[buffer_pool]") {
  // Frames are reused memory, so a reader racing evict-then-refill would get
  // bytes that are part old and part new. This is what the seqlock is for.
  ScratchFile file{1024 * 1024};
  bytecask::Counters counters;
  const std::size_t capacity = 24 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}, counters};

  constexpr int kThreads = 8;
  constexpr int kIters = 3000;
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng{static_cast<unsigned long long>(t) + 1};
      std::vector<std::byte> got;
      for (int i = 0; i < kIters; ++i) {
        const auto len = static_cast<std::size_t>(1 + rng() % 2000);
        const auto offset =
            static_cast<std::size_t>(rng() % (file.size() - len));
        got.assign(len, std::byte{0});
        pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
        if (got != file.expected(offset, len)) {
          mismatches.fetch_add(1);
        }
      }
    });
  }
  for (auto &th : threads) th.join();
  CHECK(mismatches.load() == 0);
  CHECK(counters.pool_evictions.load() > 0);
}
