// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — model test for the buffered blind tree
// (bytecaskdb/buffered_btree.cppm): seeded writes, undos, vacuums and
// discarded builders against a std::map, with snapshots read after later
// merges. Tests shrink the buffer to a handful of slots, so freezes,
// installs, merges in the builder and backpressure happen constantly.
//
// Records are simulated as the engine lays them out: a location names one
// record while it lives, an undone write's location is reused for the next
// record, and a vacuumed file's records vanish for versions made after it.
// A read of a location that holds no record throws, so a tree that keeps an
// undone record or reads a vacuumed one fails loudly.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
import bytecask.blind_btree;
import bytecask.buffered_btree;

namespace {

using bytecask::BlindRef;
using Bytes = std::span<const std::byte>;

auto to_bytes(std::string_view sv) -> Bytes {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}
auto to_string(Bytes b) -> std::string {
  return {reinterpret_cast<const char *>(b.data()), b.size()};
}
auto pack(BlindRef r) -> std::uint64_t {
  return (std::uint64_t{r.file_id} << 32) | r.offset;
}

// Every record written, by location. Shared with the merger thread.
struct Records {
  std::mutex mu;
  std::unordered_map<std::uint64_t, std::string> keys;
};
using Files = std::set<std::uint32_t>;

// Reads the records of one version: those in the files it holds.
struct Resolver {
  std::shared_ptr<Records> rec;
  std::shared_ptr<const Files> files;
  std::string buf;
  std::size_t reads{0};

  auto try_key_at(BlindRef r) -> std::optional<Bytes> {
    ++reads;
    if (!files->contains(r.file_id)) return std::nullopt;
    std::lock_guard<std::mutex> lk{rec->mu};
    const auto it = rec->keys.find(pack(r));
    if (it == rec->keys.end())
      throw std::logic_error{std::format("read of a location with no record: {}:{}",
                                         r.file_id, r.offset)};
    buf = it->second;
    return to_bytes(buf);
  }
  auto key_at(BlindRef r) -> Bytes {
    if (auto k = try_key_at(r)) return *k;
    throw std::logic_error{"read of a record in a vacuumed file"};
  }
};

struct Source {
  std::shared_ptr<Records> rec;
  std::shared_ptr<const Files> files;
  template <typename F> void with_resolver(F &&f) const {
    Resolver r{rec, files, {}, 0};
    f(r);
  }
};

using Tree = bytecask::BufferedBlindBTree<1024, Source>;
using Model = std::map<std::string, BlindRef>;

// A published version, the files it holds, and what it must contain.
struct Version {
  Tree tree;
  std::shared_ptr<const Files> files;
  Model model;
};

void check_version(const Version &v, const std::shared_ptr<Records> &rec, std::mt19937_64 &rng,
                   const std::vector<std::string> &universe) {
  Resolver res{rec, v.files, {}, 0};
  REQUIRE(v.tree.size() == v.model.size());
  for (int i = 0; i < 8; ++i) {
    const auto &k = universe[rng() % universe.size()];
    const auto got = v.tree.get(to_bytes(k), res);
    const auto want = v.model.find(k);
    REQUIRE(got.has_value() == (want != v.model.end()));
    if (got) {
      REQUIRE(*got == want->second);
      REQUIRE(v.tree.holds(to_bytes(k), want->second));
    }
  }
  // Forward from the start.
  {
    auto it = v.tree.begin();
    it.settle(res);
    auto want = v.model.begin();
    for (; !(it == std::default_sentinel); it.next(res), ++want) {
      REQUIRE(want != v.model.end());
      REQUIRE(to_string(it.key(res)) == want->first);
      REQUIRE(*it == want->second);
    }
    REQUIRE(want == v.model.end());
  }
  // Backward from the end.
  {
    auto it = v.tree.end_iter();
    it.settle(res);
    it.prev(res);
    auto want = v.model.rbegin();
    for (; !(it == std::default_sentinel); it.prev(res), ++want) {
      REQUIRE(want != v.model.rend());
      REQUIRE(to_string(it.key(res)) == want->first);
    }
    REQUIRE(want == v.model.rend());
  }
  // From a random bound, a few steps forward, then back.
  {
    const auto &from = universe[rng() % universe.size()];
    auto it = v.tree.lower_bound(to_bytes(from), res);
    it.settle(res);
    auto want = v.model.lower_bound(from);
    std::vector<std::string> seen;
    for (int s = 0; s < 5 && !(it == std::default_sentinel); ++s, it.next(res), ++want) {
      REQUIRE(want != v.model.end());
      REQUIRE(to_string(it.key(res)) == want->first);
      seen.push_back(want->first);
    }
    if (!(it == std::default_sentinel)) {
      REQUIRE(want != v.model.end());
      it.prev(res);
      for (auto s = seen.rbegin(); s != seen.rend(); ++s) {
        REQUIRE(!(it == std::default_sentinel));
        REQUIRE(to_string(it.key(res)) == *s);
        it.prev(res);
      }
    } else {
      REQUIRE(want == v.model.end());
    }
  }
  // Counts between two bounds.
  {
    auto a = universe[rng() % universe.size()];
    auto b = universe[rng() % universe.size()];
    if (b < a) std::swap(a, b);
    const auto limit = rng() % 4 == 0 ? std::size_t{3} : std::size_t{1'000'000};
    const auto first = v.tree.lower_bound(to_bytes(a), res);
    const auto last = v.tree.lower_bound(to_bytes(b), res);
    const auto want = static_cast<std::size_t>(
        std::distance(v.model.lower_bound(a), v.model.lower_bound(b)));
    REQUIRE(first.count_until(last, limit, res) == std::min(want, limit));
  }
}

} // namespace

