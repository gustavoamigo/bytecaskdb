// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — the buffered blind tree: a blind B+ tree with a small
// unordered write buffer in front, folded into the tree by a merger thread
// (docs/commit_path_scaling_design.md, "Phase 1b").
//
// It implements the blind tree's interface — the member operations of
// PersistentBlindBTree / TransientBlindBTree and their iterator — so the
// engine's kd_* helpers use it unchanged; BYTECASK_KEYDIR=buffered selects
// it through the KeyDirTree aliases. Buffering, freezing, merging, installing
// and backpressure all happen here.
//
// Versions. A version is (tree T, frozen buffers F1..Fk oldest first, k at
// most kMaxFrozen, active buffer A with its slot count per partition). A
// frozen buffer is immutable. A is only appended to past a version's counts,
// into arrays that never move, so a reader of a version never sees a slot
// past its counts. A key's newest entry is in A, else Fk, ..., else F1, else
// T.
//
// The builder (TransientBufferedBlindBTree) appends every write to A and
// never edits the tree. When A is full it freezes: A joins the frozen
// buffers and a fresh buffer becomes A. A frozen buffer reaches the merger
// when the version holding it is published, together with what the merger
// needs to read records (the source, RS). The merger folds them in order,
// each into the tree the one before it produced: T1 = T + F1, T2 = T1 + F2.
// A builder made later from (T, F1..Fk, A) starts from (T1, F2..Fk, A) once
// T1 is done, which holds the same keys, and installs every finished merge
// from the oldest end. Frozen buffers queue, so a merge that runs long does
// not stop the writer; a builder waits for the merger only when kMaxFrozen
// buffers are all unmerged (backpressure), and one whose frozen buffers it
// froze itself, none of them handed over yet, merges the oldest on the
// spot.
//
// Draining. A buffer freezes when it is full, so once writes stop the last
// slots would stay buffered, and every read near them would order them among
// the tree's keys. Instead every publish installs the merges that have
// finished, and the merger thread watches for writes to pause: once a
// version holding buffered slots is published and no builder publishes for
// kIdleWindow, it calls the drain hook the engine set. The engine then
// freezes A (freeze_buffer, only when no frozen buffer is left unmerged,
// so draining never brings a writer closer to backpressure), waits for the
// merge without its write lock (wait_merged), and publishes the merge
// installed: a version with nothing buffered, which reads as the plain tree.
// The freeze and the install each take the write lock for O(1) work. A
// commit arriving meanwhile proceeds as usual and installs the merge itself
// if it has finished. A hook that finds a commit in flight publishes nothing
// and returns true; the merger tries again at the next pause. Commits pay no
// signal: a publish bumps a counter, and takes the merger's lock only when
// the version it publishes goes from nothing buffered to something buffered
// or back. Nothing buffered, nothing to watch: the merger sleeps.
//
// Displaced records. Every slot also records the record it displaced. The
// tree keeps entries a buffer overrides until the merge, and the record
// behind one may be gone — vacuum relocates a file's keys (appending slots)
// and drops the file in the same version. When the tree has to read such a
// record to find its way, the key comes from the slot that displaced it.
// A resolver that can tell a missing file apart (try_key_at) enables this.
//
// Undo. The engine undoes a plan by location, newest write first
// (kd_restore / kd_remove). The records an undone plan named are never
// written and their offsets are reused, so they must not reach a tree: the
// builder pops the slot it undoes instead of appending the inverse, and a
// slot it merged into its own tree already is undone in that tree.

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

// The merger is a thread: without one, every merge would run on the commit
// that froze the buffer, and nothing would drain the buffer once writes
// pause. Builds without threads (WASM) use the blind tree.
#ifdef BYTECASK_SINGLE_THREADED
#error "the buffered key directory needs threads; WASM builds use the blind tree"
#endif

export module bytecask.buffered_btree;
import bytecask.blind_btree;

namespace bytecask {

namespace buffered_detail {

using Bytes = std::span<const std::byte>;

// Partitions by a hash of the whole key: a point lookup scans one, about
// kBufferSlots / kPartitions fingerprints per buffer, however the keys
// cluster (a single-table workload writes nearly every key into one index).
inline constexpr std::size_t kPartitions = 64;
// Slots per buffer, across its partitions. Tests use a handful, so freezes,
// installs, merges on the spot and backpressure happen constantly.
#ifdef BYTECASK_TESTING
inline constexpr std::size_t kBufferSlots = 6;
#else
inline constexpr std::size_t kBufferSlots = 512;
#endif
// By-location writes (replace_at, erase_at) take no resolver, so they can
// never merge; they may run a buffer this far past kBufferSlots instead.
// Slots a partition holds: room for skew and for by-location writes past
// the target. A write whose partition is full makes room as a full buffer
// does. Whole SIMD chunks.
#ifdef BYTECASK_TESTING
inline constexpr std::size_t kSlotCapacity = 16;
#else
inline constexpr std::size_t kSlotCapacity = 128;
#endif
static_assert(kSlotCapacity % 16 == 0);
// A buffer runs past kBufferSlots only for writes by location; past this it
// takes no more.
inline constexpr std::size_t kBufferCapacity = kBufferSlots * 4;
// Key bytes per buffer. Holds at least one key of the largest size.
inline constexpr std::size_t kArenaBytes = std::size_t{1} << 18;
static_assert(kArenaBytes >= 4 * 65536);
// A tombstone, and "displaced nothing". File ids use 20 bits, so neither is
// ever a record.
inline constexpr BlindRef kNoRef{0xFFFF'FFFFu, 0xFFFF'FFFFu};
inline constexpr std::uint32_t kFrozen = std::numeric_limits<std::uint32_t>::max();

inline auto is_none(const BlindRef &r) noexcept -> bool { return r == kNoRef; }

inline auto compare(Bytes a, Bytes b) noexcept -> int {
  const auto n = std::min(a.size(), b.size());
  const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0) return c < 0 ? -1 : 1;
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

// Eight bytes of the key from `at`, big-endian, zero-padded: order prefixes.
inline auto word(Bytes k, std::size_t at) noexcept -> std::uint64_t {
  if (k.size() >= at + 8) {
    std::uint64_t v;
    std::memcpy(&v, k.data() + at, 8);
    return std::byteswap(v);
  }
  std::uint64_t v = 0;
  for (std::size_t i = at; i < at + 8; ++i)
    v = (v << 8) | (i < k.size() ? std::to_integer<std::uint64_t>(k[i]) : 0);
  return v;
}
// The key's order prefix: its first 16 bytes. Keys of one table share the
// first eight, so eight alone would tie on every slot of a table.
struct Prefix {
  std::uint64_t hi;
  std::uint64_t lo;
  friend auto operator<=>(const Prefix &, const Prefix &) = default;
};
inline auto prefix(Bytes k) noexcept -> Prefix { return {word(k, 0), word(k, 8)}; }

// A key's partition and fingerprint, from one hash of the key.
struct Hash {
  std::size_t part;
  std::uint16_t fp;
};
inline auto hash_of(Bytes k) noexcept -> Hash {
  const std::string_view s{reinterpret_cast<const char *>(k.data()), k.size()};
  const auto h = static_cast<std::uint64_t>(std::hash<std::string_view>{}(s));
  return {static_cast<std::size_t>(h >> 58), static_cast<std::uint16_t>(h)};
}
static_assert(kPartitions == 64);

// One partition's slots, structure of arrays. Apart from the fingerprints,
// left uninitialised: a slot is written before any count covers it.
struct Part {
  // Zeroed once, when the buffer is made: the fingerprint scan reads whole
  // chunks, past the count, and masks what it read there.
  std::array<std::uint16_t, kSlotCapacity> fp{};
  std::array<std::uint64_t, kSlotCapacity> hi;
  std::array<std::uint64_t, kSlotCapacity> lo;
  std::array<BlindRef, kSlotCapacity> ref;  // kNoRef: an erase
  std::array<BlindRef, kSlotCapacity> old;  // the record it displaced, or kNoRef
  std::array<std::uint32_t, kSlotCapacity> off;
  std::array<std::uint16_t, kSlotCapacity> len;
};

struct Buffer {
  std::array<Part, kPartitions> parts;
  std::unique_ptr<std::byte[]> arena{new std::byte[kArenaBytes]};
  // Slots some builder has written, in all: a builder appends only when
  // this equals its own count, so no builder overwrites a slot another
  // version can see. kFrozen once frozen: nobody appends again.
  std::atomic<std::uint32_t> written{0};

