// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — persistent B+ tree with blind leaves.
//
// Inner nodes, path copying, BuildSession and the VersionChain are the B+
// tree's (btree.cppm). A leaf stores no key bytes: each entry is a crit bit
// (the first bit where the key differs from the previous key in the leaf), a
// 24-bit fingerprint and the location of the record that holds the key: 12
// bytes. A search in a leaf tests crit bits only and reaches one candidate,
// whose key a caller-supplied resolver reads to confirm the match or to find
// the exact position. See docs/blind_leaf_btree_design.md.

module;
#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

export module bytecask.blind_btree;
import bytecask.btree;
import bytecask.version_chain;

namespace bytecask {

// Where the record holding a key lives. Within one process a (file_id,
// offset) pair names at most one record, so equal locations mean the same
// record. Sizes are the record's own business: its header has them.
export struct BlindRef {
  std::uint32_t file_id{0};
  std::uint32_t offset{0};
  friend auto operator==(const BlindRef &, const BlindRef &) -> bool = default;
};

export inline constexpr std::uint32_t kBlindMaxFileId = (1u << 20) - 1;

// Reads the key of the record at a location. The returned span is valid until
// the next call on the same resolver; the tree never holds two at once.
export template <typename R>
concept BlindKeyResolver = requires(R &r, BlindRef ref) {
  { r.key_at(ref) } -> std::convertible_to<std::span<const std::byte>>;
};

export template <std::size_t LeafBytes> class PersistentBlindBTree;
export template <std::size_t LeafBytes> class TransientBlindBTree;
export template <std::size_t LeafBytes> class BlindBTreeIterator;
export template <std::size_t LeafBytes> class BlindBulkLoader;

namespace blind_detail {

using Bytes = std::span<const std::byte>;
using N = btree_detail::Node<BlindRef>;

export inline constexpr std::uint32_t kNoCrit = 0xFFFF'FFFFu;

// Keys are compared as bit strings in which every byte is preceded by a 1
// and the end of the key is a 0, then 0 forever. The encoding keeps
// byte-lexicographic order and makes a key and its prefix differ ("ab" vs
// "ab\0" differ at the continuation bit of the third byte).
//
// A bit position is written byte << 4 | r: r = 0 is the byte's continuation
// bit, r = 1..8 its bits from the most significant. Positions compare in bit
// order, a search splits one with a shift and a mask instead of a division
// by 9, and the largest (65,534 << 4 | 8) fits the 20 bits a leaf stores.
export inline constexpr std::uint32_t kPosShift = 4;

export inline auto bit(Bytes k, std::uint32_t p) noexcept -> std::uint32_t {
  const std::size_t i = p >> kPosShift;
  const auto r = p & 15u;
  if (i >= k.size())
    return 0;
  if (r == 0)
    return 1;
  return (std::to_integer<std::uint32_t>(k[i]) >> (8 - r)) & 1u;
}

// The first bit where a and b differ, or kNoCrit if they are equal.
export inline auto crit(Bytes a, Bytes b) noexcept -> std::uint32_t {
  const auto cpl = btree_detail::common_prefix_length(a, b);
  if (cpl == a.size() && cpl == b.size())
    return kNoCrit;
  const auto base = static_cast<std::uint32_t>(cpl << kPosShift);
  if (cpl == a.size() || cpl == b.size())
    return base; // the shorter key ends: its continuation bit is 0
  const auto x = std::to_integer<std::uint8_t>(a[cpl] ^ b[cpl]);
  return base + 1 + static_cast<std::uint32_t>(std::countl_zero(x));
}

// A 24-bit hash of the whole key. Not used for ordering: it lets a lookup
// reject a candidate that is not the key without reading it. Every lookup
// and write computes one, so it is a few multiplies over 8-byte words,
// inlined, rather than a library call; the bytes are read in little-endian
// order so a fingerprint does not depend on the host.
export inline auto fingerprint(Bytes k) noexcept -> std::uint32_t {
  constexpr std::uint64_t kMul = 0x9E37'79B9'7F4A'7C15u;
  auto word = [](const std::byte *p, std::size_t n) {
    std::uint64_t w = 0;
    for (std::size_t i = 0; i < n; ++i)
      w |= std::uint64_t{std::to_integer<std::uint8_t>(p[i])} << (8 * i);
    return w;
  };
  std::uint64_t h = k.size() * kMul;
  std::size_t i = 0;
  for (; i + 8 <= k.size(); i += 8)
    h = (h ^ word(k.data() + i, 8)) * kMul;
  if (i < k.size())
    h = (h ^ word(k.data() + i, k.size() - i)) * kMul;
  h ^= h >> 29;
  h *= kMul;
  return static_cast<std::uint32_t>(h >> 40);
}

// ---------------------------------------------------------------------------
// Leaf<LeafBytes> — the layout of a blind leaf. A blind leaf is a btree Node
// with is_leaf set (so the version chain and BuildSession handle it like any
// other node) whose bytes after the header are two arrays instead of slots
// and a heap, 12 bytes per entry:
//
//   meta[cap]  u32  crit:20 | fp_lo:12    crit against the previous entry;
//                                        unused at index 0
//   loc[cap]   u64  file_id:20 | offset:32 | fp_hi:12
//
// The search reads only meta. Capacity follows from the allocation size, so
// LeafBytes should be a malloc size class.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> struct Leaf {
  static constexpr std::size_t kHeader = N::header_bytes();
  static constexpr std::size_t kCap = (LeafBytes - kHeader - 4) / 12;
  static constexpr std::size_t kMetaOff = kHeader;
  static constexpr std::size_t kLocOff = (kMetaOff + 4 * kCap + 7) / 8 * 8;
  static_assert(LeafBytes % 16 == 0);
  static_assert(kLocOff + 8 * kCap <= LeafBytes);
  static_assert(kCap >= 4 && kCap < N::kNoLastPos);

