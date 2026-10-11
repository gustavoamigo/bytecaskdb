// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — ChangeIterator: the lazy entry stream behind changes_since
//
// A partition rather than a module of its own because it reads EngineState,
// which the :internals partition declares. It needs nothing of DB: the DB
// hands it a state, its change index and the slice's bounds.

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

export module bytecask:change_iterator;

import :internals;
import bytecask.batch_iterator;
import bytecask.change_index;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.types;

namespace bytecask {

// ---------------------------------------------------------------------------
// ChangeIterator — yields raw entries in ascending sequence order (lazy)
//
// Used by changes_since() for replication. Walks data files lazily in
// min_sequence order, scanning one entry at a time via CommittedEntryIterator.
// Yields entries with sequence > from_sequence.
// Holds a snapshot reference to keep file descriptors open during iteration.
// The file holding from_sequence is entered at the change index's bookmark
// below it, when the index has one, and every scan lays bookmarks for the
// index as it goes (#443).
// ---------------------------------------------------------------------------

export class ChangeIterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type = DataEntryView;
  using difference_type = std::ptrdiff_t;

  ChangeIterator() = default;
  ~ChangeIterator(); // Defined in .cpp where Impl is complete

  // Move-only semantics
  ChangeIterator(const ChangeIterator&) = delete;
  ChangeIterator& operator=(const ChangeIterator&) = delete;
  ChangeIterator(ChangeIterator&&) noexcept; // Defined in .cpp where Impl is complete
  ChangeIterator& operator=(ChangeIterator&&) noexcept; // Defined in .cpp where Impl is complete

  // Constructor for implementation use - not part of public API
  explicit ChangeIterator(std::shared_ptr<const EngineState> state,
                          std::shared_ptr<ChangeIndex> index,
                          std::uint64_t from_sequence,
                          std::uint64_t durable_sequence,
                          std::size_t max_bytes);

  auto operator++() -> ChangeIterator &;
  void operator++(int);
  auto operator*() const -> const value_type &;
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  // Implementation details hidden from public interface
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// One changes_since result, which is one ingest call: where the slice starts
// and which history it is from, and the entries, read lazily.
export struct ChangeBatch {
  ChangeHeader header;
  std::ranges::subrange<ChangeIterator, std::default_sentinel_t> entries;
};

// ---------------------------------------------------------------------------
// ChangeIterator implementation
// ---------------------------------------------------------------------------
// Lazy ChangeIterator: walks files in min_sequence order, scanning one
// batch at a time. Only the current batch's entries are held in memory.
// Files in a single-writer engine have disjoint, monotonically increasing
// sequence ranges, so iterating files by min_sequence and scanning forward
// within each file yields globally ascending sequence order.
//
// Only the first file can hold from_sequence; it is entered at the index's
// bookmark below from_sequence when there is one that holds, else at its first
// byte. Every file is scanned with the index's plan for it, and the bookmarks
// laid are handed to the index when the scan leaves the file or ends, so a
// slice that stops early, or is dropped, still indexes what it crossed.
class ChangeIterator::Impl {
public:
  Impl(std::shared_ptr<const EngineState> state,
       std::shared_ptr<ChangeIndex> index,
       std::uint64_t from_sequence,
       std::uint64_t durable_sequence,
       std::size_t max_bytes)
    : state_(std::move(state)), index_(std::move(index)),
      from_sequence_(from_sequence),
      durable_sequence_(durable_sequence),
      // Counted in 64 bits whatever size_t is, so a scan past 4 GiB on a
      // 32-bit build neither wraps the count nor ends an unlimited slice.
      max_bytes_(max_bytes == kUnlimitedBytes
                     ? std::numeric_limits<std::uint64_t>::max()
                     : static_cast<std::uint64_t>(max_bytes)) {

    // Build sorted file list — O(num_files), typically tiny.
    for (const auto [file_id, stats] : state_->file_stats.all()) {
      if (stats.max_sequence > from_sequence && stats.min_sequence <= durable_sequence) {
        file_queue_.emplace_back(stats.min_sequence, file_id);
      }
    }
    std::sort(file_queue_.begin(), file_queue_.end());

    // Position at first valid entry.
    advance_to_next_valid();
  }