  [[nodiscard]] auto key(std::size_t p, std::size_t i) const noexcept -> Bytes {
    return {arena.get() + parts[p].off[i], parts[p].len[i]};
  }
};

using Counts = std::array<std::uint16_t, kPartitions>;

// What a version sees of one buffer: the buffer and how many slots of each
// partition are its own; `net` is the change those slots make to the key
// count, `arena` the arena bytes they use, `id` names a frozen buffer's
// merge.
struct View {
  std::shared_ptr<const Buffer> buf;
  Counts counts{};
  std::int64_t net{0};
  std::uint32_t arena{0};
  std::uint32_t total{0};
  std::uint64_t id{0};
};

// A slot found by a lookup or a scan. `key` points into the buffer's arena,
// alive while a view holding the buffer is.
struct Slot {
  Bytes key;
  BlindRef ref;
};

// Frozen buffers a version holds at most. On the 48-vCPU box one was not
// enough: a merge that ran past the ~1.2 ms a buffer takes to fill stalled
// the writer, 10 us a commit, though the merger was a third idle. Each more
// costs every point lookup one more fingerprint scan while it is unmerged.
inline constexpr std::size_t kMaxFrozen = 3;

// How long writes must pause before the merger drains the buffers. Short
// enough that reads after a burst soon cost what the plain tree's do, long
// enough that a steady stream of commits does not freeze a buffer per
// commit. A build-time define overrides it, for measurement only.
#ifndef BYTECASK_BUFFER_IDLE_MS
#define BYTECASK_BUFFER_IDLE_MS 2
#endif
inline constexpr std::chrono::milliseconds kIdleWindow{BYTECASK_BUFFER_IDLE_MS};

// A version's buffers: the frozen ones, oldest first, and the active one.
struct Layers {
  std::array<View, kMaxFrozen> f{};
  std::size_t nf{0};
  View a;

  [[nodiscard]] auto net() const noexcept -> std::int64_t {
    auto n = a.net;
    for (std::size_t i = 0; i < nf; ++i) n += f[i].net;
    return n;
  }
  [[nodiscard]] inline auto total() const noexcept -> std::size_t { return total_from(0); }
  // Slots from the i-th frozen buffer on, the active one included.
  [[nodiscard]] inline auto total_from(std::size_t i) const noexcept -> std::size_t {
    std::size_t n = a.total;
    for (; i < nf; ++i) n += f[i].total;
    return n;
  }
  // Oldest first: a later view's slot is newer than an earlier one's. The
  // first `skip` frozen buffers are left out: a merge a reader uses covers
  // them.
  template <typename F> void oldest_first(F &&fn, std::size_t skip = 0) const {
    for (std::size_t i = skip; i < nf; ++i) fn(f[i]);
    fn(a);
  }
  // Drops the oldest frozen buffer.
  void pop_oldest() {
    for (std::size_t i = 1; i < nf; ++i) f[i - 1] = std::move(f[i]);
    f[--nf] = View{};
  }
  // From the i-th frozen buffer on: what a merge of f[i] reads through.
  [[nodiscard]] auto from(std::size_t i) const -> Layers {
    Layers l;
    for (std::size_t j = i; j < nf; ++j) l.f[l.nf++] = f[j];
    l.a = a;
    return l;
  }
};

// Bit 2i set where fps[i] == fp, for 16 fingerprints (the byte mask of a
// 16-bit compare, one bit kept per lane).
inline auto match16(const std::uint16_t *fps, std::uint16_t fp) noexcept -> std::uint32_t {
#if defined(__AVX2__)
  const auto v = _mm256_loadu_si256(static_cast<const __m256i *>(static_cast<const void *>(fps)));
  const auto eq = _mm256_cmpeq_epi16(v, _mm256_set1_epi16(static_cast<short>(fp)));
  return static_cast<std::uint32_t>(_mm256_movemask_epi8(eq)) & 0x5555'5555u;
#else
  std::uint32_t mask = 0;
  for (std::uint32_t i = 0; i < 16; ++i)
    mask |= static_cast<std::uint32_t>(fps[i] == fp) << (2 * i);
  return mask;
#endif
}

// match16's mask for the first n < 16 fingerprints only. The active buffer
// is appended to while readers scan it: fingerprints past a version's count
// may be being written, so a reader must not load them, not even to mask
// the result away (a data race, undefined behaviour).
inline auto match_first(const std::uint16_t *fps, std::size_t n, std::uint16_t fp) noexcept
    -> std::uint32_t {
  std::uint32_t mask = 0;
  for (std::size_t i = 0; i < n; ++i)
    mask |= static_cast<std::uint32_t>(fps[i] == fp) << (2 * i);
  return mask;
}

// The newest slot for `key` in a view, or none.
inline auto find_in(const View &v, Bytes key, Hash h) -> std::optional<Slot> {
  if (!v.buf) return std::nullopt;
  const auto p = h.part;
  const auto fp = h.fp;
  const auto &part = v.buf->parts[p];
  const std::size_t n = v.counts[p];
  // Newest first, a chunk of 16 fingerprints at a time.
  for (std::size_t end = n; end > 0;) {
    const auto from = (end - 1) / 16 * 16;
    std::uint32_t mask = end - from == 16 ? match16(part.fp.data() + from, fp)
                                          : match_first(part.fp.data() + from, end - from, fp);
    end = from;
    while (mask != 0) {
      const auto bit = 31 - std::countl_zero(mask);
      mask &= ~(std::uint32_t{1} << bit);
      const auto i = from + static_cast<std::size_t>(bit / 2);
      const auto k = v.buf->key(p, i);
      if (compare(k, key) == 0) return Slot{k, part.ref[i]};
    }
  }
  return std::nullopt;
}

// A key's newest entry in the buffers of a version: A, then the frozen ones
// newest first.
inline auto newest(const Layers &l, Bytes key, std::size_t skip = 0) -> std::optional<Slot> {
  const auto h = hash_of(key);
  if (auto s = find_in(l.a, key, h)) return s;
  for (std::size_t i = l.nf; i-- > skip;)
    if (auto s = find_in(l.f[i], key, h)) return s;
  return std::nullopt;
}

// The key of a record some visible slot displaced, or none.
inline auto displaced_key(const View &v, BlindRef r) -> std::optional<Bytes> {
  if (!v.buf) return std::nullopt;
  for (std::size_t p = 0; p < kPartitions; ++p)
    for (std::size_t i = 0; i < v.counts[p]; ++i)
      if (v.buf->parts[p].old[i] == r) return v.buf->key(p, i);
  return std::nullopt;
}

// Reads keys for the tree under a version's buffers: through the inner
// resolver, and for a record whose file is gone, from the slot that
// displaced it.
template <BlindKeyResolver R> struct Overlay {
  R &inner;
  const Layers &l;
  std::size_t skip{0};

  auto key_at(BlindRef r) -> Bytes {
    if constexpr (requires {
                    { inner.try_key_at(r) } -> std::same_as<std::optional<Bytes>>;
                  }) {
      if (auto k = inner.try_key_at(r)) return *k;
      if (auto k = displaced_key(l.a, r)) return *k;
      for (std::size_t i = l.nf; i-- > skip;)
        if (auto k = displaced_key(l.f[i], r)) return *k;
      throw std::logic_error{
          "buffered key directory: the tree references a missing record that "
          "no buffered write displaced"};
    } else {
      return inner.key_at(r);
    }
  }
};

enum class Pick : std::uint8_t { Min, Max };

// Min: the smallest key > (>=) `bound`. Max: the largest key < (<=)
// `bound`, or the largest of all when `bound` is none. Among the slots of
// the version's buffers; the newest slot wins a tie.
template <Pick P>
inline auto buffer_pick(const Layers &l, std::optional<Bytes> bound, bool inclusive)
    -> std::optional<Slot> {
  const auto bp = bound ? prefix(*bound) : Prefix{};
  std::optional<Slot> best;
  Prefix best_p{};
  l.oldest_first([&](const View &view) {  // oldest first: a later slot wins a tie
    const View *v = &view;
    if (!v->buf) return;
    for (std::size_t p = 0; p < kPartitions; ++p) {
      const auto &part = v->buf->parts[p];
      for (std::size_t i = 0; i < v->counts[p]; ++i) {
        const Prefix sp{part.hi[i], part.lo[i]};
        if (bound && (P == Pick::Min ? sp < bp : sp > bp)) continue;
        if (best && (P == Pick::Min ? sp > best_p : sp < best_p)) continue;
        const auto k = v->buf->key(p, i);
        if (bound && sp == bp) {
          const int c = compare(k, *bound);
          if (P == Pick::Min ? (c < 0 || (c == 0 && !inclusive))
                             : (c > 0 || (c == 0 && !inclusive)))
            continue;
        }
        if (best) {
          const int c = compare(k, best->key);
          if (P == Pick::Min ? c > 0 : c < 0) continue;
        }
        best = Slot{k, part.ref[i]};
        best_p = sp;
      }
    }
  });
  return best;
}

// Whether any slot of the version's buffers holds a key in [lo, hi) (hi
// none: no bound). Full keys only where a prefix ties.
inline auto any_in(const Layers &l, Bytes lo, std::optional<Bytes> hi) -> bool {
  const auto lp = prefix(lo);
  const auto hp = hi ? prefix(*hi) : Prefix{~std::uint64_t{0}, ~std::uint64_t{0}};
  bool found = false;
  l.oldest_first([&](const View &view) {
    const View *v = &view;
    if (found || !v->buf) return;
    for (std::size_t p = 0; p < kPartitions; ++p) {
      const auto &part = v->buf->parts[p];
      for (std::size_t i = 0; i < v->counts[p]; ++i) {
        const Prefix sp{part.hi[i], part.lo[i]};
        if (sp < lp || (hi && sp > hp)) continue;
        if (sp == lp || (hi && sp == hp)) {
          const auto k = v->buf->key(p, i);
          if (compare(k, lo) < 0 || (hi && compare(k, *hi) >= 0)) continue;
        }
        found = true;
        return;
      }
    }
  });
  return found;
}

} // namespace buffered_detail

// ---------------------------------------------------------------------------
// BufferedMerger — one per key directory, shared by its versions. Owns the
// merger thread (started by the first merge, joined when the last version
// goes) and its queue. A frozen buffer's merge result belongs to the
// versions holding the buffer (FrozenMerge): when the last of them goes, so
// does the result, and a result nobody holds any more is dropped at once,
// so a tree a discarded version derived never blocks the next derivation.
// RS, the source a published version hands over, provides with_resolver(f),
// calling f with a resolver for its records.
// ---------------------------------------------------------------------------
template <std::size_t LeafBytes> struct FrozenMerge {
  std::uint64_t id{0};
  // Under the merger's mutex.
  std::optional<PersistentBlindBTree<LeafBytes>> merged;
  std::exception_ptr error;
  // The merged tree for readers, lock-free: set once, after `merged`, and
  // valid while this state lives — as long as a version holds the buffer.
  std::atomic<const PersistentBlindBTree<LeafBytes> *> done{nullptr};
};

namespace buffered_detail {
// Recycled write buffers, shared with the deleters of the buffers it hands
// out. (Not the engine's BufferPool, the record cache.)
struct BufferRecycler {
  static constexpr std::size_t kKept = 4;
  std::mutex mu;
  std::vector<std::unique_ptr<buffered_detail::Buffer>> free;