  // Two parallel arrays: one leaf's, or a scratch copy during a split.
  struct Arrays {
    std::uint32_t *meta;
    std::uint64_t *loc;
  };

  [[nodiscard]] static auto arrays(N *n) noexcept -> Arrays {
    auto *b = n->bytes();
    return {btree_detail::as_ptr<std::uint32_t>(b + kMetaOff),
            btree_detail::as_ptr<std::uint64_t>(b + kLocOff)};
  }
  [[nodiscard]] static auto meta(const N *n) noexcept -> const std::uint32_t * {
    return btree_detail::as_ptr<std::uint32_t>(n->bytes() + kMetaOff);
  }
  [[nodiscard]] static auto loc(const N *n) noexcept -> const std::uint64_t * {
    return btree_detail::as_ptr<std::uint64_t>(n->bytes() + kLocOff);
  }

  [[nodiscard]] static auto crit_at(const N *n, std::uint32_t i) noexcept
      -> std::uint32_t {
    return meta(n)[i] >> 12;
  }
  [[nodiscard]] static auto fp_at(const N *n, std::uint32_t i) noexcept
      -> std::uint32_t {
    return static_cast<std::uint32_t>((loc(n)[i] & 0xFFFu) << 12) |
           (meta(n)[i] & 0xFFFu);
  }
  [[nodiscard]] static auto ref_of(std::uint64_t l) noexcept -> BlindRef {
    return {static_cast<std::uint32_t>(l >> 44),
            static_cast<std::uint32_t>(l >> 12)};
  }
  [[nodiscard]] static auto ref_at(const N *n, std::uint32_t i) noexcept
      -> BlindRef {
    return ref_of(loc(n)[i]);
  }

  static void write(Arrays a, std::uint32_t i, std::uint32_t crit_bit,
                    std::uint32_t fp, BlindRef ref) noexcept {
    a.meta[i] = (crit_bit << 12) | (fp & 0xFFFu);
    a.loc[i] = (std::uint64_t{ref.file_id} << 44) |
               (std::uint64_t{ref.offset} << 12) | (fp >> 12);
  }
  // Replaces the record, keeping the crit bit and the fingerprint.
  static void set_ref(N *n, std::uint32_t i, BlindRef ref) noexcept {
    auto a = arrays(n);
    a.loc[i] = (std::uint64_t{ref.file_id} << 44) |
               (std::uint64_t{ref.offset} << 12) | (a.loc[i] & 0xFFFu);
  }
  static void set_crit(Arrays a, std::uint32_t i, std::uint32_t crit_bit) noexcept {
    a.meta[i] = (crit_bit << 12) | (a.meta[i] & 0xFFFu);
  }
  static void copy(Arrays dst, std::uint32_t to, Arrays src, std::uint32_t from,
                   std::uint32_t n) noexcept {
    std::memmove(dst.meta + to, src.meta + from, n * sizeof(std::uint32_t));
    std::memmove(dst.loc + to, src.loc + from, n * sizeof(std::uint64_t));
  }

  // The blind search: a walk of the Patricia trie the sorted keys and their
  // crit bits imply, done in one branch-free pass. `s` is the crit bit of the
  // last left turn still in force; boundaries at or above it are that node's
  // right subtree, which the search did not enter. A right turn enters a
  // subtree and resets it.
  [[nodiscard]] static auto candidate(const N *n, Bytes q) noexcept
      -> std::uint32_t {
    const auto *m = meta(n);
    const std::size_t len = q.size();
    const std::byte zero{};
    const std::byte *d = len > 0 ? q.data() : &zero;
    const std::size_t last = len > 0 ? len - 1 : 0;
    std::uint32_t c = 0;
    std::uint32_t s = kNoCrit;
    for (std::uint32_t i = 1; i < n->count; ++i) {
      const auto p = m[i] >> 12;
      const std::size_t bi = p >> kPosShift;
      const auto r = p & 15u;
      const auto byte = std::to_integer<std::uint32_t>(d[std::min(bi, last)]);
      const auto enc = bi < len ? (0x100u | byte) : 0u;
      const auto right = (enc >> (8 - r)) & 1u;
      const bool on_path = p < s;
      c = (on_path && right != 0) ? i : c;
      s = on_path ? (right != 0 ? kNoCrit : p) : s;
    }
    return c;
  }

  // Index of `q` in the leaf, if present. Reads the candidate's key only when
  // its fingerprint matches.
  template <BlindKeyResolver R>
  [[nodiscard]] static auto find(const N *n, Bytes q, std::uint32_t fpq, R &res)
      -> std::optional<std::uint32_t> {
    if (n->count == 0)
      return std::nullopt;
    const auto c = candidate(n, q);
    if (fp_at(n, c) != fpq)
      return std::nullopt;
    const Bytes kc = res.key_at(ref_at(n, c));
    if (btree_detail::compare_bytes(kc, q) != 0)
      return std::nullopt;
    return c;
  }

