// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — change index: sparse per-file bookmarks for changes_since (#443)
//
// A data file has no seek by sequence: a slice starting in the middle of a
// file would otherwise be parsed from the file's first byte, on every pull.
// Each changes_since scan lays a bookmark (sequence, offset) at the first unit
// boundary after every `spacing` bytes it crosses, and a later slice starts at
// the last bookmark at or below its from_sequence.
//
// A cache, not state: in memory only, built by scans, never persisted. Every
// bookmark names a unit boundary that was durable when the scan saw it, in a
// file that is append-only, so it holds for the file's life; vacuum writes
// into a new file id and an id is never reused. The scan checks the header at
// the bookmark all the same, and drops the file's table if it does not carry the
// bookmark's sequence. Bounded: at most max_files tables, least recently used
// out, and a table that reaches max_bookmarks halves its density, so a file of
// any size costs at most max_bookmarks bookmarks.

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

export struct Bookmark {
  std::uint64_t sequence;  // first sequence of the unit that starts at offset
  Offset offset;
  auto operator==(const Bookmark &) const -> bool = default;
};

// Where a scan of a file lays its next bookmark, how far apart they go, and
// which table it is extending: 0 is no table, and a scan that holds a plan
// for no table began at the file's first byte. A table lost to eviction is
// not recreated from its tail: extend() takes bookmarks only for the table the
// plan was taken from, so a table always starts at the file's first bookmark.
export struct BookmarkPlan {
  Offset next_offset;
  Offset spacing;
  std::uint64_t generation;
};

export struct ChangeIndexCounters {
  std::atomic<std::int64_t> hits{0};    // slices that started at a bookmark
  std::atomic<std::int64_t> misses{0};  // slices that started at the file's first byte
};

export class ChangeIndex {
public:
  static constexpr std::size_t kMaxFiles = 16;
  static constexpr Offset kBookmarkSpacing = 4096;
  static constexpr std::size_t kMaxBookmarksPerFile = 16384;  // 256 KiB per file

  explicit ChangeIndex(std::size_t max_files = kMaxFiles,
                       std::size_t max_bookmarks = kMaxBookmarksPerFile)
      : max_files_{max_files}, max_bookmarks_{max_bookmarks} {}

  // The last bookmark of file_id at or below `from`: where a scan for the
  // sequences above `from` can start. The unit there is at or below `from`,
  // so the scan's own filter skips it and misses nothing between the bookmark
  // and `from`. nullopt if the file has no table, or none that low.
  [[nodiscard]] auto seek(std::uint32_t file_id, std::uint64_t from)
      -> std::optional<Bookmark> {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) return std::nullopt;
    it->second.used = ++clock_;
    const auto &bookmarks = it->second.bookmarks;
    auto above = std::ranges::upper_bound(
        bookmarks, from, {}, [](const Bookmark &f) { return f.sequence; });
    if (above == bookmarks.begin()) return std::nullopt;
    return *std::prev(above);
  }

  // Where a scan of file_id lays its next bookmark. A file without a table
  // starts a spacing in: a bookmark at offset 0 would save nothing.
  [[nodiscard]] auto plan(std::uint32_t file_id) const -> BookmarkPlan {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) return {kBookmarkSpacing, kBookmarkSpacing, 0};
    return it->second.plan();
  }

  // Appends the bookmarks a scan laid, in offset order, to the table its plan
  // was taken from, and returns the table's plan now, for the scan to go
  // on with: the table may have thinned, or another scan carried it
  // further. A bookmark not a spacing past the table's last is dropped: a
  // concurrent scan laid it first. A table that reaches max_bookmarks keeps
  // every other bookmark and doubles its spacing. A plan for no table creates
  // the table, the least recently used one out past max_files. nullopt when
  // the bookmarks went to no table: the plan's table is gone, or another has
  // taken its place; the scan then lays no more for this file.
  [[nodiscard]] auto extend(std::uint32_t file_id, std::uint64_t generation,
                            std::span<const Bookmark> bookmarks)
      -> std::optional<BookmarkPlan> {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    if (it == tables_.end()) {
      if (generation != 0) return std::nullopt;
      if (tables_.size() >= max_files_) evict_oldest();
      it = tables_.try_emplace(file_id).first;
      it->second.generation = ++clock_;
    } else if (it->second.generation != generation) {
      return std::nullopt;
    }
    auto &t = it->second;
    t.used = ++clock_;
    for (const auto &f : bookmarks) {
      const auto floor =
          t.bookmarks.empty() ? t.spacing : t.bookmarks.back().offset + t.spacing;
      if (f.offset < floor) continue;
      t.bookmarks.push_back(f);
      if (t.bookmarks.size() >= max_bookmarks_) t.thin();
    }
    if (t.bookmarks.empty()) {  // nothing accepted: no table
      tables_.erase(it);
      return std::nullopt;
    }
    return t.plan();
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
    for (const auto &[_, t] : tables_) n += t.bookmarks.size() * sizeof(Bookmark);
    return n;
  }
  [[nodiscard]] auto file_ids() const -> std::vector<std::uint32_t> {
    std::lock_guard<std::mutex> lk{mu_};
    std::vector<std::uint32_t> ids;
    for (const auto &[id, _] : tables_) ids.push_back(id);
    std::ranges::sort(ids);
    return ids;
  }
  [[nodiscard]] auto bookmarks(std::uint32_t file_id) const
      -> std::vector<Bookmark> {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    return it == tables_.end() ? std::vector<Bookmark>{} : it->second.bookmarks;
  }
  [[nodiscard]] auto spacing(std::uint32_t file_id) const -> Offset {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = tables_.find(file_id);
    return it == tables_.end() ? kBookmarkSpacing : it->second.spacing;
  }

private:
  struct Table {
    std::vector<Bookmark> bookmarks;  // ascending in sequence and offset
    Offset spacing{kBookmarkSpacing};
    std::uint64_t used{0};        // clock_ at the last seek or extend
    std::uint64_t generation{0};  // clock_ at creation; never 0

    [[nodiscard]] auto plan() const -> BookmarkPlan {
      const auto last = bookmarks.empty() ? Offset{0} : bookmarks.back().offset;
      return {last + spacing, spacing, generation};
    }

    // Keeps every other bookmark, the first included, and doubles the spacing;
    // what remains is still at least a spacing apart.
    void thin() {
      std::size_t kept = 0;
      for (std::size_t i = 0; i < bookmarks.size(); i += 2) bookmarks[kept++] = bookmarks[i];
      bookmarks.resize(kept);
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
  std::size_t max_bookmarks_;
  ChangeIndexCounters counters_;
};

}  // namespace bytecask
