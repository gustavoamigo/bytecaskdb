// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — internal engine types shared across implementation units

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

export module bytecask:internals;

import bytecask.blind_btree;
import bytecask.btree;
import bytecask.buffer_pool;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.radix_tree;
import bytecask.types;
import bytecask.u32_map;
import bytecask.util;

namespace bytecask {

// Forward-declared in the primary interface; defined here so the DB
// class definition (in bytecask.cppm) can use them as member types.

// ---------------------------------------------------------------------------
// FileStats — per-file byte counters for vacuum file selection.
// Updated under write_mu_ on every write; rebuilt during recovery.
// tombstone_bytes counts Delete and RangeDel entries, marker_bytes the
// BulkBegin/BulkEnd markers. Neither is ever live, and neither is what vacuum
// selects a file for: compaction keeps every marker, and every tombstone that
// may be all that stands between an older file's Put and its resurrection at
// recovery (see NeededTombstones).
// Exported only in BYTECASK_TESTING builds so the public API stays minimal.
// ---------------------------------------------------------------------------
#ifdef BYTECASK_TESTING
export struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
  std::uint64_t tombstone_bytes{0};
  std::uint64_t marker_bytes{0};

  // Dead Put bytes: what compacting this file is sure to reclaim. Vacuum
  // selects files by it, so a file that is mostly tombstones or markers is
  // not picked again and again for a compaction that cannot shrink it.
  [[nodiscard]] auto reclaimable_bytes() const noexcept -> std::uint64_t {
    const auto kept = live_bytes + tombstone_bytes + marker_bytes;
    return total_bytes > kept ? total_bytes - kept : 0;
  }
};
#else
struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
  std::uint64_t tombstone_bytes{0};
  std::uint64_t marker_bytes{0};

  // Dead Put bytes: what compacting this file is sure to reclaim. Vacuum
  // selects files by it, so a file that is mostly tombstones or markers is
  // not picked again and again for a compaction that cannot shrink it.
  [[nodiscard]] auto reclaimable_bytes() const noexcept -> std::uint64_t {
    const auto kept = live_bytes + tombstone_bytes + marker_bytes;
    return total_bytes > kept ? total_bytes - kept : 0;
  }
};
#endif

// File registry: a COW map from file_id to shared DataFile.
// PersistentU32Map provides O(1) snapshot sharing; rotation and vacuum
// fork a transient, mutate it, and freeze — no O(N) map clone.

// ---------------------------------------------------------------------------
// KeyDirEntry — one slot in the in-memory key directory.
//
// Bit-packed into two 64-bit words to reduce per-node radix tree overhead.
// Access is through accessor methods so the internal layout can be changed
// without touching call sites.
//
// Layout (variant 3b — split file_id):
//   word0 [63:16] sequence     (48 bits, max 281 T)
//         [15: 0] file_id_low  (16 bits)
//   word1 [63:32] file_offset  (32 bits, max 4 GiB)
//         [31: 4] value_size   (28 bits, max 256 MiB)
//         [ 3: 0] file_id_high ( 4 bits)
//
// file_id = (file_id_high << 16) | file_id_low — effective 20 bits (max 1 048 575).
//
// file_id is a monotonic integer handle, assigned by DB, that indexes
// into the engine's file registry. file_offset is the byte offset where
// the full DataEntry begins.
// ---------------------------------------------------------------------------
export struct KeyDirEntry {
  std::uint64_t word0_{};
  std::uint64_t word1_{};

  // Field limits — checked at construction time.
  static constexpr std::uint64_t kMaxSequence  = (std::uint64_t{1} << 48) - 1;
  static constexpr std::uint32_t kMaxFileId    = (std::uint32_t{1} << 20) - 1;
  static constexpr std::uint64_t kMaxFileOffset = (std::uint64_t{1} << 32) - 1;
  static constexpr std::uint32_t kMaxValueSize = (std::uint32_t{1} << 28) - 1;