  // Where `q` is or belongs. For an absent key, `j` is its crit bit against
  // the candidate, and `after` says whether it sorts after the candidate's
  // run (the keys that share bits [0, j) with it) or before it.
  struct Pos {
    std::uint32_t idx{0};
    bool exact{false};
    bool after{false};
    std::uint32_t j{0};
  };

  // One read unless the leaf is empty. The run argument: j cannot be a crit
  // bit on the candidate's path, or the search would have followed q's bit
  // there, so no boundary inside the run has crit j and q sorts at one of
  // its ends.
  template <BlindKeyResolver R>
  [[nodiscard]] static auto position(const N *n, Bytes q, std::uint32_t fpq,
                                     R &res) -> Pos {
    if (n->count == 0)
      return {};
    const auto c = candidate(n, q);
    const Bytes kc = res.key_at(ref_at(n, c));
    const auto j = crit(q, kc);
    if (j == kNoCrit) {
      assert(fp_at(n, c) == fpq);
      (void)fpq;
      return {c, true, false, 0};
    }
    auto a = c;
    while (a > 0 && crit_at(n, a) > j)
      --a;
    auto b = c + 1;
    while (b < n->count && crit_at(n, b) > j)
      ++b;
    if (bit(q, j) != 0)
      return {b, false, true, j};
    return {a, false, false, j};
  }

  // Inserts at p.idx into arrays holding `count` entries. Only the new entry
  // and at most one neighbour's crit bit change.
  static void insert_into(Arrays a, std::uint32_t count, const Pos &p,
                          std::uint32_t fpq, BlindRef ref) noexcept {
    copy(a, p.idx + 1, a, p.idx, count - p.idx);
    if (p.after) {
      // After the run: q differs from its predecessor at j; the key after it
      // differed from the run at a lower bit, which is also its bit against q.
      write(a, p.idx, p.j, fpq, ref);
    } else {
      // Before the run: q takes the boundary the run's first key had, and
      // that key now differs from q at j.
      const auto inherited = p.idx > 0 ? (a.meta[p.idx + 1] >> 12) : 0u;
      write(a, p.idx, inherited, fpq, ref);
      if (p.idx + 1 <= count)
        set_crit(a, p.idx + 1, p.j);
    }
  }

  // The first difference between two keys is the smallest first difference
  // between any adjacent pair between them, so erasing needs no read.
  static void remove_at(N *n, std::uint32_t i) noexcept {
    auto a = arrays(n);
    if (i > 0 && i + 1 < n->count)
      set_crit(a, i + 1, std::min(a.meta[i] >> 12, a.meta[i + 1] >> 12));
    copy(a, i, a, i + 1, n->count - i - 1);
    --n->count;
    n->last_pos = N::kNoLastPos;
  }

  [[nodiscard]] static auto allocate(std::uint64_t tag) -> N * {
    return N::allocate(LeafBytes, true, tag, {});
  }
};

// Checks the limits of what a leaf entry can store.
inline void check_ref(Bytes key, BlindRef ref) {
  if (key.size() > kBTreeMaxKeyBytes)
    throw std::length_error{"BlindBTree: key exceeds 65535 bytes"};
  if (ref.file_id > kBlindMaxFileId)
    throw std::out_of_range{"BlindBTree: file_id exceeds 20 bits"};
}

// ---------------------------------------------------------------------------
// BlindSession<LeafBytes> — a BuildSession whose leaf steps are blind. The
// descent, the inner-node path copy and splits, and node ownership are the
// base's; this adds only what a blind leaf does differently.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes>
class BlindSession : public btree_detail::BuildSession<BlindRef> {
  using Base = btree_detail::BuildSession<BlindRef>;
  using L = Leaf<LeafBytes>;

public:
  using Result = Base::Result;

  template <BlindKeyResolver R, typename Pred>
  auto upsert(N *root, Bytes key, BlindRef ref, R &res, Pred &should_replace)
      -> Result {
    displaced_.reset();
    const auto fpq = fingerprint(key);
    if (!root) {
      auto *leaf = L::allocate(tag_);
      L::write(L::arrays(leaf), 0, 0, fpq, ref);
      leaf->count = 1;
      return {leaf, nullptr, true, true};
    }
    auto step = [&](N *leaf) {
      return upsert_leaf(leaf, key, fpq, ref, res, should_replace);
    };
    return this->descend_upsert(root, key, step);
  }

