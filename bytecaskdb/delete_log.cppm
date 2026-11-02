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
// Lookups cost the deletes that matter to the key or range asked about:
// point deletes are indexed by (key, sequence), range deletes by a map of
// disjoint key segments each stamped with the latest range delete that
// covered it (RangeSegments). A queue per kind keeps sequence order for
// dropping the oldest, and, for ranges, for the rare query against a state
// older than the newest range delete over a key (the `below` bound).
//
// The log trusts only the sequences it was handed. Every write group calls
// record() with its sequence range, deletes or not. A sequence the log never
// saw (an open's or a promotion's marker, an ingest, a group resume()
// replayed without its record) may have removed keys it does not hold, so
// it is a gap: a plan read below one is too old. No other path has to tell
// the log anything.
//
// In memory only: the data files are the record. Bounded by max_bytes: past
// it the oldest deletes are dropped and the horizon rises to the newest
// dropped one. With max_bytes 0 nothing is kept: every delete raises the
// horizon.
//
// Thread-safety: record is called under the DB's write lock; the queries run
// there and, for a plan that appended nothing, on its writer thread against
// the published state (DB::settle_unappended). An internal mutex serialises
// the two, and each query answers under one hold of it. StagedRemovals is
// not thread-safe: it belongs to one TransientEngineState.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <tuple>
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

// Disjoint key segments, each stamped with the latest range delete that
// covered it. Stamps arrive in ascending sequence, so a new one is the latest
// for everything it covers: stamping [from, to) splits the segments at its
// two bounds and replaces whatever lies between with one segment. Removing a
// stamp, oldest first, removes the segments that still carry it.
export class RangeSegments {
public:
  void stamp(ByteView from, ByteView to, std::uint64_t seq) {
    split_at(from);
    split_at(to);
    // Allocated before anything is erased, so a failure leaves every
    // segment as it was (the splits above change no stamp).
    Map one;
    one.try_emplace(LogBytes{from.begin(), from.end()},
                    Segment{LogBytes{to.begin(), to.end()}, seq});
    const auto first = segs_.lower_bound(from);
    const auto last = segs_.lower_bound(to);
    for (auto it = first; it != last; ++it) bytes_ -= cost(*it);
    segs_.erase(first, last);
    bytes_ += cost(*one.begin());
    segs_.insert(one.extract(one.begin()));
  }

  // The latest stamp covering key; 0 if none does.
  [[nodiscard]] auto latest_at(ByteView key) const -> std::uint64_t {
    auto it = segs_.upper_bound(key);
    if (it == segs_.begin()) return 0;
    --it;
    return byte_less(key, it->second.end) ? it->second.seq : 0;
  }

  // Whether `matches` holds for the stamp of a segment overlapping
  // [from, to).
  template <typename Pred>
  [[nodiscard]] auto any_overlapping(ByteView from, ByteView to,
                                     Pred matches) const -> bool {
    for (auto it = first_overlapping(from); it != segs_.end(); ++it) {
      if (!byte_less(it->first, to)) break;
      if (matches(it->second.seq)) return true;
    }
    return false;
  }

