// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — DeleteLog: the keys recent writes removed from the key
// directory, with the sequence that removed each.
//
// A plan that carries only the sequence it read at (its read sequence R)
// can see from the key directory alone whether a present key changed since
// R: its entry's sequence is above R. A key absent now leaves no entry, so
// "absent at R too" and "removed since R" look the same. This log tells
// them apart for every R at or above horizon(); below it the removals have
// been dropped, and a plan read there is too old to check.
//
// In memory only: the data files are the record. An open starts the log
// empty with the horizon at the latest recovered sequence, so a plan read
// before a restart is too old. Bounded by max_bytes: past it the oldest
// removals are dropped and the horizon rises to the newest dropped one.
//
// Thread-safety: record_all and reset are called under the DB's write lock; the
// queries run there and, for a plan that appended nothing, on its writer
// thread against the published state (DB::settle_unappended). An internal
// mutex serialises the two.

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

// A key a write removed from the key directory, and that write's sequence.
export struct KeyRemoval {
  std::vector<std::byte> key;
  std::uint64_t seq;
};

export class DeleteLog {
public:
  explicit DeleteLog(std::size_t max_bytes) : max_bytes_{max_bytes} {}
  DeleteLog(const DeleteLog &) = delete;
  auto operator=(const DeleteLog &) -> DeleteLog & = delete;

  // Forgets every removal: a plan read below `latest` is too old from now
  // on. Used at open, and wherever the key directory changes by a path that
  // does not record its removals (resume(), ingest).
  void reset(std::uint64_t latest) noexcept {
    std::lock_guard<std::mutex> lk{mu_};
    clear_locked(latest);
  }

  // A group's removals, in ascending sequence order, once the group has
  // applied in memory. A failure to store them forgets everything instead,
  // which makes older plans too old rather than unchecked: every removal
  // lost is at or below the group's last.
  void record_all(std::span<const KeyRemoval> removals) noexcept {
    if (removals.empty()) return;
    std::lock_guard<std::mutex> lk{mu_};
    try {
      for (const auto &r : removals) {
        auto [it, inserted] = latest_.try_emplace(r.key, r.seq);
        if (!inserted) it->second = r.seq;
        order_.push_back({r.seq, it});
        if (inserted) bytes_ += r.key.size() + kNodeBytes;
        bytes_ += kRemovalBytes;
      }
    } catch (...) {
      clear_locked(removals.back().seq);
      return;
    }
    while (bytes_ > max_bytes_ && !order_.empty()) drop_oldest_locked();
  }

  // Whether a plan read at read_seq can be checked: every removal above it
  // is still here.
  [[nodiscard]] auto covers(std::uint64_t read_seq) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    return read_seq >= horizon_;
  }

  // Whether key was removed by a write above read_seq and below `below`
  // (the next sequence of the state being checked: removals a later state
  // made are not part of it).
  [[nodiscard]] auto removed(std::span<const std::byte> key,
                             std::uint64_t read_seq,
                             std::uint64_t below) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    // The key's latest removal answers alone unless it is at or above
    // `below`; then an earlier one may still fall in the window.
    const auto it = latest_.find(key);
    if (it == latest_.end()) return false;
    if (it->second < below) return it->second > read_seq;
    return any_removal_locked(read_seq, below, [&](const Bytes &k) {
      return std::ranges::equal(k, key);
    });
  }

  // Whether any key in [from, to) was removed by a write above read_seq and
  // below `below`. Walks the removals newer than read_seq, newest first.
  [[nodiscard]] auto removed_in(std::span<const std::byte> from,
                                std::span<const std::byte> to,
                                std::uint64_t read_seq,
                                std::uint64_t below) const -> bool {
    std::lock_guard<std::mutex> lk{mu_};
    return any_removal_locked(read_seq, below, [&](const Bytes &k) {
      return !byte_less(k, from) && byte_less(k, to);
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
  using Bytes = std::vector<std::byte>;

  // Byte-wise order, the key directory's: std::byte compares unsigned.
  static auto byte_less(std::span<const std::byte> a,
                        std::span<const std::byte> b) -> bool {
    return std::ranges::lexicographical_compare(a, b);
  }
  struct KeyLess {
    using is_transparent = void;
    auto operator()(std::span<const std::byte> a,
                    std::span<const std::byte> b) const -> bool {
      return byte_less(a, b);
    }
  };
  using Map = std::map<Bytes, std::uint64_t, KeyLess>;
  struct Removal {
    std::uint64_t seq;
    Map::iterator key;  // map iterators stay valid until their node is erased
  };

  // Rough cost of a map node and of a queued removal beyond the key bytes,
  // for the max_bytes budget.
  static constexpr std::size_t kNodeBytes = 64;
  static constexpr std::size_t kRemovalBytes = 16;

  template <typename Pred>
  auto any_removal_locked(std::uint64_t read_seq, std::uint64_t below,
                          Pred matches) const -> bool {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      if (it->seq <= read_seq) return false;
      if (it->seq < below && matches(it->key->first)) return true;
    }
    return false;
  }

  void drop_oldest_locked() noexcept {
    const auto r = order_.front();
    order_.pop_front();
    bytes_ -= kRemovalBytes;
    horizon_ = std::max(horizon_, r.seq);
    if (r.key->second == r.seq) {
      bytes_ -= r.key->first.size() + kNodeBytes;
      latest_.erase(r.key);
    }
  }

  void clear_locked(std::uint64_t latest) noexcept {
    order_.clear();
    latest_.clear();
    bytes_ = 0;
    horizon_ = std::max(horizon_, latest);
  }

  mutable std::mutex mu_;
  std::size_t max_bytes_;
  std::size_t bytes_{0};
  std::uint64_t horizon_{0};
  // Each key's latest removal, for point lookups.
  Map latest_;
  // Every removal in sequence order, for range lookups and for dropping the
  // oldest; several may name one key.
  std::deque<Removal> order_;
};

} // namespace bytecask