  template <BlindKeyResolver R>
  auto erase(N *root, Bytes key, R &res) -> Result {
    displaced_.reset();
    if (!root)
      return {nullptr, nullptr, false, false};
    const auto fpq = fingerprint(key);
    auto step = [&](N *leaf) -> Result {
      const auto idx = L::find(leaf, key, fpq, res);
      if (!idx)
        return {leaf, nullptr, false, false};
      displaced_ = L::ref_at(leaf, *idx);
      auto *n = own_leaf(leaf);
      L::remove_at(n, *idx);
      if (n->count == 0) {
        discard(n);
        return {nullptr, nullptr, true, false};
      }
      return {n, nullptr, true, false};
    };
    return this->descend_erase(root, key, step);
  }

private:
  // The base's own() rebuilds a node as a slotted page; a blind leaf is
  // cloned by copying its arrays.
  auto own_leaf(N *leaf) -> N * {
    if (owns(leaf))
      return leaf;
    auto *fresh = L::allocate(tag_);
    L::copy(L::arrays(fresh), 0, L::arrays(leaf), 0, leaf->count);
    fresh->count = leaf->count;
    fresh->last_pos = leaf->last_pos;
    discard(leaf);
    return fresh;
  }

  template <BlindKeyResolver R, typename Pred>
  auto upsert_leaf(N *leaf, Bytes key, std::uint32_t fpq, BlindRef ref, R &res,
                   Pred &should_replace) -> Result {
    const auto p = L::position(leaf, key, fpq, res);
    if (p.exact) {
      const auto existing = L::ref_at(leaf, p.idx);
      if (!should_replace(existing, ref))
        return {leaf, nullptr, false, false};
      auto *n = own_leaf(leaf);
      L::set_ref(n, p.idx, ref);
      displaced_ = existing;
      return {n, nullptr, true, false};
    }
    if (leaf->count < L::kCap) {
      auto *n = own_leaf(leaf);
      L::insert_into(L::arrays(n), n->count, p, fpq, ref);
      ++n->count;
      n->last_pos = static_cast<std::uint16_t>(p.idx);
      return {n, nullptr, true, true};
    }
    return split_leaf(leaf, p, key, fpq, ref, res);
  }

  // Splits a full leaf plus the new entry into two. The crit bit at the
  // split is already known, so the separator's length is too; its bytes are
  // the right half's first key, which is read unless it is the new key.
  template <BlindKeyResolver R>
  auto split_leaf(N *leaf, const typename L::Pos &p, Bytes key,
                  std::uint32_t fpq, BlindRef ref, R &res) -> Result {
    constexpr auto kCap = static_cast<std::uint32_t>(L::kCap);
    std::uint32_t meta[kCap + 1];
    std::uint64_t loc[kCap + 1];
    const typename L::Arrays tmp{meta, loc};
    L::copy(tmp, 0, L::arrays(leaf), 0, leaf->count);
    L::insert_into(tmp, leaf->count, p, fpq, ref);
    const auto total = kCap + 1;
    // In order of preference:
    //   1. an ascending stream appending to the leaf starts the right leaf
    //      with the new key, so the left one stays full;
    //   2. an ascending stream about to run into another key family (the
    //      next key shares 8 bytes fewer with the new key than its
    //      predecessor does) ends the left leaf with the new key: the stream
    //      goes on filling leaves of its own instead of carrying the other
    //      family along and leaving a half-full leaf at every split;
    //   3. a descending stream splits after the first entry;
    //   4. otherwise halves. A short ascending run in the middle of a leaf
    //      (every key_N after key_N/10) is no evidence of a stream.
    constexpr std::uint32_t kFamilyGap = 8u << kPosShift;
    const bool ascending = leaf->last_pos != N::kNoLastPos &&
                           p.idx > leaf->last_pos &&
                           p.idx <= leaf->last_pos + 2u;
    std::uint32_t m = total / 2;
    if (ascending && p.idx + 1 < total &&
        (meta[p.idx + 1] >> 12) + kFamilyGap <= (meta[p.idx] >> 12))
      m = p.idx + 1;
    else if (ascending)
      m = std::max(p.idx, total / 2);
    else if (p.idx == 0 && leaf->last_pos == 0)
      m = 1;
    m = std::clamp<std::uint32_t>(m, 1, total - 1);

    auto *left = L::allocate(tag_);
    L::copy(L::arrays(left), 0, tmp, 0, m);
    left->count = m;
    auto *right = L::allocate(tag_);
    L::copy(L::arrays(right), 0, tmp, m, total - m);
    right->count = total - m;
    L::set_crit(L::arrays(right), 0, 0);
    if (p.idx < m)
      left->last_pos = static_cast<std::uint16_t>(p.idx);
    else
      right->last_pos = static_cast<std::uint16_t>(p.idx - m);

    const auto cut = std::size_t{meta[m] >> 12 >> kPosShift} + 1;
    const Bytes first_right =
        m == p.idx ? key : res.key_at(L::ref_of(loc[m]));
    assert(cut <= first_right.size());
    sep_.assign(first_right.begin(),
                first_right.begin() + static_cast<std::ptrdiff_t>(cut));
    discard(leaf);
    return {left, right, true, true};
  }
};

// Lookup shared by the persistent and transient trees.
template <std::size_t LeafBytes, BlindKeyResolver R>
auto find_ref(const N *cur, Bytes key, R &res) -> std::optional<BlindRef> {
  if (!cur)
    return std::nullopt;
  while (!cur->is_leaf)
    cur = cur->child(cur->child_index(key));
  const auto idx = Leaf<LeafBytes>::find(cur, key, fingerprint(key), res);
  if (!idx)
    return std::nullopt;
  return Leaf<LeafBytes>::ref_at(cur, *idx);
}

} // namespace blind_detail

