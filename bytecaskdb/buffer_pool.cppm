// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — bounded buffer pool for data file reads.
//
// See docs/buffer_pool_design.md. The pool makes the engine's memory footprint
// an operator-set number rather than one the kernel negotiates: the key
// directory must be fully resident and cannot be evicted, so bounding the
// value cache is the only way to protect it.

module;
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/types.h>
#include <unistd.h>

export module bytecask.buffer_pool;

import bytecask.counters;
import bytecask.util;

namespace bytecask {

// The O_DIRECT alignment unit and the device transfer minimum: the pool
// caches, evicts and serves hits in frames of this size.
export inline constexpr std::size_t kPoolFrameBytes = 4096;

// While the pool has free frames, a miss is filled a block at a time: the
// file-aligned block around it is read in one O_DIRECT read and every whole
// frame in it admitted. Frame-sized fills leave a cold pool warming one 4 KiB
// read per miss — sysbench on a 2.8 GB dataset spent whole minutes cold, and
// on SATA each of those reads queued behind the commit's cache flush. 128 KiB
// is the kernel's default readahead, the rate the page-cache backends warm at.
export inline constexpr std::size_t kPoolFillBlockBytes = 128 * 1024;
static_assert(kPoolFillBlockBytes % kPoolFrameBytes == 0);

export struct BufferPoolOptions {
  // TOTAL footprint — frames and the index table both come out of it, so
  // configuring 1 GiB yields 1 GiB of resident memory rather than 1 GiB plus
  // overhead. 0 is rejected by DB::open.
  std::size_t capacity_bytes{0};
  // Fill frames with O_DIRECT so the pool is the only consumer of memory for
  // sealed-file data. This is the mechanism, not an optimisation: a pool
  // filled through the page cache bounds nothing but itself. A filesystem
  // that refuses O_DIRECT falls back to buffered fills per file, counted in
  // pool_direct_io_fallbacks.
  bool direct_io{true};
};

// The descriptors a pool-backed file hands to the pool. `direct` is opened
// O_DIRECT and serves frame-aligned fills only; it is -1 where the filesystem
// refused it, and fills then go through `buffered`. `buffered` serves
// everything else: oversize reads and the fallback.
export struct PoolFile {
  int buffered{-1};
  int direct{-1};
};

// Opens path read-only on a descriptor whose reads bypass the page cache, so
// the pool is the only consumer of memory for this file's data. Returns -1
// where the platform or the filesystem will not serve one; the caller counts
// the fallback and fills through an ordinary descriptor instead.
//
// This is the codebase's only place that names a platform's uncached-read
// mechanism. Everything else — the engine, the pool, the tests — asks here and
// branches on the -1.
//
// The descriptor is proved with one aligned read before it is handed back:
// some filesystems accept the flag at open and fail at read, so the open alone
// proves nothing.
export [[nodiscard]] inline auto open_uncached(
    const std::filesystem::path &path, std::size_t file_size) -> int {
#if defined(O_DIRECT)
  auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
  if (fd == -1) return -1;
#elif defined(F_NOCACHE)
  // macOS: no O_DIRECT. F_NOCACHE is the analogue the design names — reads
  // bypass the buffer cache, with no alignment requirement, so the aligned
  // fills below are simply valid reads.
  auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) return -1;
  if (::fcntl(fd, F_NOCACHE, 1) == -1) {
    ::close(fd);
    return -1;
  }
#else
  (void)path;
  (void)file_size;
  return -1;  // no uncached read on this platform: every file falls back
#endif
  if (file_size == 0) return fd;  // nothing to probe, nothing to fill
  void *probe = std::aligned_alloc(kPoolFrameBytes, kPoolFrameBytes);
  if (probe == nullptr) {
    ::close(fd);
    return -1;
  }
  const auto n = ::pread(fd, probe, kPoolFrameBytes, 0);
  std::free(probe);
  if (n <= 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// The pool's own counters, owned by the pool rather than by the DB's
// Counters: a pool-backed data file holds the pool, an iterator holds the
// file, and either can outlive the DB. All zero when there is no pool.
// hits/misses are the primary metric: unlike disk_reads, a miss here is
// known to have left the pool, which the page cache cannot tell you.
export struct PoolCounters {
  StripedCounter hits;
  StripedCounter misses;
  // Frames admitted by a read miss. The writer's inserts on append are not
  // fills: they cost no I/O.
  std::atomic<std::int64_t> fills{0};
  std::atomic<std::int64_t> evictions{0};
  std::int64_t frames_total{0};
  // Gauge: frames currently holding a file's bytes. Resident / total is the
  // fill level an operator sizes against.
  std::atomic<std::int64_t> frames_resident{0};
  // Files whose filesystem refused O_DIRECT and fill through the page cache
  // instead. Catches a CI mount that would otherwise measure the wrong thing.
  std::atomic<std::int64_t> direct_io_fallbacks{0};
};

export class BufferPool;

// A pin on one frame, held by a reader that was lent a span into it rather
// than a copy. The span stays valid until the lease is reset or destroyed;
// the frame cannot be evicted or refilled while it is held. Move-only, and
// meant to live in the object that holds the span — an iterator — so the two
// lifetimes cannot come apart.
export class FrameLease {
public:
  FrameLease() = default;
  ~FrameLease() { reset(); }
  FrameLease(const FrameLease &) = delete;
  auto operator=(const FrameLease &) -> FrameLease & = delete;
  FrameLease(FrameLease &&o) noexcept : pool_{o.pool_}, frame_{o.frame_} {
    o.pool_ = nullptr;
  }
  auto operator=(FrameLease &&o) noexcept -> FrameLease & {
    if (this != &o) {
      reset();
      pool_ = o.pool_;
      frame_ = o.frame_;
      o.pool_ = nullptr;
    }
    return *this;
  }

  // Releases the pin. Any span lent under this lease is dangling afterwards.
  void reset() noexcept;

  [[nodiscard]] explicit operator bool() const noexcept {
    return pool_ != nullptr;
  }

private:
  friend class BufferPool;
  FrameLease(BufferPool *pool, std::uint32_t frame) noexcept
      : pool_{pool}, frame_{frame} {}

  BufferPool *pool_{nullptr};
  std::uint32_t frame_{0};
};

// ---------------------------------------------------------------------------
// BufferPool — fixed-size frame cache keyed by (file_id, frame_index).
//
// Three arrays: an arena of 4 KiB frames, an open-addressed index of 16-byte
// slots — key, frame number, CLOCK reference bit and a seqlock version — and
// a pin count per frame.
//
// A frame's bytes are immutable while any reader holds a pin on it. A reader
// probes the index with acquire loads, pins the frame the slot names, checks
// the slot did not change under it, and then reads the frame with a plain
// memcpy: nothing can write those bytes until it unpins. Eviction claims a
// frame by moving its pin word from zero to kDead, which fails while a pin is
// held and makes any later pin attempt back off, so a frame is filled only
// when no reader can be in it and no reader can enter it. The only concurrent
// write a pinned frame ever sees is the active file's append extending it
// past the size every reader is bounded by — disjoint bytes, no race.
//
// Every change to the index or to a frame's contents happens under one mutex,
// and each change to a slot is bracketed by its version going odd and then
// even again, so a reader that pinned a frame and then saw the slot's version
// unchanged knows the slot still maps its key to that frame. The mutex is not
// held across device reads — a miss reads the frame into a thread-local
// buffer first and admits it afterwards.
//
// Deletion is by backward shift, not tombstones: a tombstoned linear-probing
// table gets slower with every eviction and never recovers.
//
// Values are copied into the caller's buffer, never returned as a span into
// a frame: frames are reused memory, and a span into one would dangle the
// moment it was evicted.
// ---------------------------------------------------------------------------
export class BufferPool {
public:
  explicit BufferPool(const BufferPoolOptions &opts)
      : direct_io_{opts.direct_io},
        oversize_limit_{opts.capacity_bytes / kOversizeDivisor} {
    // The table is a power of two at most 70 % full; the arena and the pin
    // words take what is left of the budget. Frame count is derived from the
    // budget, never the budget from the frame count — the bound is the
    // contract.
    constexpr auto kPerFrame = kPoolFrameBytes + sizeof(std::atomic<std::uint32_t>);
    const auto wanted = opts.capacity_bytes / (kPerFrame + sizeof(Slot));
    const auto table = std::bit_ceil(std::max<std::size_t>(wanted * 10 / 7, 2));
    const auto table_bytes = table * sizeof(Slot);
    const auto frames = std::min(
        opts.capacity_bytes > table_bytes
            ? (opts.capacity_bytes - table_bytes) / kPerFrame
            : 0,
        table * 7 / 10);
    if (frames == 0) {
      throw std::invalid_argument{std::format(
          "BufferPool: capacity_bytes = {} is too small to hold one {}-byte "
          "frame plus its index",
          opts.capacity_bytes, kPoolFrameBytes)};
    }
    frame_count_ = frames;
    table_mask_ = table - 1;

    // Value-initialising the arena touches every page now, so the operator's
    // memory is taken at open rather than faulted in one page at a time on
    // the read path, where it showed up as a p99 that grew with pool size.
    arena_ = std::vector<std::byte>(frame_count_ * kPoolFrameBytes);
    pins_ = std::vector<std::atomic<std::uint32_t>>(frame_count_);
    table_ = std::vector<Slot>(table);
    counters_.frames_total = narrow<std::int64_t>(frame_count_);
  }

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  [[nodiscard]] auto frame_count() const noexcept -> std::size_t {
    return frame_count_;
  }
  [[nodiscard]] auto counters() const noexcept -> const PoolCounters & {
    return counters_;
  }

  // Reads exactly len bytes at offset of file_id's file into dst, serving what
  // is resident and filling the rest. file_size bounds admission: a frame that
  // extends past EOF is served directly and never cached, which is what lets
  // a frame's contents be fixed-length with no per-frame valid-length field.
  //
  // Throws std::system_error if the underlying read fails or comes up short.
  void read_at(std::uint32_t file_id, PoolFile file, std::uint64_t offset,
               std::size_t len, std::size_t file_size, std::byte *dst);

  // Lends the resident bytes from offset to the end of its frame, or to
  // file_size if that comes first, without copying: the returned span points
  // into the frame and lease holds the pin that keeps it valid. Empty if the
  // frame is not resident, in which case nothing is pinned and the caller
  // reads through read_at. Counts nothing: the caller records a hit with
  // note_hit() once it has used the span, since a span too short for its
  // entry sends it to read_at, which counts that read itself.
  [[nodiscard]] auto view(std::uint32_t file_id, std::uint64_t offset,
                          std::size_t file_size, FrameLease &lease)
      -> std::span<const std::byte>;
  void note_hit() noexcept { counters_.hits.add(1); }

  // The active file's frames are never evicted: CLOCK skips them, and the
  // engine moves this at rotation (under the write lock), at which point the
  // previous active file's frames become ordinary — no sweep, one comparison
  // in the victim check. kNoActiveFile pins nothing.
  static constexpr std::uint32_t kNoActiveFile = ~std::uint32_t{0};
  void set_active_file(std::uint32_t file_id) noexcept {
    active_file_id_.store(file_id, std::memory_order_relaxed);
  }

  // The writer just appended bytes at [offset, offset + bytes.size()) of the
  // active file. Puts them in the pool so the active file stays resident and
  // read-your-own-writes never touches disk. A frame already resident is
  // extended in place with no version bump: an append never modifies existing
  // bytes, and a reader only asks for bytes below the published file size.
  // A frame not yet
  // resident is admitted with its written prefix. Best effort: if no frame can
  // be claimed the bytes stay on disk and reads take the buffered fallback.
  void append_resident(std::uint32_t file_id, std::uint64_t offset,
                       std::span<const std::byte> bytes);

  [[nodiscard]] auto direct_io() const noexcept -> bool { return direct_io_; }

  // A pool-backed file reports that its filesystem refused O_DIRECT and it
  // will fill through the page cache. Surfaced so a CI run on such a mount
  // cannot silently measure the wrong thing.
  void note_direct_io_fallback() noexcept {
    counters_.direct_io_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }

private:
  friend class FrameLease;

  struct alignas(16) Slot {
    std::atomic<std::uint64_t> key{kEmptyKey};
    // Frame index, with the CLOCK reference bit in the top bit.
    std::atomic<std::uint32_t> frame{0};
    // Odd while the slot or the frame it maps is being changed.
    std::atomic<std::uint32_t> version{0};
  };
  static_assert(sizeof(Slot) == 16);

  static constexpr std::uint64_t kEmptyKey = ~std::uint64_t{0};
  static constexpr std::uint32_t kRefBit = std::uint32_t{1} << 31;
  // Set in a frame's pin word from the moment eviction claims it until its
  // new contents are in place. A reader whose pin lands on a dead frame
  // backs off; eviction's claim fails while any pin is held.
  static constexpr std::uint32_t kDead = std::uint32_t{1} << 31;
  static constexpr std::size_t kNoSlot = ~std::size_t{0};
  // An entry larger than capacity / this is read straight to the caller and
  // never admitted: admitting it would evict the working set for one value.
  static constexpr std::size_t kOversizeDivisor = 8;
  // Beyond this a racing eviction is likely pathological, and a direct read
  // is always correct — so the reader stops retrying and pays the syscall.
  static constexpr int kMaxRetries = 8;

  [[nodiscard]] static auto make_key(std::uint32_t file_id,
                                     std::uint64_t frame_index) noexcept
      -> std::uint64_t {
    return (static_cast<std::uint64_t>(file_id) << 32) |
           (frame_index & 0xFFFFFFFFULL);
  }

  [[nodiscard]] static auto hash_key(std::uint64_t k) noexcept -> std::size_t {
    // splitmix64 finalizer — frame indices are dense, so the low bits alone
    // would collide heavily under linear probing.
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return static_cast<std::size_t>(k);
  }

  [[nodiscard]] auto home(std::uint64_t key) const noexcept -> std::size_t {
    return hash_key(key) & table_mask_;
  }

  [[nodiscard]] auto frame_bytes(std::uint32_t frame) noexcept -> std::byte * {
    return arena_.data() + static_cast<std::size_t>(frame) * kPoolFrameBytes;
  }

  // Takes a pin on frame. Returns false, with the pin already dropped, if
  // the frame is dead: eviction has claimed it and its bytes are or will be
  // some other key's.
  [[nodiscard]] auto pin(std::uint32_t frame) noexcept -> bool {
    if ((pins_[frame].fetch_add(1, std::memory_order_acquire) & kDead) == 0) {
      return true;
    }
    unpin(frame);
    return false;
  }
  void unpin(std::uint32_t frame) noexcept {
    pins_[frame].fetch_sub(1, std::memory_order_release);
  }

  // O_DIRECT needs the offset, the length and the buffer aligned to the
  // device block size, and frames give all three at 4 KiB — a multiple of
  // every real block size, so no statx probe is needed. The length is rounded
  // up, and a read that runs past EOF comes back short, which is allowed.
  // Returns false on anything else so the caller retries buffered: a device
  // wanting alignment above 4 KiB answers EINVAL here, not at open.
  [[nodiscard]] static auto pread_direct(int fd, std::byte *dst,
                                         std::size_t len,
                                         std::uint64_t offset) noexcept
      -> bool {
    const auto want = align_up(len, kPoolFrameBytes);
    std::size_t done = 0;
    while (done < want) {
      const auto n = ::pread(fd, dst + done, want - done,
                             static_cast<off_t>(offset + done));
      if (n < 0) return false;
      if (n == 0) break;  // EOF
      done += static_cast<std::size_t>(n);
    }
    return done >= len;
  }

  static void pread_exact(int fd, std::byte *dst, std::size_t len,
                          std::uint64_t offset) {
    std::size_t done = 0;
    while (done < len) {
      const auto n = ::pread(fd, dst + done, len - done,
                             narrow<off_t>(offset + done));
      if (n <= 0) {
        throw std::system_error{errno, std::generic_category(),
                                "BufferPool: pread failed"};
      }
      done += static_cast<std::size_t>(n);
    }
  }

  [[nodiscard]] static constexpr auto align_up(std::size_t v,
                                               std::size_t a) noexcept
      -> std::size_t {
    return (v + a - 1) / a * a;
  }

  // Page-aligned, grow-only scratch for fills, held thread_local by read_at:
  // a fresh buffer would heap-allocate on every miss, and a direct read needs
  // an aligned one anyway.
  class AlignedScratch {
  public:
    AlignedScratch() = default;
    ~AlignedScratch() { std::free(p_); }
    AlignedScratch(const AlignedScratch &) = delete;
    auto operator=(const AlignedScratch &) -> AlignedScratch & = delete;

    // bytes must be a multiple of kPoolFrameBytes (aligned_alloc requires it).
    [[nodiscard]] auto ensure(std::size_t bytes) -> std::byte * {
      if (bytes > cap_) {
        std::free(p_);
        p_ = static_cast<std::byte *>(std::aligned_alloc(kPoolFrameBytes, bytes));
        if (p_ == nullptr) {
          cap_ = 0;
          throw std::bad_alloc{};
        }
        cap_ = bytes;
      }
      return p_;
    }

  private:
    std::byte *p_{nullptr};
    std::size_t cap_{0};
  };

  // Copies this frame's overlap with [offset, offset+len) into dst. Returns
  // false on a miss or after losing a race to an eviction; callers then
  // refill the whole read, so a partial write to dst is fine.
  //
  // Pin first, validate second: the slot named frame f for this key when we
  // read it, but f could have been claimed and refilled between that load
  // and the pin. Its slot would have been erased to do that, and a slot's
  // version only ever moves, so the version being what it was proves the
  // slot still maps key to f — and the pin now held proves f stays put.
  [[nodiscard]] auto copy_out(std::uint64_t key, std::uint64_t frame_index,
                              std::uint64_t offset, std::size_t len,
                              std::byte *dst) noexcept -> bool {
    const auto f = find_pinned(key);
    if (f == kNoFrame) return false;
    const auto frame_start = frame_index * kPoolFrameBytes;
    const auto from = std::max(offset, frame_start);
    const auto to = std::min(offset + len, frame_start + kPoolFrameBytes);
    std::memcpy(dst + (from - offset), frame_bytes(f) + (from - frame_start),
                static_cast<std::size_t>(to - from));
    unpin(f);
    return true;
  }

  static constexpr std::uint32_t kNoFrame = ~std::uint32_t{0};

  // Probes for key and returns its frame with a pin held, or kNoFrame with
  // nothing held: a miss, or a race with eviction lost kMaxRetries times, and
  // the caller then reads the device, which is always correct.
  [[nodiscard]] auto find_pinned(std::uint64_t key) noexcept -> std::uint32_t {
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      for (auto h = home(key);; h = (h + 1) & table_mask_) {
        auto &s = table_[h];
        const auto v1 = s.version.load(std::memory_order_acquire);
        const auto k = s.key.load(std::memory_order_relaxed);
        if (k == kEmptyKey) return kNoFrame;
        if (k != key) continue;
        if ((v1 & 1U) != 0U) break;  // being changed: retry from the top
        const auto tagged = s.frame.load(std::memory_order_relaxed);
        const auto f = tagged & ~kRefBit;
        if (!pin(f)) break;
        // The seqlock reader's fence: the key and frame loads above may not
        // sink below the version re-read, and neither an acquire load nor
        // the acquiring pin RMW orders the loads before them.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.version.load(std::memory_order_acquire) != v1) {
          unpin(f);
          break;
        }
        // Test-then-set: a hot frame writes nothing after the first touch.
        if ((tagged & kRefBit) == 0) {
          s.frame.fetch_or(kRefBit, std::memory_order_relaxed);
        }
        return f;
      }
    }
    return kNoFrame;
  }

