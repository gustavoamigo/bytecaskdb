// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — bounded buffer pool for sealed data file reads.
//
// See docs/buffer_pool_design.md. The pool exists to make the engine's memory
// footprint an operator-set number rather than one the kernel negotiates: the
// key directory must be fully resident and cannot be evicted, so bounding the
// value cache is the only way to protect it.
//
// V0 scope (design Phase 1): buffered pread fills, CLOCK eviction, copy-out.
// No O_DIRECT yet, so the page cache still backs the fills — this bounds the
// pool's own memory, not yet the total.

module;
#include <algorithm>
#include <atomic>
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
  // 0 disables the pool. This is TOTAL footprint — frames, per-frame metadata
  // and the index table all come out of it, so that configuring 1 GiB yields
  // 1 GiB of resident memory rather than 1 GiB plus overhead.
  std::size_t capacity_bytes{0};
  // An entry whose extent exceeds capacity/this is read straight to the caller
  // and never admitted: admitting it would evict the working set to hold a
  // single value. Derived from capacity, not a tuning knob.
  unsigned oversize_guard_divisor{8};
};

// ---------------------------------------------------------------------------
// BufferPool — fixed-size frame cache keyed by (cache_id, frame_index).
//
// Reads are lock-free: a probe of the index table with acquire loads, then a
// seqlock-protected memcpy out of the frame. Nothing on the hit path writes
// except a test-then-set of the CLOCK reference bit, so a hot frame writes
// nothing after its first touch and 32 readers of one frame never take a
// cache line exclusive.
//
// Fill and eviction take one mutex, but release it across the device read, so
// the hold is a few hundred nanoseconds against a 10–100 µs read. The design
// (§8.2) proposes ~64 stripes; V0 takes the simpler structure until a measured
// workload shows the single lock is contended, per tenet 2 over tenet 4. The
// number that matters — lock-free reads — is unaffected either way.
//
// Copy-out, never a span into a frame: frames are reused memory, so a reader
// racing eviction would otherwise get bytes that are part old and part new.
// Ordering makes the slot consistent; only copying under the version check
// makes the bytes consistent.
// ---------------------------------------------------------------------------
export class BufferPool {
public:
  // Thrown when capacity is too small to hold any frame at all.
  BufferPool(const BufferPoolOptions &opts, Counters &counters)
      : counters_{counters},
        oversize_limit_{opts.oversize_guard_divisor > 0
                            ? opts.capacity_bytes / opts.oversize_guard_divisor
                            : opts.capacity_bytes} {
    // Frame count is derived from the budget, never the budget from the frame
    // count — see "The bound is the contract" in the design.
    const auto per_frame = kPoolFrameBytes + sizeof(FrameMeta) + kTableShare;
    const auto frames = opts.capacity_bytes / per_frame;
    if (frames == 0) {
      throw std::invalid_argument{std::format(
          "BufferPool: capacity_bytes = {} is too small to hold one {}-byte "
          "frame plus its metadata (needs at least {})",
          opts.capacity_bytes, kPoolFrameBytes, per_frame)};
    }
    frame_count_ = frames;
    table_mask_ = round_up_pow2(frames * 10 / 7) - 1;

    const auto arena_bytes = frame_count_ * kPoolFrameBytes;
    arena_ = static_cast<std::byte *>(
        std::aligned_alloc(kPoolFrameBytes, arena_bytes));
    if (arena_ == nullptr) {
      throw std::bad_alloc{};
    }
    // Touch every page now. Without this the arena faults in lazily, one page
    // at a time, on the read path — so a larger pool has a WORSE tail, which
    // is backwards. It also makes "configure N bytes, observe N bytes
    // resident" true: the operator asked for this memory, so take it at open
    // rather than charging it to p99 during serving.
    std::memset(arena_, 0, arena_bytes);
    meta_ = std::vector<FrameMeta>(frame_count_);
    table_ = std::vector<Slot>(table_mask_ + 1);
    counters_.pool_frames_total = narrow<std::int64_t>(frame_count_);
  }

  ~BufferPool() { std::free(arena_); }

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  [[nodiscard]] auto frame_count() const noexcept -> std::size_t {
    return frame_count_;
  }

