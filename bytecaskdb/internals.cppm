// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — internal engine types shared across implementation units

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bytecask:internals;

import bytecask.blind_btree;
import bytecask.btree;
import bytecask.buffer_pool;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.patch_map;
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
  // ChangeMarker entries: kept for ever, so a file holding one is never
  // removed whole and compaction publishes it even when the marker is all it
  // kept (#397).
  std::uint64_t change_marker_bytes{0};

  // Dead Put bytes: what compacting this file is sure to reclaim. Vacuum
  // selects files by it, so a file that is mostly tombstones or markers is
  // not picked again and again for a compaction that cannot shrink it.
  [[nodiscard]] auto reclaimable_bytes() const noexcept -> std::uint64_t {
    const auto kept =
        live_bytes + tombstone_bytes + marker_bytes + change_marker_bytes;
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
  // ChangeMarker entries: kept for ever, so a file holding one is never
  // removed whole and compaction publishes it even when the marker is all it
  // kept (#397).
  std::uint64_t change_marker_bytes{0};

  // Dead Put bytes: what compacting this file is sure to reclaim. Vacuum
  // selects files by it, so a file that is mostly tombstones or markers is
  // not picked again and again for a compaction that cannot shrink it.
  [[nodiscard]] auto reclaimable_bytes() const noexcept -> std::uint64_t {
    const auto kept =
        live_bytes + tombstone_bytes + marker_bytes + change_marker_bytes;
    return total_bytes > kept ? total_bytes - kept : 0;
  }
};
#endif

// A change to one file's stats, as the write path makes it: additions to
// the counters, a subtraction from live_bytes, and the sequences of a batch.
// FileStatsMap records these and applies them when read.
struct FileStatsDelta {
  std::uint64_t live_added{0};
  std::uint64_t live_removed{0};
  std::uint64_t total_added{0};
  std::uint64_t tombstone_added{0};
  std::uint64_t marker_added{0};
  std::uint64_t change_marker_added{0};
  std::uint64_t min_sequence{0}; // first batch's first sequence; 0 = none
  std::uint64_t max_sequence{0};
};

struct FileStatsPolicy {
  using Delta = FileStatsDelta;
  static void apply(FileStats &fs, const Delta &d) {
    // Additions first: the stats are consistent before the writes a delta
    // totals and after them, so live_bytes cannot wrap in between.
    fs.live_bytes += d.live_added;
    fs.live_bytes -= d.live_removed;
    fs.total_bytes += d.total_added;
    fs.tombstone_bytes += d.tombstone_added;
    fs.marker_bytes += d.marker_added;
    fs.change_marker_bytes += d.change_marker_added;
    // A file's sequences ascend, so its minimum is its first batch's.
    if (fs.min_sequence == 0) fs.min_sequence = d.min_sequence;
    fs.max_sequence = std::max(fs.max_sequence, d.max_sequence);
  }
  static void combine(Delta &older, const Delta &d) {
    older.live_added += d.live_added;
    older.live_removed += d.live_removed;
    older.total_added += d.total_added;
    older.tombstone_added += d.tombstone_added;
    older.marker_added += d.marker_added;
    older.change_marker_added += d.change_marker_added;
    if (older.min_sequence == 0) older.min_sequence = d.min_sequence;
    older.max_sequence = std::max(older.max_sequence, d.max_sequence);
  }
};

// Per-file stats, written on every commit and read only off the write path
// (vacuum's choice of file, ChangeIterator, resume, the consistency checks).
// A commit group's changes are one patch run, so a commit writes no map;
// every kFileStatsMaxDepth groups a freeze squashes the runs into the paged
// table (#367). 8 measured level with 32 on commit_probe's serial section,
// with squashes a quarter the size (~10 µs on ~950 files, ~5 µs at defaults).
inline constexpr std::size_t kFileStatsMaxDepth = 8;
using FileStatsMap =
    PersistentPatchMap<FileStats, FileStatsPolicy, kFileStatsMaxDepth>;
using FileStatsTransient =
    TransientPatchMap<FileStats, FileStatsPolicy, kFileStatsMaxDepth,
                      PersistentU32Table<FileStats>>;

// File registry: a COW map from file_id to shared DataFile.
// PersistentU32Table provides O(1) snapshot sharing; rotation and vacuum
// fork a transient, mutate it, and freeze, copying only the pages they touch.

