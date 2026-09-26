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
    if (emit_idx_ >= pending_size_) {
      advance();
    }
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return emit_idx_ >= pending_size_;
  }

  // Byte offset past the last committed entry or batch yielded.
  // After exhaustion, this is the offset past all committed data —
  // useful for resume() to know where to truncate.
  [[nodiscard]] auto committed_offset() const noexcept -> Offset {
    return committed_offset_;
  }

private:
  // pending_ keeps its slots across advances and stage() assigns into them,
  // so a sweep does not allocate per entry once the slots have grown.
  void stage(const value_type& e) {
    if (pending_size_ == pending_.size()) {
      pending_.push_back(e);
    } else {
      auto& [entry, off] = pending_[pending_size_];
      entry.sequence = e.first.sequence;
      entry.entry_type = e.first.entry_type;
      entry.key.assign(e.first.key.begin(), e.first.key.end());
      entry.value.assign(e.first.value.begin(), e.first.value.end());
      off = e.second;
    }
    ++pending_size_;
  }

  void advance() {
    pending_size_ = 0;
    emit_idx_ = 0;
    if (step_pending_) {
      step_pending_ = false;
      ++cur_;
    }

    while (!(cur_ == std::default_sentinel)) {
      const auto& entry = (*cur_).first;

      if (entry.entry_type == EntryType::BulkBegin) {
        // Buffer entries until matching BulkEnd or EOF.
        stage(*cur_);
        ++cur_;

        while (!(cur_ == std::default_sentinel)) {
          stage(*cur_);
          if ((*cur_).first.entry_type == EntryType::BulkEnd) {
            committed_offset_ = cur_.next_offset();
            step_pending_ = true;
            return;
          }
          ++cur_;
        }
        // EOF before BulkEnd — discard incomplete batch.
        pending_size_ = 0;
        continue;
      }

      // Standalone entry (Put, Delete, RangeDel).
      committed_offset_ = cur_.next_offset();
      stage(*cur_);
      step_pending_ = true;
      return;
    }
  }

  DataFileIterator cur_;
  std::vector<value_type> pending_;
  std::size_t pending_size_{0};  // live prefix of pending_
  std::size_t emit_idx_{0};
  Offset committed_offset_{};
  // The scan past the last committed entry or batch is deferred to the next
  // advance(), so the entry after it is parsed only once the caller moves on.
  // A damaged entry then throws out of the operator++ that steps past an
  // intact one the caller has already seen and counted in committed_offset(),
  // not out of the one that was about to yield it. Parse and I/O errors still
  // propagate to every caller: this iterator never decides that damage is
  // the end of the file.
  bool step_pending_{false};
};

export inline auto scan_committed(const DataFile& file, Offset start = 0)
    -> std::ranges::subrange<CommittedEntryIterator, std::default_sentinel_t> {
  return {CommittedEntryIterator{DataFileIterator{file, start}},
          std::default_sentinel};
}

} // namespace bytecask