// ---------------------------------------------------------------------------
// BlindBTreeIterator<LeafBytes> — an in-order cursor over one version.
//
// Advancing is in memory. operator* is the record reference; the key is not
// in the tree, so key(resolver) reads it (one read per call).
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> class BlindBTreeIterator {
  using N = blind_detail::N;
  using L = blind_detail::Leaf<LeafBytes>;
  using Bytes = blind_detail::Bytes;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = BlindRef;
  using difference_type = std::ptrdiff_t;

  BlindBTreeIterator() = default;

  auto operator*() const -> BlindRef {
    const auto &f = stack_.back();
    return L::ref_at(f.node, f.idx);
  }
  // Valid until the next call on `res`.
  template <BlindKeyResolver R> [[nodiscard]] auto key(R &res) const -> Bytes {
    return res.key_at(**this);
  }

  auto operator++() -> BlindBTreeIterator & {
    advance();
    return *this;
  }
  // --end() is the last entry; --begin() is end.
  auto operator--() -> BlindBTreeIterator & {
    retreat();
    return *this;
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return stack_.empty();
  }
  // Same position; iterators over different versions never compare equal
  // unless both are at the end.
  auto operator==(const BlindBTreeIterator &o) const noexcept -> bool {
    if (stack_.empty() || o.stack_.empty())
      return stack_.empty() && o.stack_.empty();
    return stack_.back().node == o.stack_.back().node &&
           stack_.back().idx == o.stack_.back().idx;
  }

private:
  struct Frame {
    const N *node;
    std::uint32_t idx;
  };

  PersistentBlindBTree<LeafBytes> tree_; // pin; empty for a transient's
  const N *root_{nullptr};
  std::vector<Frame> stack_;

  enum class Seek { First, Last, End };

  BlindBTreeIterator(PersistentBlindBTree<LeafBytes> tree, const N *root,
                     Seek where)
      : tree_{std::move(tree)}, root_{root} {
    stack_.reserve(8);
    if (!root_ || where == Seek::End)
      return;
    if (where == Seek::First)
      descend_leftmost(root_);
    else
      descend_rightmost(root_);
  }

  // The first entry >= target: one read unless the leaf is empty.
  template <BlindKeyResolver R>
  BlindBTreeIterator(PersistentBlindBTree<LeafBytes> tree, const N *root,
                     Bytes target, R &res)
      : tree_{std::move(tree)}, root_{root} {
    stack_.reserve(8);
    const N *cur = root_;
    while (cur && !cur->is_leaf) {
      const auto idx = cur->child_index(target);
      stack_.push_back({cur, idx});
      cur = cur->child(idx);
    }
    if (!cur || cur->count == 0)
      return;
    const auto p = L::position(cur, target, blind_detail::fingerprint(target),
                               res);
    stack_.push_back({cur, p.idx});
    if (p.idx == cur->count) {
      stack_.back().idx = cur->count - 1;
      advance();
    }
  }

  void descend_leftmost(const N *n) {
    while (!n->is_leaf) {
      stack_.push_back({n, 0});
      n = n->child(0);
    }
    stack_.push_back({n, 0});
  }
  void descend_rightmost(const N *n) {
    while (!n->is_leaf) {
      stack_.push_back({n, n->count});
      n = n->child(n->count);
    }
    stack_.push_back({n, static_cast<std::uint32_t>(n->count - 1)});
  }

  void advance() {
    if (stack_.empty())
      return;
    if (++stack_.back().idx < stack_.back().node->count)
      return;
    stack_.pop_back();
    while (!stack_.empty()) {
      auto &f = stack_.back();
      if (f.idx < f.node->count) {
        ++f.idx;
        descend_leftmost(f.node->child(f.idx));
        return;
      }
      stack_.pop_back();
    }
  }

  void retreat() {
    if (stack_.empty()) {
      if (root_)
        descend_rightmost(root_);
      return;
    }
    if (stack_.back().idx > 0) {
      --stack_.back().idx;
      return;
    }
    stack_.pop_back();
    while (!stack_.empty()) {
      auto &f = stack_.back();
      if (f.idx > 0) {
        --f.idx;
        descend_rightmost(f.node->child(f.idx));
        return;
      }
      stack_.pop_back();
    }
  }

  friend class PersistentBlindBTree<LeafBytes>;
  friend class TransientBlindBTree<LeafBytes>;
};

// ---------------------------------------------------------------------------
// PersistentBlindBTree<LeafBytes> — a handle to one immutable version: a
// root pointer, a size and a pin, as PersistentBTree. Every operation that
// needs a key's bytes takes the resolver as an argument; the tree never
// stores one.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> class PersistentBlindBTree {
  using N = blind_detail::N;
  using L = blind_detail::Leaf<LeafBytes>;
  using Bytes = blind_detail::Bytes;
  using Chain = VersionChain<btree_detail::ChainTraits<BlindRef>>;