  // Limit-checking helpers — usable both from make() and from call sites
  // that validate individual fields before construction (e.g. file rotation).
  static void check_sequence(std::uint64_t v) {
    if (v > kMaxSequence)
      throw std::runtime_error("sequence " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxSequence));
  }
  static void check_file_id(std::uint32_t v) {
    if (v > kMaxFileId)
      throw std::runtime_error("file_id " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxFileId));
  }
  static void check_file_offset(std::uint64_t v) {
    if (v > kMaxFileOffset)
      throw std::runtime_error("file_offset " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxFileOffset));
  }
  static void check_value_size(std::uint32_t v) {
    if (v > kMaxValueSize)
      throw std::runtime_error("value_size " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxValueSize));
  }

  static auto make(std::uint64_t sequence, std::uint64_t file_offset,
                   std::uint32_t file_id, std::uint32_t value_size)
      -> KeyDirEntry {
    check_sequence(sequence);
    check_file_id(file_id);
    check_file_offset(file_offset);
    check_value_size(value_size);
    KeyDirEntry e;
    e.word0_ = (sequence << 16) | (file_id & 0xFFFFu);
    e.word1_ = (file_offset << 32) |
               (static_cast<std::uint64_t>(value_size) << 4) |
               (file_id >> 16);
    return e;
  }

  [[nodiscard]] auto sequence()    const -> std::uint64_t { return word0_ >> 16; }
  [[nodiscard]] auto file_id()     const -> std::uint32_t {
    auto low  = static_cast<std::uint32_t>(word0_ & 0xFFFFu);
    auto high = static_cast<std::uint32_t>(word1_ & 0xFu);
    return (high << 16) | low;
  }
  [[nodiscard]] auto file_offset() const -> std::uint64_t { return word1_ >> 32; }
  [[nodiscard]] auto value_size()  const -> std::uint32_t { return static_cast<std::uint32_t>((word1_ >> 4) & 0xFFF'FFFFu); }
};
static_assert(sizeof(KeyDirEntry) == 16);

// Two data files hold entries under the same sequence number. Recovery throws
// this to stop at the first sign of it; DB::recovery_open catches it and
// decides whether the pair is what an interrupted vacuum leaves.
export class SequenceOverlap : public std::runtime_error {
public:
  SequenceOverlap(std::uint32_t a, std::uint32_t b)
      : std::runtime_error{"bytecask: corrupt database — two data files hold "
                           "entries under the same sequence number"},
        file_a{a}, file_b{b} {}
  SequenceOverlap(const SequenceOverlap &) = default;
  auto operator=(const SequenceOverlap &) -> SequenceOverlap & = default;
  ~SequenceOverlap() override;

  std::uint32_t file_a;
  std::uint32_t file_b;
};

SequenceOverlap::~SequenceOverlap() = default;

// Canonical key-ownership comparator. Returns true if `a` is strictly newer
// than `b`. Sequence numbers are unique per logical write, so equal sequences
// must point to the same physical record. Two files under one sequence throw
// SequenceOverlap; two offsets in one file are corruption outright.
export inline auto kde_newer(const KeyDirEntry &a, const KeyDirEntry &b) -> bool {
  if (a.sequence() != b.sequence()) return a.sequence() > b.sequence();
  if (a.file_id() != b.file_id()) {
    throw SequenceOverlap{a.file_id(), b.file_id()};
  }
  if (a.file_offset() != b.file_offset()) {
    throw std::runtime_error(
        "bytecask: corrupt database — two entries share the same sequence "
        "number but differ in physical location");
  }
  return false; // identical record
}

// Returns the on-disk size of a data file entry given key and value sizes.
export inline constexpr auto entry_size(std::size_t key_size,
                                        std::size_t value_size)
    -> std::uint64_t {
  return kHeaderSize + key_size + value_size + kCrcSize;
}

// The bytes an entry adds to FileStats::tombstone_bytes: its full size for a
// Delete or RangeDel (whose value is the range's end key), 0 otherwise.
export inline constexpr auto tombstone_size(EntryType type,
                                            std::size_t key_size,
                                            std::size_t value_size)
    -> std::uint64_t {
  switch (type) {
  case EntryType::Delete:
  case EntryType::RangeDel:
    return entry_size(key_size, value_size);
  case EntryType::Put:
  case EntryType::BulkBegin:
  case EntryType::BulkEnd:
    return 0;
  }
  return 0;
}

// The bytes an entry adds to FileStats::marker_bytes: a header and a CRC for
// a BulkBegin or BulkEnd marker, 0 otherwise.
export inline constexpr auto marker_size(EntryType type) -> std::uint64_t {
  switch (type) {
  case EntryType::BulkBegin:
  case EntryType::BulkEnd:
    return entry_size(0, 0);
  case EntryType::Put:
  case EntryType::Delete:
  case EntryType::RangeDel:
    return 0;
  }
  return 0;
}

// Forward declaration — defined in bytecask.cppm (primary interface).
export class TransientEngineState;

// ---------------------------------------------------------------------------
// Records the batch being built has placed in the active file but not yet
// written: phase 1 of a commit (validate and apply) runs before the batch's
// pwritev, so a key directory that reads keys back from the data files must
// find these in memory. Keyed by file_id << 32 | offset.
// ---------------------------------------------------------------------------
export struct PendingRecord {
  std::uint64_t sequence{0};
  std::uint32_t value_size{0};
  std::vector<std::byte> key;
};
export using PendingRecords = std::unordered_map<std::uint64_t, PendingRecord>;

export inline constexpr auto pending_slot(std::uint32_t file_id,
                                          std::uint64_t offset) noexcept
    -> std::uint64_t {
  return (std::uint64_t{file_id} << 32) | offset;
}

// ---------------------------------------------------------------------------
// KeyDirCtx — where a key directory that does not store keys reads them: the
// file registry of the version it belongs to (a published state's, or the
// writer's transient one plus the records it has not written yet). The B+
// tree and the radix tree ignore it. Non-owning; valid while the state it
// points into is.
// ---------------------------------------------------------------------------
export struct KeyDirCtx {
  const PersistentU32Map<std::shared_ptr<DataFile>> *files{nullptr};
  const TransientU32Map<std::shared_ptr<DataFile>> *writer_files{nullptr};
  const PendingRecords *pending{nullptr};
  bool verify{true};

  [[nodiscard]] auto file(std::uint32_t id) const -> const DataFile * {
    const auto *f = files ? files->get(id) : writer_files->get(id);
    return f ? f->get() : nullptr;
  }
};

// ---------------------------------------------------------------------------
// The key directory. The engine names only these types and the kd_*
// functions below, which speak its terms: a lookup returns a KeyDirEntry, a
// put or erase returns what it displaced (a KeyDirHit: file, offset and value
// size), iterators yield (key, KeyDirEntry) or a KeyDirHit.
//
// The B+ tree (docs/persistent_btree_design.md) is the default;
// BYTECASK_KEYDIR=radix builds the engine on the radix tree and
// BYTECASK_KEYDIR=blind on the blind-leaf tree
// (docs/blind_leaf_btree_design.md), which stores no key bytes and reads them
// back through the KeyDirCtx.
//
// Recovery builds a RecoveryKeyDirTree: the B+ tree in the blind build too,
// converted once by key_dir_from_recovered() until recovery can build a
// blind tree from the hint files directly.
// ---------------------------------------------------------------------------
#if defined(BYTECASK_KEYDIR_BLIND)
export inline constexpr bool kKeyDirReadsKeys = true;
#else
export inline constexpr bool kKeyDirReadsKeys = false;
#endif

#ifdef BYTECASK_USE_BTREE
export using RecoveryKeyDirTree = PersistentBTree<KeyDirEntry>;
export using RecoveryKeyDirIter = BTreeIterator<KeyDirEntry>;
// Bulk build of a key directory from ascending keys, and the concatenation
// of the slices several threads built — what recovery_load_ranged uses.
export using KeyDirBulkLoader = btree_detail::BulkLoader<KeyDirEntry>;
export using KeyDirLeafRun = btree_detail::LeafRun<KeyDirEntry>;
#else
export using RecoveryKeyDirTree = PersistentRadixTree<KeyDirEntry>;
export using RecoveryKeyDirIter = RadixTreeIterator<KeyDirEntry>;
#endif

#if !defined(BYTECASK_KEYDIR_BLIND)

#ifdef BYTECASK_USE_BTREE
export using KeyDirTree = PersistentBTree<KeyDirEntry>;
export using KeyDirTransient = TransientBTree<KeyDirEntry>;
export using KeyDirIter = BTreeIterator<KeyDirEntry>;
export using KeyDirValueIter = BTreeValueIterator<KeyDirEntry>;
export using KeyDirReverseValueIter = ReverseBTreeValueIterator<KeyDirEntry>;
#else
export using KeyDirTree = PersistentRadixTree<KeyDirEntry>;
export using KeyDirTransient = TransientRadixTree<KeyDirEntry>;
export using KeyDirIter = RadixTreeIterator<KeyDirEntry>;
export using KeyDirValueIter = ValueIterator<KeyDirEntry>;
export using KeyDirReverseValueIter = ReverseValueIterator<KeyDirEntry>;
#endif
export using KeyDirHit = KeyDirEntry;

// The value size a value iterator's entry expects, as a read hint.
export inline auto value_size_hint(const KeyDirEntry &e) noexcept
    -> std::uint32_t {
  return e.value_size();
}

// The trees that hold keys and full entries: every function forwards and the
// context goes unused.
export template <typename T>
auto kd_get(const T &t, std::span<const std::byte> key, const KeyDirCtx &)
    -> std::optional<KeyDirEntry> {
  return t.get(key);
}
export template <typename T>
auto kd_contains(const T &t, std::span<const std::byte> key, const KeyDirCtx &)
    -> bool {
  return t.contains(key);
}
// Inserts or replaces; returns the entry replaced. One descent.
export inline auto kd_put(KeyDirTransient &t, std::span<const std::byte> key,
                          const KeyDirEntry &e, const KeyDirCtx &)
    -> std::optional<KeyDirHit> {
  return t.upsert(key, e, [](const KeyDirEntry &, const KeyDirEntry &) {
    return true;
  });
}
export inline auto kd_erase(KeyDirTransient &t, std::span<const std::byte> key,
                            const KeyDirCtx &) -> std::optional<KeyDirHit> {
  auto e = t.get(key);
  if (e)
    (void)t.erase(key);
  return e;
}
export template <typename T>
auto kd_lower_bound(const T &t, std::span<const std::byte> key,
                    const KeyDirCtx &) {
  return t.lower_bound(key);
}
export inline auto kd_begin(const KeyDirTree &t, const KeyDirCtx &) -> KeyDirIter {
  return t.begin();
}
// Past the last key; -- from it is the last key.
export inline auto kd_end(const KeyDirTree &t, const KeyDirCtx &) -> KeyDirIter {
  return t.rbegin().base();
}
export inline auto kd_upper_bound(const KeyDirTree &t,
                                  std::span<const std::byte> key,
                                  const KeyDirCtx &) -> KeyDirIter {
  return t.upper_bound(key);
}
export inline auto kd_value_lower_bound(const KeyDirTree &t,
                                        std::span<const std::byte> from,
                                        const KeyDirCtx &) -> KeyDirValueIter {
  return from.empty() ? t.value_begin() : t.value_lower_bound(from);
}
// Reverse from the last key <= from, or from the last key if from is empty.
export inline auto kd_value_rlower_bound(const KeyDirTree &t,
                                         std::span<const std::byte> from,
                                         const KeyDirCtx &)
    -> KeyDirReverseValueIter {
  return from.empty() ? t.value_rbegin() : t.value_rlower_bound(from);
}
export inline auto key_dir_from_recovered(RecoveryKeyDirTree t) -> KeyDirTree {
  return t;
}

#else // BYTECASK_KEYDIR_BLIND

// Recovery loads leaves between 60% and 100% full, spread so they do not all
// reach capacity at once. Loaded full, every leaf splits within the first few
// percent of random inserts after open: a burst of allocation on the write
// path and a memory peak near twice the loaded size. Keys written in order
// never refill the slack, so they keep ~26% more than a full load.
export inline constexpr double kBlindRecoveryFillMin = 0.6;
export inline constexpr double kBlindRecoveryFillMax = 1.0;
export using KeyDirTree = PersistentBlindBTree<kBlindLeafBytes>;
export using KeyDirTransient = TransientBlindBTree<kBlindLeafBytes>;

// What a blind key directory holds for a key: where its record is. Its sizes
// and sequence are in the record's header.
export struct KeyDirLoc {
  BlindRef ref;
  [[nodiscard]] auto file_id() const noexcept -> std::uint32_t {
    return ref.file_id;
  }
  [[nodiscard]] auto file_offset() const noexcept -> std::uint64_t {
    return ref.offset;
  }
};
// A blind value iterator knows no size: the read takes it from the header.
export inline auto value_size_hint(const KeyDirLoc &) noexcept
    -> std::uint32_t {
  return 0;
}

// What a put or an erase displaced: the record, and its value size, read
// from its header when the operation confirmed the key.
export struct KeyDirHit {
  BlindRef ref;
  std::uint32_t size;
  [[nodiscard]] auto file_id() const noexcept -> std::uint32_t {
    return ref.file_id;
  }
  [[nodiscard]] auto file_offset() const noexcept -> std::uint64_t {
    return ref.offset;
  }
  [[nodiscard]] auto value_size() const noexcept -> std::uint32_t {
    return size;
  }
};

inline auto to_blind_ref(const KeyDirEntry &e) -> BlindRef {
  return {e.file_id(), static_cast<std::uint32_t>(e.file_offset())};
}

// Resolves a record to its key: from the batch being built if it is there,
// otherwise through DataFile::lend_record (one pool lookup; CRC-checked
// unless ctx.verify is off). Keeps the entry's sequence and value from the
// last read, so a lookup that confirms a key has what a KeyDirEntry or a get
// needs without a second read. Spans are valid until the next key_at, or
// until `buf` or `lease` changes.
export struct KeyReader {
  const KeyDirCtx &ctx;
  std::vector<std::byte> &buf;
  FrameLease &lease;
  // Of the last record read.
  BlindRef last{};
  std::uint64_t sequence{0};
  std::uint32_t value_size{0};
  std::span<const std::byte> value; // empty for a record not yet written

  auto key_at(BlindRef r) -> std::span<const std::byte> {
    last = r;
    if (ctx.pending) {
      if (auto it = ctx.pending->find(pending_slot(r.file_id, r.offset));
          it != ctx.pending->end()) {
        sequence = it->second.sequence;
        value_size = it->second.value_size;
        value = {};
        return it->second.key;
      }
    }
    const auto *f = ctx.file(r.file_id);
    if (!f)
      throw std::logic_error{
          "key directory references a data file missing from the registry"};
    const auto v = f->lend_record(r.offset, 0, ctx.verify, buf, lease);
    if (v.entry_type != EntryType::Put)
      throw std::runtime_error{
          "bytecask: corrupt database — key directory points at an entry "
          "that is not a put"};
    sequence = v.sequence;
    value_size = static_cast<std::uint32_t>(v.value.size());
    value = v.value;
    return v.key;
  }

  // The size of `ref`'s value, which must be the record read last — as it
  // is after a put or an erase that confirmed the key.
  [[nodiscard]] auto displaced(BlindRef ref) const -> KeyDirHit {
    if (!(ref == last))
      throw std::logic_error{
          "key directory: displaced record was not the one last read"};
    return {ref, value_size};
  }
};

// Scratch buffer for the reads of one call, reused per thread.
inline auto key_read_buffer() -> std::vector<std::byte> & {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local std::vector<std::byte> buf;
#pragma clang diagnostic pop
  return buf;
}

// ---------------------------------------------------------------------------
// BlindKeyDirIter<Keyed> — the engine's iterator over a blind key directory.
// A keyed iterator yields (key, KeyDirEntry) and reads the record on every
// dereference; a value iterator yields a KeyDirHit and reads nothing. An
// iterator over a published state keeps its own handle on that state's file
// registry, so it can outlive the call that made it (keys_from). The key
// span lives until the next dereference or advance.
// ---------------------------------------------------------------------------
export template <bool Keyed> class BlindKeyDirIter {
  using Inner = BlindBTreeIterator<kBlindLeafBytes>;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using difference_type = std::ptrdiff_t;
  using value_type =
      std::conditional_t<Keyed,
                         std::pair<std::span<const std::byte>, KeyDirEntry>,
                         KeyDirLoc>;

  BlindKeyDirIter() = default;
  BlindKeyDirIter(Inner cur, const KeyDirCtx &ctx) : cur_{std::move(cur)} {
    if (ctx.files) {
      files_ = *ctx.files; // O(1) handle copy: pins the registry
      owned_ = true;
    }
    ctx_ = ctx;
    ctx_.files = nullptr;
  }

  auto operator*() const -> value_type {
    const auto ref = *cur_;
    if constexpr (Keyed) {
      const auto ctx = context();
      KeyReader reader{ctx, buf_, lease_};
      const auto key = reader.key_at(ref);
      return {key, KeyDirEntry::make(reader.sequence, ref.offset, ref.file_id,
                                     reader.value_size)};
    } else {
      return KeyDirLoc{ref};
    }
  }
  // The iterator's key without the rest (one read).
  [[nodiscard]] auto key() const -> std::span<const std::byte> {
    const auto ctx = context();
    KeyReader reader{ctx, buf_, lease_};
    return reader.key_at(*cur_);
  }

  // A copy holds no lease: its first dereference reads again.
  BlindKeyDirIter(const BlindKeyDirIter &o)
      : cur_{o.cur_}, files_{o.files_}, owned_{o.owned_}, ctx_{o.ctx_} {}
  auto operator=(const BlindKeyDirIter &o) -> BlindKeyDirIter & {
    if (this != &o) {
      lease_.reset();
      cur_ = o.cur_;
      files_ = o.files_;
      owned_ = o.owned_;
      ctx_ = o.ctx_;
    }
    return *this;
  }
  BlindKeyDirIter(BlindKeyDirIter &&) noexcept = default;
  auto operator=(BlindKeyDirIter &&) noexcept -> BlindKeyDirIter & = default;
  ~BlindKeyDirIter() = default;

  auto operator++() -> BlindKeyDirIter & {
    lease_.reset();
    ++cur_;
    return *this;
  }
  auto operator++(int) -> BlindKeyDirIter {
    auto tmp = *this;
    ++*this;
    return tmp;
  }
  auto operator--() -> BlindKeyDirIter & {
    lease_.reset();
    --cur_;
    return *this;
  }
  auto operator--(int) -> BlindKeyDirIter {
    auto tmp = *this;
    --*this;
    return tmp;
  }
  auto operator==(const BlindKeyDirIter &o) const noexcept -> bool {
    return cur_ == o.cur_;
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  Inner cur_;
  PersistentU32Map<std::shared_ptr<DataFile>> files_;
  bool owned_{false};
  KeyDirCtx ctx_;
  mutable std::vector<std::byte> buf_;
  // Pins the pool frame the last key span points into. After buf_, so it is
  // released first.
  mutable FrameLease lease_;

  [[nodiscard]] auto context() const -> KeyDirCtx {
    auto c = ctx_;
    if (owned_)
      c.files = &files_;
    return c;
  }
};

export using KeyDirIter = BlindKeyDirIter<true>;
export using KeyDirValueIter = BlindKeyDirIter<false>;

// Descending value iterator: holds the forward cursor and pre-decrements.
export class KeyDirReverseValueIter {
public:
  KeyDirReverseValueIter() = default;
  explicit KeyDirReverseValueIter(KeyDirValueIter past_pos)
      : cur_{std::move(past_pos)} {
    --cur_;
  }
  auto operator*() const -> KeyDirLoc { return *cur_; }
  auto operator++() -> KeyDirReverseValueIter & {
    --cur_;
    return *this;
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  KeyDirValueIter cur_;
};

// A present key costs one read (the candidate's record, which is the key's
// own); an absent key almost never reads.
export template <typename T>
auto kd_get(const T &t, std::span<const std::byte> key, const KeyDirCtx &ctx)
    -> std::optional<KeyDirEntry> {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  const auto ref = t.get(key, reader);
  if (!ref)
    return std::nullopt;
  return KeyDirEntry::make(reader.sequence, ref->offset, ref->file_id,
                           reader.value_size);
}
export template <typename T>
auto kd_contains(const T &t, std::span<const std::byte> key,
                 const KeyDirCtx &ctx) -> bool {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  return t.get(key, reader).has_value();
}
// Looks the key up and copies its value into `out`: the read that confirms
// the key is the read of the value. False if absent.
export inline auto kd_read_value(const KeyDirTree &t,
                                 std::span<const std::byte> key,
                                 const KeyDirCtx &ctx,
                                 std::vector<std::byte> &out) -> bool {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  const auto ref = t.get(key, reader);
  if (!ref)
    return false;
  out.assign(reader.value.begin(), reader.value.end());
  return true;
}
// Inserts or replaces; returns what it replaced. One read, plus one when a
// leaf splits.
export inline auto kd_put(KeyDirTransient &t, std::span<const std::byte> key,
                          const KeyDirEntry &e, const KeyDirCtx &ctx)
    -> std::optional<KeyDirHit> {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  const auto displaced = t.upsert(
      key, to_blind_ref(e), reader,
      [](const BlindRef &, const BlindRef &) { return true; });
  if (!displaced)
    return std::nullopt;
  return reader.displaced(*displaced);
}
export inline auto kd_erase(KeyDirTransient &t, std::span<const std::byte> key,
                            const KeyDirCtx &ctx) -> std::optional<KeyDirHit> {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  const auto erased = t.erase(key, reader);
  if (!erased)
    return std::nullopt;
  return reader.displaced(*erased);
}
export template <typename T>
auto kd_lower_bound(const T &t, std::span<const std::byte> key,
                    const KeyDirCtx &ctx) -> KeyDirIter {
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  return {t.lower_bound(key, reader), ctx};
}
export inline auto kd_begin(const KeyDirTree &t, const KeyDirCtx &ctx)
    -> KeyDirIter {
  return {t.begin(), ctx};
}
export inline auto kd_end(const KeyDirTree &t, const KeyDirCtx &ctx)
    -> KeyDirIter {
  return {t.end_iter(), ctx};
}
export inline auto kd_upper_bound(const KeyDirTree &t,
                                  std::span<const std::byte> key,
                                  const KeyDirCtx &ctx) -> KeyDirIter {
  auto it = kd_lower_bound(t, key, ctx);
  if (it != std::default_sentinel &&
      btree_detail::compare_bytes(it.key(), key) == 0)
    ++it;
  return it;
}
export inline auto kd_value_lower_bound(const KeyDirTree &t,
                                        std::span<const std::byte> from,
                                        const KeyDirCtx &ctx)
    -> KeyDirValueIter {
  if (from.empty())
    return {t.begin(), ctx};
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  return {t.lower_bound(from, reader), ctx};
}
export inline auto kd_value_rlower_bound(const KeyDirTree &t,
                                         std::span<const std::byte> from,
                                         const KeyDirCtx &ctx)
    -> KeyDirReverseValueIter {
  if (from.empty())
    return KeyDirReverseValueIter{KeyDirValueIter{t.end_iter(), ctx}};
  // Start past the last key <= from: at the first key > from.
  FrameLease lease;
  KeyReader reader{ctx, key_read_buffer(), lease};
  auto fwd = KeyDirValueIter{t.lower_bound(from, reader), ctx};
  if (fwd != std::default_sentinel &&
      btree_detail::compare_bytes(fwd.key(), from) == 0)
    ++fwd;
  return KeyDirReverseValueIter{std::move(fwd)};
}
export inline auto key_dir_from_recovered(RecoveryKeyDirTree t) -> KeyDirTree {
  BlindBulkLoader<kBlindLeafBytes> out{kBlindRecoveryFillMin,
                                       kBlindRecoveryFillMax};
  for (auto it = t.begin(); it != std::default_sentinel; ++it) {
    auto [key, e] = *it;
    out.append(key, to_blind_ref(e));
  }
  return std::move(out).finish();
}

#endif // BYTECASK_KEYDIR_BLIND

// ---------------------------------------------------------------------------
// EngineState — immutable snapshot of all mutable engine state.
//
// Each write produces a new EngineState via a pure transition method.
// The old state stays alive as long as any reader holds a shared_ptr.
// ---------------------------------------------------------------------------
export struct EngineState {
  KeyDirTree key_dir;
  PersistentU32Map<std::shared_ptr<DataFile>> files;
  PersistentU32Map<FileStats> file_stats;
  std::uint32_t active_file_id{};
  std::uint32_t next_file_id{};
  std::uint64_t next_seq{1};
  std::uint64_t durable_seq{0};
  // Highest sequence written by a sync=true slot. A state whose
  // sync_requested_seq exceeds its durable_seq owes an fdatasync and must
  // not be published — store_state enforces durable_seq >= sync_requested_seq.
  std::uint64_t sync_requested_seq{0};
  Mode mode{Mode::Leader};
  bool degraded{false};
  std::string degraded_reason;

  [[nodiscard]] auto is_write_allowed() const noexcept -> bool {
    return mode == Mode::Leader && !degraded;
  }

  [[nodiscard]] auto is_ingestion_allowed() const noexcept -> bool {
    return mode == Mode::Follower && !degraded;
  }

  // Where this state's key directory reads the keys it does not store.
  [[nodiscard]] auto kd_ctx(bool verify = true) const -> KeyDirCtx {
    return {&files, nullptr, nullptr, verify};
  }

  [[nodiscard]] auto active_file() -> DataFile & {
    return **files.get(active_file_id);
  }

  [[nodiscard]] auto active_file() const -> const DataFile & {
    return **files.get(active_file_id);
  }

  // Creates a mutable working copy for the write path.
  // Defined in bytecask.cpp (needs TransientEngineState's full definition).
  [[nodiscard]] auto transient() const -> TransientEngineState;

  // The same state marked degraded. A handle copy, not a derived version:
  // the key directory's version history stays linear even when the state
  // being degraded is the published one and an unpublished head derived
  // from it is still alive (a failed flush).
  [[nodiscard]] auto degraded_copy(std::string reason) const
      -> std::shared_ptr<EngineState> {
    auto s = std::make_shared<EngineState>(*this);
    s->degraded = true;
    s->degraded_reason = std::move(reason);
    return s;
  }
};

// ---------------------------------------------------------------------------
// VacuumMapping — per-live-entry mapping produced during vacuum I/O phase.
// The commit phase uses these to remap key_dir entries.
// ---------------------------------------------------------------------------
export struct VacuumMapping {
  std::vector<std::byte> key;
  std::uint64_t new_offset;
  std::uint64_t sequence;
  std::uint32_t value_size;
};

// ---------------------------------------------------------------------------
// ResumeEntry — one valid committed entry collected during resume()'s scan.
// Passed to TransientEngineState::apply_resume for key_dir replay.
// ---------------------------------------------------------------------------
export struct ResumeEntry {
  std::uint64_t sequence;
  EntryType entry_type;
  std::uint64_t file_offset;
  std::uint32_t value_size;
  // For RangeDel, `key` is the range's inclusive lower bound and `range_end`
  // its exclusive upper bound. Empty for every other entry type.
  std::vector<std::byte> key;
  std::vector<std::byte> range_end;
};

export struct VacuumScanResult {
  std::vector<VacuumMapping> mappings;
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
  std::uint64_t tombstone_bytes{0};
  std::uint64_t marker_bytes{0};
  std::uint64_t tombstones_dropped{0};
};

// RecoveredFile and RecoveryResult are private to bytecask.cpp.

// ---------------------------------------------------------------------------
// Key — owning byte sequence for iterator value_type and recovery tombstone map.
// Needs operator<=> for use as map key in recovery.
// ---------------------------------------------------------------------------
export class Key {
public:
  Key() = default;
  explicit Key(std::span<const std::byte> v) : data_{v.begin(), v.end()} {}

  [[nodiscard]] auto begin() const { return data_.begin(); }
  [[nodiscard]] auto end() const { return data_.end(); }
  [[nodiscard]] auto size() const noexcept { return data_.size(); }

  auto operator<=>(const Key &other) const -> std::strong_ordering {
    return std::lexicographical_compare_three_way(
        data_.begin(), data_.end(), other.data_.begin(), other.data_.end(),
        [](std::byte a, std::byte b) -> std::strong_ordering {
          return std::to_integer<unsigned char>(a) <=>
                 std::to_integer<unsigned char>(b);
        });
  }

  auto operator==(const Key &other) const -> bool {
    return data_ == other.data_;
  }

private:
  std::vector<std::byte> data_;
};

export struct RecoveredFile {
  std::uint32_t file_id;
  std::shared_ptr<DataFile> data_file;
  std::filesystem::path hint_path;
  std::uint64_t total_bytes{0};
};

export struct RangeTombstone {
  Key start;
  Key end; // exclusive — [start, end)
  std::uint64_t seq;
  std::uint32_t file_id;
  // Set when recovery sees this tombstone beat a Put held by another file.
  bool needed{false};
};

// The newest point tombstone recovery has seen for a key.
export struct PointTombstone {
  std::uint64_t seq{0};
  std::uint32_t file_id{0};
};

export struct RecoveryResult {
  RecoveryKeyDirTree key_dir;
  std::map<Key, PointTombstone> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::uint64_t max_seq{0};
  PersistentU32Map<FileStats> file_stats;
  // Sequences of point tombstones seen beating a Put in another file.
  // Unsorted, may repeat; range tombstones carry their own flag.
  std::vector<std::uint64_t> needed_tombstones;
  // A lenient open skipped a file it could not read. Its Puts were never
  // seen, so no tombstone can be shown unneeded.
  bool skipped_files{false};
};

// ---------------------------------------------------------------------------
// NeededTombstones — which tombstones compaction must keep, decided once by
// recovery.
//
// A tombstone matters only while some other file holds an older Put of a key
// it deletes: dropping it would let that Put back at the next open. Recovery
// sees every hint entry, so it records the tombstones it saw beat a Put held
// by a different file. An older Put in the tombstone's own file needs no
// record — compaction drops that dead Put in the same pass that drops the
// tombstone.
//
// The decision holds for the life of the process. Every later write, ingest
// or resume carries a higher sequence, so no older Put can appear, and
// vacuum only ever removes entries. Tombstones above `horizon` were never
// examined and are always kept; a tombstone that stops being needed after
// open is dropped only after the next one.
//
// Every entry carries its own sequence, so a sequence names one tombstone
// exactly: the set holds 8 bytes per needed tombstone, and nothing for the
// tombstones that can go.
// ---------------------------------------------------------------------------
export struct NeededTombstones {
  std::uint64_t horizon{0};
  std::vector<std::uint64_t> sequences; // sorted, unique

  [[nodiscard]] auto droppable(std::uint64_t seq) const -> bool {
    return seq <= horizon && !std::ranges::binary_search(sequences, seq);
  }
};

} // namespace bytecask