// ---------------------------------------------------------------------------
// KeyDirEntry — one slot in the in-memory key directory.
//
// Bit-packed into two 64-bit words to keep the key directory small.
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
// file_id is an integer handle, leased by DB (bytecask.file_id), that indexes
// into the engine's file registry. It is reused once nothing holds the file
// it named, so ids say nothing of age. file_offset is the byte offset where
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
  case EntryType::ChangeMarker:
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
  case EntryType::ChangeMarker:
    return 0;
  }
  return 0;
}

// The bytes an entry adds to FileStats::change_marker_bytes: a ChangeMarker
// entry's whole record (its value is the 8-byte id), 0 otherwise.
export inline constexpr auto change_marker_size(EntryType type)
    -> std::uint64_t {
  switch (type) {
  case EntryType::ChangeMarker:
    return entry_size(0, kChangeMarkerIdBytes);
  case EntryType::Put:
  case EntryType::Delete:
  case EntryType::BulkBegin:
  case EntryType::BulkEnd:
  case EntryType::RangeDel:
    return 0;
  }
  return 0;
}

// A node's marker at sequence: the last of markers (sorted by
// since_sequence) at or below it, kOriginMarker if none. A null list holds
// none.
export inline auto marker_at(const std::vector<ChangeMarker> *markers,
                             std::uint64_t sequence) noexcept -> ChangeMarker {
  auto at = kOriginMarker;
  if (!markers) return at;
  for (const auto &m : *markers) {
    if (m.since_sequence > sequence) break;
    at = m;
  }
  return at;
}

// Forward declaration — defined in bytecask.cppm (primary interface).
export class TransientEngineState;

// ---------------------------------------------------------------------------
// The validation a plan made on the writer's thread, against a published
// state P, before it joined a batch (WritePlan::prepare_validation), and
// what the answer read. The section uses the answer when RecentWrites shows
// that nothing it read was written since P.
// ---------------------------------------------------------------------------
export struct PreparedChecks {
  // The plan's guards held at P, and so did its write-write checks when
  // writes_checked. False also for a plan that was not prepared: then the
  // section validates it as before.
  bool held{false};
  bool writes_checked{false};
  std::uint64_t seq{0};  // P's last sequence
  // DB::head_generation_ as read before P, the head, was loaded.
  std::uint64_t generation{0};
  // key_hash of each key the answer read: guarded keys, and written keys
  // when writes_checked.
  std::vector<std::uint64_t> probes;
  // Each range guard, a prefix range: its prefix's hash and length.
  struct Range {
    std::uint64_t hash;
    std::uint32_t len;
  };
  std::vector<Range> ranges;
  // Hashes of the keys the plan writes, and of their prefixes at the first
  // `lengths` prefix lengths RecentWrites had registered, so the section
  // need not hash them. Without `hashed` it does.
  bool hashed{false};
  std::vector<std::uint64_t> writes;
  std::vector<std::uint64_t> prefixes;
  std::size_t lengths{0};
};

// A key's hash for RecentWrites.
export [[nodiscard]] inline auto key_hash(std::span<const std::byte> key) noexcept
    -> std::uint64_t {
  return std::hash<std::string_view>{}(
      std::string_view{reinterpret_cast<const char *>(key.data()), key.size()});
}

// ---------------------------------------------------------------------------
// RecentWrites — which keys the head has written since a given sequence, as
// far as a fixed table can tell. A slot per key hash holds the highest
// sequence of any plan that wrote a key hashing there. A collision only
// makes a key look written, so the table can answer "maybe written" for a
// key that was not, never "not written" for one that was. Writes that name
// no key (a range delete, an ingested slice, resume()'s replay) raise a
// floor instead: an answer prepared below it is not used.
//
// Range guards over a prefix are probed by the prefix: a writer records its
// key's prefix at every prefix length a range guard has used. A length is
// registered the first time a guard needs it, and trusted only for answers
// prepared after that, since writes before it did not record it.
//
// The table, the floor and the registration belong to the write section
// (DB::write_mu_). The registered lengths are also read by writers on their
// own threads, to hash their keys' prefixes before they join a batch.
// ---------------------------------------------------------------------------
export class RecentWrites {
public:
  static constexpr std::size_t kSlots = std::size_t{1} << 18;
  static constexpr std::size_t kMaxPrefixLengths = 8;
  using Lengths = std::array<std::uint32_t, kMaxPrefixLengths>;

