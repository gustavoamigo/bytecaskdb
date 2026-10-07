// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — PersistentPatchMap: a persistent uint32_t-keyed map whose
// writes are O(1) and whose reads pay for them.
//
// A version is a base map plus a chain of patch runs, newest first. A write
// records a patch — set a value, erase it, or apply a delta — in the
// transient; freezing the transient pushes its patches as one run. Nothing
// touches the base until the chain grows past MaxDepth, when the freeze
// squashes every run into it. A read walks the chain, so it costs up to
// MaxDepth binary searches more than a base lookup.
//
// Use it for a map written on every commit and read off the hot path
// (file_stats, #367). A map read on every record access wants its base
// directly: PersistentU32Table.
//
// Versions are values: copying one is O(1), and a later write or squash
// changes nothing an older version sees. Every read returns an owned value,
// since a value with deltas applied exists nowhere to point at.

module;
#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.patch_map;

import bytecask.u32_map;

namespace bytecask {

// How a Delta changes a V. Generic code never looks inside a Delta.
//   apply(v, d):       changes v by d.
//   combine(older, d): folds the newer d into older, so that applying the
//                      result is applying older and then d. A transient
//                      relies on it to keep one patch per key, and all() to
//                      merge the chain. The compiler cannot check it: every
//                      policy gets a check_patch_policy case in
//                      tests/patch_map_test.cpp.
export template <typename P, typename V>
concept PatchPolicy =
    std::copyable<typename P::Delta> &&
    !std::same_as<typename P::Delta, V> &&
    requires(V &v, typename P::Delta &older, const typename P::Delta &d) {
      P::apply(v, d);
      P::combine(older, d);
    };

struct Erased {};

// One key's net change over some span of writes.
template <typename V, typename D> using Patch = std::variant<V, Erased, D>;

template <typename V, typename D> struct PatchEntry {
  std::uint32_t key;
  Patch<V, D> patch;
};

// Folds the newer patch into the older one for the same key. A set or an
// erase replaces whatever came before; a delta lands on a set value, is
// dropped after an erase (a delta to an absent key is a no-op), and
// combines with an older delta.
template <typename V, typename P>
void fold(Patch<V, typename P::Delta> &older,
          Patch<V, typename P::Delta> newer) {
  using D = typename P::Delta;
  const auto *d = std::get_if<D>(&newer);
  if (d == nullptr) {
    older = std::move(newer);
  } else if (auto *v = std::get_if<V>(&older)) {
    P::apply(*v, *d);
  } else if (auto *od = std::get_if<D>(&older)) {
    P::combine(*od, *d);
  }
}

// One frozen transient's patches, one per key, sorted by key. Immutable.
template <typename V, typename D> struct PatchRun {
  std::vector<PatchEntry<V, D>> entries;
  std::shared_ptr<const PatchRun> prev; // older run, or null
};

template <typename V, typename D>
auto find_patch(const std::vector<PatchEntry<V, D>> &entries,
                std::uint32_t key) -> const Patch<V, D> * {
  const auto it = std::ranges::lower_bound(entries, key, {},
                                           &PatchEntry<V, D>::key);
  return it != entries.end() && it->key == key ? &it->patch : nullptr;
}

export template <typename V, PatchPolicy<V> P, std::size_t MaxDepth,
                 typename Base>
class TransientPatchMap;

// What both sides share: the base, the chain, and reads through them.
template <typename V, typename P, std::size_t MaxDepth, typename Base>
class PatchMapCore {
protected:
  using D = typename P::Delta;
  using Entry = PatchEntry<V, D>;
  using Run = PatchRun<V, D>;

  PatchMapCore() = default;
  PatchMapCore(Base base, std::shared_ptr<const Run> runs, std::size_t depth)
      : base_{std::move(base)}, runs_{std::move(runs)}, depth_{depth} {}

