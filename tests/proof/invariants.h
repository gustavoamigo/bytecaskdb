// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Invariant-checking helpers for the correctness validation framework.
//
// Requires the including translation unit to `import bytecask;` before
// this header (same pattern as fault_injector.h).

#pragma once
#ifdef BYTECASK_TESTING

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bytecask::testing {

// ---- Conversion helpers (same logic as bytecask_test.cpp) ----------------

inline auto to_bytes(std::string_view sv) -> BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

inline auto to_string(const Bytes &bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  std::ranges::transform(bytes, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

inline auto to_string(const Key &key) -> std::string {
  std::string s(key.size(), '\0');
  std::ranges::transform(key, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

// ---- Data types ----------------------------------------------------------

// Owned snapshot of DB state before a transition.
// Uses an owned map (not a Snapshot) to avoid holding file descriptors alive
// during fault injection tests.
struct Baseline {
  std::uint64_t next_lsn;
  std::map<std::string, Bytes> key_values;
};

// Expected outcome of a state transition, as defined by the reference model.
struct ExpectedDelta {
  std::vector<std::string> keys_added;
  std::vector<std::string> keys_removed;
  std::uint64_t lsn_advance;
  bool poisoned;
};

// ---- Core functions ------------------------------------------------------

// Captures a baseline snapshot of the DB for later delta comparison.
inline auto capture_baseline(const DB &db) -> Baseline {
  Baseline bl;
  bl.next_lsn = db.engine_state()->next_lsn;
  for (const auto &[key, value] : db.iter_from({})) {
    bl.key_values[to_string(key)] = value;
  }
  return bl;
}

// Validates structural consistency of the published EngineState.
// Uses Catch2 CHECK macros — call from a TEST_CASE context.
inline void assert_consistent(const DB &db) {
  auto state = db.engine_state();

  // 1. live_bytes matches key_dir.
  std::map<std::uint32_t, std::uint64_t> computed_live;
  std::uint64_t max_seq = 0;
  for (auto it = state->key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    computed_live[entry.file_id] +=
        entry_size(key_span.size(), entry.value_size);

    // 2. No dangling file references.
    INFO("key references file_id=" << entry.file_id);
    CHECK(state->files->contains(entry.file_id));

    // 5. Track max sequence for next_lsn check.
    if (entry.sequence > max_seq) max_seq = entry.sequence;
  }

  for (const auto &[file_id, fs] : state->file_stats) {
    auto it = computed_live.find(file_id);
    auto expected_live = (it != computed_live.end()) ? it->second : 0ULL;
    INFO("file_id=" << file_id << " live_bytes");
    CHECK(fs.live_bytes == expected_live);
  }

  // 3. Active file exists.
  CHECK(state->files->contains(state->active_file_id));

  // 4. file_stats covers all files.
  for (const auto &[file_id, _] : *state->files) {
    INFO("file_id=" << file_id << " missing from file_stats");
    CHECK(state->file_stats.contains(file_id));
  }

  // 5. next_lsn ahead of all sequences.
  if (max_seq > 0) {
    CHECK(state->next_lsn > max_seq);
  }
}

// Validates the transition delta against the reference model's expected delta.
inline void assert_delta(const Baseline &before, const DB &db,
                         const ExpectedDelta &expected) {
  // Key membership.
  for (const auto &key : expected.keys_added) {
    INFO("expected key added: " << key);
    CHECK(db.contains_key(to_bytes(key)));
  }
  for (const auto &key : expected.keys_removed) {
    INFO("expected key removed: " << key);
    CHECK_FALSE(db.contains_key(to_bytes(key)));
  }

  // LSN advancement.
  auto after = db.engine_state();
  CHECK(after->next_lsn == before.next_lsn + expected.lsn_advance);

  // Structural consistency.
  assert_consistent(db);

  // Poison state.
  CHECK(db.is_poisoned() == expected.poisoned);
}

// Opens a fresh DB from on-disk files and verifies the recovered state matches
// the expected state. Validates the persistence invariant: committed
// transitions survive recovery, uncommitted ones do not.
//
// The caller must ensure no other DB instance has `dir` open (the original
// DB's destructor flushes hint files that recovery depends on).
inline void assert_recoverable(const std::filesystem::path &dir,
                               const Baseline &before,
                               const ExpectedDelta &expected) {
  auto recovered = DB::open(dir);

  // Pre-existing keys that were not removed must be present with correct values.
  for (const auto &[key, value] : before.key_values) {
    bool was_removed =
        std::ranges::find(expected.keys_removed, key) !=
        expected.keys_removed.end();
    if (!was_removed) {
      INFO("pre-existing key must survive recovery: " << key);
      CHECK(recovered.contains_key(to_bytes(key)));
      Bytes out;
      if (recovered.get({}, to_bytes(key), out)) {
        CHECK(out == value);
      }
    }
  }

  // Added keys must be present.
  for (const auto &key : expected.keys_added) {
    INFO("added key must survive recovery: " << key);
    CHECK(recovered.contains_key(to_bytes(key)));
  }

  // Removed keys must be absent.
  for (const auto &key : expected.keys_removed) {
    INFO("removed key must be absent after recovery: " << key);
    CHECK_FALSE(recovered.contains_key(to_bytes(key)));
  }

  // No extra keys.
  for (const auto &rk : recovered.keys_from({})) {
    auto key_str = to_string(rk);
    bool in_baseline =
        before.key_values.contains(key_str) &&
        std::ranges::find(expected.keys_removed, key_str) ==
            expected.keys_removed.end();
    bool in_added =
        std::ranges::find(expected.keys_added, key_str) !=
        expected.keys_added.end();
    INFO("unexpected key after recovery: " << key_str);
    CHECK((in_baseline || in_added));
  }

  // Structural consistency on recovered state.
  assert_consistent(recovered);
}

}  // namespace bytecask::testing

#endif  // BYTECASK_TESTING