  enum class Verdict : std::uint8_t {
    kUnchanged,     // nothing the answer read was written: use it
    kNotHeld,       // not prepared, did not hold at P, or P was dropped
    kBelowFloor,    // prepared before a write that names no key
    kWritten,       // a key or prefix it read may have been written
    kNewLength,     // a range guard's prefix length not tracked since P
  };

  RecentWrites() = default;
  RecentWrites(const RecentWrites &) = delete;
  auto operator=(const RecentWrites &) -> RecentWrites & = delete;
  ~RecentWrites() { delete[] table_.load(std::memory_order_relaxed); }

  // Any thread: the prefix lengths registered so far, in order. Returns how
  // many; each is in `out` before the count that exposes it.
  auto lengths(Lengths &out) const noexcept -> std::size_t {
    const auto n = count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i)
      out[i] = lengths_[i].load(std::memory_order_relaxed);
    return n;
  }

  // Any thread: whether the table exists. Without it nothing is recorded,
  // so a writer need not hash its keys.
  [[nodiscard]] auto has_table() const noexcept -> bool {
    return table_.load(std::memory_order_acquire) != nullptr;
  }

  // Any thread, outside the write lock: allocates the table (2 MiB, zeroed)
  // if it does not exist yet. Called by a writer whose prepared answer read
  // something, so a database whose plans read nothing (plain puts, a
  // follower) never holds it, and the allocation never runs in the section.
  // Out of memory leaves it unallocated: the plans are validated in the
  // section, as without it.
  void ensure_table() noexcept {
    if (has_table()) return;
    auto *fresh = new (std::nothrow) std::uint64_t[kSlots]();
    if (fresh == nullptr) return;
    std::uint64_t *expected = nullptr;
    if (!table_.compare_exchange_strong(expected, fresh,
                                        std::memory_order_acq_rel))
      delete[] fresh;
  }

  // Under the write lock: whether the prepared answer still holds for the
  // head, whose last sequence is head_last and whose generation is
  // `generation` (DB::head_generation_). writes_checked_here: the section
  // checks the plan's writes itself (apply_puts_checked), so an answer that
  // did not check them at P is enough.
  //
  // The table records nothing until the section first finds it allocated:
  // then it raises the floor to head_last, since no write before was
  // recorded, and no answer prepared before that is used.
  [[nodiscard]] auto verify(const PreparedChecks &c, bool writes_checked_here,
                            std::uint64_t head_last, std::uint64_t generation)
      -> Verdict {
    if (!c.held || !(c.writes_checked || writes_checked_here))
      return Verdict::kNotHeld;
    // P was a head the engine has since gone back from (store_head).
    if (c.generation != generation)
      return Verdict::kNotHeld;
    // An answer that read nothing cannot have changed.
    if (c.probes.empty() && c.ranges.empty())
      return Verdict::kUnchanged;
    // Every new prefix length is registered before anything can return, so
    // a range guard on a busy key still gets its length tracked.
    auto untracked = false;
    for (const auto &r : c.ranges) {
      if (!tracked_since(r.len, c.seq, head_last)) untracked = true;
    }
    if (!active_) {
      if (!has_table()) return Verdict::kNotHeld;
      active_ = true;
      raise_floor(head_last);
    }
    if (c.seq < floor_)
      return Verdict::kBelowFloor;
    if (untracked)
      return Verdict::kNewLength;
    for (const auto h : c.probes) {
      if (slot(h) > c.seq)
        return Verdict::kWritten;
    }
    for (const auto &r : c.ranges) {
      if (slot(r.hash) > c.seq)
        return Verdict::kWritten;
    }
    return Verdict::kUnchanged;
  }

  // Under the write lock, as soon as a plan is applied and before the next
  // plan is verified: the plan's point writes, at its last sequence `seq`.
  // for_each_key(f) calls f(key) for each point write's key. Nothing until
  // the table is active (see verify).
  template <typename ForEachKey>
  void record(const PreparedChecks &c, std::uint64_t seq,
              ForEachKey &&for_each_key) noexcept {
    if (!active_) return;
    const auto n = count_.load(std::memory_order_relaxed);
    auto first_length = std::size_t{0};
    if (c.hashed) {
      for (const auto h : c.writes) mark(h, seq);
      for (const auto h : c.prefixes) mark(h, seq);
      first_length = c.lengths;
    } else {
      for_each_key([&](std::span<const std::byte> key) {
        mark(key_hash(key), seq);
      });
    }
    for (auto i = first_length; i < n; ++i) {
      const auto len = lengths_[i].load(std::memory_order_relaxed);
      for_each_key([&](std::span<const std::byte> key) {
        if (key.size() >= len) mark(key_hash(key.first(len)), seq);
      });
    }
  }

  // Under the write lock: every key may have changed up to seq.
  void raise_floor(std::uint64_t seq) noexcept {
    floor_ = std::max(floor_, seq);
  }

