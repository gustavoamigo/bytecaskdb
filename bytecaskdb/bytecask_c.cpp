// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_c.cpp — C API implementation for ByteCaskDB.
//
// Contains no MariaDB headers. Bridges the stable C API (include/bytecask_c.h)
// to the C++23 module-based engine.
//
// Build note: compiled by xmake into libbytecask.a alongside the other
// bytecaskdb/ sources. Contains no MariaDB headers.

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

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
// Helper — convert uint8_t pointer + length to BytesView
// ---------------------------------------------------------------------------
static bytecask::BytesView to_view(const uint8_t *p, std::size_t len) {
  return {reinterpret_cast<const std::byte *>(p), len};
}

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
  std::variant<bytecask::EntryIterator, bytecask::ReverseEntryIterator> iter;
  bool valid;

  explicit bytecask_iter(bytecask::EntryIterator c)
      : iter{std::move(c)},
        valid{std::get<bytecask::EntryIterator>(iter) != std::default_sentinel} {}

  explicit bytecask_iter(bytecask::ReverseEntryIterator c)
      : iter{std::move(c)},
        valid{std::get<bytecask::ReverseEntryIterator>(iter) != std::default_sentinel} {}

  void advance() {
    if (!valid) return;

    std::visit([this](auto& it) {
      ++it;
      this->valid = (it != std::default_sentinel);
    }, iter);
  }

  auto get_current() const -> std::pair<std::span<const std::byte>, std::span<const std::byte>> {
    return std::visit([](const auto& it) -> std::pair<std::span<const std::byte>, std::span<const std::byte>> {
      const auto& entry = *it;
      return {entry.key, entry.value};
    }, iter);
  }
};

struct bytecask_snapshot {
  bytecask::Snapshot snap;
  explicit bytecask_snapshot(bytecask::Snapshot s) : snap{std::move(s)} {}
};

