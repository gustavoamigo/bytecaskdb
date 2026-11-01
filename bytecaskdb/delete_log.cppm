// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — DeleteLog: the deletes recent writes made, each with its
// sequence. A `del` is logged by its key, a `del_range` by its bounds.
//
// A plan that carries only the sequence it read at (its read sequence R)
// can see from the key directory alone whether a present key changed since
// R: its entry's sequence is above R. A key absent now leaves no entry, so
// "absent at R too" and "deleted since R" look the same. This log tells
// them apart for every R at or above horizon(); below it the deletes have
// been dropped, and a plan read there is too old to check. A delete counts
// for every key it names, present or not: a `del_range` costs one entry
// whatever it covers.
//
// In memory only: the data files are the record. An open starts the log
// empty with the horizon at the latest recovered sequence, so a plan read
// before a restart is too old. Bounded by max_bytes: past it the oldest
// deletes are dropped and the horizon rises to the newest dropped one. With
// max_bytes 0 nothing is kept: every delete raises the horizon.
//
// Thread-safety: record and reset are called under the DB's write lock; the
// queries run there and, for a plan that appended nothing, on its writer
// thread against the published state (DB::settle_unappended). An internal
// mutex serialises the two. StagedRemovals is not thread-safe: it belongs to
// one TransientEngineState.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <vector>

export module bytecask.delete_log;

namespace bytecask {

using LogBytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

// Byte-wise order, the key directory's: std::byte compares unsigned.
export [[nodiscard]] inline auto byte_less(ByteView a, ByteView b) -> bool {
  return std::ranges::lexicographical_compare(a, b);
}

export struct ByteLess {
  using is_transparent = void;
  auto operator()(ByteView a, ByteView b) const -> bool {
    return byte_less(a, b);
  }
};

// key in [from, to).
export [[nodiscard]] inline auto key_in_range(ByteView key, ByteView from,
                                              ByteView to) -> bool {
  return !byte_less(key, from) && byte_less(key, to);
}

// [a_from, a_to) and [b_from, b_to) share a key. Both ranges are non-empty.
export [[nodiscard]] inline auto ranges_overlap(ByteView a_from, ByteView a_to,
                                                ByteView b_from,
                                                ByteView b_to) -> bool {
  return byte_less(a_from, b_to) && byte_less(b_from, a_to);
}

// A del_range's bounds and the sequence that wrote it.
export struct RangeRemoval {
  LogBytes from;
  LogBytes to;
  std::uint64_t seq;
};

export class DeleteLog;

// The deletes one group's slots applied so far, indexed for the checks of
// the group's later slots. Every one is above any read sequence the group
// checks (apply_batch refuses one past the published state, and the group
// only writes above it), so being here is the whole answer. Handed to the
// DeleteLog once the group is appended (DeleteLog::record).
export class StagedRemovals {
public:
  void add_key(ByteView key, std::uint64_t seq) {
    keys_.insert_or_assign(LogBytes{key.begin(), key.end()}, seq);
    last_seq_ = seq;
  }
  void add_range(ByteView from, ByteView to, std::uint64_t seq) {
    ranges_.push_back({{from.begin(), from.end()}, {to.begin(), to.end()}, seq});
    last_seq_ = seq;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return last_seq_ == 0; }

  // Whether a staged delete names key.
  [[nodiscard]] auto removes(ByteView key) const -> bool {
    if (keys_.contains(key)) return true;
    return std::ranges::any_of(ranges_, [&](const RangeRemoval &r) {
      return key_in_range(key, r.from, r.to);
    });
  }

