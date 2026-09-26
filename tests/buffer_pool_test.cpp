// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Unit tests for BufferPool — the bounded frame cache behind
// IoBackend::BufferPool. See docs/buffer_pool_design.md.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    // -1 where the platform or the filesystem will not serve uncached reads;
    // the pool then fills buffered, the same fallback a real sealed file takes.
    direct_fd_ = bytecask::open_uncached(path_, size);
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

// The read-path counters are striped across threads so that concurrent
// readers do not serialise on one cache line. What makes that a counter
// rather than an estimate: every increment lands, whichever stripe a thread
// draws, and more threads than stripes share them without losing any.
TEST_CASE("StripedCounter: concurrent adds sum exactly",
          "[buffer_pool][concurrency]") {
  bytecask::StripedCounter counter;
  constexpr int kThreads = 40;  // more than the stripe count
  constexpr int kAddsPerThread = 10'000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&counter, t] {
      for (int i = 0; i < kAddsPerThread; ++i) {
        counter.add(1 + (t % 3));
      }
    });
  }
  for (auto &th : threads) th.join();

  std::int64_t expected = 0;
  for (int t = 0; t < kThreads; ++t) {
    expected += static_cast<std::int64_t>(kAddsPerThread) * (1 + (t % 3));
  }
  CHECK(counter.load() == expected);
}

TEST_CASE("BufferPool: capacity below one frame is rejected",
          "[buffer_pool]") {
  // Silently producing a zero-frame pool would be a cache that does nothing.
  CHECK_THROWS_AS(
      bytecask::BufferPool(bytecask::BufferPoolOptions{.capacity_bytes = 128}),
      std::invalid_argument);
}

TEST_CASE("BufferPool: frame count is derived from the budget",
          "[buffer_pool]") {
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  // The bound is the contract: everything the pool owns — frames, per-frame
  // metadata, table share — comes out of capacity_bytes, so the frames alone
  // must be strictly under it rather than equal to it.
  CHECK(pool.frame_count() * bytecask::kPoolFrameBytes < kRoomyCapacity);
  CHECK(pool.frame_count() > 0);
  CHECK(pool.counters().frames_total ==
        static_cast<std::int64_t>(pool.frame_count()));
}