  ~Impl() { hand_over_bookmarks(); }
  Impl(const Impl &) = delete;
  auto operator=(const Impl &) -> Impl & = delete;

  auto has_more() const -> bool { return has_entry_; }

  auto current() const -> const DataEntryView& { return cached_entry_; }

  void advance() {
    advance_to_next_valid();
  }

private:
  // Scans forward across entries and files until the next entry of the
  // slice is found, the slice is cut, or all files are exhausted. The cached
  // BytesView spans point into the iterator's current entry, so we must NOT
  // advance the iterator after caching — that would invalidate the view.
  // Instead, we set needs_advance_ and increment on the next call.
  //
  // The slice is whole units. A unit starts at any entry outside a batch; it
  // is in the slice when its first sequence is above from_sequence_, so a
  // batch that straddles from_sequence_ is left out whole. Durability is
  // per unit too: the durable sequence always sits on a unit boundary, so
  // the first entry above it ends the slice, and a torn batch at the end of
  // the active file, which lies above it, is never reached.
  void advance_to_next_valid() {
    has_entry_ = false;

    // Advance past the previously cached entry (deferred from last call).
    // Set only once an entry was read from entry_iter_.
    if (needs_advance_) {
      ++(*entry_iter_);
      needs_advance_ = false;
    }
    if (done_) return;

    while (true) {
      // Try next entry in the current file.
      if (entry_iter_ && !(*entry_iter_ == std::default_sentinel)) {
        const auto& [entry, entry_off] = **entry_iter_;
        if (!in_batch_) {
          included_ = entry.sequence > from_sequence_;
          // A bookmark names a durable unit boundary: nothing above the durable
          // sequence, which resume() may yet cut.
          if (entry.sequence <= durable_sequence_) lay_bookmark(entry.sequence, entry_off);
        }
        if (entry.entry_type == EntryType::BulkBegin) in_batch_ = true;
        else if (entry.entry_type == EntryType::BulkEnd) in_batch_ = false;
        if (!included_) {
          ++(*entry_iter_);
          continue;
        }
        if (entry.sequence > durable_sequence_) {
          finish();
          return;
        }
        cache_entry(entry);
        needs_advance_ = true;
        sent_ += entry_size(entry.key.size(), entry.value.size());
        // Cut after the unit that passes max_bytes: never inside a batch.
        if (sent_ >= max_bytes_ && !in_batch_) finish();
        return;
      }

      // Try next file. A batch never spans two files.
      if (file_idx_ < file_queue_.size()) {
        hand_over_bookmarks();
        const auto [min_sequence, file_id] = file_queue_[file_idx_];
        ++file_idx_;
        in_batch_ = false;
        auto file_ptr = state_->files.get(file_id);
        if (file_ptr) {
          file_id_ = file_id;
          plan_ = index_->plan(file_id);
          laying_ = true;
          Offset start = 0;
          if (min_sequence <= from_sequence_) start = seek(**file_ptr);
          entry_iter_.emplace(DataFileIterator{**file_ptr, start});
        }
        continue;
      }

      // All files exhausted.
      hand_over_bookmarks();
      return;
    }
  }

  // The slice ended: cut, or past durable. Its bookmarks go to the index now,
  // not when the caller is done with the slice.
  void finish() {
    done_ = true;
    hand_over_bookmarks();
  }

  // Where the scan of the file holding from_sequence_ starts: the index's
  // bookmark below it, if the header there carries the bookmark's sequence, else
  // the first byte, with the file's table dropped for the scan to rebuild.
  // A bookmark can only be wrong by a bug, since nothing it names moves; the
  // check costs one header read.
  auto seek(const DataFile& file) -> Offset {
    auto& counters = index_->counters();
    if (auto bookmark = index_->seek(file_id_, from_sequence_)) {
      if (bookmark_holds(file, *bookmark)) {
        counters.hits.fetch_add(1, std::memory_order_relaxed);
        return bookmark->offset;
      }
      index_->forget(file_id_);
      plan_ = index_->plan(file_id_);
    }
    counters.misses.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }

  static auto bookmark_holds(const DataFile& file, const Bookmark& bookmark) -> bool {
    std::array<std::byte, kHeaderSize> header;
    if (file.read_raw(bookmark.offset, header) < kHeaderSize) return false;
    return read_header(header).sequence == bookmark.sequence;
  }

