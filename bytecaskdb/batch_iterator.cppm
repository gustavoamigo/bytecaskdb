// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — committed entry iterator over data file entries
//
// Yields individual std::pair<DataEntry, Offset> values, including
// BulkBegin/BulkEnd markers. Incomplete batches (BulkBegin without
// a matching BulkEnd before EOF) are silently discarded.

module;
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

export module bytecask.batch_iterator;

import bytecask.data_entry;
import bytecask.data_file;
import bytecask.types;

namespace bytecask {

// Forward-only iterator that wraps a DataFileIterator and yields
// committed entries one at a time. BulkBegin/BulkEnd markers appear
// as regular entries — consumers decide whether to use or skip them.
//
// When a BulkBegin is encountered, entries are buffered internally
// until the matching BulkEnd is found. If EOF arrives first, the
// incomplete batch is discarded. Standalone entries (not inside a
// batch) are yielded immediately with no buffering.
export class CommittedEntryIterator {
public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = std::pair<DataEntry, Offset>;
  using difference_type = std::ptrdiff_t;

  CommittedEntryIterator() = default;

  explicit CommittedEntryIterator(DataFileIterator cur)
      : cur_{std::move(cur)} {
    advance();
  }

  auto operator*() const -> const value_type& { return pending_[emit_idx_]; }

  auto operator++() -> CommittedEntryIterator& {
    ++emit_idx_;
    if (emit_idx_ >= pending_.size()) {
      advance();
    }
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return emit_idx_ >= pending_.size();
  }

  // Byte offset past the last committed entry or batch yielded.
  // After exhaustion, this is the offset past all committed data —
  // useful for resume() to know where to truncate.
  [[nodiscard]] auto committed_offset() const noexcept -> Offset {
    return committed_offset_;
  }

private:
  void advance() {
    pending_.clear();
    emit_idx_ = 0;

    while (!(cur_ == std::default_sentinel)) {
      const auto& [entry, entry_off] = *cur_;

      if (entry.entry_type == EntryType::BulkBegin) {
        // Buffer entries until matching BulkEnd or EOF.
        pending_.emplace_back(entry, entry_off);
        ++cur_;

        while (!(cur_ == std::default_sentinel)) {
          const auto& [inner, inner_off] = *cur_;
          pending_.emplace_back(inner, inner_off);
          if (inner.entry_type == EntryType::BulkEnd) {
            committed_offset_ = cur_.next_offset();
            ++cur_;
            return;
          }
          ++cur_;
        }
        // EOF before BulkEnd — discard incomplete batch.
        pending_.clear();
        continue;
      }

      // Standalone entry (Put, Delete, RangeDel).
      committed_offset_ = cur_.next_offset();
      pending_.emplace_back(entry, entry_off);
      ++cur_;
      return;
    }
  }

  DataFileIterator cur_;
  std::vector<value_type> pending_;
  std::size_t emit_idx_{0};
  Offset committed_offset_{};
};

export inline auto scan_committed(const DataFile& file, Offset start = 0)
    -> std::ranges::subrange<CommittedEntryIterator, std::default_sentinel_t> {
  return {CommittedEntryIterator{DataFileIterator{file, start}},
          std::default_sentinel};
}

} // namespace bytecask