TEST_CASE("BufferPool: a repeated read is served from cache", "[buffer_pool]") {
  ScratchFile file{64 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  std::vector<std::byte> got(100);
  pool.read_at(1, file.fd(), 512, got.size(), file.size(), got.data());
  CHECK(got == file.expected(512, got.size()));
  CHECK(pool.counters().misses.load() == 1);
  CHECK(pool.counters().hits.load() == 0);

  // Same range again — must not reach the device.
  std::fill(got.begin(), got.end(), std::byte{0});
  pool.read_at(1, file.fd(), 512, got.size(), file.size(), got.data());
  CHECK(got == file.expected(512, got.size()));
  CHECK(pool.counters().hits.load() == 1);
  CHECK(pool.counters().misses.load() == 1);
}

TEST_CASE("BufferPool: file_id is part of the key", "[buffer_pool]") {
  ScratchFile a{16 * 1024};
  ScratchFile b{16 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  std::vector<std::byte> from_a(64);
  std::vector<std::byte> from_b(64);
  pool.read_at(1, a.fd(), 0, from_a.size(), a.size(), from_a.data());
  // Same offset, different file — must not be served a's frame.
  pool.read_at(2, b.fd(), 0, from_b.size(), b.size(), from_b.data());

  CHECK(from_a == a.expected(0, from_a.size()));
  CHECK(from_b == b.expected(0, from_b.size()));
  CHECK(pool.counters().misses.load() == 2);
}

TEST_CASE("BufferPool: reads spanning frames are served whole",
          "[buffer_pool]") {
  ScratchFile file{64 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  // Straddles three frames: starts mid-frame 0, ends mid-frame 2.
  const std::size_t offset = bytecask::kPoolFrameBytes - 10;
  const std::size_t len = 2 * bytecask::kPoolFrameBytes + 20;
  std::vector<std::byte> got(len);
  pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
  CHECK(got == file.expected(offset, len));
  CHECK(pool.counters().misses.load() == 1);  // one coalesced fill, not three

  std::fill(got.begin(), got.end(), std::byte{0});
  pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
  CHECK(got == file.expected(offset, len));
  CHECK(pool.counters().hits.load() == 1);  // every covered frame was resident
}

TEST_CASE("BufferPool: a short tail frame is correct but never admitted",
          "[buffer_pool]") {
  // Final frame is partial, so it must be served without being cached.
  ScratchFile file{bytecask::kPoolFrameBytes + 100};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  const std::size_t offset = bytecask::kPoolFrameBytes + 10;
  std::vector<std::byte> got(50);
  for (int i = 0; i < 3; ++i) {
    std::fill(got.begin(), got.end(), std::byte{0});
    pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
    CHECK(got == file.expected(offset, got.size()));
  }
  // Never cached, so every read is a miss — correctness over hit ratio.
  CHECK(pool.counters().hits.load() == 0);
  CHECK(pool.counters().misses.load() == 3);
}

TEST_CASE("BufferPool: an oversize read bypasses admission", "[buffer_pool]") {
  ScratchFile file{1024 * 1024};
  // 64 frames; the guard rejects anything over an eighth of capacity.
  const std::size_t capacity = 64 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

  const std::size_t len = capacity / 4;  // comfortably over capacity/8
  std::vector<std::byte> got(len);
  pool.read_at(1, file.fd(), 0, len, file.size(), got.data());
  CHECK(got == file.expected(0, len));

  // Admitting it would have evicted the working set to hold one value, so
  // it is neither a hit nor a miss: it never touched the pool.
  pool.read_at(1, file.fd(), 0, len, file.size(), got.data());
  CHECK(pool.counters().hits.load() == 0);
  CHECK(pool.counters().misses.load() == 0);
  CHECK(pool.counters().fills.load() == 0);
}

TEST_CASE("BufferPool: differential against pread under constant eviction",
          "[buffer_pool]") {
  // The design's differential test: random reads compared byte-for-byte with
  // the file's true contents, at a pool far too small to hold the working set
  // so that eviction runs continuously.
  ScratchFile file{2 * 1024 * 1024};
  const std::size_t capacity = 16 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

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
  CHECK(pool.counters().evictions.load() > 0);
}

TEST_CASE("BufferPool: a leased frame survives eviction pressure",
          "[buffer_pool]") {
  // What makes a span into a frame safe to hand out: while the lease is
  // held the frame is neither reclaimed nor refilled, however hard the rest
  // of the pool churns. Same sizing as the differential test, so eviction
  // runs continuously around the one frame that must not move.
  ScratchFile file{2 * 1024 * 1024};
  const std::size_t capacity = 16 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

  // Make the frame resident, then lend it.
  const std::size_t leased_offset = 7 * bytecask::kPoolFrameBytes + 100;
  std::vector<std::byte> scratch(64);
  pool.read_at(1, file.fd(), leased_offset, scratch.size(), file.size(),
               scratch.data());
  bytecask::FrameLease lease;
  const auto span = pool.view(1, leased_offset, file.size(), lease);
  REQUIRE(static_cast<bool>(lease));
  REQUIRE(span.size() == bytecask::kPoolFrameBytes - 100);
  const std::vector<std::byte> lent_before(span.begin(), span.end());
  CHECK(lent_before == file.expected(leased_offset, span.size()));

  // Churn every other frame many times over.
  std::mt19937_64 rng{777};
  std::vector<std::byte> got;
  for (int i = 0; i < 4000; ++i) {
    const auto len = static_cast<std::size_t>(1 + rng() % 3000);
    const auto offset =
        static_cast<std::size_t>(rng() % (file.size() - len));
    got.assign(len, std::byte{0});
    pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
    REQUIRE(got == file.expected(offset, len));
  }
  CHECK(pool.counters().evictions.load() > 100);

  // The lent bytes are exactly what they were, read through the same span.
  CHECK(std::vector<std::byte>(span.begin(), span.end()) == lent_before);

  // Released, the frame is ordinary again: the next churn may reuse it, and
  // a fresh view of the same offset must still read the file's bytes —
  // either from a surviving frame or after a refill — never stale ones.
  lease.reset();
  CHECK_FALSE(static_cast<bool>(lease));
  for (int i = 0; i < 2000; ++i) {
    const auto len = static_cast<std::size_t>(1 + rng() % 3000);
    const auto offset =
        static_cast<std::size_t>(rng() % (file.size() - len));
    got.assign(len, std::byte{0});
    pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
  }
  got.assign(scratch.size(), std::byte{0});
  pool.read_at(1, file.fd(), leased_offset, got.size(), file.size(),
               got.data());
  CHECK(got == file.expected(leased_offset, got.size()));
}

TEST_CASE("BufferPool: a view of a straddling or absent range is empty",
          "[buffer_pool]") {
  ScratchFile file{64 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};
  bytecask::FrameLease lease;

  // Not resident: nothing lent, nothing pinned, no hit counted.
  CHECK(pool.view(1, 4096, file.size(), lease).empty());
  CHECK_FALSE(static_cast<bool>(lease));
  CHECK(pool.counters().hits.load() == 0);

  // Resident: lent up to the end of its frame. The hit is the caller's to
  // record once the span proves long enough for what it wanted.
  std::vector<std::byte> got(16);
  pool.read_at(1, file.fd(), 4096, got.size(), file.size(), got.data());
  const auto span = pool.view(1, 4096 + 4000, file.size(), lease);
  CHECK(static_cast<bool>(lease));
  CHECK(span.size() == 96);
  CHECK(std::vector<std::byte>(span.begin(), span.end()) ==
        file.expected(4096 + 4000, 96));
  CHECK(pool.counters().hits.load() == 0);
  pool.note_hit();
  CHECK(pool.counters().hits.load() == 1);

  // At or past the size the caller is bounded by: nothing.
  CHECK(pool.view(1, file.size(), file.size(), lease).empty());
  CHECK_FALSE(static_cast<bool>(lease));
}

// Bytes this process has passed through read(2)-family syscalls, cached or
// not: the direct measure of what a read cost the pool in I/O.
auto rchar() -> long long {
  std::ifstream io{"/proc/self/io"};
  std::string key;
  long long value = 0;
  while (io >> key >> value) {
    if (key == "rchar:") return value;
  }
  return 0;
}

TEST_CASE("BufferPool: a partial miss reads only the missing frames",
          "[buffer_pool]") {
  // Eviction takes frames, not values, so a multi-frame value routinely loses
  // some frames while the rest stay resident. Re-reading it must fetch the
  // frames that are gone and no others; the first version re-read the whole
  // value, which at 2 MiB a value was the difference between a 4 KiB read
  // and a 2 MiB one on every partial miss. The pool is too small for a fill
  // block to pass the oversize guard, so fills here are frame-sized; the
  // block-sized bound is "a partial miss reads only the blocks it touches".
  ScratchFile file{1024 * 1024};
  // ~64 frames; the oversize guard admits a value up to an eighth of that.
  const std::size_t capacity = 64 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};
  const auto frames = pool.frame_count();
  REQUIRE(frames >= 32);

  // V: eight whole frames at the start of the file.
  constexpr std::size_t kVFrames = 8;
  const std::size_t v_len = kVFrames * bytecask::kPoolFrameBytes;
  std::vector<std::byte> v(v_len);
  pool.read_at(1, file.fd(), 0, v_len, file.size(), v.data());
  REQUIRE(pool.counters().fills.load() == kVFrames);

  // Read the first half of V again, so SIEVE's hand passes those frames and
  // evicts the unread half — V is the oldest data, and read only once it
  // would go whole.
  constexpr std::size_t kKept = kVFrames / 2;
  for (std::size_t f = 0; f < kKept; ++f) {
    std::vector<std::byte> head(16);
    pool.read_at(1, file.fd(), f * bytecask::kPoolFrameBytes, head.size(),
                 file.size(), head.data());
  }

  // Fill the rest of the pool with one frame each, then keep going so the
  // hand evicts a good fraction of everything — including V's unread half.
  std::vector<std::byte> one(16);
  const auto singles = (frames - kVFrames) + frames * 3 / 8;
  for (std::size_t i = 0; i < singles; ++i) {
    const auto off = (kVFrames + 1 + i) * bytecask::kPoolFrameBytes;
    pool.read_at(1, file.fd(), off, one.size(), file.size(), one.data());
  }
  REQUIRE(pool.counters().evictions.load() > 0);

  const auto fills_before = pool.counters().fills.load();
  const auto bytes_before = rchar();
  std::fill(v.begin(), v.end(), std::byte{0});
  pool.read_at(1, file.fd(), 0, v_len, file.size(), v.data());
  const auto refilled = pool.counters().fills.load() - fills_before;
  const auto bytes_read = rchar() - bytes_before;

  CHECK(v == file.expected(0, v_len));
  // The case this test exists for: some of V went, some stayed. If either
  // bound fails, the eviction pattern changed and the counts below need
  // re-tuning, not the pool.
  REQUIRE(refilled == static_cast<std::int64_t>(kVFrames - kKept));
  INFO("frames of V refilled: " << refilled << " of " << kVFrames);
  // Only the missing frames were read (+ the /proc read that measured it).
  CHECK(bytes_read <= refilled * static_cast<long long>(bytecask::kPoolFrameBytes) + 4096);
}

TEST_CASE("BufferPool: concurrent readers never observe a torn frame",
          "[buffer_pool][concurrency]") {
  // Frames are reused memory, so a reader racing evict-then-refill would get
  // bytes that are part old and part new. This is what the seqlock is for.
  ScratchFile file{1024 * 1024};
  const std::size_t capacity = 24 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

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
  CHECK(pool.counters().evictions.load() > 0);
}

TEST_CASE("BufferPool: every frame is claimable and pinnable again once "
          "readers stop",
          "[buffer_pool][concurrency]") {
  // A reader that loaded a slot just before eviction claimed its frame pins
  // a dead frame: the pin fails and is dropped again. If the refill cleared
  // the dead bit with a store of zero in between, that drop would wrap the
  // pin word below zero — the frame could never be pinned or claimed again,
  // its key would miss forever, and the dead bit would flicker off under
  // later failed pins, letting a reader into a frame being written. The
  // post-condition below catches the first symptom: with the churn over,
  // reading any frame twice in a row must hit the second time.
  ScratchFile file{1024 * 1024};
  const std::size_t capacity = 24 * (bytecask::kPoolFrameBytes + 64);
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

  constexpr int kThreads = 8;
  constexpr int kIters = 20000;
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng{static_cast<unsigned long long>(t) + 11};
      std::vector<std::byte> got(64);
      bytecask::FrameLease lease;
      for (int i = 0; i < kIters; ++i) {
        // Forty frames contend for twenty-four: every read is close to
        // an eviction of a frame some other thread is about to pin.
        const auto offset = static_cast<std::size_t>(
            (rng() % 40) * bytecask::kPoolFrameBytes + 8);
        if ((i & 1) == 0) {
          pool.read_at(1, file.fd(), offset, got.size(), file.size(),
                       got.data());
        } else if (const auto span = pool.view(1, offset, file.size(), lease);
                   !span.empty()) {
          got.assign(span.begin(), span.begin() + 64);
          lease.reset();
        } else {
          pool.read_at(1, file.fd(), offset, got.size(), file.size(),
                       got.data());
        }
        if (got != file.expected(offset, 64)) mismatches.fetch_add(1);
      }
    });
  }
  for (auto &th : threads) th.join();
  CHECK(mismatches.load() == 0);
  REQUIRE(pool.counters().evictions.load() > 0);

  std::vector<std::byte> got(64);
  for (std::size_t f = 0; f < 40; ++f) {
    const auto offset = f * bytecask::kPoolFrameBytes;
    pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
    const auto hits_before = pool.counters().hits.load();
    pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
    INFO("frame " << f);
    CHECK(pool.counters().hits.load() == hits_before + 1);
  }
}