  static auto acquire(const std::shared_ptr<BufferRecycler> &pool)
      -> std::shared_ptr<buffered_detail::Buffer> {
    std::unique_ptr<buffered_detail::Buffer> b;
    {
      std::lock_guard<std::mutex> lk{pool->mu};
      if (!pool->free.empty()) {
        b = std::move(pool->free.back());
        pool->free.pop_back();
      }
    }
    if (!b) b = std::make_unique<buffered_detail::Buffer>();
    b->written.store(0, std::memory_order_relaxed);
    return {b.release(), [pool](buffered_detail::Buffer *raw) {
              std::unique_ptr<buffered_detail::Buffer> owned{raw};
              std::lock_guard<std::mutex> lk{pool->mu};
              if (pool->free.size() < kKept) pool->free.push_back(std::move(owned));
            }};
  }
};
} // namespace buffered_detail

template <std::size_t LeafBytes, typename RS> class BufferedMerger {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Buffer = buffered_detail::Buffer;
  using View = buffered_detail::View;
  using Layers = buffered_detail::Layers;
  using Frozen = FrozenMerge<LeafBytes>;

public:
  BufferedMerger() = default;
  BufferedMerger(const BufferedMerger &) = delete;
  auto operator=(const BufferedMerger &) -> BufferedMerger & = delete;
  BufferedMerger(BufferedMerger &&) = delete;
  auto operator=(BufferedMerger &&) -> BufferedMerger & = delete;
  ~BufferedMerger() {
    {
      std::lock_guard<std::mutex> lk{mu_};
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] auto next_id() noexcept -> std::uint64_t {
    return next_id_.fetch_add(1, std::memory_order_relaxed);
  }
  [[nodiscard]] auto acquire() -> std::shared_ptr<Buffer> { return buffered_detail::BufferRecycler::acquire(pool_); }

  // Hands a frozen buffer over. The merger folds it into `base`, or, when
  // `prev` is set, into the tree prev's merge produced — the frozen buffer
  // before it, handed over earlier and merged first. `overlay` is the
  // buffer and every newer one, as the publishing version holds them: the
  // merge reads records through them and `src`.
  void submit(std::optional<Tree> base, std::shared_ptr<Frozen> prev, View frozen,
              Layers overlay, const std::shared_ptr<Frozen> &state, RS src) {
    Job job{std::move(base), std::move(prev), std::move(frozen), std::move(overlay), state,
            std::move(src)};
    {
      std::lock_guard<std::mutex> lk{mu_};
      jobs_.push_back(std::move(job));
      if (!thread_.joinable()) thread_ = std::thread{[this] { run(); }};
    }
    cv_.notify_all();
  }

  // The merged tree for F, if the merger has finished it.
  [[nodiscard]] auto ready(const Frozen &f) -> std::optional<Tree> {
    std::lock_guard<std::mutex> lk{mu_};
    return f.merged;
  }

  // The merged tree for F (backpressure). F must have been submitted: a
  // version holds it. While no merge is running, the caller runs the queued
  // ones itself, in order, rather than wait for a merger thread the
  // scheduler has not run yet; merges still run one at a time.
  [[nodiscard]] auto wait(const Frozen &f) -> Tree {
    std::unique_lock<std::mutex> lk{mu_};
    if (!f.merged && !f.error) {
      const auto t0 = std::chrono::steady_clock::now();
      while (!f.merged && !f.error) {
        if (!running_ && !jobs_.empty()) {
          auto job = std::move(jobs_.front());
          jobs_.pop_front();
          execute(job, lk);
          inline_merges_.fetch_add(1, std::memory_order_relaxed);
        } else {
          cv_.wait(lk);
        }
      }
      stalls_.fetch_add(1, std::memory_order_relaxed);
      stall_ns_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count(),
                          std::memory_order_relaxed);
    }
    if (f.error) std::rethrow_exception(f.error);
    return *f.merged;
  }

  // Waits until no merge is queued or running. A builder about to derive a
  // tree itself calls it, so no merge for a discarded version still holds a
  // tree derived from the same base; so does the drain, for the merge of the
  // buffer it froze. While no merge is running the caller runs the queued
  // ones itself, as wait() does: the merger thread may be the caller (the
  // drain hook), or blocked in the hook on the write lock the caller holds.
  void drain() {
    std::unique_lock<std::mutex> lk{mu_};
    while (!jobs_.empty() || running_) {
      if (running_) {
        cv_.wait(lk);
        continue;
      }
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      execute(job, lk);
    }
  }

  // The drain hook (see "Draining" above), called on the merger thread.
  // Replacing or clearing it waits for a call in progress, so once
  // clear_drain_hook returns the hook is never called again: its owner may
  // go. Never call either from the hook.
  void set_drain_hook(std::function<bool()> hook) {
    std::unique_lock<std::mutex> lk{mu_};
    cv_.wait(lk, [&] { return !hook_running_; });
    hook_ = std::move(hook);
  }
  void clear_drain_hook() { set_drain_hook(nullptr); }

  // A builder published a version; `buffered`: it holds buffered slots.
  // Called for every publish, so it only counts, unless the version goes
  // from nothing buffered to something buffered (the merger starts watching
  // for writes to pause) or back.
  void note_publish(bool buffered) {
    publishes_.fetch_add(1, std::memory_order_relaxed);
    if (armed_.load(std::memory_order_relaxed) == buffered) return;
    {
      std::lock_guard<std::mutex> lk{mu_};
      armed_.store(buffered, std::memory_order_relaxed);
      if (!buffered || !hook_) return;
      if (!thread_.joinable()) thread_ = std::thread{[this] { run(); }};
    }
    cv_.notify_all();
  }

  // Folds a frozen buffer into base, reading records through the buffers
  // of `overlay` (it and every newer one). Per key, only its newest slot
  // counts: an older one may name a record that a later write overrode.
  //
  // Most keys need no read. A key's oldest slot in the buffer displaced the
  // entry base holds for it, so an update or an erase is applied by that
  // location (replace_at / erase_at, which read nothing and succeed only
  // where base holds the key at that very record). An insert — the key was
  // absent — and any key whose location base does not hold take the keyed
  // path, which reads records to place the key.
  template <BlindKeyResolver R>
  static auto merge_with(const Tree &base, const View &frozen, const Layers &overlay, R &res)
      -> Tree {
    const auto t0 = std::chrono::steady_clock::now();
    auto tr = base.transient();
    buffered_detail::Overlay<R> ov{res, overlay};
    // Per key: its oldest and newest slot. Applied in key order across the
    // whole buffer, so consecutive keys walk the same path and leaf.
    struct Entry {
      buffered_detail::Bytes key;
      std::uint32_t part;
      std::uint32_t oldest;
      std::uint32_t newest;
    };
    std::vector<Entry> entries;
    entries.reserve(frozen.total);
    std::unordered_map<std::string_view, std::size_t> index;
    std::int64_t slots = 0;
    std::int64_t by_location = 0;
    for (std::size_t p = 0; p < buffered_detail::kPartitions; ++p) {
      index.clear();
      for (std::uint32_t i = 0; i < frozen.counts[p]; ++i) {
        const auto k = frozen.buf->key(p, i);
        const auto [it, fresh] = index.try_emplace(
            std::string_view{reinterpret_cast<const char *>(k.data()), k.size()}, entries.size());
        if (fresh)
          entries.push_back({k, static_cast<std::uint32_t>(p), i, i});
        else
          entries[it->second].newest = i;
      }
      slots += frozen.counts[p];
    }
    std::ranges::sort(entries, [](const Entry &a, const Entry &b) {
      return buffered_detail::compare(a.key, b.key) < 0;
    });
    for (const auto &e : entries) {
      const auto &part = frozen.buf->parts[e.part];
      const auto ref = part.ref[e.newest];
      const auto held = part.old[e.oldest];  // what base holds for the key
      if (!buffered_detail::is_none(held)) {
        const bool done = buffered_detail::is_none(ref) ? tr.erase_at(e.key, held)
                                                        : tr.replace_at(e.key, held, ref);
        if (done) {
          ++by_location;
          continue;
        }
      }
      if (buffered_detail::is_none(ref))
        (void)tr.erase(e.key, ov);
      else
        tr.set(e.key, ref, ov);
    }
    auto merged = std::move(tr).persistent();
    merges_.fetch_add(1, std::memory_order_relaxed);
    merge_slots_.fetch_add(slots, std::memory_order_relaxed);
    merge_by_location_.fetch_add(by_location, std::memory_order_relaxed);
    merge_ns_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count(),
                        std::memory_order_relaxed);
    return merged;
  }