struct bytecask_write_plan {
  bytecask::WritePlan plan;
  explicit bytecask_write_plan(bytecask::WritePlan p) : plan{std::move(p)} {}
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
// Helper — convert C write options to bytecask::WriteOptions, and fill a
// nullable CommitResult out-param from the module's result type.
// ---------------------------------------------------------------------------
static bytecask::WriteOptions to_write_options(const bytecask_write_options_t *opts) {
  if (!opts) return {};
  bytecask::WriteOptions wo;
  wo.sync = (opts->sync != 0);
  wo.solo = (opts->solo != 0);
  return wo;
}

static void fill_result(bytecask_commit_result_t *out, const bytecask::CommitResult &r) {
  if (!out) return;
  out->sequence = r.sequence;
  out->durable = r.durable ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------------

int bytecask_put(bytecask_db_t *db,
                 const uint8_t *key, std::size_t key_len,
                 const uint8_t *val, std::size_t val_len,
                 const bytecask_write_options_t *opts,
                 bytecask_commit_result_t *out) {
  clear_errmsg();
  if (out) *out = {};
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    auto result = db->db.put(to_write_options(opts), to_view(key, key_len),
                             to_view(val, val_len));
    fill_result(out, result);
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
                 const bytecask_write_options_t *opts,
                 bytecask_commit_result_t *out) {
  clear_errmsg();
  if (out) *out = {};
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    auto result = db->db.del(to_write_options(opts), to_view(key, key_len));
    if (!result) return 0;
    fill_result(out, *result);
    return 1;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Del range
// ---------------------------------------------------------------------------

int bytecask_del_range(bytecask_db_t *db,
                       const uint8_t *from, std::size_t from_len,
                       const uint8_t *to, std::size_t to_len,
                       const bytecask_write_options_t *opts,
                       bytecask_commit_result_t *out) {
  clear_errmsg();
  if (out) *out = {};
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    auto result = db->db.del_range(to_write_options(opts), to_view(from, from_len),
                                   to_view(to, to_len));
    fill_result(out, result);
    return 0;
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
    bool found = db->db.get(opts, to_view(key, key_len), buf);
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
  return db->db.contains_key({}, to_view(key, key_len)) ? 1 : 0;
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
      from_view = to_view(from, from_len);
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
    const auto [key_span, value_span] = iter->get_current();
    // Key exposes begin()/end() iterators over std::byte; no data() member.
    const std::byte *kptr = key_span.size() > 0 ? key_span.data() : nullptr;
    auto *copy = dup_bytes(kptr, key_span.size());
    if (!copy && key_span.size() > 0) {
      set_errmsg("out of memory");
      return -1;
    }
    *out_key     = copy;
    *out_key_len = key_span.size();
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
    const auto [key_span, value_span] = iter->get_current();
    auto *copy = dup_bytes(value_span.data(), value_span.size());
    if (!copy && !value_span.empty()) {
      set_errmsg("out of memory");
      return -1;
    }
    *out_val     = copy;
    *out_val_len = value_span.size();
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
// Reverse Iteration
// ---------------------------------------------------------------------------

bytecask_iter_t *bytecask_riter_open(bytecask_db_t *db,
                                     const uint8_t *from, std::size_t from_len) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return nullptr; }
  try {
    bytecask::ReadOptions opts{};
    bytecask::BytesView from_view{};
    if (from && from_len > 0) {
      from_view = to_view(from, from_len);
    }
    auto range = db->db.riter_from(opts, from_view);
    return new bytecask_iter{range.begin()};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

int bytecask_riter_next(bytecask_iter_t *iter) {
  if (!iter) { return 0; }
  iter->advance();
  return iter->valid ? 1 : 0;
}

bytecask_iter_t *bytecask_snapshot_riter_open(bytecask_snapshot_t *snap,
                                              const uint8_t *from,
                                              std::size_t from_len) {
  clear_errmsg();
  if (!snap) { set_errmsg("null snapshot handle"); return nullptr; }
  try {
    bytecask::BytesView from_view{};
    if (from && from_len > 0) {
      from_view = to_view(from, from_len);
    }
    auto range = snap->snap.riter_from({}, from_view);
    return new bytecask_iter{range.begin()};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

bytecask_snapshot_t *bytecask_snapshot(bytecask_db_t *db) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return nullptr; }
  try {
    return new struct bytecask_snapshot{db->db.snapshot()};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

int bytecask_snapshot_get(const bytecask_snapshot_t *snap,
                          const uint8_t *key, std::size_t key_len,
                          uint8_t **out_val, std::size_t *out_val_len) {
  clear_errmsg();
  if (!snap || !out_val || !out_val_len) {
    set_errmsg("null argument");
    return -1;
  }
  *out_val = nullptr;
  *out_val_len = 0;
  try {
    bytecask::Bytes buf;
    bool found = snap->snap.get({}, to_view(key, key_len), buf);
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

int bytecask_snapshot_contains_key(const bytecask_snapshot_t *snap,
                                   const uint8_t *key, std::size_t key_len) {
  if (!snap) { return 0; }
  return snap->snap.contains_key({}, to_view(key, key_len)) ? 1 : 0;
}

bytecask_iter_t *bytecask_snapshot_iter_open(bytecask_snapshot_t *snap,
                                             const uint8_t *from,
                                             std::size_t from_len) {
  clear_errmsg();
  if (!snap) { set_errmsg("null snapshot handle"); return nullptr; }
  try {
    bytecask::BytesView from_view{};
    if (from && from_len > 0) {
      from_view = to_view(from, from_len);
    }
    auto range = snap->snap.iter_from({}, from_view);
    return new bytecask_iter{range.begin()};
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return nullptr;
  }
}

void bytecask_snapshot_free(bytecask_snapshot_t *snap) {
  delete snap;
}

// ---------------------------------------------------------------------------
// Write plans
// ---------------------------------------------------------------------------

bytecask_write_plan_t *bytecask_write_plan_new(void) {
  return new bytecask_write_plan{bytecask::WritePlan{}};
}

bytecask_write_plan_t *bytecask_write_plan_new_with_snapshot(
    bytecask_snapshot_t *snap) {
  if (!snap) {
    set_errmsg("null snapshot handle");
    return nullptr;
  }
  auto *plan = new bytecask_write_plan{
      bytecask::WritePlan{std::move(snap->snap)}};
  delete snap;
  return plan;
}

void bytecask_write_plan_put(bytecask_write_plan_t *plan,
                             const uint8_t *key, std::size_t key_len,
                             const uint8_t *val, std::size_t val_len) {
  if (!plan) { return; }
  plan->plan.put(to_view(key, key_len), to_view(val, val_len));
}

void bytecask_write_plan_del(bytecask_write_plan_t *plan,
                             const uint8_t *key, std::size_t key_len) {
  if (!plan) { return; }
  plan->plan.del(to_view(key, key_len));
}

void bytecask_write_plan_del_range(bytecask_write_plan_t *plan,
                                   const uint8_t *from, std::size_t from_len,
                                   const uint8_t *to, std::size_t to_len) {
  if (!plan) { return; }
  plan->plan.del_range(to_view(from, from_len), to_view(to, to_len));
}

void bytecask_write_plan_ensure_present(bytecask_write_plan_t *plan,
                                        const uint8_t *key,
                                        std::size_t key_len) {
  if (!plan) { return; }
  plan->plan.ensure_present(to_view(key, key_len));
}

void bytecask_write_plan_ensure_absent(bytecask_write_plan_t *plan,
                                       const uint8_t *key,
                                       std::size_t key_len) {
  if (!plan) { return; }
  plan->plan.ensure_absent(to_view(key, key_len));
}

int bytecask_write_plan_ensure_unchanged(bytecask_write_plan_t *plan,
                                         const uint8_t *key,
                                         std::size_t key_len) {
  clear_errmsg();
  if (!plan) { set_errmsg("null plan handle"); return -1; }
  try {
    plan->plan.ensure_unchanged(to_view(key, key_len));
    return 0;
  } catch (const std::logic_error &e) {
    set_errmsg(e.what());
    return -1;
  }
}

int bytecask_write_plan_ensure_range_unchanged(bytecask_write_plan_t *plan,
                                               const uint8_t *from,
                                               std::size_t from_len,
                                               const uint8_t *to,
                                               std::size_t to_len) {
  clear_errmsg();
  if (!plan) { set_errmsg("null plan handle"); return -1; }
  try {
    plan->plan.ensure_range_unchanged(to_view(from, from_len),
                                      to_view(to, to_len));
    return 0;
  } catch (const std::logic_error &e) {
    set_errmsg(e.what());
    return -1;
  }
}

void bytecask_write_plan_free(bytecask_write_plan_t *plan) {
  delete plan;
}

// ---------------------------------------------------------------------------
// apply_batch
// ---------------------------------------------------------------------------

int bytecask_apply_batch(bytecask_db_t *db,
                         bytecask_write_plan_t *plan,
                         const bytecask_write_options_t *opts,
                         bytecask_commit_result_t *out) {
  clear_errmsg();
  if (out) *out = {};
  if (!db || !plan) {
    set_errmsg(!db ? "null db handle" : "null plan handle");
    delete plan;
    return -1;
  }
  try {
    auto result = db->db.apply_batch(to_write_options(opts), std::move(plan->plan));
    delete plan;
    if (!result) return 0;
    fill_result(out, *result);
    return 1;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    delete plan;
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Durable sequence
// ---------------------------------------------------------------------------

uint64_t bytecask_durable_sequence(bytecask_db_t *db,
                                   uint64_t min_sequence,
                                   uint64_t timeout_ms) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return 0; }
  try {
    return db->db.durable_sequence(min_sequence,
                                   std::chrono::milliseconds{timeout_ms});
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return 0;
  }
}

// ---------------------------------------------------------------------------
// Vacuum
// ---------------------------------------------------------------------------

int bytecask_vacuum(bytecask_db_t *db) {
  clear_errmsg();
  if (!db) { set_errmsg("null db handle"); return -1; }
  try {
    bool vacuumed = db->db.vacuum();
    return vacuumed ? 1 : 0;
  } catch (const std::exception &e) {
    set_errmsg(e.what());
    return -1;
  }
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