private:
  auto slot(std::uint64_t h) noexcept -> std::uint64_t & {
    return table_.load(std::memory_order_relaxed)[h & (kSlots - 1)];
  }
  void mark(std::uint64_t h, std::uint64_t seq) noexcept {
    auto &s = slot(h);
    s = std::max(s, seq);
  }
  // Whether writes after `seq` all recorded their prefix of length len.
  // Registers a length seen for the first time: writes after head_last
  // record it, and answers prepared at or after that can trust it. With no
  // room left, every range guard over a new length is checked in the
  // section.
  auto tracked_since(std::uint32_t len, std::uint64_t seq,
                     std::uint64_t head_last) noexcept -> bool {
    const auto n = count_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
      if (lengths_[i].load(std::memory_order_relaxed) == len)
        return seq >= since_[i];
    }
    if (n == kMaxPrefixLengths) return false;
    lengths_[n].store(len, std::memory_order_relaxed);
    since_[n] = head_last;
    count_.store(n + 1, std::memory_order_release);
    return false;
  }

  // Written once, by whichever writer allocates it first; read by writers
  // (has_table) and the section.
  std::atomic<std::uint64_t *> table_{nullptr};
  // The section's, under the write lock.
  bool active_{false};
  std::uint64_t floor_{0};
  std::array<std::atomic<std::uint32_t>, kMaxPrefixLengths> lengths_{};
  std::array<std::uint64_t, kMaxPrefixLengths> since_{};
  std::atomic<std::size_t> count_{0};
};

// ---------------------------------------------------------------------------
// Records the batch being built has placed in the active file but not yet
// written: phase 1 of a commit (validate and apply) runs before the batch's
// pwritev, so a key directory that reads keys back from the data files must
// find these in memory. Keyed by file_id << 32 | offset.
//
// `key` borrows the bytes of the write that placed the record: a WritePlan's
// key, or an ingested entry's. Both outlive every read of the record: a plan
// stays with its slot until the group that applies it ends, and ingest holds
// its entries for the whole call. The commit section empties the list before
// its slots go back to their writers (DB::execute_group).
// ---------------------------------------------------------------------------
export struct PendingRecord {
  std::uint64_t slot{0};  // pending_slot(file_id, offset)
  std::uint64_t sequence{0};
  std::uint32_t value_size{0};
  std::span<const std::byte> key;
};

// Sorted by slot. A batch places its records at rising offsets in the active
// file, and a rotation clears the list (TransientEngineState::
// apply_rotate_file), so put() appends even when the next file takes a lower,
// reused id. A vector kept between groups allocates nothing per record, where
// a hash map allocated a node and a key copy for each (#754).
export class PendingRecords {
public:
  using value_type = PendingRecord;

  // Adds r, or replaces the record at r.slot.
  void put(const PendingRecord &r) {
    if (v_.empty() || v_.back().slot < r.slot) {
      v_.push_back(r);
      return;
    }
    // r.slot is at most the last record's, so `it` is a record.
    const auto it = std::ranges::lower_bound(v_, r.slot, {}, &PendingRecord::slot);
    if (it->slot == r.slot) {
      *it = r;
    } else {
      v_.insert(it, r);
    }
  }
  [[nodiscard]] auto find(std::uint64_t slot) const noexcept
      -> const PendingRecord * {
    const auto it = std::ranges::lower_bound(v_, slot, {}, &PendingRecord::slot);
    return it != v_.end() && it->slot == slot ? &*it : nullptr;
  }
  void erase(std::uint64_t slot) noexcept {
    const auto it = std::ranges::lower_bound(v_, slot, {}, &PendingRecord::slot);
    if (it != v_.end() && it->slot == slot) v_.erase(it);
  }
  void clear() noexcept { v_.clear(); }
  void swap(PendingRecords &other) noexcept { v_.swap(other.v_); }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return v_.size(); }
  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return v_.capacity();
  }