// Block fills. kRoomyCapacity puts the oversize guard (capacity / 8) above
// kPoolFillBlockBytes, so every pool below fills a block per miss.
namespace {
constexpr std::size_t kBlockFrames =
    bytecask::kPoolFillBlockBytes / bytecask::kPoolFrameBytes;
static_assert(kRoomyCapacity / 8 >= bytecask::kPoolFillBlockBytes);
} // namespace

TEST_CASE("BufferPool: a miss fills the file-aligned block around it",
          "[buffer_pool]") {
  ScratchFile file{1024 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  std::vector<std::byte> got(100);
  const std::size_t offset = 5 * bytecask::kPoolFrameBytes + 7;
  pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
  CHECK(got == file.expected(offset, got.size()));
  CHECK(pool.counters().misses.load() == 1);
  CHECK(pool.counters().fills.load() ==
        static_cast<std::int64_t>(kBlockFrames));

  // The block's last frame came in with it; the next block's first did not.
  const std::size_t last_in_block = (kBlockFrames - 1) * bytecask::kPoolFrameBytes;
  pool.read_at(1, file.fd(), last_in_block, got.size(), file.size(), got.data());
  CHECK(got == file.expected(last_in_block, got.size()));
  CHECK(pool.counters().hits.load() == 1);

  const std::size_t next_block = bytecask::kPoolFillBlockBytes;
  pool.read_at(1, file.fd(), next_block, got.size(), file.size(), got.data());
  CHECK(got == file.expected(next_block, got.size()));
  CHECK(pool.counters().misses.load() == 2);
  CHECK(pool.counters().fills.load() ==
        static_cast<std::int64_t>(2 * kBlockFrames));
}

TEST_CASE("BufferPool: a read straddling a block boundary fills both blocks",
          "[buffer_pool]") {
  ScratchFile file{1024 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  const std::size_t offset = bytecask::kPoolFillBlockBytes - 10;
  std::vector<std::byte> got(20);
  const auto bytes_before = rchar();
  pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
  const auto bytes_read = rchar() - bytes_before;
  CHECK(got == file.expected(offset, got.size()));
  CHECK(pool.counters().misses.load() == 1);
  CHECK(pool.counters().fills.load() ==
        static_cast<std::int64_t>(2 * kBlockFrames));
  // Two blocks, read once (+ the /proc read that measured it).
  CHECK(bytes_read <=
        2 * static_cast<long long>(bytecask::kPoolFillBlockBytes) + 4096);
}

TEST_CASE("BufferPool: a block is clipped to the file and its short tail",
          "[buffer_pool]") {
  // Block 1 holds three whole frames and a partial one before EOF.
  constexpr std::size_t kTailFrames = 3;
  ScratchFile file{bytecask::kPoolFillBlockBytes +
                   kTailFrames * bytecask::kPoolFrameBytes + 100};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  std::vector<std::byte> got(50);
  const std::size_t offset = bytecask::kPoolFillBlockBytes + 10;
  pool.read_at(1, file.fd(), offset, got.size(), file.size(), got.data());
  CHECK(got == file.expected(offset, got.size()));
  CHECK(pool.counters().fills.load() == static_cast<std::int64_t>(kTailFrames));

  // The partial frame is served every time and never admitted.
  const std::size_t tail =
      bytecask::kPoolFillBlockBytes + kTailFrames * bytecask::kPoolFrameBytes + 20;
  for (int i = 0; i < 2; ++i) {
    std::fill(got.begin(), got.end(), std::byte{0});
    pool.read_at(1, file.fd(), tail, got.size(), file.size(), got.data());
    CHECK(got == file.expected(tail, got.size()));
  }
  CHECK(pool.counters().fills.load() == static_cast<std::int64_t>(kTailFrames));
  CHECK(pool.counters().misses.load() == 3);
}

TEST_CASE("BufferPool: a partial miss reads only the blocks it touches",
          "[buffer_pool]") {
  // A value over three blocks with the outer two resident: re-reading it
  // fetches the middle block and nothing else.
  ScratchFile file{1024 * 1024};
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = kRoomyCapacity}};

  std::vector<std::byte> one(16);
  pool.read_at(1, file.fd(), 0, one.size(), file.size(), one.data());
  pool.read_at(1, file.fd(), 2 * bytecask::kPoolFillBlockBytes, one.size(),
               file.size(), one.data());
  REQUIRE(pool.counters().fills.load() ==
          static_cast<std::int64_t>(2 * kBlockFrames));

  const std::size_t v_len = 3 * bytecask::kPoolFillBlockBytes;
  std::vector<std::byte> v(v_len);
  const auto bytes_before = rchar();
  pool.read_at(1, file.fd(), 0, v_len, file.size(), v.data());
  const auto bytes_read = rchar() - bytes_before;

  CHECK(v == file.expected(0, v_len));
  CHECK(pool.counters().fills.load() ==
        static_cast<std::int64_t>(3 * kBlockFrames));
  CHECK(bytes_read <=
        static_cast<long long>(bytecask::kPoolFillBlockBytes) + 4096);
}

