// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — change index: sparse per-file fences for changes_since (#443)
//
// A data file has no seek by sequence: a slice starting in the middle of a
// file would otherwise be parsed from the file's first byte, on every pull.
// Each changes_since sweep lays a fence (sequence, offset) at the first unit
// boundary after every `spacing` bytes it crosses, and a later slice starts at
// the last fence at or below its from_sequence.
//
// A cache, not state: in memory only, built by sweeps, never persisted. Every
// fence names a unit boundary that was durable when the sweep saw it, in a
// file that is append-only, so it holds for the file's life; vacuum writes
// into a new file id and an id is never reused. The sweep checks the header at
// the fence all the same, and drops the file's table if it does not carry the
// fence's sequence. Bounded: at most max_files tables, least recently used
// out, and a table that reaches max_fences halves its density, so a file of
// any size costs at most max_fences fences.

module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

export module bytecask.change_index;

import bytecask.data_file;

namespace bytecask {

export struct ChangeFence {
  std::uint64_t sequence;  // first sequence of the unit that starts at offset
  Offset offset;
  auto operator==(const ChangeFence &) const -> bool = default;
};

// Where a sweep of a file lays its next fence, and how far apart they go.
export struct FencePlan {
  Offset next_offset;
  Offset spacing;
};

export struct ChangeIndexCounters {
  std::atomic<std::int64_t> hits{0};    // slices that started at a fence
  std::atomic<std::int64_t> misses{0};  // slices that started at the file's first byte
};

export class ChangeIndex {
public:
  static constexpr std::size_t kMaxFiles = 16;
  static constexpr Offset kFenceSpacing = 4096;
  static constexpr std::size_t kMaxFencesPerFile = 16384;  // 256 KiB per file

  explicit ChangeIndex(std::size_t max_files = kMaxFiles,
                       std::size_t max_fences = kMaxFencesPerFile)
      : max_files_{max_files}, max_fences_{max_fences} {}

  // The last fence of file_id at or below `from`: where a sweep for the
  // sequences above `from` can start. The unit there is at or below `from`,
  // so the sweep's own filter skips it and misses nothing between the fence
  // and `from`. nullopt if the file has no table, or none that low.
  [[nodiscard]] auto seek(std::uint32_t file_id, std::uint64_t from)
      -> std::optional<ChangeFence> {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) return std::nullopt;
    it->second.used = ++clock_;
    const auto &fences = it->second.fences;
    auto above = std::ranges::upper_bound(
        fences, from, {}, [](const ChangeFence &f) { return f.sequence; });
    if (above == fences.begin()) return std::nullopt;
    return *std::prev(above);
  }

  // Where a sweep of file_id lays its next fence. A file without a table
  // starts a spacing in: a fence at offset 0 would save nothing.
  [[nodiscard]] auto plan(std::uint32_t file_id) const -> FencePlan {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) return {kFenceSpacing, kFenceSpacing};
    const auto &t = it->second;
    const auto last = t.fences.empty() ? Offset{0} : t.fences.back().offset;
    return {last + t.spacing, t.spacing};
  }

  // Appends the fences a sweep laid, in offset order. One not a spacing past
  // the table's last fence is dropped: a concurrent sweep laid it first. A
  // table that reaches max_fences keeps every other fence and doubles its
  // spacing. Creates the table, the least recently used one out past
  // max_files.
  void extend(std::uint32_t file_id, std::span<const ChangeFence> fences) {
    if (fences.empty()) return;
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) {
      if (tables_.size() >= max_files_) evict_oldest();
      it = tables_.try_emplace(file_id).first;
    }
    auto &t = it->second;
    t.used = ++clock_;
    for (const auto &f : fences) {
      const auto floor =
          t.fences.empty() ? t.spacing : t.fences.back().offset + t.spacing;
      if (f.offset < floor) continue;
      t.fences.push_back(f);
      if (t.fences.size() >= max_fences_) t.thin();
    }
    if (t.fences.empty()) tables_.erase(it);  // nothing accepted: no table
  }

  void forget(std::uint32_t file_id) {
    std::lock_guard<std::mutex> lk{mu_};
    tables_.erase(file_id);
  }

  void clear() {
    std::lock_guard<std::mutex> lk{mu_};
    tables_.clear();
  }

  [[nodiscard]] auto counters() noexcept -> ChangeIndexCounters & {
    return counters_;
  }

  // Gauges for stats(), and the tables for tests.
  [[nodiscard]] auto files() const -> std::size_t {
    std::lock_guard<std::mutex> lk{mu_};
    return tables_.size();
  }
  [[nodiscard]] auto bytes() const -> std::size_t {
    std::lock_guard<std::mutex> lk{mu_};
    std::size_t n = 0;
    for (const auto &[_, t] : tables_) n += t.fences.size() * sizeof(ChangeFence);
    return n;
  }
  [[nodiscard]] auto file_ids() const -> std::vector<std::uint32_t> {
    std::lock_guard<std::mutex> lk{mu_};
    std::vector<std::uint32_t> ids;
    for (const auto &[id, _] : tables_) ids.push_back(id);
    std::ranges::sort(ids);
    return ids;
  }
  [[nodiscard]] auto fences(std::uint32_t file_id) const
      -> std::vector<ChangeFence> {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    return it == tables_.end() ? std::vector<ChangeFence>{} : it->second.fences;
  }
  [[nodiscard]] auto spacing(std::uint32_t file_id) const -> Offset {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    return it == tables_.end() ? kFenceSpacing : it->second.spacing;
  }

private:
  struct Table {
    std::vector<ChangeFence> fences;  // ascending in sequence and offset
    Offset spacing{kFenceSpacing};
    std::uint64_t used{0};  // clock_ at the last seek or extend

    // Keeps every other fence, the first included, and doubles the spacing;
    // what remains is still at least a spacing apart.
    void thin() {
      std::size_t kept = 0;
      for (std::size_t i = 0; i < fences.size(); i += 2) fences[kept++] = fences[i];
      fences.resize(kept);
      spacing *= 2;
    }
  };

  void evict_oldest() {
    auto oldest = std::ranges::min_element(
        tables_, {}, [](const auto &kv) { return kv.second.used; });
    tables_.erase(oldest);
  }

  mutable std::mutex mu_;
  std::unordered_map<std::uint32_t, Table> tables_;
  std::uint64_t clock_{0};
  std::size_t max_files_;
  std::size_t max_fences_;
  ChangeIndexCounters counters_;
};

}  // namespace bytecask
