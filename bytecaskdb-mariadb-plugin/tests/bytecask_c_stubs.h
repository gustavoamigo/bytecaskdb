// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_c_stubs.h — Minimal stubs for ByteCask C API to enable testing
// MariaDBTxn without requiring a real ByteCask database.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <optional>

// Mock ByteCask handles as opaque pointers
struct bytecask_db { int dummy = 1; };
struct bytecask_snapshot { int dummy = 1; };
struct bytecask_write_plan { int dummy = 1; };
struct bytecask_iter { int dummy = 1; };

// Type aliases for consistency with C API
using bytecask_db_t = bytecask_db;
using bytecask_snapshot_t = bytecask_snapshot;
using bytecask_write_plan_t = bytecask_write_plan;
using bytecask_iter_t = bytecask_iter;

// Forward declare key encoding functions for tests
namespace bytecaskdb {
  // Stub implementations for key encoding functions used by MariaDBTxn
  inline bool key_belongs_to_table(const uint8_t* /*key*/, std::size_t /*len*/,
                                  uint32_t /*table_id*/) {
    return true;  // For tests, assume all keys belong to table
  }

  inline bool key_belongs_to_index(const uint8_t* /*key*/, std::size_t /*len*/,
                                  uint32_t /*table_id*/, uint16_t /*index_id*/) {
    return true;  // For tests, assume all keys belong to index
  }
}

// Global test state
namespace bytecask_test_state {
  // Mock snapshot storage
  inline std::map<const void*, std::map<std::vector<uint8_t>, std::vector<uint8_t>>> snapshots;

  // Mock iterator state
  struct IterState {
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>::iterator it;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>::iterator end;
    bool valid = false;
  };
  inline std::map<const void*, IterState> iterators;

  // Next available snapshot ID
  inline uintptr_t next_snapshot_id = 0x1000;
  inline uintptr_t next_iter_id = 0x2000;
}

// Snapshot operations
inline bytecask_snapshot_t* bytecask_snapshot(bytecask_db_t* /*db*/) {
  auto* snap = reinterpret_cast<bytecask_snapshot_t*>(++bytecask_test_state::next_snapshot_id);

  // Initialize empty snapshot
  bytecask_test_state::snapshots[snap] = {};

  return snap;
}

inline void bytecask_snapshot_free(bytecask_snapshot_t* snap) {
  if (snap) {
    bytecask_test_state::snapshots.erase(snap);
  }
}

inline int bytecask_snapshot_get(const bytecask_snapshot_t* snap,
                                const uint8_t* key, std::size_t key_len,
                                uint8_t** out_val, std::size_t* out_val_len) {
  if (!snap || !key || !out_val || !out_val_len) {
    return -1;
  }

  *out_val = nullptr;
  *out_val_len = 0;

  auto snap_it = bytecask_test_state::snapshots.find(snap);
  if (snap_it == bytecask_test_state::snapshots.end()) {
    return 0; // Snapshot not found
  }

  std::vector<uint8_t> k(key, key + key_len);
  auto val_it = snap_it->second.find(k);
  if (val_it == snap_it->second.end()) {
    return 0; // Key not found
  }

  const auto& val = val_it->second;
  if (!val.empty()) {
    *out_val = static_cast<uint8_t*>(malloc(val.size()));
    if (*out_val) {
      std::memcpy(*out_val, val.data(), val.size());
      *out_val_len = val.size();
    }
  }

  return 1; // Found
}

inline int bytecask_snapshot_contains_key(const bytecask_snapshot_t* snap,
                                         const uint8_t* key, std::size_t key_len) {
  if (!snap || !key) {
    return 0;
  }

  auto snap_it = bytecask_test_state::snapshots.find(snap);
  if (snap_it == bytecask_test_state::snapshots.end()) {
    return 0;
  }

  std::vector<uint8_t> k(key, key + key_len);
  return snap_it->second.count(k) > 0 ? 1 : 0;
}

