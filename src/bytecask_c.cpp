// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_c.cpp — C API implementation for ByteCaskDB.
//
// Contains no MariaDB headers. Bridges the stable C API (include/bytecask_c.h)
// to the C++23 module-based engine.
//
// Build note: this file is compiled by mariadb/CMakeLists.txt as part of the
// plugin — NOT by xmake. This is intentional: it keeps libbytecask.a free of
// C ABI symbols so that the test suite links against a pure C++23 engine.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>

#include "../include/bytecask_c.h"
import bytecask;

// ---------------------------------------------------------------------------
// Thread-local error string
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
// thread_local destruction is intentional: each thread's error message lives
// as long as the thread and is released when the thread exits.
static thread_local std::string tl_errmsg;
#pragma clang diagnostic pop

static void set_errmsg(const char *msg) {
  tl_errmsg = msg ? msg : "";
}

static void clear_errmsg() { tl_errmsg.clear(); }

// ---------------------------------------------------------------------------
// Opaque structs
// ---------------------------------------------------------------------------

// bytecask::DB is neither copyable nor movable.  We wrap it in a struct
// and use guaranteed copy elision (C++17) in the member initializer to
// construct the DB directly inside a heap-allocated wrapper.
struct bytecask_db {
  bytecask::DB db;
  bytecask_db(const char *dir, bytecask::Options opts)
      : db{bytecask::DB::open(dir, std::move(opts))} {}
};

struct bytecask_iter {
  bytecask::EntryIterator cur;
  bool valid;

  explicit bytecask_iter(bytecask::EntryIterator c)
      : cur{std::move(c)}, valid{cur != std::default_sentinel} {}

  void advance() {
    if (valid) {
      ++cur;
      valid = (cur != std::default_sentinel);
    }
  }
};

// ---------------------------------------------------------------------------
// Helper — copy bytes out into a malloc'd buffer
// ---------------------------------------------------------------------------
static uint8_t *dup_bytes(const std::byte *src, std::size_t len) {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  auto *buf = static_cast<uint8_t *>(std::malloc(len));
  if (buf && len > 0) {
    std::memcpy(buf, src, len);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

bytecask_db_t *bytecask_open(const char *dir, unsigned recovery_threads) {
  clear_errmsg();
  try {
    bytecask::Options opts;
    if (recovery_threads > 0) {
      opts.recovery_threads = recovery_threads;
    }
    return new bytecask_db{dir, std::move(opts)};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

void bytecask_close(bytecask_db_t *db) {
  delete db;
}

// ---------------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------------

int bytecask_put(bytecask_db_t *db,
                 const uint8_t *key, std::size_t key_len,
                 const uint8_t *val, std::size_t val_len,
                 int sync) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    bytecask::WriteOptions opts;
    opts.sync = (sync != 0);
    db->db.put(opts,
               bytecask::BytesView{reinterpret_cast<const std::byte *>(key), key_len},
               bytecask::BytesView{reinterpret_cast<const std::byte *>(val), val_len});
    return 0;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Del
// ---------------------------------------------------------------------------

int bytecask_del(bytecask_db_t *db,
                 const uint8_t *key, std::size_t key_len,
                 int sync) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    bytecask::WriteOptions opts;
    opts.sync = (sync != 0);
    bool existed = db->db.del(
        opts,
        bytecask::BytesView{reinterpret_cast<const std::byte *>(key), key_len});
    return existed ? 1 : 0;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

int bytecask_get(bytecask_db_t *db,
                 const uint8_t *key, std::size_t key_len,
                 uint8_t **out_val, std::size_t *out_val_len) {
  clear_errmsg();
  if (!db || !out_val || !out_val_len) {
    set_errmsg("null argument");
    return -1;
  }
  *out_val = nullptr;
  *out_val_len = 0;
  try {
    bytecask::ReadOptions opts{};
    bytecask::Bytes buf;
    bool found = db->db.get(
        opts,
        bytecask::BytesView{reinterpret_cast<const std::byte *>(key), key_len},
        buf);
    if (!found) { return 0; }
    auto *copy = dup_bytes(buf.data(), buf.size());
    if (!copy && !buf.empty()) {
      set_errmsg("out of memory");
      return -1;
    }
    *out_val     = copy;
    *out_val_len = buf.size();
    return 1;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Contains key
// ---------------------------------------------------------------------------

int bytecask_contains_key(bytecask_db_t *db,
                          const uint8_t *key, std::size_t key_len) {
  if (!db) { return 0; }
  return db->db.contains_key(
             bytecask::BytesView{reinterpret_cast<const std::byte *>(key), key_len})
             ? 1
             : 0;
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

bytecask_iter_t *bytecask_iter_open(bytecask_db_t *db,
                                    const uint8_t *from, std::size_t from_len) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return nullptr; }
  try {
    bytecask::ReadOptions opts{};
    bytecask::BytesView from_view{};
    if (from && from_len > 0) {
      from_view = bytecask::BytesView{reinterpret_cast<const std::byte *>(from), from_len};
    }
    auto range = db->db.iter_from(opts, from_view);
    return new bytecask_iter{range.begin()};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

int bytecask_iter_next(bytecask_iter_t *iter) {
  if (!iter) { return 0; }
  iter->advance();
  return iter->valid ? 1 : 0;
}

int bytecask_iter_valid(const bytecask_iter_t *iter) {
  return (iter && iter->valid) ? 1 : 0;
}

int bytecask_iter_key(const bytecask_iter_t *iter,
                      uint8_t **out_key, std::size_t *out_key_len) {
  if (!iter || !iter->valid || !out_key || !out_key_len) {
    if (out_key)     *out_key     = nullptr;
    if (out_key_len) *out_key_len = 0;
    return -1;
  }
  try {
    const auto &[k, v] = *iter->cur;
    // Key exposes begin()/end() iterators over std::byte; no data() member.
    const std::byte *kptr = k.size() > 0 ? &*k.begin() : nullptr;
    auto *copy = dup_bytes(kptr, k.size());
    if (!copy && k.size() > 0) {
      set_errmsg("out of memory");
      return -1;
    }
    *out_key     = copy;
    *out_key_len = k.size();
    return 0;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

int bytecask_iter_value(const bytecask_iter_t *iter,
                        uint8_t **out_val, std::size_t *out_val_len) {
  if (!iter || !iter->valid || !out_val || !out_val_len) {
    if (out_val)     *out_val     = nullptr;
    if (out_val_len) *out_val_len = 0;
    return -1;
  }
  try {
    const auto &[k, v] = *iter->cur;
    auto *copy = dup_bytes(v.data(), v.size());
    if (!copy && !v.empty()) {
      set_errmsg("out of memory");
      return -1;
    }
    *out_val     = copy;
    *out_val_len = v.size();
    return 0;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

void bytecask_iter_free(bytecask_iter_t *iter) {
  delete iter;
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

void bytecask_free_buf(void *buf) {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  std::free(buf);
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

const char *bytecask_errmsg(void) {
  return tl_errmsg.empty() ? nullptr : tl_errmsg.c_str();
}