public:
  static constexpr std::size_t kLeafEntries = L::kCap;

  PersistentBlindBTree() = default;
  PersistentBlindBTree(const PersistentBlindBTree &other)
      : root_{other.root_}, size_{other.size_}, version_{other.version_} {
    if (version_)
      chain().pin(version_);
  }
  auto operator=(const PersistentBlindBTree &other) -> PersistentBlindBTree & {
    if (this == &other)
      return *this;
    if (other.version_)
      chain().pin(other.version_);
    release();
    root_ = other.root_;
    size_ = other.size_;
    version_ = other.version_;
    return *this;
  }
  PersistentBlindBTree(PersistentBlindBTree &&other) noexcept
      : root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        version_{std::exchange(other.version_, 0)} {}
  auto operator=(PersistentBlindBTree &&other) noexcept
      -> PersistentBlindBTree & {
    if (this == &other)
      return *this;
    release();
    root_ = std::exchange(other.root_, nullptr);
    size_ = std::exchange(other.size_, 0);
    version_ = std::exchange(other.version_, 0);
    return *this;
  }
  ~PersistentBlindBTree() { release(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }

  // A present key costs one read; an absent key almost never reads.
  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    return blind_detail::find_ref<LeafBytes>(root_, key, res);
  }
  template <BlindKeyResolver R>
  [[nodiscard]] auto contains(Bytes key, R &res) const -> bool {
    return get(key, res).has_value();
  }

  template <BlindKeyResolver R>
  [[nodiscard]] auto set(Bytes key, BlindRef ref, R &res) const
      -> PersistentBlindBTree;
  template <BlindKeyResolver R>
  [[nodiscard]] auto erase(Bytes key, R &res) const -> PersistentBlindBTree;
  [[nodiscard]] auto transient() const -> TransientBlindBTree<LeafBytes>;

  [[nodiscard]] auto begin() const -> BlindBTreeIterator<LeafBytes> {
    return {*this, root_, BlindBTreeIterator<LeafBytes>::Seek::First};
  }
  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }
  [[nodiscard]] auto last() const -> BlindBTreeIterator<LeafBytes> {
    return {*this, root_, BlindBTreeIterator<LeafBytes>::Seek::Last};
  }
  // Past the last entry; -- from here is the last entry.
  [[nodiscard]] auto end_iter() const -> BlindBTreeIterator<LeafBytes> {
    return {*this, root_, BlindBTreeIterator<LeafBytes>::Seek::End};
  }
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &res) const
      -> BlindBTreeIterator<LeafBytes> {
    return {*this, root_, key, res};
  }

  // -- Test and debug support --------------------------------------------

  [[nodiscard]] static auto parked_nodes() -> std::vector<const void *> {
    return chain().parked_nodes();
  }
  // Live versions and parked retired nodes, for stats().
  [[nodiscard]] static auto reclamation_gauges() { return chain().gauges(); }
  // Calls f(const void*) for every node reachable from this version.
  template <typename F> void visit_nodes(F &&f) const {
    std::vector<const N *> stack;
    if (root_)
      stack.push_back(root_);
    while (!stack.empty()) {
      const auto *n = stack.back();
      stack.pop_back();
      f(static_cast<const void *>(n));
      n->for_each_child([&](N *c) { stack.push_back(c); });
    }
  }

  // Shape statistics. used_bytes counts a leaf as its header plus its
  // entries, so used/capacity is the leaf fill.
  [[nodiscard]] auto stats() const -> BTreeStats {
    BTreeStats st;
    std::vector<std::pair<const N *, std::size_t>> stack;
    if (root_)
      stack.emplace_back(root_, 1);
    while (!stack.empty()) {
      const auto [n, depth] = stack.back();
      stack.pop_back();
      ++st.nodes;
      st.capacity_bytes += n->capacity;
      st.height = std::max(st.height, depth);
      if (n->is_leaf) {
        ++st.leaves;
        st.entries += n->count;
        st.used_bytes += L::kHeader + 16 * std::size_t{n->count};
        st.leaf_counts.push_back(n->count);
        continue;
      }
      st.used_bytes += n->capacity - n->free_bytes();
      st.dead_bytes += n->dead_bytes;
      n->for_each_child([&](N *c) { stack.emplace_back(c, depth + 1); });
    }
    return st;
  }

  // Checks every structural invariant, reading every key through `res`:
  // order, the crit bit and fingerprint of every entry, the separators'
  // bounds, leaf depth and the size. Throws std::logic_error on the first
  // violation. Returns the height (0 for an empty tree).
  template <BlindKeyResolver R> auto validate(R &res) const -> std::size_t {
    if (!root_)
      return 0;
    std::size_t leaf_depth = 0;
    std::size_t keys = 0;
    validate_node(root_, 1, nullptr, nullptr, res, leaf_depth, keys);
    if (keys != size_)
      throw std::logic_error{"blind btree: size does not match key count"};
    return leaf_depth;
  }