  // `pending`, if given, is newer than every run.
  [[nodiscard]] auto lookup(std::uint32_t key,
                            const std::vector<Entry> *pending) const
      -> std::optional<V> {
    // Deltas met on the way down, newest first, until a set or an erase
    // decides the value; then applied to it oldest first.
    std::array<const D *, MaxDepth + 1> deltas{};
    std::size_t n = 0;
    const Patch<V, D> *decided = nullptr;
    const auto step = [&](const std::vector<Entry> &entries) -> bool {
      const auto *p = find_patch(entries, key);
      if (p == nullptr) return false;
      if (const auto *d = std::get_if<D>(p)) {
        assert(n < deltas.size());
        deltas[n++] = d;
        return false;
      }
      decided = p;
      return true;
    };
    auto done = pending != nullptr && step(*pending);
    for (const auto *r = runs_.get(); !done && r != nullptr; r = r->prev.get())
      done = step(r->entries);

    std::optional<V> value;
    if (decided != nullptr) {
      if (const auto *v = std::get_if<V>(decided)) value = *v;
    } else if (const auto *b = base_.get(key)) {
      value = *b;
    }
    if (value) {
      while (n > 0) P::apply(*value, *deltas[--n]);
    }
    return value;
  }

  // Calls f on each run's entries, oldest first, then on `pending`'s.
  template <typename F>
  void for_each_run(const std::vector<Entry> *pending, F &&f) const {
    std::array<const Run *, MaxDepth> chain{};
    std::size_t n = 0;
    for (const auto *r = runs_.get(); r != nullptr; r = r->prev.get()) {
      assert(n < chain.size());
      chain[n++] = r;
    }
    while (n > 0) f(chain[--n]->entries);
    if (pending != nullptr) f(*pending);
  }

  // Every run and `pending`, folded into one patch per key, sorted by key.
  // One stable sort of pointers over every entry, oldest run first, so equal
  // keys meet in write order: O(e log e) for e entries, where merging the
  // runs one by one would copy the growing result once per run.
  [[nodiscard]] auto collect(const std::vector<Entry> *pending) const
      -> std::vector<Entry> {
    std::vector<const Entry *> sorted;
    for_each_run(pending, [&sorted](const std::vector<Entry> &entries) {
      for (const auto &e : entries) sorted.push_back(&e);
    });
    std::ranges::stable_sort(sorted, {}, &Entry::key);

    std::vector<Entry> acc;
    for (const auto *e : sorted) {
      if (!acc.empty() && acc.back().key == e->key) {
        fold<V, P>(acc.back().patch, e->patch);
      } else {
        acc.push_back(*e);
      }
    }
    return acc;
  }

  [[nodiscard]] auto all(const std::vector<Entry> *pending) const
      -> std::vector<std::pair<std::uint32_t, V>> {
    const auto patches = collect(pending);
    std::vector<std::pair<std::uint32_t, V>> out;
    auto p = patches.begin();
    // A patch with no base entry yields a value only if it sets one.
    const auto emit_patch_only = [&](const Entry &e) {
      if (const auto *v = std::get_if<V>(&e.patch)) out.emplace_back(e.key, *v);
    };
    for (auto it = base_.begin(); it != std::default_sentinel; ++it) {
      const auto [key, base_value] = *it;
      while (p != patches.end() && p->key < key) emit_patch_only(*p++);
      if (p == patches.end() || p->key != key) {
        out.emplace_back(key, base_value);
        continue;
      }
      if (const auto *v = std::get_if<V>(&p->patch)) {
        out.emplace_back(key, *v);
      } else if (const auto *d = std::get_if<D>(&p->patch)) {
        auto v2 = base_value;
        P::apply(v2, *d);
        out.emplace_back(key, std::move(v2));
      }
      ++p;
    }
    while (p != patches.end()) emit_patch_only(*p++);
    return out;
  }

  Base base_;
  std::shared_ptr<const Run> runs_;
  std::size_t depth_{0};
};

export template <typename V, PatchPolicy<V> P, std::size_t MaxDepth,
                 typename Base = PersistentU32Table<V>>
class PersistentPatchMap : PatchMapCore<V, P, MaxDepth, Base> {
  using Core = PatchMapCore<V, P, MaxDepth, Base>;
  static_assert(MaxDepth > 0);
  static_assert(PersistentU32MapOf<Base, V>);

public:
  using Policy = P;

  PersistentPatchMap() = default;
  // Wraps a map built elsewhere (recovery) as a version with no patches.
  explicit PersistentPatchMap(Base base) : Core{std::move(base), nullptr, 0} {}