TEST_CASE("BufferPool: block fills against pread under constant eviction",
          "[buffer_pool]") {
  // The differential test again, with a pool large enough to fill blocks and
  // far too small for the file, so admitting a block always evicts.
  ScratchFile file{8 * 1024 * 1024};
  const std::size_t capacity = 12 * bytecask::kPoolFillBlockBytes;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

  std::mt19937_64 rng{4242};
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
  CHECK(pool.counters().evictions.load() > 0);
  CHECK(pool.counters().fills.load() > pool.counters().misses.load());
}

TEST_CASE("BufferPool: concurrent block fills never tear a frame",
          "[buffer_pool][concurrency]") {
  ScratchFile file{8 * 1024 * 1024};
  const std::size_t capacity = 12 * bytecask::kPoolFillBlockBytes;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};

  constexpr int kThreads = 8;
  constexpr int kIters = 3000;
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng{static_cast<unsigned long long>(t) + 101};
      std::vector<std::byte> got;
      for (int i = 0; i < kIters; ++i) {
        const auto len = static_cast<std::size_t>(1 + rng() % 2000);
        const auto offset =
            static_cast<std::size_t>(rng() % (file.size() - len));
        got.assign(len, std::byte{0});
        pool.read_at(1, file.fd(), offset, len, file.size(), got.data());
        if (got != file.expected(offset, len)) mismatches.fetch_add(1);
      }
    });
  }
  for (auto &th : threads) th.join();
  CHECK(mismatches.load() == 0);
  CHECK(pool.counters().evictions.load() > 0);
}