TEST_CASE("buffered tree matches a map through merges, undos and vacuums",
          "[buffered_btree][model]") {
  const auto seed = GENERATE(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
  CAPTURE(seed);
  std::mt19937_64 rng{seed};

  // Keys over a few "indexes" (first 6 bytes), so partitions and the
  // index-scoped scans are exercised, plus some shorter than 6 bytes.
  std::vector<std::string> universe;
  // 600 keys: several leaves, so scans cross leaf fences.
  for (int idx = 0; idx < 3; ++idx)
    for (int i = 0; i < 200; ++i)
      universe.push_back(std::format("ix{:04d}{:05d}", idx, i * 7));
  for (int i = 0; i < 6; ++i) universe.push_back(std::format("s{}", i));
  std::ranges::sort(universe);

  auto rec = std::make_shared<Records>();
  auto files = std::make_shared<Files>(Files{1});
  std::uint32_t active = 1;
  std::uint32_t next_offset = 0;
  std::vector<std::uint32_t> reusable;  // offsets of undone records

  const auto new_record = [&](const std::string &key) {
    std::uint32_t off = next_offset;
    if (!reusable.empty() && rng() % 2 == 0) {
      off = reusable.back();
      reusable.pop_back();
    } else {
      ++next_offset;
    }
    const BlindRef r{active, off};
    std::lock_guard<std::mutex> lk{rec->mu};
    rec->keys[pack(r)] = key;
    return r;
  };
  const auto drop_record = [&](BlindRef r) {
    std::lock_guard<std::mutex> lk{rec->mu};
    rec->keys.erase(pack(r));
    reusable.push_back(r.offset);
  };

  std::vector<Version> versions;
  versions.push_back({Tree{}, files, {}});
  std::size_t most_frozen = 0;  // frozen buffers queued behind the merger

  for (int round = 0; round < 400; ++round) {
    const auto &head = versions.back();
    auto b = head.tree.transient();
    auto model = head.model;
    auto cur_files = std::make_shared<Files>(*files);
    Resolver res{rec, cur_files, {}, 0};
    // What a discarded builder leaves behind: nothing. Its locations are
    // written again, as after a failed append.
    const auto saved = std::tuple{active, next_offset, reusable};

    const auto ops = 1 + rng() % 30;
    for (std::size_t o = 0; o < ops; ++o) {
      const auto &k = universe[rng() % universe.size()];
      const auto dice = rng() % 100;
      if (dice < 50) {
        const auto r = new_record(k);
        const auto displaced = b.upsert(to_bytes(k), r, res,
                                        [](const BlindRef &, const BlindRef &) { return true; });
        const auto want = model.find(k);
        REQUIRE(displaced.has_value() == (want != model.end()));
        if (displaced) REQUIRE(*displaced == want->second);
        model[k] = r;
      } else if (dice < 65) {
        const auto erased = b.erase(to_bytes(k), res);
        REQUIRE(erased.has_value() == model.contains(k));
        model.erase(k);
      } else if (dice < 75) {
        // By location, as a snapshot plan writes.
        if (const auto it = model.find(k); it != model.end()) {
          if (rng() % 3 == 0) {
            // Declined when out of room: the caller goes by key.
            if (!b.erase_at(to_bytes(k), it->second)) REQUIRE(b.erase(to_bytes(k), res));
            model.erase(it);
          } else {
            const auto r = new_record(k);
            if (b.replace_at(to_bytes(k), it->second, r)) {
              it->second = r;
            } else {
              drop_record(r);  // declined: out of room; the caller goes by key
            }
          }
        }
        REQUIRE_FALSE(b.replace_at(to_bytes(k), BlindRef{active, 0x7fff'ffff}, BlindRef{active, 1}));
      } else if (dice < 90) {
        // A plan applied and undone, newest write first.
        struct Undo {
          std::string key;
          BlindRef now;
          std::optional<BlindRef> was;
        };
        std::vector<Undo> undo;
        const auto n = 1 + rng() % 12;
        for (std::size_t i = 0; i < n; ++i) {
          const auto &pk = universe[rng() % universe.size()];
          const auto r = new_record(pk);
          const auto displaced = b.upsert(to_bytes(pk), r, res,
                                          [](const BlindRef &, const BlindRef &) { return true; });
          undo.push_back({pk, r, displaced});
        }
        for (auto u = undo.rbegin(); u != undo.rend(); ++u) {
          const bool undone = u->was ? b.replace_at(to_bytes(u->key), u->now, *u->was)
                                     : b.erase_at(to_bytes(u->key), u->now);
          REQUIRE(undone);
          drop_record(u->now);
        }
      } else if (dice < 95) {
        // Vacuum: relocate a sealed file's live keys to a new file, by key,
        // then drop the file from this version on.
        std::vector<std::uint32_t> sealed(cur_files->begin(), cur_files->end());
        std::erase(sealed, active);
        if (!sealed.empty()) {
          const auto victim = sealed[rng() % sealed.size()];
          const auto dest = ++active;  // rotate: relocations land in a new file
          cur_files->insert(dest);
          for (auto &[mk, mr] : model) {
            if (mr.file_id != victim) continue;
            const auto r = new_record(mk);
            (void)b.upsert(to_bytes(mk), r, res,
                           [](const BlindRef &, const BlindRef &) { return true; });
            mr = r;
          }
          cur_files->erase(victim);
        }
      } else {
        // Rotate the active file.
        cur_files->insert(++active);
        next_offset = 0;
        reusable.clear();
      }
      REQUIRE(b.size() == model.size());
    }

    if (rng() % 10 == 0) {  // discard the builder: nothing published
      std::tie(active, next_offset, reusable) = saved;
      continue;
    }
    *files = *cur_files;
    versions.push_back({std::move(b).persistent(Source{rec, cur_files}), cur_files,
                        std::move(model)});

    most_frozen = std::max(most_frozen, versions.back().tree.test_frozen());
    if (rng() % 8 == 0) versions.back().tree.test_wait_merged();
    // Check the head and a random older version (read after later merges).
    check_version(versions.back(), rec, rng, universe);
    check_version(versions[rng() % versions.size()], rec, rng, universe);
    if (versions.size() > 24) versions.erase(versions.begin() + static_cast<std::ptrdiff_t>(rng() % 12));
  }
  for (const auto &v : versions) check_version(v, rec, rng, universe);
  // Several frozen buffers queued at once, merged in order.
  CHECK(most_frozen >= 2);
}

TEST_CASE("buffered tree: a count reads no key per counted entry", "[buffered_btree]") {
  auto rec = std::make_shared<Records>();
  auto files = std::make_shared<const Files>(Files{1});
  Resolver res{rec, files, {}, 0};
  Tree t;
  // Enough keys that most are merged into the tree, the last few buffered.
  for (std::uint32_t i = 0; i < 2000; ++i) {
    const auto k = std::format("k{:06d}", i);
    {
      std::lock_guard<std::mutex> lk{rec->mu};
      rec->keys[pack({1, i})] = k;
    }
    auto b = t.transient();
    b.upsert(to_bytes(k), {1, i}, res, [](const BlindRef &, const BlindRef &) { return true; });
    t = std::move(b).persistent(Source{rec, files});
    t.test_wait_merged();
  }
  REQUIRE(t.test_buffered() > 0);
  res.reads = 0;
  const auto first = t.lower_bound(to_bytes("k000100"), res);
  const auto last = t.lower_bound(to_bytes("k001500"), res);
  REQUIRE(first.count_until(last, 1'000'000, res) == 1400);
  // The tree places each bound: a read or two each, never one per key.
  CHECK(res.reads <= 4);
}