  // Identifies a file's frames within the pool. Deliberately not the engine's
  // file_id: that is minted by the engine at points the file factory does not
  // reach (vacuum assigns it only once the compacted file is already open), and
  // borrowing it would force either a reordering of vacuum or a mutable field
  // on a type lock-free readers share. A pool-local id needs neither, and only
  // has to be unique — which is all the frame key ever asked of it.
  //
  // Monotonic and never reused, so a vacuumed file's frames are orphans that
  // CLOCK reclaims on its next pass rather than stale entries needing
  // invalidation.
  [[nodiscard]] inline auto acquire_cache_id() -> std::uint32_t {
    const auto id = next_cache_id_.fetch_add(1, std::memory_order_relaxed);
    if (id == kMaxCacheId) {
      // Wrapping would alias a live file's frames onto a new file and serve
      // its bytes. Unreachable in practice — it needs 2^32 sealed files in one
      // process — but silently wrong if it ever happened.
      throw std::runtime_error{
          "BufferPool: cache id space exhausted; reopen the database"};
    }
    return id;
  }

  // Reads exactly len bytes at offset of cache_id's file into dst, serving what is
  // resident and filling the rest. file_size bounds admission: a frame that
  // extends past EOF is served directly and never cached, which is what lets a
  // frame's contents be fixed-length with no per-frame valid-length field.
  //
  // Throws std::system_error if the underlying read fails or comes up short.
  void read_at(std::uint32_t cache_id, int fd, std::uint64_t offset,
               std::size_t len, std::size_t file_size, std::byte *dst) {
    if (len == 0) return;

    // An oversize entry would evict the working set to hold one value.
    if (len > oversize_limit_) {
      counters_.pool_oversize_reads.fetch_add(1, std::memory_order_relaxed);
      pread_exact(fd, dst, len, offset);
      return;
    }

    const auto first = offset / kPoolFrameBytes;
    const auto last = (offset + len - 1) / kPoolFrameBytes;
    if (first != last) {
      counters_.pool_multi_frame_reads.fetch_add(1, std::memory_order_relaxed);
    }

    // Try to serve every frame from cache before touching the device, so a
    // fully-resident multi-frame read costs no I/O at all.
    bool all_hit = true;
    for (auto f = first; f <= last && all_hit; ++f) {
      all_hit = copy_out_of_frame(make_key(cache_id, f), f, offset, len, dst);
    }
    if (all_hit) {
      counters_.pool_hits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    counters_.pool_misses.fetch_add(1, std::memory_order_relaxed);

    // Coalesce the misses into one read of the frame-aligned extent, then
    // admit each whole frame it covered.
    const auto extent_start = first * kPoolFrameBytes;
    const auto extent_end = (last + 1) * kPoolFrameBytes;
    const auto clamped_end = std::min<std::uint64_t>(extent_end, file_size);
    if (clamped_end <= extent_start) {
      pread_exact(fd, dst, len, offset);
      return;
    }
    const auto extent_len = static_cast<std::size_t>(clamped_end - extent_start);

    // Reused across calls. A fresh vector here would heap-allocate AND
    // zero-fill several KiB on every miss, immediately before overwriting all
    // of it — an allocation on the read path, which tenet 3 rules out.
    // Thread-exit destructor is intentional; suppress the Clang diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    thread_local std::vector<std::byte> scratch;
#pragma clang diagnostic pop
    scratch.resize(extent_len);
    pread_exact(fd, scratch.data(), extent_len, extent_start);

    // The caller's bytes come from the buffer we just read, not from the
    // frames: a frame admitted here can be evicted before we copy out of it.
    const auto copy_from = static_cast<std::size_t>(offset - extent_start);
    if (copy_from + len > extent_len) {
      throw std::system_error{
          EIO, std::generic_category(),
          std::format("BufferPool::read_at: file {} is shorter than the "
                      "requested range [{}, {})",
                      cache_id, offset, offset + len)};
    }
    std::memcpy(dst, scratch.data() + copy_from, len);

    for (auto f = first; f <= last; ++f) {
      const auto frame_start = f * kPoolFrameBytes;
      // Only whole frames are admitted; a short tail frame stays uncached.
      if (frame_start + kPoolFrameBytes > file_size) break;
      admit(make_key(cache_id, f),
            scratch.data() + (frame_start - extent_start));
    }
  }

private:
  // Both are 16 bytes; the table is sized at a 0.7 load factor, so each frame
  // carries 16/0.7 ≈ 23 bytes of table share inside the bound.
  struct alignas(16) FrameMeta {
    std::atomic<std::uint64_t> key{kEmptyKey};
    // Even = readable, odd = being filled. Readers retry on odd or on change.
    std::atomic<std::uint32_t> version{0};
    // CLOCK reference bit, test-then-set so a hot frame stops writing.
    std::atomic<std::uint8_t> ref{0};
  };

  struct alignas(16) Slot {
    std::atomic<std::uint64_t> key{kEmptyKey};
    std::atomic<std::uint32_t> frame{0};
  };

  static constexpr std::uint64_t kEmptyKey = ~std::uint64_t{0};
  static constexpr std::uint64_t kTombstoneKey = kEmptyKey - 1;
  static constexpr std::size_t kTableShare = sizeof(Slot) * 10 / 7;
  // Beyond this a racing eviction is likely pathological, and a direct read is
  // always correct — so the reader stops spinning and pays for the syscall.
  static constexpr int kMaxOptimisticRetries = 4;
  static constexpr std::uint32_t kMaxCacheId = ~std::uint32_t{0};

  [[nodiscard]] static auto make_key(std::uint32_t cache_id,
                                     std::uint64_t frame_index) noexcept
      -> std::uint64_t {
    return (static_cast<std::uint64_t>(cache_id) << 32) |
           (frame_index & 0xFFFFFFFFULL);
  }

  [[nodiscard]] static auto round_up_pow2(std::size_t v) noexcept
      -> std::size_t {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
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

  // Lock-free probe. Returns the frame index, or frame_count_ for a miss.
  [[nodiscard]] auto lookup(std::uint64_t key) const noexcept -> std::size_t {
    auto h = hash_key(key) & table_mask_;
    for (std::size_t probes = 0; probes <= table_mask_; ++probes) {
      const auto k = table_[h].key.load(std::memory_order_acquire);
      if (k == kEmptyKey) return frame_count_;
      if (k == key) return table_[h].frame.load(std::memory_order_acquire);
      h = (h + 1) & table_mask_;  // tombstones fall through and keep probing
    }
    return frame_count_;
  }

  // Copies this frame's overlap with [offset, offset+len) into dst under the
  // seqlock. Returns false on a miss or a lost race, leaving dst untouched in
  // the region it would have written only if it never started — callers treat
  // false as "the whole read must be refilled", so a partial write is fine.
  [[nodiscard]] auto copy_out_of_frame(std::uint64_t key,
                                       std::uint64_t frame_index,
                                       std::uint64_t offset, std::size_t len,
                                       std::byte *dst) -> bool {
    const auto frame_start = frame_index * kPoolFrameBytes;
    const auto from = std::max(offset, frame_start);
    const auto to = std::min(offset + len, frame_start + kPoolFrameBytes);
    if (to <= from) return false;

    for (int attempt = 0; attempt <= kMaxOptimisticRetries; ++attempt) {
      const auto f = lookup(key);
      if (f >= frame_count_) return false;

      auto &m = meta_[f];
      const auto v1 = m.version.load(std::memory_order_acquire);
      if ((v1 & 1U) != 0U) {  // mid-fill
        counters_.pool_optimistic_retries.fetch_add(1,
                                                    std::memory_order_relaxed);
        continue;
      }
      if (m.key.load(std::memory_order_acquire) != key) return false;

      std::memcpy(dst + (from - offset),
                  arena_ + f * kPoolFrameBytes + (from - frame_start),
                  static_cast<std::size_t>(to - from));

      std::atomic_thread_fence(std::memory_order_acquire);
      if (m.version.load(std::memory_order_relaxed) == v1) {
        // Test-then-set: a hot frame writes nothing after the first touch.
        if (m.ref.load(std::memory_order_relaxed) == 0) {
          m.ref.store(1, std::memory_order_relaxed);
        }
        return true;
      }
      counters_.pool_optimistic_retries.fetch_add(1, std::memory_order_relaxed);
    }
    return false;  // caller falls back to a direct read, always correct
  }

  // Caches one whole frame. Best-effort: a duplicate or a full sweep drops it,
  // since the caller already holds the bytes.
  void admit(std::uint64_t key, const std::byte *src) {
    std::size_t victim = 0;
    {
      std::lock_guard<std::mutex> lk{fill_mu_};
      if (lookup(key) < frame_count_) return;  // another thread won the fill
      if (!claim_victim(victim)) return;

      const auto old = meta_[victim].key.load(std::memory_order_relaxed);
      if (old != kEmptyKey) {
        table_erase(old);
        counters_.pool_evictions.fetch_add(1, std::memory_order_relaxed);
      }
      // Odd version parks readers and keeps CLOCK off this frame while the
      // copy below runs without the lock.
      meta_[victim].version.fetch_add(1, std::memory_order_release);
      meta_[victim].key.store(kEmptyKey, std::memory_order_relaxed);
    }

    std::memcpy(arena_ + victim * kPoolFrameBytes, src, kPoolFrameBytes);

    {
      std::lock_guard<std::mutex> lk{fill_mu_};
      // Publish contents before the key, so a reader that sees the key is
      // guaranteed to see the matching bytes.
      meta_[victim].key.store(key, std::memory_order_release);
      meta_[victim].ref.store(1, std::memory_order_relaxed);
      meta_[victim].version.fetch_add(1, std::memory_order_release);
      table_insert(key, victim);
    }
    counters_.pool_fills.fetch_add(1, std::memory_order_relaxed);
    counters_.pool_fill_bytes.fetch_add(narrow<std::int64_t>(kPoolFrameBytes),
                                        std::memory_order_relaxed);
  }

  // CLOCK: advance the hand, clearing reference bits, until an unreferenced
  // frame turns up. Frames mid-fill (odd version) are skipped. Caller holds
  // fill_mu_. Returns false if every frame is busy.
  [[nodiscard]] auto claim_victim(std::size_t &out) noexcept -> bool {
    const auto limit = frame_count_ * 2;
    for (std::size_t steps = 0; steps < limit; ++steps) {
      const auto f = hand_;
      hand_ = (hand_ + 1) % frame_count_;
      if ((meta_[f].version.load(std::memory_order_relaxed) & 1U) != 0U) {
        continue;
      }
      if (meta_[f].ref.load(std::memory_order_relaxed) != 0) {
        meta_[f].ref.store(0, std::memory_order_relaxed);
        continue;
      }
      out = f;
      return true;
    }
    return false;
  }

  // Caller holds fill_mu_.
  void table_insert(std::uint64_t key, std::size_t frame) noexcept {
    auto h = hash_key(key) & table_mask_;
    for (std::size_t probes = 0; probes <= table_mask_; ++probes) {
      const auto k = table_[h].key.load(std::memory_order_relaxed);
      if (k == kEmptyKey || k == kTombstoneKey || k == key) {
        table_[h].frame.store(narrow<std::uint32_t>(frame),
                              std::memory_order_release);
        table_[h].key.store(key, std::memory_order_release);
        return;
      }
      h = (h + 1) & table_mask_;
    }
  }

  // Caller holds fill_mu_. Tombstones rather than clears: a cleared slot would
  // cut a probe chain and hide keys inserted past it.
  void table_erase(std::uint64_t key) noexcept {
    auto h = hash_key(key) & table_mask_;
    for (std::size_t probes = 0; probes <= table_mask_; ++probes) {
      const auto k = table_[h].key.load(std::memory_order_relaxed);
      if (k == kEmptyKey) return;
      if (k == key) {
        table_[h].key.store(kTombstoneKey, std::memory_order_release);
        return;
      }
      h = (h + 1) & table_mask_;
    }
  }

  Counters &counters_;
  std::size_t oversize_limit_;
  std::size_t frame_count_{0};
  std::size_t table_mask_{0};
  std::byte *arena_{nullptr};
  std::vector<FrameMeta> meta_;
  mutable std::vector<Slot> table_;
  std::mutex fill_mu_;
  std::size_t hand_{0};  // guarded by fill_mu_
  std::atomic<std::uint32_t> next_cache_id_{0};
};

} // namespace bytecask