  // O(depth × log run).
  [[nodiscard]] auto get(std::uint32_t key) const -> std::optional<V> {
    return this->lookup(key, nullptr);
  }
  [[nodiscard]] auto contains(std::uint32_t key) const -> bool {
    return get(key).has_value();
  }
  // Every entry in ascending key order. O(size + patches in the chain).
  [[nodiscard]] auto all() const
      -> std::vector<std::pair<std::uint32_t, V>> {
    return Core::all(nullptr);
  }
  // Runs in the chain, at most MaxDepth.
  [[nodiscard]] auto depth() const noexcept -> std::size_t {
    return this->depth_;
  }
  // O(1): the transient shares this version's base and chain.
  [[nodiscard]] auto transient() const
      -> TransientPatchMap<V, P, MaxDepth, Base> {
    return TransientPatchMap<V, P, MaxDepth, Base>{this->base_, this->runs_,
                                                   this->depth_};
  }

private:
  friend class TransientPatchMap<V, P, MaxDepth, Base>;
  PersistentPatchMap(Base base, std::shared_ptr<const typename Core::Run> runs,
                     std::size_t depth)
      : Core{std::move(base), std::move(runs), depth} {}
};

export template <typename V, PatchPolicy<V> P, std::size_t MaxDepth,
                 typename Base>
class TransientPatchMap : PatchMapCore<V, P, MaxDepth, Base> {
  using Core = PatchMapCore<V, P, MaxDepth, Base>;
  using typename Core::D;
  using typename Core::Entry;

public:
  using Delta = D;

  TransientPatchMap(const TransientPatchMap &) = delete;
  auto operator=(const TransientPatchMap &) -> TransientPatchMap & = delete;
  TransientPatchMap(TransientPatchMap &&) noexcept = default;
  auto operator=(TransientPatchMap &&) noexcept
      -> TransientPatchMap & = default;

  // Each is a fold into this transient's pending patch for the key: O(1)
  // when the key is the one last written, O(log keys written) otherwise.
  void set(std::uint32_t key, V value) { record(key, std::move(value)); }
  void erase(std::uint32_t key) { record(key, Erased{}); }
  // A delta to an absent key is a no-op.
  void patch(std::uint32_t key, D delta) { record(key, std::move(delta)); }

  // Sees this transient's writes. O(depth × log run).
  [[nodiscard]] auto get(std::uint32_t key) const -> std::optional<V> {
    return this->lookup(key, &pending_);
  }

  // Freezes: pushes the pending patches as one run. A chain that would
  // exceed MaxDepth is squashed instead — every run written into the base —
  // so the result's chain is empty. Older versions keep their own base and
  // chain.
  [[nodiscard]] auto persistent() && -> PersistentPatchMap<V, P, MaxDepth, Base> {
    if (pending_.empty())
      return {std::move(this->base_), std::move(this->runs_), this->depth_};
    if (this->depth_ < MaxDepth) {
      auto run = std::make_shared<const typename Core::Run>(
          typename Core::Run{std::move(pending_), std::move(this->runs_)});
      return {std::move(this->base_), std::move(run), this->depth_ + 1};
    }
    // Replays every run into the base, oldest first. Merging the runs per
    // key first would save base writes but cost a sort, which on the paged
    // table is the larger part (#367).
    auto t = this->base_.transient();
    this->for_each_run(&pending_, [&t](const std::vector<Entry> &entries) {
      for (const auto &e : entries) {
        if (const auto *v = std::get_if<V>(&e.patch)) {
          t.set(e.key, *v);
        } else if (const auto *d = std::get_if<D>(&e.patch)) {
          t.update(e.key, [d](V &value) { P::apply(value, *d); });
        } else {
          (void)t.erase(e.key);
        }
      }
    });
    return {std::move(t).persistent(), nullptr, 0};
  }

private:
  friend class PersistentPatchMap<V, P, MaxDepth, Base>;
  TransientPatchMap(Base base, std::shared_ptr<const typename Core::Run> runs,
                    std::size_t depth)
      : Core{std::move(base), std::move(runs), depth} {}

  void record(std::uint32_t key, Patch<V, D> p) {
    if (last_ < pending_.size() && pending_[last_].key == key) {
      fold<V, P>(pending_[last_].patch, std::move(p));
      return;
    }
    auto it = std::ranges::lower_bound(pending_, key, {}, &Entry::key);
    if (it != pending_.end() && it->key == key) {
      fold<V, P>(it->patch, std::move(p));
    } else {
      it = pending_.insert(it, Entry{key, std::move(p)});
    }
    last_ = static_cast<std::size_t>(it - pending_.begin());
  }

  // This transient's writes, one patch per key, sorted by key.
  std::vector<Entry> pending_;
  std::size_t last_{0};
};

} // namespace bytecask