TEST_CASE("BufferPool: a full pool fills only the frames a miss needs",
          "[buffer_pool]") {
  // Blocks warm a cold pool; once it is full, a block's neighbours would
  // evict frames that earned their place, so fills shrink back to frames.
  ScratchFile file{8 * 1024 * 1024};
  const std::size_t capacity = 12 * bytecask::kPoolFillBlockBytes;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};
  const auto total = pool.counters().frames_total;

  std::vector<std::byte> one(16);
  std::size_t block = 0;
  while (pool.counters().frames_resident.load() < total) {
    const auto before = pool.counters().fills.load();
    pool.read_at(1, file.fd(), block * bytecask::kPoolFillBlockBytes,
                 one.size(), file.size(), one.data());
    // Filling a cold pool admits a whole block per miss.
    CHECK(pool.counters().fills.load() - before ==
          static_cast<std::int64_t>(kBlockFrames));
    ++block;
    REQUIRE(block * bytecask::kPoolFillBlockBytes < file.size());
  }

  const auto fills_before = pool.counters().fills.load();
  const std::size_t offset = block * bytecask::kPoolFillBlockBytes + 5000;
  const auto bytes_before = rchar();
  pool.read_at(1, file.fd(), offset, one.size(), file.size(), one.data());
  const auto bytes_read = rchar() - bytes_before;
  CHECK(one == file.expected(offset, one.size()));
  CHECK(pool.counters().fills.load() - fills_before == 1);
  CHECK(bytes_read <= 2 * static_cast<long long>(bytecask::kPoolFrameBytes));
}