private:
  std::vector<PendingRecord> v_;
};

export inline constexpr auto pending_slot(std::uint32_t file_id,
                                          std::uint64_t offset) noexcept
    -> std::uint64_t {
  return (std::uint64_t{file_id} << 32) | offset;
}

// ---------------------------------------------------------------------------
// KeyDirCtx — where a key directory that does not store keys reads them: the
// file registry of the version it belongs to (a published state's, or the
// writer's transient one plus the records it has not written yet). The keyed
// B+ tree ignores it. Non-owning; valid while the state it
// points into is.
// ---------------------------------------------------------------------------
export struct KeyDirCtx {
  const PersistentU32Table<std::shared_ptr<DataFile>> *files{nullptr};
  const TransientU32Table<std::shared_ptr<DataFile>> *writer_files{nullptr};
  const PendingRecords *pending{nullptr};
  bool verify{true};
  // The DB's Options::thread_scratch_bytes: what the thread's record buffer
  // may keep after the call.
  std::size_t scratch_bytes;

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
// The blind-leaf tree (docs/blind_leaf_btree_design.md) is the default: it
// stores no key bytes and reads them back through the KeyDirCtx.
// BYTECASK_KEYDIR=btree builds the engine on the B+ tree that keeps its keys
// (docs/persistent_btree_design.md).
//
// The keyed build recovers into a RecoveryKeyDirTree, a keyed B+ tree; the
// blind build recovers straight from the hint files (recovery_load_streams).
// ---------------------------------------------------------------------------
#if defined(BYTECASK_KEYDIR_BLIND)
export inline constexpr bool kKeyDirReadsKeys = true;
#else
export inline constexpr bool kKeyDirReadsKeys = false;
#endif

export using RecoveryKeyDirTree = PersistentBTree<KeyDirEntry>;
export using RecoveryKeyDirIter = BTreeIterator<KeyDirEntry>;
// Bulk build of a key directory from ascending keys, and the concatenation
// of the slices several threads built — what recovery_load_ranged uses.
export using KeyDirBulkLoader = btree_detail::BulkLoader<KeyDirEntry>;
export using KeyDirLeafRun = btree_detail::LeafRun<KeyDirEntry>;

#if !defined(BYTECASK_KEYDIR_BLIND)

export using KeyDirTree = PersistentBTree<KeyDirEntry>;
export using KeyDirTransient = TransientBTree<KeyDirEntry>;
export using KeyDirIter = BTreeIterator<KeyDirEntry>;
export using KeyDirValueIter = BTreeValueIterator<KeyDirEntry>;
export using KeyDirReverseValueIter = ReverseBTreeValueIterator<KeyDirEntry>;
export using KeyDirHit = KeyDirEntry;

// Scratch for the record DB::get and Snapshot::get read, reused per thread.
inline auto value_read_buffer() -> std::vector<std::byte> & {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // TLS: scratch — this call's record; freed past the DB's
  // thread_scratch_bytes (#384).
  thread_local std::vector<std::byte> buf;
#pragma clang diagnostic pop
  return buf;
}

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
// Operations on a key whose entry was looked up earlier, `at` (see the blind
// versions). A keyed tree reads nothing to find a key, so it has nothing to
// save: these decline and the caller takes the keyed path.
export inline auto kd_holds(const KeyDirTransient &, std::span<const std::byte>,
                            const KeyDirEntry &) -> bool {
  return false;
}
export inline auto kd_holds(const KeyDirTree &, std::span<const std::byte>,
                            const KeyDirEntry &) -> bool {
  return false;
}
export inline auto kd_put_at(KeyDirTransient &, std::span<const std::byte>,
                             const KeyDirEntry &, const KeyDirEntry &) -> bool {
  return false;
}
export inline auto kd_erase_at(KeyDirTransient &, std::span<const std::byte>,
                               const KeyDirEntry &) -> bool {
  return false;
}
// Paths prepared before the write section (blind key directory only; see
// PreparedPath). The keyed tree compares keys on its way down and reads no
// record to place one, so a path would save it nothing: it prepares none,
// and its writes go by key.
export inline void kd_prepare_path(const KeyDirTree &,
                                   std::span<const std::byte>,
                                   const KeyDirCtx &, bool, PreparedPath &p) {
  p = {};
}
export inline auto kd_put_at(KeyDirTransient &t, std::span<const std::byte> key,
                             const KeyDirEntry &at, const KeyDirEntry &e,
                             const PreparedPath *, PathFollow &) -> bool {
  return kd_put_at(t, key, at, e);
}
export inline auto kd_put(KeyDirTransient &t, std::span<const std::byte> key,
                          const KeyDirEntry &e, const KeyDirCtx &ctx,
                          const PreparedPath *, PathFollow &)
    -> std::optional<KeyDirHit> {
  return kd_put(t, key, e, ctx);
}
export inline auto kd_erase_at(KeyDirTransient &t,
                               std::span<const std::byte> key,
                               const KeyDirEntry &at, const PreparedPath *,
                               PathFollow &) -> bool {
  return kd_erase_at(t, key, at);
}
export inline auto kd_path_stats(const KeyDirTransient &) -> PathStats {
  return {};
}
// Undo for a plan applied with its checks folded into the descents (blind
// key directory only; see TransientEngineState::apply_puts_checked). The
// keyed tree never takes that path.
export inline auto kd_restore(KeyDirTransient &, std::span<const std::byte>,
                              const KeyDirEntry &, const KeyDirHit &) -> bool {
  return false;
}
export inline auto kd_remove(KeyDirTransient &, std::span<const std::byte>,
                             const KeyDirEntry &) -> bool {
  return false;
}
// What a put or an erase of the entry `at` displaced.
export inline auto kd_hit(const KeyDirEntry &at) -> KeyDirHit { return at; }
export template <typename T>
auto kd_lower_bound(const T &t, std::span<const std::byte> key,
                    const KeyDirCtx &) {
  return t.lower_bound(key);
}
// Keys in [from, to), counted no further than `limit`. The keys are in
// memory: the B+ tree counts leaf sizes between the two positions.
export inline auto kd_count(const KeyDirTree &t, std::span<const std::byte> from,
                            std::span<const std::byte> to, std::size_t limit,
                            const KeyDirCtx &) -> std::size_t {
  return t.lower_bound(from).count_until(t.lower_bound(to), limit);
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
// Members are marked inline: in a module interface, a member defined in
// its class is not inline by default, and these sit on every scan step.
export struct KeyDirLoc {
  BlindRef ref;
  [[nodiscard]] inline auto file_id() const noexcept -> std::uint32_t {
    return ref.file_id;
  }
  [[nodiscard]] inline auto file_offset() const noexcept -> std::uint64_t {
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
      if (const auto *p = ctx.pending->find(pending_slot(r.file_id, r.offset))) {
        sequence = p->sequence;
        value_size = p->value_size;
        value = {};
        return p->key;
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
  // TLS: scratch — this call's records; freed past the DB's
  // thread_scratch_bytes (#384).
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

  inline auto operator*() const -> value_type {
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

  inline auto operator++() -> BlindKeyDirIter & {
    lease_.reset();
    ++cur_;
    return *this;
  }
  auto operator++(int) -> BlindKeyDirIter {
    auto tmp = *this;
    ++*this;
    return tmp;
  }
  inline auto operator--() -> BlindKeyDirIter & {
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
  PersistentU32Table<std::shared_ptr<DataFile>> files_;
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  return t.get(key, reader).has_value();
}
// Looks the key up and copies its value into `out`: the read that confirms
// the key is the read of the value. False if absent.
export inline auto kd_read_value(const KeyDirTree &t,
                                 std::span<const std::byte> key,
                                 const KeyDirCtx &ctx,
                                 std::vector<std::byte> &out) -> bool {
  FrameLease lease;
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  const auto erased = t.erase(key, reader);
  if (!erased)
    return std::nullopt;
  return reader.displaced(*erased);
}
// Operations on a key whose entry was looked up earlier, `at` — in a
// snapshot, outside the write lock. A record location names one record, and
// a record holds one key, so an entry that still points at `at` is the key,
// unchanged since: none of these reads a record. Each declines (false,
// nothing changed) when the entry no longer points at `at`, and the caller
// takes the keyed path.
export inline auto kd_holds(const KeyDirTransient &t,
                            std::span<const std::byte> key,
                            const KeyDirEntry &at) -> bool {
  return t.holds(key, to_blind_ref(at));
}
export inline auto kd_holds(const KeyDirTree &t, std::span<const std::byte> key,
                            const KeyDirEntry &at) -> bool {
  return t.holds(key, to_blind_ref(at));
}
export inline auto kd_put_at(KeyDirTransient &t, std::span<const std::byte> key,
                             const KeyDirEntry &at, const KeyDirEntry &e)
    -> bool {
  return t.replace_at(key, to_blind_ref(at), to_blind_ref(e));
}
export inline auto kd_erase_at(KeyDirTransient &t,
                               std::span<const std::byte> key,
                               const KeyDirEntry &at) -> bool {
  return t.erase_at(key, to_blind_ref(at));
}
// Where `key` goes in `t`, for a write on a later version to replay
// (PreparedPath). With `place`, also where an absent key would go in its
// leaf: one read, which the write then does not make.
export inline void kd_prepare_path(const KeyDirTree &t,
                                   std::span<const std::byte> key,
                                   const KeyDirCtx &ctx, bool place,
                                   PreparedPath &p) {
  FrameLease lease;
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  t.prepare_path(key, reader, place, p);
  // A key found in place: the read that found it is the record's, the last
  // the reader made, so its value size is the found record's.
  if (p.displaced_found(p.found)) p.found_value_size = reader.value_size;
}
// The writes above, down `path` where it still leads to the key's leaf
// (TransientBlindBTree::route), by key otherwise or when path is null.
// `follow` is the plan's: one per plan, across its writes.
export inline auto kd_put_at(KeyDirTransient &t, std::span<const std::byte> key,
                             const KeyDirEntry &at, const KeyDirEntry &e,
                             const PreparedPath *path, PathFollow &follow)
    -> bool {
  if (!path)
    return kd_put_at(t, key, at, e);
  return t.replace_at(key, to_blind_ref(at), to_blind_ref(e), *path, follow);
}
export inline auto kd_put(KeyDirTransient &t, std::span<const std::byte> key,
                          const KeyDirEntry &e, const KeyDirCtx &ctx,
                          const PreparedPath *path, PathFollow &follow)
    -> std::optional<KeyDirHit> {
  if (!path)
    return kd_put(t, key, e, ctx);
  FrameLease lease;
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  const auto displaced = t.upsert(
      key, to_blind_ref(e), reader,
      [](const BlindRef &, const BlindRef &) { return true; }, *path, follow);
  if (!displaced)
    return std::nullopt;
  // Replaced where the path found it: no record was read here.
  if (path->displaced_found(*displaced))
    return KeyDirHit{*displaced, path->found_value_size};
  return reader.displaced(*displaced);
}
export inline auto kd_erase_at(KeyDirTransient &t,
                               std::span<const std::byte> key,
                               const KeyDirEntry &at, const PreparedPath *path,
                               PathFollow &follow) -> bool {
  if (!path)
    return kd_erase_at(t, key, at);
  return t.erase_at(key, to_blind_ref(at), *path, follow);
}
export inline auto kd_path_stats(const KeyDirTransient &t) -> PathStats {
  return t.path_stats();
}
// What a put or an erase of the entry `at` displaced: `at`'s own record.
export inline auto kd_hit(const KeyDirEntry &at) -> KeyDirHit {
  return {to_blind_ref(at), at.value_size()};
}
// Undo for a plan applied with its checks folded into the descents (see
// TransientEngineState::apply_puts_checked), by location, reading nothing:
// puts back the record `was` where the key now points at `now`, or removes
// the key where it points at `now`. False if the key does not point at
// `now`, which the undo never expects.
export inline auto kd_restore(KeyDirTransient &t, std::span<const std::byte> key,
                              const KeyDirEntry &now, const KeyDirHit &was)
    -> bool {
  return t.replace_at(key, to_blind_ref(now), was.ref);
}
export inline auto kd_remove(KeyDirTransient &t, std::span<const std::byte> key,
                             const KeyDirEntry &now) -> bool {
  return t.erase_at(key, to_blind_ref(now));
}
export template <typename T>
auto kd_lower_bound(const T &t, std::span<const std::byte> key,
                    const KeyDirCtx &ctx) -> KeyDirIter {
  FrameLease lease;
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  return {t.lower_bound(key, reader), ctx};
}
// Keys in [from, to), counted no further than `limit`: one read to place
// each end within its leaf, and the leaves between them counted from their
// sizes — no read per key.
export inline auto kd_count(const KeyDirTree &t, std::span<const std::byte> from,
                            std::span<const std::byte> to, std::size_t limit,
                            const KeyDirCtx &ctx) -> std::size_t {
  FrameLease lease;
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
  const auto first = t.lower_bound(from, reader);
  return first.count_until(t.lower_bound(to, reader), limit);
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
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
  ScratchUse scratch{key_read_buffer(), ctx.scratch_bytes};
  KeyReader reader{ctx, scratch.buf(), lease};
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
  PersistentU32Table<std::shared_ptr<DataFile>> files;
  FileStatsMap file_stats;
  std::uint32_t active_file_id{};
  std::uint64_t next_seq{1};
  std::uint64_t durable_seq{0};
  // Highest sequence written by a sync=true slot. A state whose
  // sync_requested_seq exceeds its durable_seq owes an fdatasync and must
  // not be published — store_state enforces durable_seq >= sync_requested_seq.
  std::uint64_t sync_requested_seq{0};
  Mode mode{Mode::Leader};
  bool degraded{false};
  std::string degraded_reason;
  // Set by DB::close(). A closed state is the last one a DB publishes and
  // holds no files or key directory (see closed_copy).
  bool closed{false};
  // Options::thread_scratch_bytes, for the key directory's reads.
  std::size_t scratch_bytes;
  // The database's RecentWrites, shared by every state and transient of it,
  // so the transient's mutators record what they change themselves.
  std::shared_ptr<RecentWrites> recent_writes;

  explicit EngineState(std::size_t scratch_limit)
      : scratch_bytes{scratch_limit} {}
  // Every ChangeMarker the data files hold, by since_sequence: 16 bytes per
  // promotion. Immutable; a promotion or an ingested marker publishes a new
  // list. Null when there is none.
  std::shared_ptr<const std::vector<ChangeMarker>> change_markers;

  // This node's marker at sequence (see ChangeMarker).
  [[nodiscard]] auto marker_at(std::uint64_t sequence) const noexcept
      -> ChangeMarker {
    return bytecask::marker_at(change_markers.get(), sequence);
  }

  [[nodiscard]] auto is_write_allowed() const noexcept -> bool {
    return mode == Mode::Leader && !degraded && !closed;
  }

  [[nodiscard]] auto is_ingestion_allowed() const noexcept -> bool {
    return mode == Mode::Follower && !degraded && !closed;
  }

  // Where this state's key directory reads the keys it does not store.
  [[nodiscard]] auto kd_ctx(bool verify = true) const -> KeyDirCtx {
    return {&files, nullptr, nullptr, verify, scratch_bytes};
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

  // The state DB::close() publishes: this state's sequences, mode and
  // degraded reason, without its files or key directory, so the DB stops
  // holding them. Snapshots and iterators keep the versions they hold.
  [[nodiscard]] auto closed_copy() const -> std::shared_ptr<EngineState> {
    auto s = std::make_shared<EngineState>(scratch_bytes);
    s->active_file_id = active_file_id;
    s->next_seq = next_seq;
    s->durable_seq = durable_seq;
    s->sync_requested_seq = sync_requested_seq;
    s->mode = mode;
    s->degraded = degraded;
    s->degraded_reason = degraded_reason;
    s->closed = true;
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
  // For ChangeMarker, the id its value holds. 0 for every other entry type.
  std::uint64_t marker_id{0};
};

export struct VacuumScanResult {
  bool cancelled{false};  // VacuumOptions::cancel stopped the scan: the copy is incomplete
  std::vector<VacuumMapping> mappings;
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
  std::uint64_t tombstone_bytes{0};
  std::uint64_t marker_bytes{0};
  std::uint64_t change_marker_bytes{0};
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
  PersistentU32Table<FileStats> file_stats;
  // Sequences of point tombstones seen beating a Put in another file.
  // Unsorted, may repeat; range tombstones carry their own flag.
  std::vector<std::uint64_t> needed_tombstones;
  // A lenient open skipped a file it could not read. Its Puts were never
  // seen, so no tombstone can be shown unneeded.
  bool skipped_files{false};
  // The ChangeMarkers of these files, in the order found; sorted by the caller.
  std::vector<ChangeMarker> change_markers;
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