  // Process-wide, across key directories.
  [[nodiscard]] static auto stalls() noexcept -> std::int64_t {
    return stalls_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto stall_ns() noexcept -> std::int64_t {
    return stall_ns_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto inline_merges() noexcept -> std::int64_t {
    return inline_merges_.load(std::memory_order_relaxed);
  }
  // Merges run, the time they took, the slots they covered, and the keys
  // applied by location rather than by key.
  [[nodiscard]] static auto merges() noexcept -> std::int64_t {
    return merges_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto merge_ns() noexcept -> std::int64_t {
    return merge_ns_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto merge_slots() noexcept -> std::int64_t {
    return merge_slots_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto merge_by_location() noexcept -> std::int64_t {
    return merge_by_location_.load(std::memory_order_relaxed);
  }
  // Buffers frozen by a drain rather than by filling up.
  [[nodiscard]] static auto drains() noexcept -> std::int64_t {
    return drains_.load(std::memory_order_relaxed);
  }
  static void count_drain() noexcept { drains_.fetch_add(1, std::memory_order_relaxed); }

private:
  struct Job {
    std::optional<Tree> base;     // none: prev's result
    std::shared_ptr<Frozen> prev;  // the merge before this one, or null
    View frozen;
    Layers overlay;
    std::weak_ptr<Frozen> state;
    RS src;
  };

  // Runs one job; called with lk held, returns with it held. Jobs run in the
  // order they were handed over, so prev has run.
  void execute(Job &job, std::unique_lock<std::mutex> &lk) {
    if (job.state.expired()) return;  // every version holding the buffer is gone
    std::optional<Tree> base = std::move(job.base);
    std::exception_ptr error;
    if (job.prev) {
      if (job.prev->merged) base = *job.prev->merged;
      else error = job.prev->error ? job.prev->error
                                   : std::make_exception_ptr(std::logic_error{
                                         "buffered key directory: merged out of order"});
    }
    running_ = true;
    lk.unlock();
#ifdef BYTECASK_TESTING
    if (auto *f = test_before_merge_.load(std::memory_order_acquire)) f();
#endif
    std::optional<Tree> merged;
    if (!error) {
      try {
        job.src.with_resolver(
            [&](auto &res) { merged = merge_with(*base, job.frozen, job.overlay, res); });
      } catch (...) {
        error = std::current_exception();
      }
    }
    base.reset();
    auto state = job.state.lock();
    job = Job{};  // release the base, F, A and the source outside the lock
    if (!state) merged.reset();  // nobody wants it: drop it before the next job
    lk.lock();
    running_ = false;
    if (state) {
      state->merged = std::move(merged);
      state->error = error;
      if (state->merged) state->done.store(&*state->merged, std::memory_order_release);
    }
    lk.unlock();
    state.reset();  // may free the result, off the lock
    lk.lock();
    cv_.notify_all();
  }

  [[nodiscard]] auto runnable() const -> bool { return !jobs_.empty() && !running_; }
  // A version with buffered slots was published and there is a hook to
  // drain it. Under mu_.
  [[nodiscard]] auto watching() const -> bool {
    return armed_.load(std::memory_order_relaxed) && hook_ != nullptr;
  }

  // Merges in order, and drains once writes pause. Sleeps while there is
  // neither a merge to run nor a buffered version to watch.
  void run() {
    std::unique_lock<std::mutex> lk{mu_};
    for (;;) {
      cv_.wait(lk, [&] { return stop_ || runnable() || watching(); });
      if (stop_ && !runnable()) return;
      if (runnable()) {
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        execute(job, lk);
        continue;
      }
      if (!writes_paused(lk)) continue;
      // Cleared before the call: every publish the hook makes sets it again
      // from what it published, and a builder that publishes after the
      // hook's last one finds it clear and arms it again.
      armed_.store(false, std::memory_order_relaxed);
      hook_running_ = true;
      lk.unlock();
      bool again = false;
      try {
        again = hook_();
      } catch (...) {
        // The hook's owner handles its own failures.
      }
      lk.lock();
      hook_running_ = false;
      if (again) armed_.store(true, std::memory_order_relaxed);
      cv_.notify_all();
    }
  }

  // Waits out the idle window. True when no builder published during it and
  // there is still something to drain and no merge to run.
  auto writes_paused(std::unique_lock<std::mutex> &lk) -> bool {
    const auto seen = publishes_.load(std::memory_order_relaxed);
    if (cv_.wait_for(lk, buffered_detail::kIdleWindow, [&] { return stop_ || runnable(); }))
      return false;
    return publishes_.load(std::memory_order_relaxed) == seen && !running_ && watching();
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool running_{false};
  bool stop_{false};
  // Under mu_; called with hook_running_ set. Returns true when it drained
  // nothing because writes resumed: retried at the next pause.
  std::function<bool()> hook_;
  bool hook_running_{false};
  // Whether the last version a builder published holds buffered slots.
  // Changed under mu_; read without it by note_publish.
  std::atomic<bool> armed_{false};
  std::atomic<std::uint64_t> publishes_{0};
  std::atomic<std::uint64_t> next_id_{1};
  std::shared_ptr<buffered_detail::BufferRecycler> pool_{
      std::make_shared<buffered_detail::BufferRecycler>()};
  static inline std::atomic<std::int64_t> stalls_{0};
  static inline std::atomic<std::int64_t> stall_ns_{0};
  static inline std::atomic<std::int64_t> inline_merges_{0};
  static inline std::atomic<std::int64_t> merges_{0};
  static inline std::atomic<std::int64_t> merge_ns_{0};
  static inline std::atomic<std::int64_t> merge_slots_{0};
  static inline std::atomic<std::int64_t> merge_by_location_{0};
  static inline std::atomic<std::int64_t> drains_{0};
#ifdef BYTECASK_TESTING
public:
  // Called on the thread about to run a merge, without the lock: a test
  // holds merges back with it.
  static inline std::atomic<void (*)()> test_before_merge_{nullptr};

private:
#endif
  std::thread thread_;  // last: joined before the rest is destroyed
};

export template <std::size_t LeafBytes, typename RS> class BufferedBlindBTree;
export template <std::size_t LeafBytes, typename RS> class TransientBufferedBlindBTree;

// ---------------------------------------------------------------------------
// BufferedMergeCursor — the tree's entries merged with the buffers', in key
// order, the newest entry winning and erases hiding what they erase. Moving
// may read keys, so it takes a resolver: settle(res) once after it is made,
// next(res) and prev(res) to move.
//
// The next buffer candidate is cached: the smallest buffer key past the
// position stays the smallest until it is passed. A tree entry's key is
// read only to compare it with a candidate, so with none ahead, stepping
// through the tree reads nothing more than the tree iterator does.
//
// One tree iterator, t_, is both the cursor and the pin on the version's
// tree: going forward it is the first tree entry not yet passed, going back
// the last. Turning around seeks again, so one serves both directions, and
// every seek repositions it in place.
// ---------------------------------------------------------------------------
template <std::size_t LeafBytes> class BufferedMergeCursor {
  using TIt = BlindBTreeIterator<LeafBytes>;
  using Bytes = buffered_detail::Bytes;
  using Layers = buffered_detail::Layers;
  using Slot = buffered_detail::Slot;
  using Pick = buffered_detail::Pick;

public:
  enum class Start : std::uint8_t { None, Begin, End, At };

  // `t` is any iterator over the tree to merge; only its version is used.
  BufferedMergeCursor(TIt t, Layers l, Start start, Bytes at)
      : t_{std::move(t)}, l_{std::move(l)}, start_{start}, start_key_(at.begin(), at.end()) {}

  [[nodiscard]] auto ref() const noexcept -> BlindRef { return ref_; }
  [[nodiscard]] auto at_end() const noexcept -> bool { return at_end_; }
  // Valid until the cursor next moves.
  template <BlindKeyResolver R> [[nodiscard]] auto key(R &res) const -> Bytes {
    ensure_key(res);
    return {key_.data(), key_.size()};
  }

  template <BlindKeyResolver R> void settle(R &res) {
    switch (std::exchange(start_, Start::None)) {
    case Start::None: return;
    case Start::Begin: seek({}, true, res); return;
    case Start::End: at_end_ = true; fwd_ = bwd_ = false; return;
    case Start::At: {
      const auto at = std::move(start_key_);
      seek({at.data(), at.size()}, true, res);
      return;
    }
    }
  }

  template <BlindKeyResolver R> void next(R &res) {
    settle(res);
    bound_ok_ = false;
    if (at_end_) return;
    if (fwd_) {
      forward(res);
      return;
    }
    ensure_key(res);
    const auto from = key_;
    seek({from.data(), from.size()}, false, res);
  }

  // From the end, the last entry; from the first entry, the end.
  template <BlindKeyResolver R> void prev(R &res) {
    settle(res);
    bound_ok_ = false;
    if (bwd_) {
      backward(res);
      return;
    }
    if (at_end_) {
      seek_back(std::nullopt, res);
      return;
    }
    ensure_key(res);
    const auto from = key_;
    seek_back(Bytes{from.data(), from.size()}, res);
  }

  // Entries from here up to `end`, counted no further than limit. When no
  // buffered key falls in the range, from the tree's leaf sizes and the
  // bounds the cursors were placed by: two reads at most, and none when
  // the tree positions are at hand. Otherwise by stepping.
  template <BlindKeyResolver R>
  [[nodiscard]] auto count_until(const BufferedMergeCursor &end, std::size_t limit,
                                 R &res) const -> std::size_t {
    if (limit == 0) return 0;
    const auto lo = range_bound(res);
    if (!lo) return 0;  // at the end
    const auto hi = end.range_bound(res);
    if (hi && buffered_detail::compare(*lo, *hi) >= 0) return 0;
    if (!buffered_detail::any_in(l_, *lo, hi)) {
      // Every entry in range is the tree's.
      if (start_ == Start::None && fwd_ && from_tree_ && end.start_ == Start::None) {
        // t_ is one past this entry; an iterator with no position counts
        // to the end.
        if (end.at_end_) return 1 + t_.count_until(TIt{}, limit - 1);
        if (end.fwd_ && end.from_tree_) return t_.count_until(end.t_, limit);
      }
      buffered_detail::Overlay<R> ov{res, l_};
      auto from = t_;
      from.seek(*lo, ov);
      if (!hi) return from.count_until(TIt{}, limit);
      auto to = t_;
      to.seek(*hi, ov);
      return from.count_until(to, limit);
    }
    auto it = *this;
    it.settle(res);
    auto stop = end;
    stop.settle(res);
    std::size_t n = 0;
    while (n < limit && !it.same_entry(stop) && !it.at_end_) {
      ++n;
      it.next(res);
    }
    return n;
  }

private:
  // Same entry: a record location names one record.
  [[nodiscard]] auto same_entry(const BufferedMergeCursor &o) const noexcept -> bool {
    if (at_end_ || o.at_end_) return at_end_ == o.at_end_;
    return ref_ == o.ref_;
  }

  // Where the entries from here start: the bound it was placed by or its
  // key; none at the end. Valid until the cursor next changes.
  template <BlindKeyResolver R> auto range_bound(R &res) const -> std::optional<Bytes> {
    switch (start_) {
    case Start::Begin: return Bytes{};
    case Start::End: return std::nullopt;
    case Start::At: return Bytes{start_key_.data(), start_key_.size()};
    case Start::None: break;
    }
    if (at_end_) return std::nullopt;
    if (bound_ok_) return Bytes{bound_.data(), bound_.size()};
    return key(res);
  }

  template <BlindKeyResolver R> void ensure_key(R &res) const {
    if (key_ok_) return;
    buffered_detail::Overlay<R> ov{res, l_};
    const auto k = ov.key_at(ref_);
    key_.assign(k.begin(), k.end());
    key_ok_ = true;
  }

  // Positions on the smallest entry > (>=) lo.
  template <BlindKeyResolver R> void seek(Bytes lo, bool inclusive, R &res) {
    buffered_detail::Overlay<R> ov{res, l_};
    if (lo.empty() && inclusive)
      t_.seek_first();
    else
      t_.seek(lo, ov);
    if (!inclusive && t_ != std::default_sentinel &&
        buffered_detail::compare(t_.key(ov), lo) == 0)
      ++t_;
    fwd_ = true;
    bwd_ = false;
    bound_ok_ = inclusive;
    if (inclusive) bound_.assign(lo.begin(), lo.end());
    find_candidate<Pick::Min>(lo, inclusive);
    forward(res);
  }

  // Positions on the largest entry < hi, or the last if none.
  template <BlindKeyResolver R> void seek_back(std::optional<Bytes> hi, R &res) {
    buffered_detail::Overlay<R> ov{res, l_};
    if (hi)
      t_.seek(*hi, ov);
    else
      t_.seek_end();
    --t_;
    bwd_ = true;
    fwd_ = false;
    find_candidate<Pick::Max>(hi, false);
    backward(res);
  }

  // The next buffered entry past `bound`.
  template <Pick P> void find_candidate(std::optional<Bytes> bound, bool inclusive) {
    cand_ = buffered_detail::buffer_pick<P>(l_, bound, inclusive);
    cand_ok_ = true;
  }

  // The fence of the tree cursor's leaf (upper going forward, lower going
  // back), cached per leaf.
  template <bool Upper> auto fence() -> std::optional<Bytes> {
    if (t_.leaf() != fence_leaf_ || fence_upper_ != Upper) {
      fence_leaf_ = t_.leaf();
      fence_upper_ = Upper;
      fence_has_ = Upper ? t_.upper_fence(fence_) : t_.lower_fence(fence_);
    }
    if (!fence_has_) return std::nullopt;
    return Bytes{fence_.data(), fence_.size()};
  }

  // Steps to the next entry: the smaller of the buffer candidate and the
  // tree's next. The position (key_) moves past erases too. A tree entry is
  // known to come first without reading its key when the candidate is at or
  // past its leaf's upper fence.
  template <BlindKeyResolver R> void forward(R &res) {
    buffered_detail::Overlay<R> ov{res, l_};
    for (;;) {
      if (!cand_ok_) {
        ensure_key(res);
        find_candidate<Pick::Min>(Bytes{key_.data(), key_.size()}, false);
      }
      const bool tree_left = t_ != std::default_sentinel;
      int c = -1;
      bool read = false;  // the tree entry's key, into tkey_
      if (cand_ && tree_left) {
        const auto up = fence<true>();
        if (up && buffered_detail::compare(cand_->key, *up) >= 0) {
          c = 1;
        } else {
          read = true;
          const auto k = t_.key(ov);
          tkey_.assign(k.begin(), k.end());
          c = buffered_detail::compare(cand_->key, {tkey_.data(), tkey_.size()});
        }
      }
      if (cand_ && c <= 0) {
        if (c == 0) ++t_;
        take_candidate();
        if (buffered_detail::is_none(ref_)) continue;
        return;
      }
      if (tree_left) {
        take_tree(*t_, read);
        ++t_;
        return;
      }
      at_end_ = true;
      fwd_ = false;
      return;
    }
  }

  // Mirror of forward: the larger of the candidate and the tree's previous,
  // with the lower fence.
  template <BlindKeyResolver R> void backward(R &res) {
    buffered_detail::Overlay<R> ov{res, l_};
    for (;;) {
      if (!cand_ok_) {
        ensure_key(res);
        find_candidate<Pick::Max>(Bytes{key_.data(), key_.size()}, false);
      }
      const bool tree_left = t_ != std::default_sentinel;
      int c = 1;
      bool read = false;
      if (cand_ && tree_left) {
        const auto down = fence<false>();
        if (down && buffered_detail::compare(cand_->key, *down) < 0) {
          c = -1;
        } else {
          read = true;
          const auto k = t_.key(ov);
          tkey_.assign(k.begin(), k.end());
          c = buffered_detail::compare(cand_->key, {tkey_.data(), tkey_.size()});
        }
      }
      if (cand_ && c >= 0) {
        if (c == 0) --t_;
        take_candidate();
        if (buffered_detail::is_none(ref_)) continue;
        return;
      }
      if (tree_left) {
        take_tree(*t_, read);
        --t_;
        return;
      }
      at_end_ = true;
      bwd_ = false;
      return;
    }
  }

  void take_candidate() {
    key_.assign(cand_->key.begin(), cand_->key.end());
    key_ok_ = true;
    ref_ = cand_->ref;
    from_tree_ = false;
    at_end_ = false;
    cand_ok_ = false;
  }
  // The key is known if it was read to compare with a candidate.
  void take_tree(BlindRef ref, bool key_read) {
    ref_ = ref;
    if (key_read) key_ = tkey_;
    key_ok_ = key_read;
    from_tree_ = true;
    at_end_ = false;
  }

  TIt t_;  // the tree cursor, and the pin on its version
  Layers l_;
  Start start_{Start::None};
  std::vector<std::byte> start_key_;
  bool fwd_{false};
  bool bwd_{false};
  bool at_end_{true};
  bool from_tree_{false};
  BlindRef ref_{};
  mutable std::vector<std::byte> key_;  // the position's key, once known
  mutable bool key_ok_{false};
  std::vector<std::byte> tkey_;
  std::optional<Slot> cand_;
  bool cand_ok_{false};
  const void *fence_leaf_{nullptr};
  bool fence_upper_{false};
  bool fence_has_{false};
  std::vector<std::byte> fence_;
  // The inclusive bound a seek placed this cursor at, until it moves.
  std::vector<std::byte> bound_;
  bool bound_ok_{false};
};

// ---------------------------------------------------------------------------
// BufferedIterator — the key directory's iterator over a version: the tree
// iterator itself when the version holds nothing buffered, a
// BufferedMergeCursor otherwise. Either way it holds one pin on the tree;
// the cursor's merge state is allocated only for a version with buffered
// slots, so copying, moving and destroying an iterator over a drained
// version costs what the blind tree's does. Moving takes a resolver
// (settle / next / prev), as the engine's key directory iterator does for
// every tree; the plain one ignores it.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> class BufferedIterator {
  using TIt = BlindBTreeIterator<LeafBytes>;
  using Cursor = BufferedMergeCursor<LeafBytes>;
  using Bytes = buffered_detail::Bytes;
  using State = std::variant<TIt, std::unique_ptr<Cursor>>;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = BlindRef;
  using difference_type = std::ptrdiff_t;

  BufferedIterator() = default;
  BufferedIterator(const BufferedIterator &o) : s_{copy_of(o.s_)} {}
  auto operator=(const BufferedIterator &o) -> BufferedIterator & {
    if (this != &o) s_ = copy_of(o.s_);
    return *this;
  }
  // A moved-from iterator is at the end, holding nothing.
  BufferedIterator(BufferedIterator &&o) noexcept : s_{std::exchange(o.s_, State{})} {}
  auto operator=(BufferedIterator &&o) noexcept -> BufferedIterator & {
    if (this != &o) s_ = std::exchange(o.s_, State{});
    return *this;
  }
  ~BufferedIterator() = default;

  [[nodiscard]] auto operator*() const -> BlindRef {
    if (const auto *t = plain()) return **t;
    return cursor().ref();
  }
  // Valid until the iterator moves or `res` is used again.
  template <BlindKeyResolver R> [[nodiscard]] auto key(R &res) const -> Bytes {
    if (const auto *t = plain()) return t->key(res);
    return cursor().key(res);
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    if (const auto *t = plain()) return *t == std::default_sentinel;
    return cursor().at_end();
  }
  // Same entry: a record location names one record.
  auto operator==(const BufferedIterator &o) const noexcept -> bool {
    const bool end = *this == std::default_sentinel;
    const bool o_end = o == std::default_sentinel;
    if (end || o_end) return end == o_end;
    return **this == *o;
  }

  // The tree's own iterator, moved out, if this one is it (a version with
  // nothing buffered); nullopt for a merging one.
  [[nodiscard]] auto take_plain() && -> std::optional<TIt> {
    if (auto *t = plain_mut()) return std::move(*t);
    return std::nullopt;
  }

  template <BlindKeyResolver R> void settle(R &res) {
    if (auto *c = merging()) c->settle(res);
  }
  template <BlindKeyResolver R> void next(R &res) {
    if (auto *t = plain_mut()) {
      ++*t;
      return;
    }
    merging()->next(res);
  }
  // From the end, the last entry; from the first entry, the end.
  template <BlindKeyResolver R> void prev(R &res) {
    if (auto *t = plain_mut()) {
      --*t;
      return;
    }
    merging()->prev(res);
  }

  // Entries from here up to `end`, an iterator over the same version,
  // counted no further than limit. Two iterators over one version are of
  // one kind: whether a version holds buffered slots never changes.
  template <BlindKeyResolver R>
  [[nodiscard]] auto count_until(const BufferedIterator &end, std::size_t limit,
                                 R &res) const -> std::size_t {
    const auto *t = plain();
    const auto *end_t = end.plain();
    if (t && end_t) return t->count_until(*end_t, limit);
    if (!t && !end_t) return cursor().count_until(end.cursor(), limit, res);
    throw std::logic_error{"buffered key directory: a count between iterators over "
                           "different versions"};
  }

private:
  template <std::size_t, typename> friend class BufferedBlindBTree;
  template <std::size_t, typename> friend class TransientBufferedBlindBTree;

  // Over a version with nothing buffered: the tree's own iterator.
  explicit BufferedIterator(TIt t) : s_{std::move(t)} {}
  explicit BufferedIterator(std::unique_ptr<Cursor> c) : s_{std::move(c)} {}

  using Start = typename Cursor::Start;
  // Over a version with buffered slots: merges tree `tree` with buffers `l`,
  // placed when settled.
  static auto merge(const PersistentBlindBTree<LeafBytes> &tree, buffered_detail::Layers l,
                    Start start, Bytes at = {}) -> BufferedIterator {
    return BufferedIterator{std::make_unique<Cursor>(tree.end_iter(), std::move(l), start, at)};
  }

  static auto copy_of(const State &s) -> State {
    if (const auto *c = std::get_if<std::unique_ptr<Cursor>>(&s))
      return std::make_unique<Cursor>(**c);
    return std::get<TIt>(s);
  }
  [[nodiscard]] auto plain() const noexcept -> const TIt * { return std::get_if<TIt>(&s_); }
  [[nodiscard]] auto plain_mut() noexcept -> TIt * { return std::get_if<TIt>(&s_); }
  [[nodiscard]] auto merging() noexcept -> Cursor * {
    auto *c = std::get_if<std::unique_ptr<Cursor>>(&s_);
    return c ? c->get() : nullptr;
  }
  [[nodiscard]] auto cursor() const -> const Cursor & {
    return *std::get<std::unique_ptr<Cursor>>(s_);
  }

  State s_;
};

// ---------------------------------------------------------------------------
// BufferedBlindBTree — a published version. Immutable; any thread.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes, typename RS> class BufferedBlindBTree {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Merger = BufferedMerger<LeafBytes, RS>;
  using Frozen = FrozenMerge<LeafBytes>;
  using Layers = buffered_detail::Layers;
  using States = std::array<std::shared_ptr<Frozen>, buffered_detail::kMaxFrozen>;
  using Bytes = buffered_detail::Bytes;
  using Iter = BufferedIterator<LeafBytes>;

public:
  using iterator = Iter;

  BufferedBlindBTree() = default;
  // A tree built elsewhere (recovery), with no buffers.
  BufferedBlindBTree(Tree t) : tree_{std::move(t)} {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return static_cast<std::size_t>(static_cast<std::int64_t>(tree_.size()) + l_.net());
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  // Reads go through the newest finished merge, if any, skipping the frozen
  // buffers it covers: a version keeps its buffers until a later builder
  // installs the merge, and a read-only stretch would otherwise scan them
  // all for nothing.
  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    // A drained version is the tree: skip finding the newest merge, as the
    // iterators do (a point read paid ~1-2% for it).
    if (unbuffered()) return tree_.get(key, res);
    const auto [tree, skip] = effective();
    // Nothing buffered: the tree holds every entry, and every record it
    // names is in this version's files.
    if (l_.total_from(skip) == 0) return tree->get(key, res);
    if (auto s = buffered_detail::newest(l_, key, skip)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      (void)res.key_at(s->ref);  // the read a lookup's caller takes the entry from
      return s->ref;
    }
    buffered_detail::Overlay<R> ov{res, l_, skip};
    return tree->get(key, ov);
  }
  template <BlindKeyResolver R>
  [[nodiscard]] auto contains(Bytes key, R &res) const -> bool {
    return get(key, res).has_value();
  }
  [[nodiscard]] auto holds(Bytes key, BlindRef ref) const -> bool {
    if (unbuffered()) return tree_.holds(key, ref);
    const auto [tree, skip] = effective();
    if (auto s = buffered_detail::newest(l_, key, skip)) return s->ref == ref;
    return tree->holds(key, ref);
  }

  // With buffered slots, unsettled: it reads nothing until settled or
  // stepped, so a count between two bounds reads only what the tree needs
  // to place them. Without, the tree's own iterator, placed at once.
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &res) const -> Iter {
    if (unbuffered()) return Iter{tree_.lower_bound(key, res)};
    return merging(Iter::Start::At, key);
  }
  // Unsettled when merging: the key directory's iterator settles it.
  [[nodiscard]] auto begin() const -> Iter {
    if (unbuffered()) return Iter{tree_.begin()};
    return merging(Iter::Start::Begin);
  }
  [[nodiscard]] auto end_iter() const -> Iter {
    if (unbuffered()) return Iter{tree_.end_iter()};
    return merging(Iter::Start::End);
  }
  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t { return {}; }

  // Every location this version holds, in no order and reading no record:
  // the tree's entries, ones a buffer overrides included, and every
  // buffered write. f returns false to stop. For checks that need
  // locations, not keys: the iterator orders buffered keys among the
  // tree's, and that reads them.
  template <typename F> void for_each_ref(F &&f) const {
    for (auto it = tree_.begin(); !(it == std::default_sentinel); ++it)
      if (!f(*it)) return;
    const auto visit = [&](const buffered_detail::View &v) {
      if (!v.buf) return true;
      for (std::size_t p = 0; p < buffered_detail::kPartitions; ++p)
        for (std::size_t i = 0; i < v.counts[p]; ++i) {
          const auto r = v.buf->parts[p].ref[i];
          if (!buffered_detail::is_none(r) && !f(r)) return false;
        }
      return true;
    };
    for (std::size_t i = 0; i < l_.nf; ++i)
      if (!visit(l_.f[i])) return;
    (void)visit(l_.a);
  }

  [[nodiscard]] auto transient() const -> TransientBufferedBlindBTree<LeafBytes, RS>;

  [[nodiscard]] static auto reclamation_gauges() { return Tree::reclamation_gauges(); }
  [[nodiscard]] static auto buffer_stalls() noexcept -> std::int64_t { return Merger::stalls(); }
  [[nodiscard]] static auto buffer_stall_ns() noexcept -> std::int64_t {
    return Merger::stall_ns();
  }
  [[nodiscard]] static auto buffer_inline_merges() noexcept -> std::int64_t {
    return Merger::inline_merges();
  }
  [[nodiscard]] static auto buffer_merges() noexcept -> std::int64_t { return Merger::merges(); }
  [[nodiscard]] static auto buffer_merge_ns() noexcept -> std::int64_t {
    return Merger::merge_ns();
  }
  [[nodiscard]] static auto buffer_merge_slots() noexcept -> std::int64_t {
    return Merger::merge_slots();
  }
  [[nodiscard]] static auto buffer_merge_by_location() noexcept -> std::int64_t {
    return Merger::merge_by_location();
  }

  [[nodiscard]] static auto buffer_drains() noexcept -> std::int64_t { return Merger::drains(); }

  // Slots this version holds in its buffers, frozen ones included until a
  // version installs their merge (a gauge: 0 once drained).
  [[nodiscard]] auto buffered_slots() const noexcept -> std::size_t { return l_.total(); }
  // Waits for every merge handed over so far, running queued ones on this
  // thread while none is running.
  void wait_merged() const {
    if (merger_) merger_->drain();
  }
  // Sets the hook the merger thread calls once writes pause (see "Draining"
  // at the top), for this version and every version derived from it. Set it
  // before the version is published.
  void set_drain_hook(std::function<bool()> hook) {
    if (!merger_) merger_ = std::make_shared<Merger>();
    merger_->set_drain_hook(std::move(hook));
  }
  // Clears it, waiting for a call in progress: once this returns the hook is
  // never called again. Not from the hook.
  void clear_drain_hook() const {
    if (merger_) merger_->clear_drain_hook();
  }

#ifdef BYTECASK_TESTING
  // How many of this version's buffers are frozen.
  [[nodiscard]] auto test_frozen() const -> std::size_t { return l_.nf; }
  // f runs before every merge, on the thread running it (null: none).
  static void test_set_before_merge(void (*f)()) noexcept {
    Merger::test_before_merge_.store(f, std::memory_order_release);
  }
#endif

private:
  friend class TransientBufferedBlindBTree<LeafBytes, RS>;
  BufferedBlindBTree(std::shared_ptr<Merger> m, Tree t, Layers l, States fs)
      : merger_{std::move(m)}, tree_{std::move(t)}, l_{std::move(l)}, fs_{std::move(fs)} {}

  // The tree to read and how many frozen buffers it covers: the newest
  // finished merge's, else this version's own.
  struct Effective {
    const Tree *tree;
    std::size_t skip;
  };
  [[nodiscard]] auto effective() const noexcept -> Effective {
    for (auto i = l_.nf; i-- > 0;)
      if (const auto *t = fs_[i]->done.load(std::memory_order_acquire)) return {t, i + 1};
    return {&tree_, 0};
  }
  // Holds no buffered slot, frozen or not: its iterators are the tree's
  // own. Fixed for a version, so every iterator over it is of one kind. A
  // version whose merges have finished but are not installed yet merges
  // (through the finished trees, skipping the buffers they cover); the
  // next publish installs them.
  [[nodiscard]] inline auto unbuffered() const noexcept -> bool { return l_.total() == 0; }

public:
  // The tree itself while nothing is buffered, so a reader can iterate it
  // with the tree's own iterator; null while something is.
  [[nodiscard]] auto drained_tree() const noexcept -> const PersistentBlindBTree<LeafBytes> * {
    return unbuffered() ? &tree_ : nullptr;
  }

private:
  [[nodiscard]] auto merging(typename Iter::Start start, Bytes at = {}) const -> Iter {
    const auto [tree, skip] = effective();
    return Iter::merge(*tree, skip == 0 ? l_ : l_.from(skip), start, at);
  }

  std::shared_ptr<Merger> merger_;  // null until a builder is made
  Tree tree_;
  Layers l_;
  States fs_;  // fs_[i]: the merge of l_.f[i]
};

// ---------------------------------------------------------------------------
// TransientBufferedBlindBTree — the builder. One thread at a time.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes, typename RS> class TransientBufferedBlindBTree {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Merger = BufferedMerger<LeafBytes, RS>;
  using Frozen = FrozenMerge<LeafBytes>;
  using View = buffered_detail::View;
  using Layers = buffered_detail::Layers;
  using States = std::array<std::shared_ptr<Frozen>, buffered_detail::kMaxFrozen>;
  using Buffer = buffered_detail::Buffer;
  using Bytes = buffered_detail::Bytes;
  using Iter = BufferedIterator<LeafBytes>;

public:
  TransientBufferedBlindBTree(const TransientBufferedBlindBTree &) = delete;
  auto operator=(const TransientBufferedBlindBTree &) -> TransientBufferedBlindBTree & = delete;
  TransientBufferedBlindBTree(TransientBufferedBlindBTree &&) noexcept = default;
  auto operator=(TransientBufferedBlindBTree &&) noexcept
      -> TransientBufferedBlindBTree & = default;
  ~TransientBufferedBlindBTree() = default;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return static_cast<std::size_t>(static_cast<std::int64_t>(tree_.size()) + l_.net());
  }

  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    if (auto s = buffered_detail::newest(l_, key)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      (void)res.key_at(s->ref);
      return s->ref;
    }
    buffered_detail::Overlay<R> ov{res, l_};
    return tree_.get(key, ov);
  }
  [[nodiscard]] auto holds(Bytes key, BlindRef ref) const -> bool {
    if (auto s = buffered_detail::newest(l_, key)) return s->ref == ref;
    return tree_.holds(key, ref);
  }