  // Whether a staged delete names a key in [from, to).
  [[nodiscard]] auto removes_any_in(ByteView from, ByteView to) const -> bool {
    const auto it = keys_.lower_bound(from);
    if (it != keys_.end() && byte_less(it->first, to)) return true;
    return std::ranges::any_of(ranges_, [&](const RangeRemoval &r) {
      return ranges_overlap(r.from, r.to, from, to);
    });
  }

private:
  friend class DeleteLog;
  // Each key's latest staged delete: an earlier one in the same group adds
  // nothing, since a state the log is checked against holds the whole group
  // or none of it.
  std::map<LogBytes, std::uint64_t, ByteLess> keys_;
  std::vector<RangeRemoval> ranges_;  // ascending sequence
  std::uint64_t last_seq_{0};         // 0: nothing staged
};

export class DeleteLog {
public:
  explicit DeleteLog(std::size_t max_bytes) : max_bytes_{max_bytes} {}
  DeleteLog(const DeleteLog &) = delete;
  auto operator=(const DeleteLog &) -> DeleteLog & = delete;

  // Forgets every delete: a plan read below `latest` is too old from now on.
  // Used at open, and wherever the key directory changes by a path that does
  // not record its deletes (resume(), ingest).
  void reset(std::uint64_t latest) noexcept {
    std::lock_guard<std::mutex> lk{mu_};
    clear_locked(latest);
  }

  // A group's deletes, once the group is appended; takes them out of
  // `staged`. Storing them may fail for want of memory: the log then forgets
  // everything instead, which makes older plans too old rather than
  // unchecked, since every delete lost is at or below the group's last.
  void record(StagedRemovals &staged) noexcept {
    if (staged.empty()) return;
    const auto last = staged.last_seq_;
    std::lock_guard<std::mutex> lk{mu_};
    if (max_bytes_ == 0) {
      clear_locked(last);
    } else {
      try {
        record_locked(staged);
      } catch (...) {
        clear_locked(last);
      }
      while (bytes_ > max_bytes_) drop_oldest_locked();
    }
    staged.keys_.clear();
    staged.ranges_.clear();
    staged.last_seq_ = 0;
  }

  // Whether a plan read at read_seq can be checked: every delete above it is
  // still here.
  [[nodiscard]] auto covers(std::uint64_t read_seq) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    return read_seq >= horizon_;
  }

  // Whether a delete above read_seq and below `below` named key. `below` is
  // the next sequence of the state being checked: deletes a later state made
  // are not part of it.
  [[nodiscard]] auto removed(ByteView key, std::uint64_t read_seq,
                             std::uint64_t below) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    if (key_removed_locked(key, read_seq, below)) return true;
    return any_range_locked(read_seq, below, [&](const RangeRemoval &r) {
      return key_in_range(key, r.from, r.to);
    });
  }

  // Whether a delete above read_seq and below `below` named a key in
  // [from, to). Point deletes are looked up by key, so the cost is the
  // deletes logged inside the range, not every delete since read_seq.
  [[nodiscard]] auto removed_in(ByteView from, ByteView to,
                                std::uint64_t read_seq,
                                std::uint64_t below) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    auto past_below = false;
    for (auto it = latest_.lower_bound(from); it != latest_.end(); ++it) {
      if (!byte_less(it->first, to)) break;
      if (it->second >= below) {
        past_below = true;
      } else if (it->second > read_seq) {
        return true;
      }
    }
    // A key whose latest delete is past `below` may have an earlier one in
    // the window.
    if (past_below) {
      const auto in_range = [&](const LogBytes &k) {
        return key_in_range(k, from, to);
      };
      if (any_key_locked(read_seq, below, in_range)) return true;
    }
    return any_range_locked(read_seq, below, [&](const RangeRemoval &r) {
      return ranges_overlap(r.from, r.to, from, to);
    });
  }

  [[nodiscard]] auto horizon() const -> std::uint64_t {
    std::lock_guard<std::mutex> lk{mu_};
    return horizon_;
  }
  [[nodiscard]] auto bytes() const -> std::size_t {
    std::lock_guard<std::mutex> lk{mu_};
    return bytes_;
  }