// Iterator operations
inline bytecask_iter_t* bytecask_snapshot_iter_open(bytecask_snapshot_t* snap,
                                                   const uint8_t* from,
                                                   std::size_t from_len) {
  if (!snap) {
    return nullptr;
  }

  auto snap_it = bytecask_test_state::snapshots.find(snap);
  if (snap_it == bytecask_test_state::snapshots.end()) {
    return nullptr;
  }

  auto* iter = reinterpret_cast<bytecask_iter_t*>(++bytecask_test_state::next_iter_id);

  std::vector<uint8_t> from_key;
  if (from && from_len > 0) {
    from_key.assign(from, from + from_len);
  }

  auto& iter_state = bytecask_test_state::iterators[iter];
  if (from_key.empty()) {
    iter_state.it = snap_it->second.begin();
  } else {
    iter_state.it = snap_it->second.lower_bound(from_key);
  }
  iter_state.end = snap_it->second.end();
  iter_state.valid = (iter_state.it != iter_state.end);

  return iter;
}

inline int bytecask_iter_valid(const bytecask_iter_t* iter) {
  if (!iter) return 0;

  auto it = bytecask_test_state::iterators.find(iter);
  return (it != bytecask_test_state::iterators.end() && it->second.valid) ? 1 : 0;
}

inline int bytecask_iter_key(const bytecask_iter_t* iter,
                            uint8_t** out_key, std::size_t* out_key_len) {
  if (!iter || !out_key || !out_key_len) {
    return -1;
  }

  auto it = bytecask_test_state::iterators.find(iter);
  if (it == bytecask_test_state::iterators.end() || !it->second.valid) {
    return -1;
  }

  const auto& key = it->second.it->first;
  *out_key = static_cast<uint8_t*>(malloc(key.size()));
  if (*out_key) {
    std::memcpy(*out_key, key.data(), key.size());
    *out_key_len = key.size();
    return 0;
  }

  return -1;
}

inline int bytecask_iter_value(const bytecask_iter_t* iter,
                              uint8_t** out_val, std::size_t* out_val_len) {
  if (!iter || !out_val || !out_val_len) {
    return -1;
  }

  auto it = bytecask_test_state::iterators.find(iter);
  if (it == bytecask_test_state::iterators.end() || !it->second.valid) {
    return -1;
  }

  const auto& val = it->second.it->second;
  *out_val = static_cast<uint8_t*>(malloc(val.size()));
  if (*out_val) {
    std::memcpy(*out_val, val.data(), val.size());
    *out_val_len = val.size();
    return 0;
  }

  return -1;
}

inline int bytecask_iter_next(bytecask_iter_t* iter) {
  if (!iter) return 0;

  auto it = bytecask_test_state::iterators.find(iter);
  if (it == bytecask_test_state::iterators.end() || !it->second.valid) {
    return 0;
  }

  ++it->second.it;
  it->second.valid = (it->second.it != it->second.end);

  return it->second.valid ? 1 : 0;
}

inline void bytecask_iter_free(bytecask_iter_t* iter) {
  if (iter) {
    bytecask_test_state::iterators.erase(iter);
  }
}

// Write plan operations (minimal stubs)
inline bytecask_write_plan_t* bytecask_write_plan_new_with_snapshot(bytecask_snapshot_t* snap) {
  // In tests, we just need to consume the snapshot
  bytecask_snapshot_free(snap);
  return reinterpret_cast<bytecask_write_plan_t*>(0x3000);
}

inline void bytecask_write_plan_put(bytecask_write_plan_t* /*plan*/,
                                   const uint8_t* /*key*/, std::size_t /*key_len*/,
                                   const uint8_t* /*val*/, std::size_t /*val_len*/) {
  // No-op in tests
}

inline void bytecask_write_plan_del(bytecask_write_plan_t* /*plan*/,
                                   const uint8_t* /*key*/, std::size_t /*key_len*/) {
  // No-op in tests
}

inline int bytecask_apply_batch(bytecask_db_t* /*db*/, bytecask_write_plan_t* /*plan*/, int /*sync*/) {
  return 1; // Success
}

inline void bytecask_free_buf(void* buf) {
  free(buf);
}

// Reverse iterator stubs
inline bytecask_iter_t* bytecask_snapshot_riter_open(bytecask_snapshot_t* snap,
                                                     const uint8_t* from,
                                                     std::size_t from_len) {
  // For test simplicity, just return a regular forward iterator
  return bytecask_snapshot_iter_open(snap, from, from_len);
}