private:
  N *root_{nullptr};
  std::size_t size_{0};
  std::uint64_t version_{0};

  PersistentBlindBTree(N *root, std::size_t size, std::uint64_t version) noexcept
      : root_{root}, size_{size}, version_{version} {}

  static auto chain() -> Chain & { return Chain::instance(); }

  void release() noexcept {
    if (version_)
      chain().unpin(version_, root_);
    root_ = nullptr;
    size_ = 0;
    version_ = 0;
  }

  template <BlindKeyResolver R>
  static void validate_node(const N *n, std::size_t depth,
                            const std::vector<std::byte> *lo,
                            const std::vector<std::byte> *hi, R &res,
                            std::size_t &leaf_depth, std::size_t &keys) {
    auto fail = [] [[noreturn]] (const char *what) {
      throw std::logic_error{std::string{"blind btree: "} + what};
    };
    auto in_bounds = [&](Bytes k) {
      if (lo && btree_detail::compare_bytes(k, Bytes{*lo}) < 0)
        fail("key below lower bound");
      if (hi && btree_detail::compare_bytes(k, Bytes{*hi}) >= 0)
        fail("key at or above upper bound");
    };
    if (n->is_leaf) {
      if (n->count == 0 || n->count > L::kCap)
        fail("leaf count out of range");
      std::vector<std::byte> prev;
      for (std::uint32_t i = 0; i < n->count; ++i) {
        const Bytes k = res.key_at(L::ref_at(n, i));
        in_bounds(k);
        if (L::fp_at(n, i) != blind_detail::fingerprint(k))
          fail("fingerprint does not match key");
        if (i > 0) {
          if (btree_detail::compare_bytes(k, Bytes{prev}) <= 0)
            fail("entries out of order");
          if (L::crit_at(n, i) != blind_detail::crit(Bytes{prev}, k))
            fail("crit bit does not match neighbours");
        }
        prev.assign(k.begin(), k.end());
      }
      if (leaf_depth == 0)
        leaf_depth = depth;
      else if (leaf_depth != depth)
        fail("leaves at different depths");
      keys += n->count;
      return;
    }
    if (n->first_child == nullptr)
      fail("inner node without children");
    std::vector<std::vector<std::byte>> seps(n->count);
    for (std::uint32_t i = 0; i < n->count; ++i) {
      const auto k = n->key(i);
      seps[i].resize(k.size());
      k.copy_tail(0, seps[i].data());
      in_bounds(Bytes{seps[i]});
      if (i > 0 &&
          btree_detail::compare_bytes(Bytes{seps[i]}, Bytes{seps[i - 1]}) <= 0)
        fail("separators out of order");
    }
    for (std::uint32_t c = 0; c <= n->count; ++c)
      validate_node(n->child(c), depth + 1, c > 0 ? &seps[c - 1] : lo,
                    c < n->count ? &seps[c] : hi, res, leaf_depth, keys);
  }

  friend class TransientBlindBTree<LeafBytes>;
  friend class BlindBTreeIterator<LeafBytes>;
  friend class BlindBulkLoader<LeafBytes>;
};

// ---------------------------------------------------------------------------
// TransientBlindBTree<LeafBytes> — a mutable working copy of one version,
// as TransientBTree. Every write reads one key (the candidate's), plus one
// more when a leaf splits and the right half starts with an existing key.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes> class TransientBlindBTree {
  using N = blind_detail::N;
  using Bytes = blind_detail::Bytes;

public:
  TransientBlindBTree(const TransientBlindBTree &) = delete;
  auto operator=(const TransientBlindBTree &) -> TransientBlindBTree & = delete;
  TransientBlindBTree(TransientBlindBTree &&other) noexcept
      : base_{std::move(other.base_)}, session_{std::move(other.session_)},
        root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        changed_{std::exchange(other.changed_, false)} {}
  auto operator=(TransientBlindBTree &&other) noexcept -> TransientBlindBTree & {
    if (this != &other) {
      discard();
      base_ = std::move(other.base_);
      session_ = std::move(other.session_);
      root_ = std::exchange(other.root_, nullptr);
      size_ = std::exchange(other.size_, 0);
      changed_ = std::exchange(other.changed_, false);
    }
    return *this;
  }
  ~TransientBlindBTree() { discard(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }

  template <BlindKeyResolver R>
  [[nodiscard]] auto get(Bytes key, R &res) const -> std::optional<BlindRef> {
    ensure_active();
    return blind_detail::find_ref<LeafBytes>(root_, key, res);
  }

  template <BlindKeyResolver R> void set(Bytes key, BlindRef ref, R &res) {
    auto always = [](const BlindRef &, const BlindRef &) { return true; };
    (void)upsert(key, ref, res, always);
  }

  // Inserts if absent, replaces if should_replace(existing, incoming).
  // Returns the displaced reference.
  template <BlindKeyResolver R, typename Pred>
  auto upsert(Bytes key, BlindRef ref, R &res, Pred &&should_replace)
      -> std::optional<BlindRef> {
    ensure_active();
    blind_detail::check_ref(key, ref);
    const auto r = session_.upsert(root_, key, ref, res, should_replace);
    if (!r.changed)
      return std::nullopt;
    changed_ = true;
    root_ = r.right ? session_.make_root(r.node, r.right) : r.node;
    if (r.inserted)
      ++size_;
    return session_.take_displaced();
  }

  // Returns the erased reference.
  template <BlindKeyResolver R>
  auto erase(Bytes key, R &res) -> std::optional<BlindRef> {
    ensure_active();
    const auto r = session_.erase(root_, key, res);
    if (!r.changed)
      return std::nullopt;
    changed_ = true;
    root_ = r.node;
    while (root_ && !root_->is_leaf && root_->count == 0) {
      auto *old = root_;
      root_ = old->first_child;
      session_.discard(old);
    }
    --size_;
    return session_.take_displaced();
  }

  [[nodiscard]] auto persistent() && -> PersistentBlindBTree<LeafBytes> {
    ensure_active();
    if (!changed_) {
      session_.finish();
      root_ = nullptr;
      size_ = 0;
      return std::move(base_);
    }
    PersistentBlindBTree<LeafBytes>::chain().publish(
        session_.tag(), base_.version_, session_.retired_list());
    const auto version = session_.tag();
    session_.finish();
    auto live_size = std::exchange(size_, 0);
    auto *root = std::exchange(root_, nullptr);
    base_ = PersistentBlindBTree<LeafBytes>{};
    return PersistentBlindBTree<LeafBytes>{root, live_size, version};
  }

  // The iterator holds raw node pointers: do not mutate while it is alive.
  template <BlindKeyResolver R>
  [[nodiscard]] auto lower_bound(Bytes key, R &res) const
      -> BlindBTreeIterator<LeafBytes> {
    ensure_active();
    return {PersistentBlindBTree<LeafBytes>{}, root_, key, res};
  }

private:
  PersistentBlindBTree<LeafBytes> base_;
  blind_detail::BlindSession<LeafBytes> session_;
  N *root_{nullptr};
  std::size_t size_{0};
  bool changed_{false};

  explicit TransientBlindBTree(const PersistentBlindBTree<LeafBytes> &base)
      : base_{base}, root_{base.root_}, size_{base.size_} {}

  void ensure_active() const {
    if (session_.tag() == 0) [[unlikely]]
      throw std::logic_error{"TransientBlindBTree already consumed"};
  }

  void discard() noexcept {
    session_.discard_all(root_);
    root_ = nullptr;
    size_ = 0;
  }

  friend class PersistentBlindBTree<LeafBytes>;
};