private:
  using Map = std::map<LogBytes, std::uint64_t, ByteLess>;
  struct KeyRemoval {
    std::uint64_t seq;
    Map::iterator key;  // map iterators stay valid until their node is erased
  };

  // Rough cost of each record beyond its key or bound bytes, for the
  // max_bytes budget: a map node plus its queue slot, a queue slot, a range.
  static constexpr std::size_t kNodeBytes = 64;
  static constexpr std::size_t kKeyRemovalBytes = 16;
  static constexpr std::size_t kRangeBytes = 64;

  void record_locked(StagedRemovals &staged) {
    // The group's point deletes join order_ in sequence order; the staged map
    // holds them in key order.
    std::vector<KeyRemoval> group;
    group.reserve(staged.keys_.size());
    for (auto &[key, seq] : staged.keys_) {
      auto [it, inserted] = latest_.try_emplace(key, seq);
      if (inserted) {
        bytes_ += it->first.size() + kNodeBytes;
      } else {
        it->second = seq;
      }
      group.push_back({seq, it});
      bytes_ += kKeyRemovalBytes;
    }
    std::ranges::sort(group, {}, &KeyRemoval::seq);
    order_.insert(order_.end(), group.begin(), group.end());
    for (auto &r : staged.ranges_) {
      bytes_ += r.from.size() + r.to.size() + kRangeBytes;
      ranges_.push_back(std::move(r));
    }
  }

  // The key's latest delete answers alone unless it is at or above `below`;
  // then an earlier one may still fall in the window.
  auto key_removed_locked(ByteView key, std::uint64_t read_seq,
                          std::uint64_t below) const -> bool {
    const auto it = latest_.find(key);
    if (it == latest_.end()) return false;
    if (it->second < below) return it->second > read_seq;
    return any_key_locked(read_seq, below, [&](const LogBytes &k) {
      return std::ranges::equal(k, key);
    });
  }

  // Walks the point deletes newer than read_seq, newest first.
  template <typename Pred>
  auto any_key_locked(std::uint64_t read_seq, std::uint64_t below,
                      Pred matches) const -> bool {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      if (it->seq <= read_seq) return false;
      if (it->seq >= below) continue;
      if (matches(it->key->first)) return true;
    }
    return false;
  }

  // Walks the range deletes newer than read_seq, newest first.
  template <typename Pred>
  auto any_range_locked(std::uint64_t read_seq, std::uint64_t below,
                        Pred matches) const -> bool {
    for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
      if (it->seq <= read_seq) return false;
      if (it->seq >= below) continue;
      if (matches(*it)) return true;
    }
    return false;
  }

  // Drops the oldest delete, point or range. Called only while bytes_ > 0,
  // so one of the two queues holds something.
  void drop_oldest_locked() noexcept {
    if (ranges_.empty()) {
      drop_oldest_key_locked();
    } else if (order_.empty()) {
      drop_oldest_range_locked();
    } else if (order_.front().seq < ranges_.front().seq) {
      drop_oldest_key_locked();
    } else {
      drop_oldest_range_locked();
    }
  }

  void drop_oldest_key_locked() noexcept {
    const auto r = order_.front();
    order_.pop_front();
    bytes_ -= kKeyRemovalBytes;
    horizon_ = std::max(horizon_, r.seq);
    if (r.key->second == r.seq) {
      bytes_ -= r.key->first.size() + kNodeBytes;
      latest_.erase(r.key);
    }
  }

  void drop_oldest_range_locked() noexcept {
    const auto &r = ranges_.front();
    bytes_ -= r.from.size() + r.to.size() + kRangeBytes;
    horizon_ = std::max(horizon_, r.seq);
    ranges_.pop_front();
  }

  void clear_locked(std::uint64_t latest) noexcept {
    order_.clear();
    latest_.clear();
    ranges_.clear();
    bytes_ = 0;
    horizon_ = std::max(horizon_, latest);
  }

  mutable std::mutex mu_;
  std::size_t max_bytes_;
  std::size_t bytes_{0};
  std::uint64_t horizon_{0};
  // Each key's latest point delete, for lookups by key.
  Map latest_;
  // Every point delete in sequence order, for dropping the oldest; several
  // may name one key.
  std::deque<KeyRemoval> order_;
  // Every range delete in sequence order.
  std::deque<RangeRemoval> ranges_;
};

} // namespace bytecask
