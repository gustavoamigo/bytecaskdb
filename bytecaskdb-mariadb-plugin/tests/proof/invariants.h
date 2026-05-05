// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// invariants.h — Reusable assertion helpers for proof tests.
//
// Each function validates one of the P-INV invariants defined in
// docs/correctness_validation.md. Test cases compose them to verify
// correctness across DML shapes, index configurations, and failure classes.

#pragma once

#include "bytecask.hpp"
#include "catalog.h"
#include "key_encoding.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bytecaskdb::testing {

struct PluginBaseline {
  std::set<std::vector<uint8_t>> pk_keys;
  std::map<uint16_t, std::set<std::vector<uint8_t>>> sec_keys;
  int64_t row_count;
};

struct PluginExpectedDelta {
  int64_t row_count_delta{0};
  int pk_keys_added{0};
  int pk_keys_removed{0};
  std::map<uint16_t, int> sec_keys_added;
  std::map<uint16_t, int> sec_keys_removed;
  int error_code{0};
  bool degraded{false};
};

inline PluginBaseline capture_baseline(bytecask::DB &db, uint32_t table_id,
                                       const std::vector<uint16_t> &index_ids,
                                       int64_t row_count) {
  PluginBaseline b;
  b.row_count = row_count;

  auto lo = table_id_prefix(table_id);
  auto hi = table_id_upper_bound(table_id);
  bytecask::Bytes val;
  for (auto &entry : db.iter_from({}, bytecask::BytesView{
           reinterpret_cast<const std::byte *>(lo.data()), lo.size()})) {
    auto kp = reinterpret_cast<const uint8_t *>(entry.key.data());
    if (entry.key.size() < lo.size() ||
        std::memcmp(kp, hi.data(), hi.size()) >= 0)
      break;
    b.pk_keys.emplace(kp, kp + entry.key.size());
  }

  for (auto idx_id : index_ids) {
    auto ilo = index_id_prefix(table_id, idx_id);
    auto ihi = index_id_upper_bound(table_id, idx_id);
    for (auto &entry : db.iter_from({}, bytecask::BytesView{
             reinterpret_cast<const std::byte *>(ilo.data()), ilo.size()})) {
      auto kp = reinterpret_cast<const uint8_t *>(entry.key.data());
      if (entry.key.size() < ilo.size() ||
          std::memcmp(kp, ihi.data(), ihi.size()) >= 0)
        break;
      b.sec_keys[idx_id].emplace(kp, kp + entry.key.size());
    }
  }

  return b;
}

// P-INV-1: Row counter matches actual PK key count in the store.
inline void assert_counter_matches_pk_count(bytecask::DB &db,
                                            uint32_t table_id,
                                            int64_t reported_count) {
  auto lo = table_id_prefix(table_id);
  auto hi = table_id_upper_bound(table_id);
  int64_t actual = 0;
  for (auto &entry : db.iter_from({}, bytecask::BytesView{
           reinterpret_cast<const std::byte *>(lo.data()), lo.size()})) {
    auto kp = reinterpret_cast<const uint8_t *>(entry.key.data());
    if (entry.key.size() < lo.size() ||
        std::memcmp(kp, hi.data(), hi.size()) >= 0)
      break;
    ++actual;
  }
  INFO("P-INV-1: reported_count=" << reported_count << " actual_pk_keys=" << actual);
  REQUIRE(reported_count == actual);
}

// P-INV-2: Row counter delta matches expected change.
inline void assert_counter_delta(int64_t before_count, int64_t after_count,
                                 int64_t expected_delta) {
  INFO("P-INV-2: before=" << before_count << " after=" << after_count
       << " expected_delta=" << expected_delta);
  REQUIRE((after_count - before_count) == expected_delta);
}

// P-INV-3: Secondary index key count matches PK key count (for non-unique
// indexes where every row has exactly one secondary entry).
inline void assert_sec_index_count_matches_pk(bytecask::DB &db,
                                              uint32_t table_id,
                                              uint16_t index_id) {
  auto lo = table_id_prefix(table_id);
  auto hi = table_id_upper_bound(table_id);
  int64_t pk_count = 0;
  for (auto &entry : db.iter_from({}, bytecask::BytesView{
           reinterpret_cast<const std::byte *>(lo.data()), lo.size()})) {
    auto kp = reinterpret_cast<const uint8_t *>(entry.key.data());
    if (entry.key.size() < lo.size() ||
        std::memcmp(kp, hi.data(), hi.size()) >= 0)
      break;
    ++pk_count;
  }

  auto ilo = index_id_prefix(table_id, index_id);
  auto ihi = index_id_upper_bound(table_id, index_id);
  int64_t sec_count = 0;
  for (auto &entry : db.iter_from({}, bytecask::BytesView{
           reinterpret_cast<const std::byte *>(ilo.data()), ilo.size()})) {
    auto kp = reinterpret_cast<const uint8_t *>(entry.key.data());
    if (entry.key.size() < ilo.size() ||
        std::memcmp(kp, ihi.data(), ihi.size()) >= 0)
      break;
    ++sec_count;
  }

  INFO("P-INV-3: pk_count=" << pk_count << " sec_count=" << sec_count
       << " index_id=" << index_id);
  REQUIRE(pk_count == sec_count);
}

// P-INV-6: Error code matches expected outcome.
inline void assert_error_code(int actual_rc, int expected_rc) {
  INFO("P-INV-6: actual_rc=" << actual_rc << " expected_rc=" << expected_rc);
  REQUIRE(actual_rc == expected_rc);
}

} // namespace bytecaskdb::testing