  // Removes the segments of [from, to) still stamped seq. Every older stamp
  // must be gone already: segments it alone covered would stay.
  void unstamp(ByteView from, ByteView to, std::uint64_t seq) noexcept {
    for (Map::const_iterator it = first_overlapping(from); it != segs_.end();) {
      if (!byte_less(it->first, to)) break;
      if (it->second.seq == seq) {
        bytes_ -= cost(*it);
        it = segs_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void clear() noexcept {
    segs_.clear();
    bytes_ = 0;
  }
  [[nodiscard]] auto bytes() const noexcept -> std::size_t { return bytes_; }

private:
  struct Segment {
    LogBytes end;
    std::uint64_t seq;
  };
  using Map = std::map<LogBytes, Segment, ByteLess>;  // by start

  // Rough cost of a segment beyond its bounds' bytes: a map node.
  static constexpr std::size_t kSegmentBytes = 64;
  static auto cost(const Map::value_type &s) noexcept -> std::size_t {
    return s.first.size() + s.second.end.size() + kSegmentBytes;
  }

  // The segment holding `from`, or else the first starting after it.
  [[nodiscard]] auto first_overlapping(ByteView from) const
      -> Map::const_iterator {
    auto it = segs_.upper_bound(from);
    if (it == segs_.begin()) return it;
    const auto prev = std::prev(it);
    return byte_less(from, prev->second.end) ? prev : it;
  }

  // Makes k a segment boundary: the segment strictly containing it becomes
  // two with its stamp. Allocates before it changes anything.
  void split_at(ByteView k) {
    auto it = segs_.upper_bound(k);
    if (it == segs_.begin()) return;
    --it;
    if (!byte_less(it->first, k)) return;      // k starts this segment
    if (!byte_less(k, it->second.end)) return; // k is past it
    Map piece;
    auto &tail = piece.try_emplace(LogBytes{k.begin(), k.end()},
                                   Segment{{}, it->second.seq})
                     .first->second;
    LogBytes head_end{k.begin(), k.end()};
    bytes_ -= cost(*it);
    tail.end = std::move(it->second.end);
    it->second.end = std::move(head_end);
    bytes_ += cost(*it) + cost(*piece.begin());
    segs_.insert(piece.extract(piece.begin()));
  }

  Map segs_;
  std::size_t bytes_{0};
};

export class DeleteLog;

// The deletes one group's slots applied so far, indexed for the checks of
// the group's later slots. Every one is above any read sequence the group
// checks (a read point at or past the head is TooOld, and the group only
// writes above it), so being here is the whole answer. Handed to the
// DeleteLog once the group is appended (DeleteLog::record).
export class StagedRemovals {
public:
  void add_key(ByteView key, std::uint64_t seq) {
    keys_.insert_or_assign(LogBytes{key.begin(), key.end()}, seq);
    last_seq_ = seq;
  }
  void add_range(ByteView from, ByteView to, std::uint64_t seq) {
    ranges_.push_back({{from.begin(), from.end()}, {to.begin(), to.end()}, seq});
    segments_.stamp(from, to, seq);
    last_seq_ = seq;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return last_seq_ == 0; }

  // Whether a staged delete names key.
  [[nodiscard]] auto removes(ByteView key) const -> bool {
    if (keys_.contains(key)) return true;
    return segments_.latest_at(key) != 0;
  }

  // Whether a staged delete names a key in [from, to).
  [[nodiscard]] auto removes_any_in(ByteView from, ByteView to) const -> bool {
    const auto it = keys_.lower_bound(from);
    if (it != keys_.end() && byte_less(it->first, to)) return true;
    return segments_.any_overlapping(from, to,
                                     [](std::uint64_t) { return true; });
  }

private:
  friend class DeleteLog;
  // Each key's latest staged delete: an earlier one in the same group adds
  // nothing, since a state the log is checked against holds the whole group
  // or none of it.
  std::map<LogBytes, std::uint64_t, ByteLess> keys_;
  std::vector<RangeRemoval> ranges_;  // ascending sequence, for the log
  RangeSegments segments_;            // ranges_, for lookups
  std::uint64_t last_seq_{0};         // 0: nothing staged
};

export class DeleteLog {
public:
  explicit DeleteLog(std::size_t max_bytes) : max_bytes_{max_bytes} {}
  DeleteLog(const DeleteLog &) = delete;
  auto operator=(const DeleteLog &) -> DeleteLog & = delete;

  // A group that assigned sequences [first, last], once it is appended,
  // with its deletes; takes them out of `staged`. A first that does not
  // follow the last sequence recorded means sequences in between came from
  // elsewhere: everything older is forgotten. Storing the deletes may fail
  // for want of memory: the log then forgets everything instead, which makes
  // older plans too old rather than unchecked, since every delete lost is at
  // or below `last`.
  void record(StagedRemovals &staged, std::uint64_t first,
              std::uint64_t last) noexcept {
    std::lock_guard<std::mutex> lk{mu_};
    if (first != recorded_through_ + 1) clear_locked(first - 1);
    recorded_through_ = last;
    if (staged.empty()) return;
    if (max_bytes_ == 0) {
      clear_locked(last);
    } else {
      try {
        record_locked(staged);
      } catch (...) {
        clear_locked(last);
      }
      while (bytes_locked() > max_bytes_) drop_oldest_locked();
    }
    staged.keys_.clear();
    staged.ranges_.clear();
    staged.segments_.clear();
    staged.last_seq_ = 0;
  }

  // Whether a delete above read_seq and below `below` named key. `below` is
  // the next sequence of the state being checked: deletes a later state made
  // are not part of it. nullopt when read_seq is below the horizon: a delete
  // above it may have been dropped. The horizon is read under the same lock
  // as the deletes, so a record() that drops some cannot fall between them.
  [[nodiscard]] auto removed(ByteView key, std::uint64_t read_seq,
                             std::uint64_t below) const -> std::optional<bool> {
    std::lock_guard<std::mutex> lk{mu_};
    if (!answers_locked(read_seq, below)) return std::nullopt;
    if (key_removed_locked(key, read_seq, below)) return true;
    // The key's latest range delete answers alone unless it is at or above
    // `below`; then an earlier one may still fall in the window.
    const auto latest = segments_.latest_at(key);
    if (latest < below) return latest > read_seq;
    return any_range_locked(read_seq, below, [&](const RangeRemoval &r) {
      return key_in_range(key, r.from, r.to);
    });
  }

  // Whether a delete above read_seq and below `below` named a key in
  // [from, to); nullopt below the horizon, as for removed(). The cost is the
  // deletes logged inside the range, not every delete since read_seq.
  [[nodiscard]] auto removed_in(ByteView from, ByteView to,
                                std::uint64_t read_seq,
                                std::uint64_t below) const
      -> std::optional<bool> {
    std::lock_guard<std::mutex> lk{mu_};
    if (!answers_locked(read_seq, below)) return std::nullopt;
    for (auto it = by_key_.lower_bound(KeyProbe{from, 0}); it != by_key_.end();
         ++it) {
      if (!byte_less(*it->key, to)) break;
      if (it->seq > read_seq && it->seq < below) return true;
    }
    auto range_past_below = false;
    const auto in_window = [&](std::uint64_t seq) {
      if (seq >= below) {
        range_past_below = true;
        return false;
      }
      return seq > read_seq;
    };
    if (segments_.any_overlapping(from, to, in_window)) return true;
    if (!range_past_below) return false;
    return any_range_locked(read_seq, below, [&](const RangeRemoval &r) {
      return ranges_overlap(r.from, r.to, from, to);
    });
  }

  [[nodiscard]] auto horizon() const -> std::uint64_t {
    std::lock_guard<std::mutex> lk{mu_};
    return horizon_;
  }
  // The last sequence of the unbroken run record() was handed.
  [[nodiscard]] auto recorded_through() const -> std::uint64_t {
    std::lock_guard<std::mutex> lk{mu_};
    return recorded_through_;
  }
  [[nodiscard]] auto bytes() const -> std::size_t {
    std::lock_guard<std::mutex> lk{mu_};
    return bytes_locked();
  }

private:
  // Each logged key and how many of the logged point deletes name it.
  using Keys = std::map<LogBytes, std::uint64_t, ByteLess>;
  // One point delete, by (key, sequence): the key's bytes live in keys_.
  struct KeySeq {
    const LogBytes *key;
    std::uint64_t seq;
  };
  struct KeyProbe {
    ByteView key;
    std::uint64_t seq;
  };
  struct KeySeqLess {
    using is_transparent = void;
    static auto parts(const KeySeq &k) -> std::pair<ByteView, std::uint64_t> {
      return {*k.key, k.seq};
    }
    static auto parts(const KeyProbe &k) -> std::pair<ByteView, std::uint64_t> {
      return {k.key, k.seq};
    }
    template <typename A, typename B>
    auto operator()(const A &a, const B &b) const -> bool {
      const auto [ak, as] = parts(a);
      const auto [bk, bs] = parts(b);
      if (byte_less(ak, bk)) return true;
      if (byte_less(bk, ak)) return false;
      return as < bs;
    }
  };
  struct KeyRemoval {
    std::uint64_t seq;
    Keys::iterator key;  // map iterators stay valid until their node is erased
  };

  // Rough cost of each record beyond its key or bound bytes, for the
  // max_bytes budget: a key's map node; a point delete's index node and
  // queue slot; a range's queue slot. A range's segments are counted by
  // RangeSegments.
  static constexpr std::size_t kNodeBytes = 64;
  static constexpr std::size_t kKeyRemovalBytes = 64;
  static constexpr std::size_t kRangeBytes = 64;

  [[nodiscard]] auto bytes_locked() const noexcept -> std::size_t {
    return bytes_ + segments_.bytes();
  }

  // Whether every delete above read_seq that a state below `below` holds is
  // here: none was dropped (the horizon), and every sequence up to below - 1
  // came through record() (no gap).
  [[nodiscard]] auto answers_locked(std::uint64_t read_seq,
                                    std::uint64_t below) const noexcept
      -> bool {
    if (read_seq < horizon_) return false;
    return below <= recorded_through_ + 1 || read_seq + 1 >= below;
  }

  void record_locked(StagedRemovals &staged) {
    // The group's point deletes join order_ in sequence order; the staged map
    // holds them in key order. Each staged node moves into keys_, so a key
    // is copied once per delete, when it is staged.
    std::vector<KeyRemoval> group;
    group.reserve(staged.keys_.size());
    while (!staged.keys_.empty()) {
      auto node = staged.keys_.extract(staged.keys_.begin());
      const auto seq = node.mapped();
      const auto key_bytes = node.key().size();
      node.mapped() = 0;
      const auto placed = keys_.insert(std::move(node));
      if (placed.inserted) bytes_ += key_bytes + kNodeBytes;
      ++placed.position->second;
      by_key_.insert(KeySeq{&placed.position->first, seq});
      group.push_back({seq, placed.position});
      bytes_ += kKeyRemovalBytes;
    }
    std::ranges::sort(group, {}, &KeyRemoval::seq);
    order_.insert(order_.end(), group.begin(), group.end());
    for (auto &r : staged.ranges_) {
      segments_.stamp(r.from, r.to, r.seq);
      bytes_ += r.from.size() + r.to.size() + kRangeBytes;
      ranges_.push_back(std::move(r));
    }
  }

  // The key's first logged delete above read_seq answers: it is the only
  // one that can fall in (read_seq, below).
  auto key_removed_locked(ByteView key, std::uint64_t read_seq,
                          std::uint64_t below) const -> bool {
    const auto it = by_key_.upper_bound(KeyProbe{key, read_seq});
    if (it == by_key_.end()) return false;
    return std::ranges::equal(*it->key, key) && it->seq < below;
  }

  // Walks the range deletes in (read_seq, below), newest first: only for a
  // state older than the newest range delete over what is asked, so the
  // walk is the range deletes of that window, found by sequence.
  template <typename Pred>
  auto any_range_locked(std::uint64_t read_seq, std::uint64_t below,
                        Pred matches) const -> bool {
    auto it = std::ranges::lower_bound(ranges_, below, {}, &RangeRemoval::seq);
    while (it != ranges_.begin()) {
      --it;
      if (it->seq <= read_seq) return false;
      if (matches(*it)) return true;
    }
    return false;
  }

  // Drops the oldest delete, point or range. Called only while the log
  // holds bytes, so one of the two queues holds something.
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
    by_key_.erase(by_key_.find(KeySeq{&r.key->first, r.seq}));
    bytes_ -= kKeyRemovalBytes;
    horizon_ = std::max(horizon_, r.seq);
    if (--r.key->second == 0) {
      bytes_ -= r.key->first.size() + kNodeBytes;
      keys_.erase(r.key);
    }
  }

  void drop_oldest_range_locked() noexcept {
    const auto &r = ranges_.front();
    segments_.unstamp(r.from, r.to, r.seq);
    bytes_ -= r.from.size() + r.to.size() + kRangeBytes;
    horizon_ = std::max(horizon_, r.seq);
    ranges_.pop_front();
  }

  void clear_locked(std::uint64_t latest) noexcept {
    order_.clear();
    by_key_.clear();
    keys_.clear();
    ranges_.clear();
    segments_.clear();
    bytes_ = 0;
    horizon_ = std::max(horizon_, latest);
  }

  mutable std::mutex mu_;
  std::size_t max_bytes_;
  std::size_t bytes_{0};
  std::uint64_t horizon_{0};
  // The last sequence of the unbroken run record() was handed; 0 before the
  // first group.
  std::uint64_t recorded_through_{0};
  // Each logged key, owning its bytes.
  Keys keys_;
  // Every point delete by (key, sequence), for lookups by key or key range.
  std::set<KeySeq, KeySeqLess> by_key_;
  // Every point delete in sequence order, for dropping the oldest; several
  // may name one key.
  std::deque<KeyRemoval> order_;
  // Every range delete in sequence order, for dropping the oldest.
  std::deque<RangeRemoval> ranges_;
  // ranges_ as segments, for lookups.
  RangeSegments segments_;
};

} // namespace bytecask