template <std::size_t LeafBytes>
auto PersistentBlindBTree<LeafBytes>::transient() const
    -> TransientBlindBTree<LeafBytes> {
  return TransientBlindBTree<LeafBytes>{*this};
}

template <std::size_t LeafBytes>
template <BlindKeyResolver R>
auto PersistentBlindBTree<LeafBytes>::set(Bytes key, BlindRef ref, R &res) const
    -> PersistentBlindBTree {
  auto t = transient();
  t.set(key, ref, res);
  return std::move(t).persistent();
}

template <std::size_t LeafBytes>
template <BlindKeyResolver R>
auto PersistentBlindBTree<LeafBytes>::erase(Bytes key, R &res) const
    -> PersistentBlindBTree {
  if (!root_)
    return *this;
  auto t = transient();
  if (!t.erase(key, res))
    return *this;
  return std::move(t).persistent();
}

// ---------------------------------------------------------------------------
// BlindBulkLoader<LeafBytes> — builds a tree from keys appended in strictly
// ascending order. Every key's bytes are in hand, so crit bits, fingerprints
// and separators come from the stream without a single read. Leaves are
// filled to capacity; the levels above them are built by the B+ tree's
// loader.
// ---------------------------------------------------------------------------
export template <std::size_t LeafBytes>
class BlindBulkLoader : private btree_detail::BulkLoader<BlindRef> {
  using Base = btree_detail::BulkLoader<BlindRef>;
  using N = blind_detail::N;
  using L = blind_detail::Leaf<LeafBytes>;
  using Bytes = blind_detail::Bytes;

public:
  BlindBulkLoader() = default;
  BlindBulkLoader(const BlindBulkLoader &) = delete;
  auto operator=(const BlindBulkLoader &) -> BlindBulkLoader & = delete;
  ~BlindBulkLoader() {
    if (leaf_)
      N::destroy(leaf_);
  }

  // Throws std::invalid_argument if `key` does not sort after the previous.
  void append(Bytes key, BlindRef ref) {
    blind_detail::check_ref(key, ref);
    std::uint32_t crit_bit = 0;
    if (this->size_ > 0) {
      const auto c = blind_detail::crit(Bytes{prev_}, key);
      if (c == blind_detail::kNoCrit || blind_detail::bit(key, c) == 0)
        throw std::invalid_argument{"BlindBulkLoader: keys not ascending"};
      crit_bit = c;
    }
    if (leaf_ && leaf_->count == L::kCap)
      seal_leaf();
    if (!leaf_) {
      leaf_ = L::allocate(this->session_.tag());
      if (this->size_ > 0)
        sep_ = Base::separator(Bytes{prev_}, key);
      crit_bit = 0;
    }
    L::write(L::arrays(leaf_), leaf_->count, crit_bit,
             blind_detail::fingerprint(key), ref);
    ++leaf_->count;
    ++this->size_;
    prev_.assign(key.begin(), key.end());
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return this->size_; }

  [[nodiscard]] auto finish() && -> PersistentBlindBTree<LeafBytes> {
    seal_leaf();
    const auto tag = this->session_.tag();
    const auto size = this->size_;
    auto *root = std::move(*this).assemble_root(tag);
    if (!root)
      return {};
    return PersistentBlindBTree<LeafBytes>{root, size, tag};
  }

private:
  N *leaf_{nullptr};
  std::vector<std::byte> prev_; // the last key appended
  std::vector<std::byte> sep_;  // separator before the leaf being filled

  void seal_leaf() {
    if (!leaf_)
      return;
    this->add_child(0, leaf_, sep_);
    leaf_ = nullptr;
    sep_.clear();
  }
};

} // namespace bytecask