  template <BlindKeyResolver R> void set(Bytes key, BlindRef ref, R &res) {
    (void)upsert(key, ref, res, [](const BlindRef &, const BlindRef &) { return true; });
  }

  // Inserts if absent, replaces if should_replace(existing, incoming);
  // returns what it replaced. As in the blind tree, the displaced record is
  // the one `res` read last.
  template <BlindKeyResolver R, typename Pred>
  auto upsert(Bytes key, BlindRef ref, R &res, Pred &&should_replace)
      -> std::optional<BlindRef> {
    bool read = false;
    const auto existing = lookup(key, res, read);
    if (existing && !should_replace(*existing, ref)) return std::nullopt;
    const bool merged = make_room(key, res);
    write_slot(key, ref, existing ? *existing : buffered_detail::kNoRef);
    if (existing && (!read || merged)) (void)res.key_at(*existing);
    return existing;
  }
  template <BlindKeyResolver R> auto erase(Bytes key, R &res) -> std::optional<BlindRef> {
    bool read = false;
    const auto existing = lookup(key, res, read);
    if (!existing) return std::nullopt;
    const bool merged = make_room(key, res);
    write_slot(key, buffered_detail::kNoRef, *existing);
    if (!read || merged) (void)res.key_at(*existing);
    return existing;
  }

