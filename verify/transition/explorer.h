// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — exhaustive state explorer for the commit path's protocol
// models (verify/transition/).
//
// A model is a struct with:
//
//   struct State { ... };   // copyable, packed exactly by KeyOf
//   struct Step { ... };    // one transition of one actor
//   static auto Enabled(const State &) -> std::vector<Step>;
//   static void Apply(State &, const Step &);
//   static auto Check(const State &, std::string *why) -> bool;
//   static auto Done(const State &) -> bool;
//   static auto Label(const State &, const Step &) -> std::string;
//   static auto KeyOf(const State &) -> Key;
//
// Every reachable state is visited once (the exploration is exhaustive over
// interleavings, not over paths), the model's invariants are checked after
// every transition, and a state with no enabled step while some writer has
// not returned is reported as a hang. Terminating requires every writer to
// have returned.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace transition {

using Key = std::array<std::uint64_t, 2>;

struct KeyHash {
  auto operator()(const Key &k) const noexcept -> std::size_t {
    auto h = static_cast<std::size_t>(k[0] ^
                                      (k[1] * 0x9e3779b97f4a7c15ULL));
    h ^= h >> 16;
    h *= 0x85ebca6bULL;
    h ^= h >> 13;
    return h;
  }
};

// Packs a state's fields, in order, into 128 bits. The packing must be
// exact — no two distinct states may share a key, since a state whose key has
// been seen is discarded — which is why each model counts its bits and
// asserts the total below.
class KeyBuilder {
 public:
  void put(std::uint64_t value, int bits) noexcept {
    wide_ |= static_cast<unsigned __int128>(value) << pos_;
    pos_ += bits;
  }

  [[nodiscard]] auto done() const noexcept -> Key {
    return {static_cast<std::uint64_t>(wide_),
            static_cast<std::uint64_t>(wide_ >> 64)};
  }

  [[nodiscard]] auto width() const noexcept -> int { return pos_; }

 private:
  unsigned __int128 wide_{0};
  int pos_{0};
};

struct Options {
  std::size_t state_limit{20'000'000};
  int depth_limit{400};
};

struct Result {
  bool clean{true};
  std::string failure;
  std::vector<std::string> trace;
  std::size_t states{0};  // distinct states visited
  // States in which every writer has returned. Interleavings share states,
  // so this counts endpoints, not schedules.
  std::size_t completions{0};
  bool truncated{false};
};

template <class Model>
auto Explore(const typename Model::State &initial,
             const Options &opt = Options{}) -> Result {
  Result r;
  std::unordered_set<Key, KeyHash> seen;
  std::vector<std::string> path;

  const auto walk = [&](const auto &self, const typename Model::State &s,
                        int depth) -> void {
    if (!r.clean) return;
    std::string why;
    if (!Model::Check(s, &why)) {
      r.clean = false;
      r.failure = std::move(why);
      r.trace = path;
      return;
    }
    if (Model::Done(s)) {
      ++r.completions;
      return;
    }
    const auto steps = Model::Enabled(s);
    if (steps.empty()) {
      r.clean = false;
      r.failure =
          "hang: no action is enabled while a writer has not returned";
      r.trace = path;
      return;
    }
    if (depth >= opt.depth_limit) {
      r.truncated = true;
      return;
    }
    for (const auto &step : steps) {
      auto next = s;
      Model::Apply(next, step);
      if (!seen.insert(Model::KeyOf(next)).second) continue;
      if (seen.size() > opt.state_limit) {
        r.clean = false;
        r.failure = "state limit reached";
        r.trace = path;
        return;
      }
      path.push_back(Model::Label(s, step));
      self(self, next, depth + 1);
      path.pop_back();
      if (!r.clean) return;
    }
  };

  seen.insert(Model::KeyOf(initial));
  walk(walk, initial, 0);
  r.states = seen.size();
  return r;
}

}  // namespace transition
