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
// Versions. A version is (tree T, frozen buffer F or none, active buffer A
// with its slot count per partition). F is immutable. A is only appended to
// past a version's counts, into arrays that never move, so a reader of a
// version never sees a slot past its counts. A key's newest entry is in A,
// else F, else T.
//
// The builder (TransientBufferedBlindBTree) appends every write to A and
// never edits the tree. When A is full it freezes: A becomes F and a fresh
// buffer becomes A. F reaches the merger when the version holding it is
// published, together with what the merger needs to read records (the
// source, RS). The merger builds T' = T + F; a builder made later from a
// version (T, F, A) starts from (T', none, A), which holds the same keys. A
// builder that must freeze while F is still being merged waits for the
// merger (backpressure); one that must freeze twice before publishing merges
// its own F on the spot.
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
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
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
    std::uint32_t mask = match16(part.fp.data() + from, fp);
    if (end - from < 16) mask &= (std::uint32_t{1} << (2 * (end - from))) - 1;
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

// A key's newest entry in the buffers of a version: A, then F.
inline auto newest(const View &f, const View &a, Bytes key) -> std::optional<Slot> {
  const auto h = hash_of(key);
  if (auto s = find_in(a, key, h)) return s;
  return find_in(f, key, h);
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
  const View &f;
  const View &a;

  auto key_at(BlindRef r) -> Bytes {
    if constexpr (requires {
                    { inner.try_key_at(r) } -> std::same_as<std::optional<Bytes>>;
                  }) {
      if (auto k = inner.try_key_at(r)) return *k;
      if (auto k = displaced_key(a, r)) return *k;
      if (auto k = displaced_key(f, r)) return *k;
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
// (F, A); the newest slot wins a tie.
template <Pick P>
inline auto buffer_pick(const View &f, const View &a, std::optional<Bytes> bound,
                        bool inclusive) -> std::optional<Slot> {
  const auto bp = bound ? prefix(*bound) : Prefix{};
  std::optional<Slot> best;
  Prefix best_p{};
  for (const View *v : {&f, &a}) {  // oldest first
    if (!v->buf) continue;
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
  }
  return best;
}

// Whether any slot of (F, A) holds a key in [lo, hi) (hi none: no bound).
// Full keys only where a prefix ties.
inline auto any_in(const View &f, const View &a, Bytes lo, std::optional<Bytes> hi)
    -> bool {
  const auto lp = prefix(lo);
  const auto hp = hi ? prefix(*hi) : Prefix{~std::uint64_t{0}, ~std::uint64_t{0}};
  for (const View *v : {&f, &a}) {
    if (!v->buf) continue;
    for (std::size_t p = 0; p < kPartitions; ++p) {
      const auto &part = v->buf->parts[p];
      for (std::size_t i = 0; i < v->counts[p]; ++i) {
        const Prefix sp{part.hi[i], part.lo[i]};
        if (sp < lp || (hi && sp > hp)) continue;
        if (sp == lp || (hi && sp == hp)) {
          const auto k = v->buf->key(p, i);
          if (compare(k, lo) < 0 || (hi && compare(k, *hi) >= 0)) continue;
        }
        return true;
      }
    }
  }
  return false;
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
};

// Recycled buffers, shared with the deleters of the buffers it hands out.
struct BufferPool {
  static constexpr std::size_t kKept = 4;
  std::mutex mu;
  std::vector<std::unique_ptr<buffered_detail::Buffer>> free;

  static auto acquire(const std::shared_ptr<BufferPool> &pool)
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

template <std::size_t LeafBytes, typename RS> class BufferedMerger {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Buffer = buffered_detail::Buffer;
  using View = buffered_detail::View;
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
  [[nodiscard]] auto acquire() -> std::shared_ptr<Buffer> { return BufferPool::acquire(pool_); }

  // Hands F over: the merger builds base + F, reading records through `src`
  // under the buffers (F, A) of the version that published F.
  void submit(Tree base, View frozen, View active, const std::shared_ptr<Frozen> &state,
              RS src) {
    Job job{std::move(base), std::move(frozen), std::move(active), state, std::move(src)};
#ifdef BYTECASK_SINGLE_THREADED
    std::unique_lock<std::mutex> lk{mu_};
    execute(job, lk);
#else
    {
      std::lock_guard<std::mutex> lk{mu_};
      jobs_.push_back(std::move(job));
      if (!thread_.joinable()) thread_ = std::thread{[this] { run(); }};
    }
    cv_.notify_all();
#endif
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
  // tree derived from the same base.
  void drain() {
    std::unique_lock<std::mutex> lk{mu_};
    cv_.wait(lk, [&] { return jobs_.empty() && !running_; });
  }

  // Folds F into base. Only each key's newest slot: an older one may name a
  // record that a later write overrode.
  template <BlindKeyResolver R>
  static auto merge_with(const Tree &base, const View &frozen, const View &active, R &res)
      -> Tree {
    auto tr = base.transient();
    buffered_detail::Overlay<R> ov{res, frozen, active};
    std::unordered_map<std::string_view, std::uint32_t> newest;
    for (std::size_t p = 0; p < buffered_detail::kPartitions; ++p) {
      newest.clear();
      for (std::uint32_t i = 0; i < frozen.counts[p]; ++i) {
        const auto k = frozen.buf->key(p, i);
        newest[std::string_view{reinterpret_cast<const char *>(k.data()), k.size()}] = i;
      }
      for (const auto &[key, i] : newest) {
        const buffered_detail::Bytes k{reinterpret_cast<const std::byte *>(key.data()),
                                       key.size()};
        const auto ref = frozen.buf->parts[p].ref[i];
        if (buffered_detail::is_none(ref))
          (void)tr.erase(k, ov);
        else
          tr.set(k, ref, ov);
      }
    }
    return std::move(tr).persistent();
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

private:
  struct Job {
    Tree base;
    View frozen;
    View active;
    std::weak_ptr<Frozen> state;
    RS src;
  };

  // Runs one job; called with lk held, returns with it held.
  void execute(Job &job, std::unique_lock<std::mutex> &lk) {
    if (job.state.expired()) return;  // every version holding F is gone
    running_ = true;
    lk.unlock();
    std::optional<Tree> merged;
    std::exception_ptr error;
    try {
      job.src.with_resolver(
          [&](auto &res) { merged = merge_with(job.base, job.frozen, job.active, res); });
    } catch (...) {
      error = std::current_exception();
    }
    auto state = job.state.lock();
    job = Job{};  // release the base, F, A and the source outside the lock
    if (!state) merged.reset();  // nobody wants it: drop it before the next job
    lk.lock();
    running_ = false;
    if (state) {
      state->merged = std::move(merged);
      state->error = error;
    }
    lk.unlock();
    state.reset();  // may free the result, off the lock
    lk.lock();
    cv_.notify_all();
  }

  void run() {
    std::unique_lock<std::mutex> lk{mu_};
    for (;;) {
      cv_.wait(lk, [&] { return stop_ || (!jobs_.empty() && !running_); });
      if (stop_ && (jobs_.empty() || running_)) return;
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      execute(job, lk);
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool running_{false};
  bool stop_{false};
  std::atomic<std::uint64_t> next_id_{1};
  std::shared_ptr<BufferPool> pool_{std::make_shared<BufferPool>()};
  static inline std::atomic<std::int64_t> stalls_{0};
  static inline std::atomic<std::int64_t> stall_ns_{0};
  static inline std::atomic<std::int64_t> inline_merges_{0};
  std::thread thread_;  // last: joined before the rest is destroyed
};

export template <std::size_t LeafBytes, typename RS> class BufferedBlindBTree;
export template <std::size_t LeafBytes, typename RS> class TransientBufferedBlindBTree;

// ---------------------------------------------------------------------------
// BufferedIterator — the tree's entries merged with the buffers', in key
// order, the newest entry winning and erases hiding what they erase.
// Moving may read keys, so the iterator takes a resolver: settle(res) once
// after it is made, next(res) and prev(res) to move. The engine's key
// directory iterator does this for every tree.
//
// The next buffer candidate is cached: the smallest buffer key past the
// position stays the smallest until it is passed. A tree entry's key is
// read only to compare it with a candidate, so with none ahead, stepping
// through the tree reads nothing more than the tree iterator does.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> class BufferedIterator {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using TIt = BlindBTreeIterator<LeafBytes>;
  using Bytes = buffered_detail::Bytes;
  using View = buffered_detail::View;
  using Slot = buffered_detail::Slot;
  using Pick = buffered_detail::Pick;
  enum class Start : std::uint8_t { None, Begin, End, At };

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = BlindRef;
  using difference_type = std::ptrdiff_t;

  BufferedIterator() = default;

  [[nodiscard]] auto operator*() const -> BlindRef { return ref_; }
  template <BlindKeyResolver R> [[nodiscard]] auto key(R &res) const -> Bytes {
    ensure_key(res);
    return {key_.data(), key_.size()};
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool { return at_end_; }
  // Same entry: a record location names one record.
  auto operator==(const BufferedIterator &o) const noexcept -> bool {
    if (at_end_ || o.at_end_) return at_end_ == o.at_end_;
    return ref_ == o.ref_;
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
  // bounds the iterators were placed by: two reads at most, and none when
  // the tree positions are at hand. Otherwise by stepping.
  template <BlindKeyResolver R>
  [[nodiscard]] auto count_until(const BufferedIterator &end, std::size_t limit,
                                 R &res) const -> std::size_t {
    if (limit == 0) return 0;
    const auto lo = range_bound(res);
    if (!lo) return 0;  // at the end
    const auto hi = end.range_bound(res);
    if (hi && buffered_detail::compare(*lo, *hi) >= 0) return 0;
    if (!buffered_detail::any_in(f_, a_, *lo, hi)) {
      // Every entry in range is the tree's.
      if (start_ == Start::None && fwd_ && from_tree_ && end.start_ == Start::None) {
        // tf_ is one past this entry.
        if (end.at_end_) return 1 + tf_.count_until(tree_.end_iter(), limit - 1);
        if (end.fwd_ && end.from_tree_) return tf_.count_until(end.tf_, limit);
      }
      buffered_detail::Overlay<R> ov{res, f_, a_};
      const auto from = tree_.lower_bound(*lo, ov);
      const auto to = hi ? tree_.lower_bound(*hi, ov) : tree_.end_iter();
      return from.count_until(to, limit);
    }
    auto it = *this;
    it.settle(res);
    auto stop = end;
    stop.settle(res);
    std::size_t n = 0;
    while (n < limit && !(it == stop) && !(it == std::default_sentinel)) {
      ++n;
      it.next(res);
    }
    return n;
  }

private:
  template <std::size_t, typename> friend class BufferedBlindBTree;
  template <std::size_t, typename> friend class TransientBufferedBlindBTree;

  BufferedIterator(Tree tree, View f, View a, Start start, Bytes at = {})
      : tree_{std::move(tree)}, f_{std::move(f)}, a_{std::move(a)}, start_{start},
        start_key_(at.begin(), at.end()) {}

  // Where the entries from here start: the bound it was placed by or its
  // key; none at the end. Valid until the iterator next changes.
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
    buffered_detail::Overlay<R> ov{res, f_, a_};
    const auto k = ov.key_at(ref_);
    key_.assign(k.begin(), k.end());
    key_ok_ = true;
  }

  // Positions on the smallest entry > (>=) lo.
  template <BlindKeyResolver R> void seek(Bytes lo, bool inclusive, R &res) {
    buffered_detail::Overlay<R> ov{res, f_, a_};
    tf_ = lo.empty() && inclusive ? tree_.begin() : tree_.lower_bound(lo, ov);
    if (!inclusive && tf_ != std::default_sentinel &&
        buffered_detail::compare(tf_.key(ov), lo) == 0)
      ++tf_;
    fwd_ = true;
    bwd_ = false;
    bound_ok_ = inclusive;
    if (inclusive) bound_.assign(lo.begin(), lo.end());
    find_candidate<Pick::Min>(lo, inclusive);
    forward(res);
  }

  // Positions on the largest entry < hi, or the last if none.
  template <BlindKeyResolver R> void seek_back(std::optional<Bytes> hi, R &res) {
    buffered_detail::Overlay<R> ov{res, f_, a_};
    tb_ = hi ? tree_.lower_bound(*hi, ov) : tree_.end_iter();
    --tb_;
    bwd_ = true;
    fwd_ = false;
    find_candidate<Pick::Max>(hi, false);
    backward(res);
  }

  // The next buffered entry past `bound`.
  template <Pick P> void find_candidate(std::optional<Bytes> bound, bool inclusive) {
    cand_ = buffered_detail::buffer_pick<P>(f_, a_, bound, inclusive);
    cand_ok_ = true;
  }

  // The fence of the tree iterator's leaf (upper going forward, lower going
  // back), cached per leaf.
  template <bool Upper> auto fence(const TIt &t) -> std::optional<Bytes> {
    if (t.leaf() != fence_leaf_ || fence_upper_ != Upper) {
      fence_leaf_ = t.leaf();
      fence_upper_ = Upper;
      fence_has_ = Upper ? t.upper_fence(fence_) : t.lower_fence(fence_);
    }
    if (!fence_has_) return std::nullopt;
    return Bytes{fence_.data(), fence_.size()};
  }

  // Steps to the next entry: the smaller of the buffer candidate and the
  // tree's next, tf_. The position (key_) moves past erases too. A tree
  // entry is known to come first without reading its key when the
  // candidate is at or past its leaf's upper fence.
  template <BlindKeyResolver R> void forward(R &res) {
    buffered_detail::Overlay<R> ov{res, f_, a_};
    for (;;) {
      if (!cand_ok_) {
        ensure_key(res);
        find_candidate<Pick::Min>(Bytes{key_.data(), key_.size()}, false);
      }
      const bool tree_left = tf_ != std::default_sentinel;
      int c = -1;
      bool read = false;  // the tree entry's key, into tkey_
      if (cand_ && tree_left) {
        const auto up = fence<true>(tf_);
        if (up && buffered_detail::compare(cand_->key, *up) >= 0) {
          c = 1;
        } else {
          read = true;
          const auto k = tf_.key(ov);
          tkey_.assign(k.begin(), k.end());
          c = buffered_detail::compare(cand_->key, {tkey_.data(), tkey_.size()});
        }
      }
      if (cand_ && c <= 0) {
        if (c == 0) ++tf_;
        take_candidate();
        if (buffered_detail::is_none(ref_)) continue;
        return;
      }
      if (tree_left) {
        take_tree(*tf_, read);
        ++tf_;
        return;
      }
      at_end_ = true;
      fwd_ = false;
      return;
    }
  }

  // Mirror of forward: the larger of the candidate and tb_, with the lower
  // fence.
  template <BlindKeyResolver R> void backward(R &res) {
    buffered_detail::Overlay<R> ov{res, f_, a_};
    for (;;) {
      if (!cand_ok_) {
        ensure_key(res);
        find_candidate<Pick::Max>(Bytes{key_.data(), key_.size()}, false);
      }
      const bool tree_left = tb_ != std::default_sentinel;
      int c = 1;
      bool read = false;
      if (cand_ && tree_left) {
        const auto down = fence<false>(tb_);
        if (down && buffered_detail::compare(cand_->key, *down) < 0) {
          c = -1;
        } else {
          read = true;
          const auto k = tb_.key(ov);
          tkey_.assign(k.begin(), k.end());
          c = buffered_detail::compare(cand_->key, {tkey_.data(), tkey_.size()});
        }
      }
      if (cand_ && c >= 0) {
        if (c == 0) --tb_;
        take_candidate();
        if (buffered_detail::is_none(ref_)) continue;
        return;
      }
      if (tree_left) {
        take_tree(*tb_, read);
        --tb_;
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

  Tree tree_;
  View f_;
  View a_;
  Start start_{Start::None};
  std::vector<std::byte> start_key_;
  TIt tf_;  // forward: the first tree entry not yet passed
  TIt tb_;  // backward: the last tree entry not yet passed
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
  // The inclusive bound a seek placed this iterator at, until it moves.
  std::vector<std::byte> bound_;
  bool bound_ok_{false};
};

// ---------------------------------------------------------------------------
// BufferedBlindBTree — a published version. Immutable; any thread.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes, typename RS> class BufferedBlindBTree {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Merger = BufferedMerger<LeafBytes, RS>;
  using Frozen = FrozenMerge<LeafBytes>;
  using View = buffered_detail::View;
  using Bytes = buffered_detail::Bytes;
  using Iter = BufferedIterator<LeafBytes>;

public:
  using iterator = Iter;

  BufferedBlindBTree() = default;
  // A tree built elsewhere (recovery), with no buffers.
  BufferedBlindBTree(Tree t) : tree_{std::move(t)} {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return static_cast<std::size_t>(static_cast<std::int64_t>(tree_.size()) + f_.net + a_.net);
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    if (auto s = buffered_detail::newest(f_, a_, key)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      (void)res.key_at(s->ref);  // the read a lookup's caller takes the entry from
      return s->ref;
    }
    buffered_detail::Overlay<R> ov{res, f_, a_};
    return tree_.get(key, ov);
  }
  template <BlindKeyResolver R>
  [[nodiscard]] auto contains(Bytes key, R &res) const -> bool {
    return get(key, res).has_value();
  }
  [[nodiscard]] auto holds(Bytes key, BlindRef ref) const -> bool {
    if (auto s = buffered_detail::newest(f_, a_, key)) return s->ref == ref;
    return tree_.holds(key, ref);
  }

  // Unsettled: it reads nothing until settled or stepped, so a count
  // between two bounds reads only what the tree needs to place them.
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &) const -> Iter {
    return {tree_, f_, a_, Iter::Start::At, key};
  }
  // Unsettled: the key directory's iterator settles it.
  [[nodiscard]] auto begin() const -> Iter { return {tree_, f_, a_, Iter::Start::Begin}; }
  [[nodiscard]] auto end_iter() const -> Iter { return {tree_, f_, a_, Iter::Start::End}; }
  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t { return {}; }

  [[nodiscard]] auto transient() const -> TransientBufferedBlindBTree<LeafBytes, RS>;

  [[nodiscard]] static auto reclamation_gauges() { return Tree::reclamation_gauges(); }
  [[nodiscard]] static auto buffer_stalls() noexcept -> std::int64_t { return Merger::stalls(); }
  [[nodiscard]] static auto buffer_stall_ns() noexcept -> std::int64_t {
    return Merger::stall_ns();
  }
  [[nodiscard]] static auto buffer_inline_merges() noexcept -> std::int64_t {
    return Merger::inline_merges();
  }

#ifdef BYTECASK_TESTING
  // Waits for every merge handed over so far.
  void test_wait_merged() const {
    if (merger_) merger_->drain();
  }
  // Slots this version sees in its buffers.
  [[nodiscard]] auto test_buffered() const -> std::size_t { return f_.total + a_.total; }
#endif

private:
  friend class TransientBufferedBlindBTree<LeafBytes, RS>;
  BufferedBlindBTree(std::shared_ptr<Merger> m, Tree t, View f, std::shared_ptr<Frozen> fs,
                     View a)
      : merger_{std::move(m)}, tree_{std::move(t)}, f_{std::move(f)}, fstate_{std::move(fs)},
        a_{std::move(a)} {}

  std::shared_ptr<Merger> merger_;  // null until a builder is made
  Tree tree_;
  View f_;
  std::shared_ptr<Frozen> fstate_;  // F's merge; set whenever F is
  View a_;
};

// ---------------------------------------------------------------------------
// TransientBufferedBlindBTree — the builder. One thread at a time.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes, typename RS> class TransientBufferedBlindBTree {
  using Tree = PersistentBlindBTree<LeafBytes>;
  using Merger = BufferedMerger<LeafBytes, RS>;
  using Frozen = FrozenMerge<LeafBytes>;
  using View = buffered_detail::View;
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
    return static_cast<std::size_t>(static_cast<std::int64_t>(tree_.size()) + f_.net + a_.net);
  }

  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    if (auto s = buffered_detail::newest(f_, a_, key)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      (void)res.key_at(s->ref);
      return s->ref;
    }
    buffered_detail::Overlay<R> ov{res, f_, a_};
    return tree_.get(key, ov);
  }
  [[nodiscard]] auto holds(Bytes key, BlindRef ref) const -> bool {
    if (auto s = buffered_detail::newest(f_, a_, key)) return s->ref == ref;
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

  // Unsettled: it reads nothing until settled or stepped, so a count
  // between two bounds reads only what the tree needs to place them.
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &) const -> Iter {
    return {tree_, f_, a_, Iter::Start::At, key};
  }

  // Publishes. A buffer this builder froze goes to the merger with `src`,
  // which reads the records of the version being published.
  [[nodiscard]] auto persistent(RS src) && -> BufferedBlindBTree<LeafBytes, RS> {
    if (frozen_here_) merger_->submit(tree_, f_, a_, fstate_, std::move(src));
    return {std::move(merger_), std::move(tree_), std::move(f_), std::move(fstate_),
            std::move(a_)};
  }

private:
  friend class BufferedBlindBTree<LeafBytes, RS>;
  TransientBufferedBlindBTree(std::shared_ptr<Merger> m, Tree t, View f,
                              std::shared_ptr<Frozen> fs, View a)
      : merger_{std::move(m)}, tree_{std::move(t)}, f_{std::move(f)}, fstate_{std::move(fs)},
        a_{std::move(a)} {}

  // A write of this builder still in a buffer, newest last.
  struct Written {
    bool in_f;
    std::uint8_t part;
  };

  // The key's current record, if it has one; `read` says whether finding it
  // read it (the tree does, the buffers do not).
  template <BlindKeyResolver R>
  auto lookup(Bytes key, R &res, bool &read) const -> std::optional<BlindRef> {
    if (auto s = buffered_detail::newest(f_, a_, key)) {
      if (buffered_detail::is_none(s->ref)) return std::nullopt;
      return s->ref;
    }
    read = true;
    buffered_detail::Overlay<R> ov{res, f_, a_};
    return tree_.get(key, ov);
  }

  // Whether A takes the key with at most `slots` slots in all.
  [[nodiscard]] auto has_room(Bytes key, std::size_t slots) const -> bool {
    return a_.total < slots && a_.counts[buffered_detail::hash_of(key).part] <
                                   buffered_detail::kSlotCapacity &&
           a_.arena + key.size() <= buffered_detail::kArenaBytes;
  }

  // Makes room in A for a key, freezing A when it is full: first the F
  // before it is installed — waiting for the merger if need be — or, if
  // this builder froze it, merged here. Returns whether it merged here
  // (reading records through `res`).
  template <BlindKeyResolver R> auto make_room(Bytes key, R &res) -> bool {
    if (has_room(key, buffered_detail::kBufferSlots)) return false;
    bool merged = false;
    if (f_.buf) {
      if (frozen_here_) {
        merger_->drain();  // no merge for a discarded version shares our base
        tree_ = Merger::merge_with(tree_, f_, a_, res);
        tree_owned_ = true;
        merged = true;
      } else {
        tree_ = merger_->wait(*fstate_);
        tree_owned_ = false;
      }
      f_ = View{};
      fstate_.reset();
      frozen_here_ = false;
      std::erase_if(log_, [](const Written &w) { return w.in_f; });
    }
    const_cast<Buffer &>(*a_.buf).written.store(buffered_detail::kFrozen,
                                               std::memory_order_relaxed);
    f_ = std::move(a_);
    fstate_ = std::make_shared<Frozen>();
    fstate_->id = merger_->next_id();
    frozen_here_ = true;
    for (auto &w : log_) w.in_f = true;
    a_ = View{merger_->acquire()};
    return merged;
  }

  void write_slot(Bytes key, BlindRef ref, BlindRef displaced) {
    auto &b = const_cast<Buffer &>(*a_.buf);  // this builder is its only writer
    const auto h = buffered_detail::hash_of(key);
    const auto p = h.part;
    auto &part = b.parts[p];
    const auto i = a_.counts[p];
    if (i >= buffered_detail::kSlotCapacity || a_.arena + key.size() > buffered_detail::kArenaBytes)
      throw std::logic_error{"buffered key directory: buffer overflow"};
    std::memcpy(b.arena.get() + a_.arena, key.data(), key.size());
    part.fp[i] = h.fp;
    part.hi[i] = buffered_detail::word(key, 0);
    part.lo[i] = buffered_detail::word(key, 8);
    part.ref[i] = ref;
    part.old[i] = displaced;
    part.off[i] = a_.arena;
    part.len[i] = static_cast<std::uint16_t>(key.size());
    a_.arena += static_cast<std::uint32_t>(key.size());
    a_.counts[p] = static_cast<std::uint16_t>(i + 1);
    ++a_.total;
    a_.net += delta(ref, displaced);
    b.written.store(a_.total, std::memory_order_relaxed);
    log_.push_back({false, static_cast<std::uint8_t>(p)});
  }

  static auto delta(BlindRef ref, BlindRef displaced) noexcept -> int {
    if (buffered_detail::is_none(displaced)) return buffered_detail::is_none(ref) ? 0 : 1;
    return buffered_detail::is_none(ref) ? -1 : 0;
  }

  auto by_location(Bytes key, BlindRef from, BlindRef to) -> bool {
    if (!holds(key, from)) return false;
    if (take_back(key, from, to)) return true;
    if (!buffered_detail::newest(f_, a_, key) && tree_owned_) {
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
    View &v = w.in_f ? f_ : a_;
    const auto p = w.part;
    const auto i = static_cast<std::size_t>(v.counts[p]) - 1;
    const auto &part = v.buf->parts[p];
    if (!(part.ref[i] == from) || !(part.old[i] == to) ||
        buffered_detail::compare(v.buf->key(p, i), key) != 0)
      return false;
    log_.pop_back();
    v.counts[p] = static_cast<std::uint16_t>(i);
    --v.total;
    v.arena = part.off[i];  // the newest slot's key is the arena's last
    v.net -= delta(from, to);
    if (!w.in_f) const_cast<Buffer &>(*v.buf).written.store(v.total, std::memory_order_relaxed);
    return true;
  }

  std::shared_ptr<Merger> merger_;
  Tree tree_;
  View f_;
  std::shared_ptr<Frozen> fstate_;
  View a_;
  bool frozen_here_{false};
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
  auto f = f_;
  auto fs = fstate_;
  if (fs) {
    if (auto merged = merger->ready(*fs)) {  // install
      tree = std::move(*merged);
      f = View{};
      fs.reset();
    }
  }
  auto a = a_;
  if (!a.buf) {
    a = View{merger->acquire()};
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
  return {std::move(merger), std::move(tree), std::move(f), std::move(fs), std::move(a)};
}

} // namespace bytecask