  // By location, reading nothing. The inverse of this builder's last write
  // — an undo — takes that write back rather than adding one.
  auto replace_at(Bytes key, BlindRef from, BlindRef to) -> bool {
    return by_location(key, from, to);
  }
  auto erase_at(Bytes key, BlindRef from) -> bool {
    return by_location(key, from, buffered_detail::kNoRef);
  }

  // As the published version's.
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &res) const -> Iter {
    if (l_.total() == 0) return Iter{tree_.lower_bound(key, res)};
    return Iter::merge(tree_, l_, Iter::Start::At, key);
  }

  // Publishes. The buffers this builder froze go to the merger, oldest
  // first, each chained to the one before it, with `src` to read the
  // records of the version being published.
  // Every finished merge is installed first: it costs nothing, and a version
  // whose buffers are all merged then reads as the plain tree.
  [[nodiscard]] auto persistent(RS src) && -> BufferedBlindBTree<LeafBytes, RS> {
    install_finished();
    for (auto i = l_.nf - here_; i < l_.nf; ++i) {
      if (i == 0)
        merger_->submit(tree_, nullptr, l_.f[i], l_.from(i), fs_[i], src);
      else
        merger_->submit(std::nullopt, fs_[i - 1], l_.f[i], l_.from(i), fs_[i], src);
    }
    merger_->note_publish(l_.total() > 0);
    return {std::move(merger_), std::move(tree_), std::move(l_), std::move(fs_)};
  }

  // The drain's freeze: A joins the frozen buffers, to be merged once this
  // builder is published. Only when no frozen buffer is left unmerged, so
  // a drain never adds to a queue a writer could have to wait on. Returns
  // whether it froze.
  auto freeze_buffer() -> bool {
    install_finished();
    if (l_.nf != 0 || l_.a.total == 0) return false;
    freeze_active();
    Merger::count_drain();
    return true;
  }

