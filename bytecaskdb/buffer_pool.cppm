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
#include <sys/types.h>
#include <unistd.h>

export module bytecask.buffer_pool;

import bytecask.counters;
import bytecask.util;

namespace bytecask {

// The O_DIRECT alignment unit and the device transfer minimum, so read
// amplification over a bare pread is approximately zero — the kernel already
// reads a page minimum today.
export inline constexpr std::size_t kPoolFrameBytes = 4096;

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

// ---------------------------------------------------------------------------
// BufferPool — fixed-size frame cache keyed by (file_id, frame_index).
//
// Two arrays: an arena of 4 KiB frames and an open-addressed index of 16-byte
// slots. A slot is the cache's only metadata — key, frame number, CLOCK
// reference bit and a seqlock version — so a hit touches one index line and
// then the frame.
//
// Reads are lock-free: probe the index with acquire loads, copy out of the
// frame under the slot's seqlock, and retry if the version moved. Nothing on
// the hit path writes except a test-then-set of the reference bit, so a hot
// frame writes nothing after its first touch.
//
// Every change to the index or to a frame's contents happens under one mutex,
// and each change is bracketed by the slot's version going odd and then even
// again. That is the whole invariant: a reader that copied a frame while its
// slot's version stayed even and unchanged copied exactly the bytes that slot
// mapped to. The mutex is not held across device reads — a miss reads the
// frame into a thread-local buffer first and admits it afterwards.
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
  BufferPool(const BufferPoolOptions &opts, Counters &counters)
      : counters_{counters}, direct_io_{opts.direct_io},
        oversize_limit_{opts.capacity_bytes / kOversizeDivisor} {
    // The table is a power of two at most 70 % full; the arena takes what is
    // left of the budget. Frame count is derived from the budget, never the
    // budget from the frame count — the bound is the contract.
    const auto wanted = opts.capacity_bytes / (kPoolFrameBytes + sizeof(Slot));
    const auto table = std::bit_ceil(std::max<std::size_t>(wanted * 10 / 7, 2));
    const auto table_bytes = table * sizeof(Slot);
    const auto frames = std::min(
        opts.capacity_bytes > table_bytes
            ? (opts.capacity_bytes - table_bytes) / kPoolFrameBytes
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
    arena_ = std::vector<std::atomic<std::uint64_t>>(frame_count_ *
                                                     kWordsPerFrame);
    table_ = std::vector<Slot>(table);
    counters_.pool_frames_total = narrow<std::int64_t>(frame_count_);
  }

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  [[nodiscard]] auto frame_count() const noexcept -> std::size_t {
    return frame_count_;
  }

  // Reads exactly len bytes at offset of file_id's file into dst, serving what
  // is resident and filling the rest. file_size bounds admission: a frame that
  // extends past EOF is served directly and never cached, which is what lets
  // a frame's contents be fixed-length with no per-frame valid-length field.
  //
  // Throws std::system_error if the underlying read fails or comes up short.
  void read_at(std::uint32_t file_id, PoolFile file, std::uint64_t offset,
               std::size_t len, std::size_t file_size, std::byte *dst);

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
  // bytes, a boundary word is observed old-or-new atomically, and a reader
  // only asks for bytes below the published file size. A frame not yet
  // resident is admitted with its written prefix. Best effort: if no frame can
  // be claimed the bytes stay on disk and reads take the buffered fallback.
  void append_resident(std::uint32_t file_id, std::uint64_t offset,
                       std::span<const std::byte> bytes);

  [[nodiscard]] auto direct_io() const noexcept -> bool { return direct_io_; }

  // A pool-backed file reports that its filesystem refused O_DIRECT and it
  // will fill through the page cache. Surfaced so a CI run on such a mount
  // cannot silently measure the wrong thing.
  void note_direct_io_fallback() noexcept {
    counters_.pool_direct_io_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }

private:
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
  static constexpr std::size_t kNoSlot = ~std::size_t{0};
  // An entry larger than capacity / this is read straight to the caller and
  // never admitted: admitting it would evict the working set for one value.
  static constexpr std::size_t kOversizeDivisor = 8;
  // Beyond this a racing eviction is likely pathological, and a direct read
  // is always correct — so the reader stops retrying and pays the syscall.
  static constexpr int kMaxRetries = 8;
  static constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
  static constexpr std::size_t kWordsPerFrame = kPoolFrameBytes / kWordBytes;

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

  [[nodiscard]] auto words(std::uint32_t frame) noexcept
      -> std::atomic<std::uint64_t> * {
    return arena_.data() + static_cast<std::size_t>(frame) * kWordsPerFrame;
  }

  // Frames hold atomic words, not plain bytes: a reader copying out of a
  // frame runs concurrently with a fill writing into it. The seqlock decides
  // whether the bytes are usable, but two plain memcpys racing is a data race
  // and therefore UB whatever the version check proves. Relaxed atomic word
  // accesses make it well-defined; on x86 they are plain movs.
  //
  // Head, body, tail: only a boundary word is sliced; every whole word is one
  // load and one 8-byte memcpy, which the compiler turns into a mov pair.
  static void read_words(const std::atomic<std::uint64_t> *w,
                         std::size_t byte_off, std::size_t len,
                         std::byte *dst) noexcept {
    w += byte_off / kWordBytes;
    std::size_t done = 0;
    if (const auto head = byte_off % kWordBytes; head != 0) {
      const auto chunk = std::min(kWordBytes - head, len);
      const auto value = w++->load(std::memory_order_relaxed);
      std::byte buf[kWordBytes];
      std::memcpy(buf, &value, kWordBytes);
      std::memcpy(dst, buf + head, chunk);
      done = chunk;
    }
    for (; done + kWordBytes <= len; done += kWordBytes) {
      const auto value = w++->load(std::memory_order_relaxed);
      std::memcpy(dst + done, &value, kWordBytes);
    }
    if (done < len) {
      const auto value = w->load(std::memory_order_relaxed);
      std::memcpy(dst + done, &value, len - done);
    }
  }

  // Writes len bytes at byte_off of a frame. A boundary word is
  // load-patch-store: the bytes it already held are unchanged, so a reader
  // sees an old-or-new word whose old part is identical either way.
  static void write_words(std::atomic<std::uint64_t> *w, std::size_t byte_off,
                          const std::byte *src, std::size_t len) noexcept {
    w += byte_off / kWordBytes;
    std::size_t done = 0;
    const auto patch = [&](std::size_t in_word, std::size_t chunk) {
      auto value = w->load(std::memory_order_relaxed);
      std::byte buf[kWordBytes];
      std::memcpy(buf, &value, kWordBytes);
      std::memcpy(buf + in_word, src + done, chunk);
      std::memcpy(&value, buf, kWordBytes);
      w++->store(value, std::memory_order_relaxed);
      done += chunk;
    };
    if (const auto head = byte_off % kWordBytes; head != 0) {
      patch(head, std::min(kWordBytes - head, len));
    }
    for (; done + kWordBytes <= len; done += kWordBytes) {
      std::uint64_t value = 0;
      std::memcpy(&value, src + done, kWordBytes);
      w++->store(value, std::memory_order_relaxed);
    }
    if (done < len) patch(0, len - done);
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

  // Copies this frame's overlap with [offset, offset+len) into dst under the
  // slot's seqlock. Returns false on a miss or after losing a race to an
  // eviction; callers then refill the whole read, so a partial write to dst
  // is fine.
  [[nodiscard]] auto copy_out(std::uint64_t key, std::uint64_t frame_index,
                              std::uint64_t offset, std::size_t len,
                              std::byte *dst) noexcept -> bool {
    const auto frame_start = frame_index * kPoolFrameBytes;
    const auto from = std::max(offset, frame_start);
    const auto to = std::min(offset + len, frame_start + kPoolFrameBytes);

    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      for (auto h = home(key);; h = (h + 1) & table_mask_) {
        auto &s = table_[h];
        const auto v1 = s.version.load(std::memory_order_acquire);
        const auto k = s.key.load(std::memory_order_relaxed);
        if (k == kEmptyKey) return false;
        if (k != key) continue;
        if ((v1 & 1U) != 0U) break;  // being changed: retry from the top
        const auto f = s.frame.load(std::memory_order_relaxed);
        read_words(words(f & ~kRefBit), from - frame_start, to - from,
                   dst + (from - offset));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.version.load(std::memory_order_relaxed) != v1) break;
        // Test-then-set: a hot frame writes nothing after the first touch.
        if ((f & kRefBit) == 0) s.frame.fetch_or(kRefBit, std::memory_order_relaxed);
        return true;
      }
    }
    return false;  // caller falls back to a direct read, always correct
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

  // Places key -> frame, writing len bytes of src into the frame first.
  // The slot stays odd for the whole frame write, so a reader probing for
  // this key waits rather than copying a half-written frame.
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
    write_words(words(frame), 0, src, len);
    end_change(s);
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
  // unreferenced, unpinned entry turns up. Returns kNoSlot if every entry is
  // pinned.
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
      return i;
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
      counters_.pool_frames_resident.store(narrow<std::int64_t>(used_frames_),
                                           std::memory_order_relaxed);
    } else {
      const auto victim = clock_victim();
      if (victim == kNoSlot) return false;
      frame = table_[victim].frame.load(std::memory_order_relaxed) & ~kRefBit;
      erase(victim);
      counters_.pool_evictions.fetch_add(1, std::memory_order_relaxed);
    }
    insert(key, frame, src, len);
    return true;
  }

  Counters &counters_;
  bool direct_io_;
  std::size_t oversize_limit_;
  std::size_t frame_count_{0};
  std::size_t table_mask_{0};
  std::vector<std::atomic<std::uint64_t>> arena_;
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

  // Serve every frame from cache before touching the device, so a fully
  // resident multi-frame read costs no I/O at all.
  bool all_hit = true;
  for (auto f = first; f <= last && all_hit; ++f) {
    all_hit = copy_out(make_key(file_id, f), f, offset, len, dst);
  }
  if (all_hit) {
    counters_.pool_hits.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  counters_.pool_misses.fetch_add(1, std::memory_order_relaxed);

  // One read of the frame-aligned extent, then admit each whole frame it
  // covered. The caller's bytes come from this buffer, not from the frames:
  // a frame admitted here can be evicted before we would copy out of it.
  const auto extent_start = first * kPoolFrameBytes;
  const auto extent_end =
      std::min<std::uint64_t>((last + 1) * kPoolFrameBytes, file_size);
  if (extent_end <= extent_start) {
    pread_exact(file.buffered, dst, len, offset);
    return;
  }
  const auto extent_len = static_cast<std::size_t>(extent_end - extent_start);
  // Thread-exit destructor is intentional; suppress the Clang diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local AlignedScratch scratch;
#pragma clang diagnostic pop
  auto *buf = scratch.ensure(align_up(extent_len, kPoolFrameBytes));
  if (file.direct < 0 || !pread_direct(file.direct, buf, extent_len, extent_start)) {
    pread_exact(file.buffered, buf, extent_len, extent_start);
  }

  const auto copy_from = static_cast<std::size_t>(offset - extent_start);
  if (copy_from + len > extent_len) {
    throw std::system_error{
        EIO, std::generic_category(),
        std::format("BufferPool::read_at: file {} is shorter than the "
                    "requested range [{}, {})",
                    file_id, offset, offset + len)};
  }
  std::memcpy(dst, buf + copy_from, len);

  std::lock_guard<std::mutex> lk{mu_};
  for (auto f = first; f <= last; ++f) {
    const auto frame_start = f * kPoolFrameBytes;
    // Only whole frames are admitted; a short tail frame stays uncached.
    if (frame_start + kPoolFrameBytes > file_size) break;
    if (admit(make_key(file_id, f), buf + (frame_start - extent_start),
              kPoolFrameBytes)) {
      counters_.pool_fills.fetch_add(1, std::memory_order_relaxed);
    }
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
      const auto frame = table_[s].frame.load(std::memory_order_relaxed);
      write_words(words(frame & ~kRefBit), in_frame, src, seg_len);
      continue;
    }
    // Only a frame whose written prefix starts here can be admitted from
    // these bytes alone; anything earlier in the frame is on disk, and a
    // read miss will admit the whole frame later.
    if (in_frame == 0) (void)admit(key, src, seg_len);
  }
}

} // namespace bytecask