TEST_CASE("BufferPool: SIEVE keeps a re-read frame and evicts the oldest "
          "frame read once",
          "[buffer_pool]") {
  // Below 8 blocks the pool fills frames only, so admission order is exactly
  // read order. Every frame is admitted unvisited; a second read marks it.
  // The hand starts at the oldest frame, passes the marked one (clearing its
  // mark), and evicts the next — a frame read once and never again.
  const std::size_t capacity = 512 * 1024;
  bytecask::BufferPool pool{
      bytecask::BufferPoolOptions{.capacity_bytes = capacity}};
  const auto n = pool.frame_count();
  ScratchFile file{(n + 2) * bytecask::kPoolFrameBytes};
  std::vector<std::byte> one(1);
  const auto read_frame = [&](std::size_t f) {
    pool.read_at(1, file.fd(), f * bytecask::kPoolFrameBytes, one.size(),
                 file.size(), one.data());
    CHECK(one == file.expected(f * bytecask::kPoolFrameBytes, one.size()));
  };

  for (std::size_t f = 0; f < n; ++f) read_frame(f);
  REQUIRE(pool.counters().frames_resident.load() ==
          static_cast<std::int64_t>(n));
  REQUIRE(pool.counters().evictions.load() == 0);

  read_frame(0);  // hit: frame 0 is now visited
  CHECK(pool.counters().hits.load() == 1);

  read_frame(n);  // miss on a full pool: one eviction
  CHECK(pool.counters().evictions.load() == 1);

  const auto misses = pool.counters().misses.load();
  read_frame(0);  // survived: the hand cleared its mark and moved on
  CHECK(pool.counters().misses.load() == misses);
  read_frame(1);  // the victim: oldest frame never read again
  CHECK(pool.counters().misses.load() == misses + 1);
}