private:
  friend class BufferedBlindBTree<LeafBytes, RS>;
  TransientBufferedBlindBTree(std::shared_ptr<Merger> m, Tree t, Layers l, States fs)
      : merger_{std::move(m)}, tree_{std::move(t)}, l_{std::move(l)}, fs_{std::move(fs)} {}

  // A write of this builder still in a buffer, newest last: the buffer (0:
  // A, else the id of the frozen buffer's merge) and its partition.
  struct Written {
    std::uint64_t layer;
    std::uint8_t part;
  };

  // The key's current record, if it has one; `read` says whether finding it
  // read it (the tree does, the buffers do not).
  template <BlindKeyResolver R>
  auto lookup(Bytes key, R &res, bool &read) const -> std::optional<BlindRef> {
    if (auto s = buffered_detail::newest(l_, key)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      return s->ref;
    }
    read = true;
    buffered_detail::Overlay<R> ov{res, l_};
    return tree_.get(key, ov);
  }

  // Whether A takes the key with at most `slots` slots in all.
  [[nodiscard]] auto has_room(Bytes key, std::size_t slots) const -> bool {
    return l_.a.total < slots && l_.a.counts[buffered_detail::hash_of(key).part] <
                                     buffered_detail::kSlotCapacity &&
           l_.a.arena + key.size() <= buffered_detail::kArenaBytes;
  }

  // Replaces the tree with the oldest frozen buffer's merge and drops the
  // buffer.
  void retire_oldest(Tree merged) {
    const auto id = fs_[0]->id;
    tree_ = std::move(merged);
    l_.pop_oldest();
    for (std::size_t i = 1; i <= l_.nf; ++i) fs_[i - 1] = std::move(fs_[i]);
    fs_[l_.nf].reset();
    if (here_ > l_.nf) here_ = l_.nf;
    std::erase_if(log_, [id](const Written &w) { return w.layer == id; });
  }

  // Makes room in A for a key, freezing A when it is full. With kMaxFrozen
  // buffers already frozen, the oldest is retired first: installed from the
  // merger, waited for if it is still merging (backpressure), or, when this
  // builder froze it and handed nothing over, merged here. Returns whether it
  // merged here (reading records through `res`).
  template <BlindKeyResolver R> auto make_room(Bytes key, R &res) -> bool {
    if (has_room(key, buffered_detail::kBufferSlots)) return false;
    bool merged = false;
    install_finished();
    if (l_.nf == buffered_detail::kMaxFrozen) {
      if (here_ == l_.nf) {
        merger_->drain();  // no merge for a discarded version shares our base
        auto t = Merger::merge_with(tree_, l_.f[0], l_, res);
        retire_oldest(std::move(t));
        tree_owned_ = true;
        merged = true;
      } else {
        retire_oldest(merger_->wait(*fs_[0]));
        tree_owned_ = false;
      }
    }
    freeze_active();
    return merged;
  }

  // Installs the merges that have finished, oldest first: free.
  void install_finished() {
    while (l_.nf > here_) {
      auto done = merger_->ready(*fs_[0]);
      if (!done) break;
      retire_oldest(std::move(*done));
      tree_owned_ = false;
    }
  }

  // A joins the frozen buffers and a fresh buffer becomes A. Needs a free
  // frozen place.
  void freeze_active() {
    const_cast<Buffer &>(*l_.a.buf).written.store(buffered_detail::kFrozen,
                                                 std::memory_order_relaxed);
    auto state = std::make_shared<Frozen>();
    state->id = merger_->next_id();
    for (auto &w : log_)
      if (w.layer == 0) w.layer = state->id;
    l_.f[l_.nf] = std::move(l_.a);
    fs_[l_.nf] = std::move(state);
    ++l_.nf;
    ++here_;
    l_.a = View{merger_->acquire()};
  }

  void write_slot(Bytes key, BlindRef ref, BlindRef displaced) {
    auto &a = l_.a;
    auto &b = const_cast<Buffer &>(*a.buf);  // this builder is its only writer
    const auto h = buffered_detail::hash_of(key);
    const auto p = h.part;
    auto &part = b.parts[p];
    const auto i = a.counts[p];
    if (i >= buffered_detail::kSlotCapacity || a.arena + key.size() > buffered_detail::kArenaBytes)
      throw std::logic_error{"buffered key directory: buffer overflow"};
    std::memcpy(b.arena.get() + a.arena, key.data(), key.size());
    part.fp[i] = h.fp;
    part.hi[i] = buffered_detail::word(key, 0);
    part.lo[i] = buffered_detail::word(key, 8);
    part.ref[i] = ref;
    part.old[i] = displaced;
    part.off[i] = a.arena;
    part.len[i] = static_cast<std::uint16_t>(key.size());
    a.arena += static_cast<std::uint32_t>(key.size());
    a.counts[p] = static_cast<std::uint16_t>(i + 1);
    ++a.total;
    a.net += delta(ref, displaced);
    b.written.store(a.total, std::memory_order_relaxed);
    log_.push_back({0, static_cast<std::uint8_t>(p)});
  }

  static auto delta(BlindRef ref, BlindRef displaced) noexcept -> int {
    if (buffered_detail::is_none(displaced)) return buffered_detail::is_none(ref) ? 0 : 1;
    return buffered_detail::is_none(ref) ? -1 : 0;
  }

  auto by_location(Bytes key, BlindRef from, BlindRef to) -> bool {
    if (!holds(key, from)) return false;
    if (take_back(key, from, to)) return true;
    if (!buffered_detail::newest(l_, key) && tree_owned_) {
      // In the tree this builder merged: change it there, where a record
      // an undo drops can never be read again.
      auto t = tree_.transient();
      const bool changed =
          buffered_detail::is_none(to) ? t.erase_at(key, from) : t.replace_at(key, from, to);
      tree_ = std::move(t).persistent();
      return changed;
    }
    // Without a resolver this builder cannot merge, so a write by location
    // runs A past its target, up to its capacity, and the next keyed write
    // makes room. Past capacity it declines, like a write whose location
    // moved: the caller takes the keyed path.
    if (!has_room(key, buffered_detail::kBufferCapacity)) return false;
    write_slot(key, to, from);
    return true;
  }

  // Pops this builder's newest write if it is (key: from, displacing to) —
  // the write that (key: to) by location undoes.
  auto take_back(Bytes key, BlindRef from, BlindRef to) -> bool {
    if (log_.empty()) return false;
    const auto w = log_.back();
    View *v = w.layer == 0 ? &l_.a : nullptr;
    for (std::size_t j = l_.nf - here_; v == nullptr && j < l_.nf; ++j)
      if (fs_[j]->id == w.layer) v = &l_.f[j];
    if (v == nullptr)
      throw std::logic_error{"buffered key directory: a logged write's buffer is gone"};
    const auto p = w.part;
    const auto i = static_cast<std::size_t>(v->counts[p]) - 1;
    const auto &part = v->buf->parts[p];
    if (!(part.ref[i] == from) || !(part.old[i] == to) ||
        buffered_detail::compare(v->buf->key(p, i), key) != 0)
      return false;
    log_.pop_back();
    v->counts[p] = static_cast<std::uint16_t>(i);
    --v->total;
    v->arena = part.off[i];  // the newest slot's key is the arena's last
    v->net -= delta(from, to);
    if (w.layer == 0)
      const_cast<Buffer &>(*v->buf).written.store(v->total, std::memory_order_relaxed);
    return true;
  }

  std::shared_ptr<Merger> merger_;
  Tree tree_;
  Layers l_;
  States fs_;
  // The newest here_ frozen buffers were frozen by this builder and are
  // not handed over yet; the rest were, by the versions it was made from.
  std::size_t here_{0};
  // tree_ was derived here, by a merge of this builder: nothing else
  // derives from it, so it can be changed in place.
  bool tree_owned_{false};
  std::vector<Written> log_;
};

