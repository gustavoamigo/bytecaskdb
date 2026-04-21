// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — batch-aware grouping iterator over data file entries

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

// A committed unit of work from a data file: either a standalone entry
// or a complete batch (BulkBegin ... BulkEnd). Incomplete batches at
// EOF are silently discarded.
export struct CommittedBatch {
  std::vector<std::pair<DataEntry, Offset>> entries;
  std::optional<std::uint64_t> bulk_begin_seq;  // set iff this was a batch
  std::optional<std::uint64_t> bulk_end_seq;     // set iff this was a batch
  Offset next_offset{};  // byte offset past the last entry/marker consumed

  [[nodiscard]] auto is_batch() const noexcept -> bool {
    return bulk_begin_seq.has_value();
  }
};

// Forward-only iterator that wraps a DataFileIterator and groups entries
// between BulkBegin/BulkEnd markers into CommittedBatch values.
// Standalone entries (not inside a batch) yield a single-entry CommittedBatch.
// Incomplete batches (BulkBegin without matching BulkEnd before EOF) are
// discarded. Exceptions from the underlying DataFileIterator propagate.
export class BatchGroupingIterator {
public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = CommittedBatch;
  using difference_type = std::ptrdiff_t;

  BatchGroupingIterator() = default;

  explicit BatchGroupingIterator(DataFileIterator cur)
      : cur_{std::move(cur)} {
    advance();
  }

  auto operator*() const -> const value_type& { return *cached_; }

  auto operator++() -> BatchGroupingIterator& {
    advance();
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return !cached_.has_value();
  }

private:
  void advance() {
    cached_.reset();

    while (!(cur_ == std::default_sentinel)) {
      const auto& [entry, entry_off] = *cur_;

      if (entry.entry_type == EntryType::BulkBegin) {
        auto batch_begin_seq = entry.sequence;

        // Accumulate all entries including BulkBegin/BulkEnd markers.
        std::vector<std::pair<DataEntry, Offset>> pending;
        pending.emplace_back(entry, entry_off);
        ++cur_;

        while (!(cur_ == std::default_sentinel)) {
          const auto& [inner_entry, inner_off] = *cur_;
          pending.emplace_back(inner_entry, inner_off);
          if (inner_entry.entry_type == EntryType::BulkEnd) {
            auto next_off = cur_.next_offset();
            cached_.emplace(CommittedBatch{
                std::move(pending),
                batch_begin_seq,
                inner_entry.sequence,
                next_off});
            ++cur_;
            return;
          }
          ++cur_;
        }
        // EOF before BulkEnd — discard incomplete batch, loop to check
        // if cur_ is at sentinel (it is, so the while condition exits).
        continue;
      }

      // Standalone entry (Put, Delete, RangeDel).
      auto next_off = cur_.next_offset();
      CommittedBatch batch;
      batch.entries.emplace_back(entry, entry_off);
      batch.next_offset = next_off;
      cached_.emplace(std::move(batch));
      ++cur_;
      return;
    }
  }

  DataFileIterator cur_;
  std::optional<CommittedBatch> cached_;
};

export inline auto scan_batches(const DataFile& file, Offset start = 0)
    -> std::ranges::subrange<BatchGroupingIterator, std::default_sentinel_t> {
  return {BatchGroupingIterator{DataFileIterator{file, start}},
          std::default_sentinel};
}

} // namespace bytecask