  // ----- everything below runs under mu_ -----

  [[nodiscard]] auto find(std::uint64_t key) const noexcept -> std::size_t {
    for (auto h = home(key);; h = (h + 1) & table_mask_) {
      const auto k = table_[h].key.load(std::memory_order_relaxed);
      if (k == key) return h;
      if (k == kEmptyKey) return kNoSlot;
    }
  }

  // The seqlock bracket. The release fence after the odd store keeps the
  // changes that follow from becoming visible before the odd version does.
  static void begin_change(Slot &s) noexcept {
    s.version.store(s.version.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
  }
  static void end_change(Slot &s) noexcept {
    s.version.store(s.version.load(std::memory_order_relaxed) + 1,
                    std::memory_order_release);
  }

  // Places key -> frame, writing len bytes of src into the frame first. The
  // frame is dead on entry — claimed by eviction, or fresh and named by no
  // slot — so no reader is in it; the dead bit is cleared last, after the
  // slot is even, so a reader that pins it then finds the slot as it is.
  // Cleared with an RMW, not a store of zero: a reader that loaded the
  // victim's old slot can still be between the fetch_add of a pin that will
  // fail and the fetch_sub that drops it, and a store would wipe that +1 so
  // the fetch_sub wraps the word below zero — a frame no one could pin or
  // claim again, and whose pin word's dead bit later flickers off.
  void insert(std::uint64_t key, std::uint32_t frame, const std::byte *src,
              std::size_t len) noexcept {
    auto h = home(key);
    while (table_[h].key.load(std::memory_order_relaxed) != kEmptyKey) {
      h = (h + 1) & table_mask_;
    }
    auto &s = table_[h];
    begin_change(s);
    s.frame.store(frame | kRefBit, std::memory_order_relaxed);
    s.key.store(key, std::memory_order_relaxed);
    std::memcpy(frame_bytes(frame), src, len);
    end_change(s);
    pins_[frame].fetch_and(~kDead, std::memory_order_release);
  }

  // Removes the entry at slot i by backward shift: every later entry in the
  // cluster whose probe path crosses i moves back into the hole, and the
  // last hole is cleared. Readers see each moved entry at its new slot before
  // the old one changes, so the only transient state is a duplicate.
  void erase(std::size_t i) noexcept {
    for (auto j = (i + 1) & table_mask_;; j = (j + 1) & table_mask_) {
      const auto k = table_[j].key.load(std::memory_order_relaxed);
      if (k == kEmptyKey) break;
      // Cyclic: does i lie in [home(k), j)? Then k's probe crosses the hole.
      const auto h = home(k);
      const bool crosses = h <= j ? (h <= i && i < j) : (h <= i || i < j);
      if (!crosses) continue;
      auto &dst = table_[i];
      begin_change(dst);
      dst.frame.store(table_[j].frame.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
      dst.key.store(k, std::memory_order_relaxed);
      end_change(dst);
      i = j;
    }
    auto &s = table_[i];
    begin_change(s);
    s.key.store(kEmptyKey, std::memory_order_relaxed);
    end_change(s);
  }

  // CLOCK over the index: advance the hand, clearing reference bits, until an
  // unreferenced entry of a sealed file turns up whose frame no reader holds.
  // That frame is claimed (dead) on return. A frame a reader is in is passed
  // over, never waited for: a pin can be held across a caller's loop body.
  // Returns kNoSlot if nothing could be claimed.
  [[nodiscard]] auto clock_victim() noexcept -> std::size_t {
    const auto active = active_file_id_.load(std::memory_order_relaxed);
    for (std::size_t steps = 0; steps <= 2 * table_mask_ + 2; ++steps) {
      const auto i = hand_;
      hand_ = (hand_ + 1) & table_mask_;
      auto &s = table_[i];
      const auto k = s.key.load(std::memory_order_relaxed);
      if (k == kEmptyKey || (k >> 32) == active) continue;
      if ((s.frame.load(std::memory_order_relaxed) & kRefBit) != 0) {
        s.frame.fetch_and(~kRefBit, std::memory_order_relaxed);
        continue;
      }
      const auto f = s.frame.load(std::memory_order_relaxed) & ~kRefBit;
      std::uint32_t unpinned = 0;
      if (pins_[f].compare_exchange_strong(unpinned, kDead,
                                           std::memory_order_acq_rel)) {
        return i;
      }
    }
    return kNoSlot;
  }

  // Caches len bytes of src as the frame for key. Returns false if the key
  // is already resident or no frame could be claimed; either way the caller
  // already holds the bytes.
  auto admit(std::uint64_t key, const std::byte *src, std::size_t len)
      -> bool {
    if (find(key) != kNoSlot) return false;
    std::uint32_t frame = 0;
    if (used_frames_ < frame_count_) {
      frame = narrow<std::uint32_t>(used_frames_++);
      // Named by no slot yet, so nothing can pin it; dead keeps the
      // invariant that a frame is written only while dead.
      pins_[frame].store(kDead, std::memory_order_relaxed);
      counters_.frames_resident.store(narrow<std::int64_t>(used_frames_),
                                           std::memory_order_relaxed);
    } else {
      const auto victim = clock_victim();
      if (victim == kNoSlot) return false;
      frame = table_[victim].frame.load(std::memory_order_relaxed) & ~kRefBit;
      erase(victim);
      counters_.evictions.fetch_add(1, std::memory_order_relaxed);
    }
    insert(key, frame, src, len);
    return true;
  }

  PoolCounters counters_;
  bool direct_io_;
  std::size_t oversize_limit_;
  std::size_t frame_count_{0};
  std::size_t table_mask_{0};
  std::vector<std::byte> arena_;
  // Per frame: the number of readers in it, kDead while eviction owns it.
  std::vector<std::atomic<std::uint32_t>> pins_;
  std::vector<Slot> table_;
  std::mutex mu_;
  std::size_t hand_{0};         // guarded by mu_
  std::size_t used_frames_{0};  // guarded by mu_; every used frame is indexed
  std::atomic<std::uint32_t> active_file_id_{kNoActiveFile};
};

void BufferPool::read_at(std::uint32_t file_id, PoolFile file,
                         std::uint64_t offset, std::size_t len,
                         std::size_t file_size, std::byte *dst) {
  if (len == 0) return;

  // An oversize entry would evict the working set to hold one value. It
  // lands in the caller's buffer, which is not aligned, so never O_DIRECT.
  if (len > oversize_limit_) {
    pread_exact(file.buffered, dst, len, offset);
    return;
  }

  const auto first = offset / kPoolFrameBytes;
  const auto last = (offset + len - 1) / kPoolFrameBytes;

  // Reads frames [a, b] in one I/O, copies their overlap with the request
  // into dst, and admits each whole frame. The caller's bytes come from the
  // read buffer, not from the frames: a frame admitted here can be evicted
  // before we would copy out of it.
  const auto fill_run = [&](std::uint64_t a, std::uint64_t b) {
    const auto run_start = a * kPoolFrameBytes;
    const auto run_end =
        std::min<std::uint64_t>((b + 1) * kPoolFrameBytes, file_size);
    const auto from = std::max(offset, run_start);
    const auto to = std::min<std::uint64_t>(offset + len, (b + 1) * kPoolFrameBytes);
    if (run_end <= run_start) {  // entirely past the size we were given
      pread_exact(file.buffered, dst + (from - offset),
                  static_cast<std::size_t>(to - from), from);
      return;
    }
    // Declared on the miss path so a hit never touches thread-local storage.
    // Thread-exit destructor is intentional; suppress the Clang diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    thread_local AlignedScratch scratch;
#pragma clang diagnostic pop
    const auto run_len = static_cast<std::size_t>(run_end - run_start);
    auto *buf = scratch.ensure(align_up(run_len, kPoolFrameBytes));
    if (file.direct < 0 || !pread_direct(file.direct, buf, run_len, run_start)) {
      pread_exact(file.buffered, buf, run_len, run_start);
    }
    if (to > run_end) {
      throw std::system_error{
          EIO, std::generic_category(),
          std::format("BufferPool::read_at: file {} is shorter than the "
                      "requested range [{}, {})",
                      file_id, offset, offset + len)};
    }
    std::memcpy(dst + (from - offset), buf + (from - run_start),
                static_cast<std::size_t>(to - from));

    std::lock_guard<std::mutex> lk{mu_};
    for (auto f = a; f <= b; ++f) {
      const auto frame_start = f * kPoolFrameBytes;
      // Only whole frames are admitted; a short tail frame stays uncached.
      if (frame_start + kPoolFrameBytes > file_size) break;
      if (admit(make_key(file_id, f), buf + (frame_start - run_start),
                kPoolFrameBytes)) {
        counters_.fills.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  // Widens a run of missing frames to the fill blocks it touches, while the
  // pool has free frames. Blocks are how a cold pool warms; in a full one
  // each block's neighbours would evict frames that earned their place — at
  // a pool a tenth of a Zipf dataset, block fills cut throughput 5x. The
  // resident count is read without the lock: it only grows until the pool is
  // full, and a stale value picks a fill size, never a correctness outcome.
  // A pool too small for a block to pass the oversize guard fills frames
  // only, or one miss would evict the frames it had just admitted. Frames of
  // the block already resident are read again and not re-admitted; fill_run
  // clips the block to the file.
  const auto fill_missing = [&](std::uint64_t a, std::uint64_t b) {
    if (kPoolFillBlockBytes <= oversize_limit_ &&
        counters_.frames_resident.load(std::memory_order_relaxed) <
            counters_.frames_total) {
      constexpr auto kBlockFrames = kPoolFillBlockBytes / kPoolFrameBytes;
      a = a / kBlockFrames * kBlockFrames;
      b = (b / kBlockFrames + 1) * kBlockFrames - 1;
    }
    fill_run(a, b);
  };

  // Serve every resident frame from the pool and read only the runs of
  // frames that are not. A value of many frames with one evicted costs one
  // small read, not the whole value again — which matters exactly when
  // values are large, since CLOCK evicts frames, not values.
  bool any_miss = false;
  bool in_run = false;
  std::uint64_t run_start = 0;
  for (auto f = first; f <= last; ++f) {
    if (copy_out(make_key(file_id, f), f, offset, len, dst)) {
      if (in_run) {
        fill_missing(run_start, f - 1);
        in_run = false;
      }
    } else if (!in_run) {
      run_start = f;
      in_run = true;
      any_miss = true;
    }
  }
  if (in_run) fill_missing(run_start, last);
  (any_miss ? counters_.misses : counters_.hits).add(1);
}

auto BufferPool::view(std::uint32_t file_id, std::uint64_t offset,
                      std::size_t file_size, FrameLease &lease)
    -> std::span<const std::byte> {
  lease.reset();
  if (offset >= file_size) return {};
  const auto frame_index = offset / kPoolFrameBytes;
  const auto f = find_pinned(make_key(file_id, frame_index));
  if (f == kNoFrame) return {};
  lease = FrameLease{this, f};
  const auto frame_start = frame_index * kPoolFrameBytes;
  const auto end = std::min<std::uint64_t>(frame_start + kPoolFrameBytes, file_size);
  return {frame_bytes(f) + (offset - frame_start),
          static_cast<std::size_t>(end - offset)};
}

void FrameLease::reset() noexcept {
  if (pool_ != nullptr) {
    pool_->unpin(frame_);
    pool_ = nullptr;
  }
}

void BufferPool::append_resident(std::uint32_t file_id, std::uint64_t offset,
                                 std::span<const std::byte> bytes) {
  if (bytes.empty()) return;
  const auto end = offset + bytes.size();
  std::lock_guard<std::mutex> lk{mu_};
  for (auto f = offset / kPoolFrameBytes; f <= (end - 1) / kPoolFrameBytes; ++f) {
    const auto frame_start = f * kPoolFrameBytes;
    const auto seg_from = std::max(offset, frame_start);
    const auto seg_to = std::min(end, frame_start + kPoolFrameBytes);
    const auto *src = bytes.data() + (seg_from - offset);
    const auto seg_len = static_cast<std::size_t>(seg_to - seg_from);
    const auto in_frame = static_cast<std::size_t>(seg_from - frame_start);
    const auto key = make_key(file_id, f);

    if (const auto s = find(key); s != kNoSlot) {
      // Extends the frame past the size every reader is bounded by: the
      // bytes written are ones no reader can be reading, pinned or not.
      const auto frame = table_[s].frame.load(std::memory_order_relaxed);
      std::memcpy(frame_bytes(frame & ~kRefBit) + in_frame, src, seg_len);
      continue;
    }
    // Only a frame whose written prefix starts here can be admitted from
    // these bytes alone; anything earlier in the frame is on disk, and a
    // read miss will admit the whole frame later.
    if (in_frame == 0) (void)admit(key, src, seg_len);
  }
}

} // namespace bytecask