template <std::size_t LeafBytes, typename RS>
auto BufferedBlindBTree<LeafBytes, RS>::transient() const
    -> TransientBufferedBlindBTree<LeafBytes, RS> {
  auto merger = merger_ ? merger_ : std::make_shared<Merger>();
  auto tree = tree_;
  auto l = l_;
  auto fs = fs_;
  // Install every finished merge, oldest first.
  while (l.nf > 0) {
    auto merged = merger->ready(*fs[0]);
    if (!merged) break;
    tree = std::move(*merged);
    l.pop_oldest();
    for (std::size_t i = 1; i <= l.nf; ++i) fs[i - 1] = std::move(fs[i]);
    fs[l.nf].reset();
  }
  auto &a = l.a;
  if (!a.buf) {
    a = buffered_detail::View{merger->acquire()};
  } else if (a.buf->written.load(std::memory_order_relaxed) != a.total) {
    // Another builder wrote past this version's slots (a discarded one, or
    // one made from this version before): append to a copy.
    auto copy = merger->acquire();
    std::memcpy(copy->arena.get(), a.buf->arena.get(), a.arena);
    for (std::size_t p = 0; p < buffered_detail::kPartitions; ++p) {
      const auto n = a.counts[p];
      const auto &from = a.buf->parts[p];
      auto &to = copy->parts[p];
      std::copy_n(from.fp.begin(), n, to.fp.begin());
      std::copy_n(from.hi.begin(), n, to.hi.begin());
      std::copy_n(from.lo.begin(), n, to.lo.begin());
      std::copy_n(from.ref.begin(), n, to.ref.begin());
      std::copy_n(from.old.begin(), n, to.old.begin());
      std::copy_n(from.off.begin(), n, to.off.begin());
      std::copy_n(from.len.begin(), n, to.len.begin());
    }
    copy->written.store(a.total, std::memory_order_relaxed);
    a.buf = std::move(copy);
  }
  return {std::move(merger), std::move(tree), std::move(l), std::move(fs)};
}

} // namespace bytecask