  // A bookmark at the first unit boundary at or past where the plan put the
  // next one. The entry at offset starts a unit at or below the durable
  // sequence. Every kHandOverBookmarks bookmarks are handed over on the spot, so
  // a scan holds at most that many (4 KiB) whatever the file's size, and
  // goes on with the plan the index returns: the table may have thinned, or
  // another scan may have carried it further. A scan whose table is gone
  // lays no more for the file: bookmarks from the middle of a file must not
  // start a table, which a scan from the first byte could then never fill
  // in, since plan() would start it past them.
  static constexpr std::size_t kHandOverBookmarks = 256;
  void lay_bookmark(std::uint64_t sequence, Offset offset) {
    if (!laying_) return;
    if (offset < plan_.next_offset) return;
    pending_bookmarks_.push_back({sequence, offset});
    plan_.next_offset = offset + plan_.spacing;
    if (pending_bookmarks_.size() >= kHandOverBookmarks) hand_over_bookmarks();
  }

  // Hands the bookmarks laid in the current file to the index and takes the
  // plan to go on with. The index is a cache: if it cannot take them, the
  // slice is unaffected and the next scan lays them again.
  void hand_over_bookmarks() noexcept {
    if (pending_bookmarks_.empty()) return;
    try {
      if (auto next = index_->extend(file_id_, plan_.generation, pending_bookmarks_)) {
        plan_ = *next;
      } else {
        laying_ = false;
      }
    } catch (...) {
    }
    pending_bookmarks_.clear();
  }

  // Caches the current entry. The BytesView spans point into
  // the iterator's internal buffer which stays alive until advance().
  void cache_entry(const DataEntry& entry) {
    cached_entry_ = {
      .sequence = entry.sequence,
      .entry_type = entry.entry_type,
      .key = std::span<const std::byte>{entry.key},
      .value = std::span<const std::byte>{entry.value}
    };
    has_entry_ = true;
  }

  std::shared_ptr<const EngineState> state_;
  std::shared_ptr<ChangeIndex> index_;
  std::uint64_t from_sequence_;
  std::uint64_t durable_sequence_;
  std::uint64_t max_bytes_;
  std::uint64_t sent_{0};   // bytes of the entries yielded so far
  bool in_batch_{false};    // inside a BulkBegin..BulkEnd of the current file
  bool included_{false};    // whether the current unit is in the slice
  bool done_{false};        // the slice ended: cut, or past durable

  // File traversal — sorted by min_sequence.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> file_queue_;
  std::size_t file_idx_{0};

  // Current file's committed entry scanner, and the bookmarks laid in it.
  std::optional<CommittedEntryIterator> entry_iter_;
  bool needs_advance_{false};
  std::uint32_t file_id_{0};
  BookmarkPlan plan_{};
  bool laying_{false};  // false once the table this scan extends is gone
  std::vector<Bookmark> pending_bookmarks_;

  // Cached current entry — BytesView into entry_iter_'s internal buffer.
  bool has_entry_{false};
  DataEntryView cached_entry_;
};

// ChangeIterator methods
ChangeIterator::ChangeIterator(std::shared_ptr<const EngineState> state,
                               std::shared_ptr<ChangeIndex> index,
                               std::uint64_t from_sequence,
                               std::uint64_t durable_sequence,
                               std::size_t max_bytes)
  : impl_(std::make_unique<Impl>(std::move(state), std::move(index),
                                 from_sequence, durable_sequence, max_bytes)) {}

ChangeIterator::~ChangeIterator() = default;

ChangeIterator::ChangeIterator(ChangeIterator&&) noexcept = default;
ChangeIterator& ChangeIterator::operator=(ChangeIterator&&) noexcept = default;

auto ChangeIterator::operator++() -> ChangeIterator& {
  if (impl_) {
    impl_->advance();
  }
  return *this;
}

void ChangeIterator::operator++(int) {
  ++(*this);
}

auto ChangeIterator::operator*() const -> const value_type& {
  return impl_->current();
}

auto ChangeIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || !impl_->has_more();
}

}  // namespace bytecask
