// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// plan_api.cpp — the plan API of bc_plan.h on the engine's public API: the
// extern "C" definitions, and bc_ctx itself.
//
// Every call resolves its handles against tables owned by the context. A
// handle carries the context's epoch, bumped by bc_ctx_reset, so a handle
// from an earlier execution is stale. A view owned by a cursor also records
// the cursor's step count, so stepping invalidates it. Errors are sticky: the
// first one is kept, and every later call returns its neutral value.

// Standard library headers must be included BEFORE the module imports, as
// bytecaskdb/bytecask_hpp.cpp does.
//
// On macOS, libc++ marks many inline/template functions with
// _LIBCPP_HIDE_FROM_ABI, which expands to __attribute__((__abi_tag__("..."))).
// When a module is imported, the compiler loads its BMI and resolves the
// exported types that reference std types (bc_params holds a std::vector of
// std::variant). On LLVM 22+ those std declarations become "known" in the
// importing TU, and a later #include of the same header then tries to add
// __abi_tag__ attributes to declarations that already exist, which LLVM 22
// rejects as a hard error: "cannot add 'abi_tag' attribute in a redeclaration".
// Including the std headers first establishes their canonical declarations in
// this TU; the import then finds them already present and consistent.
#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

// The C contract, and the engine through its public header.
#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

// bc_db, bc_catalog, bc_params and bc_ctx_affected, which bc_plan.h leaves
// opaque, and the catalog and codecs through it.
import bytecask.sql.runtime;

using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::Catalog;
using bytecask::sql::KeyLayout;
using bytecask::sql::PhysType;
using bytecask::sql::RowLayout;
using bytecask::sql::RowWriter;

namespace {

constexpr std::uint32_t kNoCursor = UINT32_MAX;

struct ViewEntry {
  const std::byte* data;
  std::uint32_t len;
  std::uint32_t cursor;  // kNoCursor: lives until reset
  std::uint64_t step;    // the cursor's step when the view was made
};

struct Cursor {
  const KeyLayout* layout{nullptr};
  Bytes prefix;  // every key of this table or index starts with it
  Bytes lo, hi;  // hi empty: to the end of the prefix
  std::uint32_t flags{0};
  std::optional<bytecask::EntryIterator> fwd;
  std::optional<bytecask::ReverseEntryIterator> rev;
  bool started{false};
  bool done{false};
  bool positioned{false};
  std::uint64_t step{0};
  Bytes key, value;
};

struct KeyBuilder {
  const KeyLayout* layout;
  Bytes buf;
  std::size_t next_part{0};
  bool done{false};
};

struct RowBuilder {
  const RowLayout* layout;
  RowWriter writer;
  bool done{false};
};

enum class EffectKind { Put, Del, DelRange, GuardAbsent };

struct Effect {
  EffectKind kind;
  Bytes a, b;
};

auto as_u8(const std::byte* p) -> const std::uint8_t* {
  return reinterpret_cast<const std::uint8_t*>(p);  // the runtime hands raw rows to C callers
}

}  // namespace

struct bc_ctx {
  bytecask::DB* db;
  const Catalog* cat;
  std::vector<std::byte> arena;
  std::size_t arena_used{0};
  std::uint32_t epoch{1};

  const bc_params* params{nullptr};
  bc_budget budget{};
  std::uint64_t reads{0};
  std::optional<bytecask::Snapshot> snap;

  bc_error_code error{BC_OK};
  std::string message;

  std::vector<ViewEntry> views;
  std::vector<Cursor> cursors;
  std::vector<KeyBuilder> keys;
  std::vector<RowBuilder> rows;
  std::vector<Effect> effects;

  // Results: rows in the row format, each preceded by its u32 length.
  Bytes results;
  std::size_t result_pos{0};
  std::optional<RowWriter> emit;
  const std::vector<PhysType>* emit_types{nullptr};
  std::uint32_t emit_col{0};
  std::int64_t affected{-1};

  bc_ctx(bytecask::DB* d, const Catalog* c, std::uint32_t arena_bytes)
      : db{d}, cat{c}, arena(arena_bytes) {}

  auto ok() const -> bool { return error == BC_OK; }

  void fail(bc_error_code code, std::string msg) {
    if (error != BC_OK) return;
    error = code;
    message = std::move(msg);
  }

  // Maps an exception from the engine or the allocator to its error code.
  void fail_current_exception() {
    try {
      throw;
    } catch (const bytecask::DbDegraded& e) {
      fail(BC_E_DEGRADED, e.what());
    } catch (const bytecask::DbClosed& e) {
      fail(BC_E_CLOSED, e.what());
    } catch (const std::system_error& e) {
      fail(BC_E_IO, e.what());
    } catch (const std::invalid_argument& e) {
      fail(BC_E_LIMIT, e.what());
    } catch (const std::bad_alloc&) {
      fail(BC_E_MEMORY, "out of memory");
    } catch (const std::exception& e) {
      fail(BC_E_CORRUPT, e.what());
    }
  }

  auto charge() -> bool {
    if (!ok()) return false;
    ++reads;
    if (budget.max_reads != 0 && reads > budget.max_reads) {
      fail(BC_E_BUDGET, "work budget exhausted");
      return false;
    }
    if (budget.deadline_ns != 0 && (reads & 63) == 0) {
      auto now = std::chrono::steady_clock::now().time_since_epoch();
      if (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) >
          budget.deadline_ns) {
        fail(BC_E_BUDGET, "deadline passed");
        return false;
      }
    }
    return true;
  }

  // Copies bytes into the arena; nullptr (and BC_E_MEMORY) if it is full.
  auto arena_copy(BytesView b) -> const std::byte* {
    if (arena.size() - arena_used < b.size()) {
      fail(BC_E_MEMORY, "arena exhausted");
      return nullptr;
    }
    auto* p = arena.data() + arena_used;
    if (!b.empty()) std::memcpy(p, b.data(), b.size());
    arena_used += b.size();
    return p;
  }

  auto make_view(const std::byte* p, std::size_t len, std::uint32_t cursor = kNoCursor, std::uint64_t step = 0)
      -> bc_view {
    views.push_back({p, static_cast<std::uint32_t>(len), cursor, step});
    return {static_cast<std::uint32_t>(views.size() - 1), epoch};
  }

  auto arena_view(BytesView b) -> bc_view {
    const auto* p = arena_copy(b);
    if (p == nullptr) return {};
    return make_view(p, b.size());
  }

  // The bytes of a view; nullopt (and BC_E_HANDLE) if it is stale or foreign.
  // The zero handle is the empty view every neutral return gives.
  auto resolve(bc_view v) -> std::optional<BytesView> {
    if (v.g == 0 && v.i == 0) return BytesView{};
    if (v.g != epoch || v.i >= views.size()) {
      fail(BC_E_HANDLE, "stale or foreign view");
      return std::nullopt;
    }
    const auto& e = views[v.i];
    if (e.cursor != kNoCursor && (e.cursor >= cursors.size() || cursors[e.cursor].step != e.step)) {
      fail(BC_E_HANDLE, "view read after its cursor moved");
      return std::nullopt;
    }
    return BytesView{e.data, e.len};
  }

  // The cursor owning view v, for sub-views that share its lifetime.
  auto owner(bc_view v) const -> std::pair<std::uint32_t, std::uint64_t> {
    if (v.g == 0 || v.i >= views.size()) return {kNoCursor, 0};
    return {views[v.i].cursor, views[v.i].step};
  }

  template <typename H, typename T>
  auto resolve_handle(H h, std::vector<T>& table, const char* what) -> T* {
    if (!ok()) return nullptr;
    if (h.g != epoch || h.i >= table.size()) {
      fail(BC_E_HANDLE, std::string{"stale or foreign "} + what);
      return nullptr;
    }
    return &table[h.i];
  }

  auto param(std::uint32_t i) -> const bc_value* {
    if (!ok()) return nullptr;
    if (params == nullptr || i >= params->values.size()) {
      fail(BC_E_HANDLE, "parameter index out of range");
      return nullptr;
    }
    return &params->values[i];
  }

  void drop_execution_state() {
    cursors.clear();  // iterators before the snapshot they read
    snap.reset();
    views.clear();
    keys.clear();
    rows.clear();
    effects.clear();
    results.clear();
    result_pos = 0;
    emit.reset();
    emit_types = nullptr;
    emit_col = 0;
    affected = -1;
    arena_used = 0;
    reads = 0;
    ++epoch;
    if (epoch == 0) epoch = 1;  // 0 is the empty view's generation
  }

  void unimplemented(const char* fn) { fail(BC_E_HANDLE, std::string{fn} + " is not implemented by the prototype"); }
};

// Every entry point catches: an exception never crosses into a plan.
#define BC_GUARD_BEGIN try {
#define BC_GUARD_END(cx) \
  }                      \
  catch (...) {          \
    (cx)->fail_current_exception(); \
  }

namespace {

// Positions a cursor on its next entry within bounds; false at the end.
auto cursor_advance(bc_ctx* cx, Cursor& c) -> bool {
  auto reverse = (c.flags & BC_SCAN_REVERSE) != 0;
  auto in_prefix = [&](BytesView k) {
    return k.size() >= c.prefix.size() && std::ranges::equal(k.first(c.prefix.size()), c.prefix);
  };
  auto cmp = [](BytesView a, BytesView b) {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end(),
                                                  [](std::byte x, std::byte y) {
                                                    return std::to_integer<int>(x) <=> std::to_integer<int>(y);
                                                  });
  };
  while (true) {
    BytesView k, v;
    if (!reverse) {
      if (!c.started) {
        c.fwd.emplace(cx->snap->iter_from({}, c.lo).begin());
        c.started = true;
      } else {
        ++*c.fwd;
      }
      if (*c.fwd == std::default_sentinel) return false;
      k = (**c.fwd).key;
      v = (**c.fwd).value;
    } else {
      if (!c.started) {
        // riter_from starts at the last key <= from: the upper bound.
        auto from = c.hi.empty() ? bytecask::sql::key_successor(c.prefix) : c.hi;
        c.rev.emplace(cx->snap->riter_from({}, from).begin());
        c.started = true;
      } else {
        ++*c.rev;
      }
      if (*c.rev == std::default_sentinel) return false;
      k = (**c.rev).key;
      v = (**c.rev).value;
    }
    if (!cx->charge()) return false;
    if (!in_prefix(k)) {
      if (reverse && !c.hi.empty() && std::is_gt(cmp(k, c.prefix))) continue;  // the successor itself
      return false;
    }
    auto lo_c = cmp(k, c.lo);
    if (std::is_lt(lo_c)) {
      if (reverse) return false;
      continue;
    }
    if ((c.flags & BC_SCAN_LO_EXCLUSIVE) != 0 && std::is_eq(lo_c)) {
      if (reverse) return false;
      continue;
    }
    if (!c.hi.empty()) {
      auto hi_c = cmp(k, c.hi);
      auto past = (c.flags & BC_SCAN_HI_INCLUSIVE) != 0 ? std::is_gt(hi_c) : std::is_gteq(hi_c);
      if (past) {
        if (reverse) continue;
        return false;
      }
    }
    c.key.assign(k.begin(), k.end());
    c.value.assign(v.begin(), v.end());
    return true;
  }
}

// Reads one record through the snapshot into the arena.
auto read_record(bc_ctx* cx, BytesView key, bool* found) -> bc_view {
  if (found != nullptr) *found = false;
  if (!cx->charge()) return {};
  Bytes out;
  if (!cx->snap->get({}, key, out)) return {};
  auto v = cx->arena_view(out);
  if (found != nullptr) *found = cx->ok();
  return v;
}

auto key_builder_part(bc_ctx* cx, bc_key k, PhysType type, bool is_null) -> KeyBuilder* {
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return nullptr;
  if (b->done || b->next_part >= b->layout->parts.size()) {
    cx->fail(BC_E_HANDLE, "more key parts than the key has");
    return nullptr;
  }
  const auto& part = b->layout->parts[b->next_part];
  if (is_null ? !part.nullable : part.type != type) {
    cx->fail(BC_E_HANDLE, "key part of the wrong type");
    return nullptr;
  }
  ++b->next_part;
  return b;
}

auto add_effect(bc_ctx* cx, EffectKind kind, bc_view a, bc_view b) -> void {
  if (!cx->ok()) return;
  auto av = cx->resolve(a);
  auto bv = cx->resolve(b);
  if (!av || !bv) return;
  cx->effects.push_back({kind, Bytes(av->begin(), av->end()), Bytes(bv->begin(), bv->end())});
}

auto row_builder(bc_ctx* cx, bc_row r) -> RowBuilder* {
  auto* b = cx->resolve_handle(r, cx->rows, "row builder");
  if (b != nullptr && b->done) {
    cx->fail(BC_E_HANDLE, "row builder already ended");
    return nullptr;
  }
  return b;
}

auto check_schema(bc_ctx* cx, const bc_schema_ref* ref) -> bool {
  if (ref == nullptr) return true;
  // prototype: an exact catalog version, stricter than the design's
  // per-column compatibility check.
  if (ref->catalog_version != cx->cat->version()) {
    cx->fail(BC_E_LAYOUT, "plan compiled against catalog version " + std::to_string(ref->catalog_version) +
                              ", live version is " + std::to_string(cx->cat->version()));
    return false;
  }
  for (std::uint32_t i = 0; i < ref->n_read; ++i) {
    if (cx->cat->table(ref->tables_read[i]) == nullptr) {
      cx->fail(BC_E_LAYOUT, "a table the plan reads no longer exists");
      return false;
    }
  }
  for (std::uint32_t i = 0; i < ref->n_written; ++i) {
    if (cx->cat->table(ref->tables_written[i]) == nullptr) {
      cx->fail(BC_E_LAYOUT, "a table the plan writes no longer exists");
      return false;
    }
  }
  return true;
}

}  // namespace

auto bc_ctx_affected(const bc_ctx* cx) -> std::int64_t { return cx->affected; }

extern "C" {

// ---------------------------------------------------------------- runtime

bc_ctx* bc_ctx_new(bc_db* db, const bc_catalog* cat, uint32_t arena_bytes) {
  try {
    return new bc_ctx(db->db, cat->cat, arena_bytes);
  } catch (...) {
    return nullptr;
  }
}

void bc_ctx_free(bc_ctx* cx) {
  if (cx == nullptr) return;
  cx->drop_execution_state();
  delete cx;
}

void bc_ctx_begin(bc_ctx* cx, const bc_params* params, bc_budget budget) {
  cx->drop_execution_state();
  cx->error = BC_OK;
  cx->message.clear();
  cx->params = params;
  cx->budget = budget;
  BC_GUARD_BEGIN
  cx->snap.emplace(cx->db->snapshot());
  BC_GUARD_END(cx)
}

void bc_ctx_run(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* compiled_against) {
  if (!cx->ok()) return;
  if (!cx->snap) {
    cx->fail(BC_E_HANDLE, "bc_ctx_run without bc_ctx_begin");
    return;
  }
  if (!check_schema(cx, compiled_against)) return;
  plan(cx);
  if (cx->ok() && cx->emit) cx->fail(BC_E_LAYOUT, "result row begun and not ended");
}

enum bc_commit_result bc_ctx_commit(bc_ctx* cx, bool sync) {
  if (!cx->ok()) return BC_FAILED;
  if (cx->effects.empty()) return BC_COMMITTED;
  try {
    cx->cursors.clear();
    bytecask::WritePlan plan{std::move(*cx->snap)};
    cx->snap.reset();
    for (const auto& e : cx->effects) {
      switch (e.kind) {
        case EffectKind::Put: plan.put(e.a, e.b); break;
        case EffectKind::Del: plan.del(e.a); break;
        case EffectKind::DelRange: plan.del_range(e.a, e.b); break;
        case EffectKind::GuardAbsent: plan.ensure_absent(e.a); break;
      }
    }
    auto r = cx->db->apply_batch({.sync = sync}, std::move(plan));
    return r ? BC_COMMITTED : BC_CONFLICT;
  } catch (...) {
    cx->fail_current_exception();
    return BC_FAILED;
  }
}

bool bc_ctx_next_result(bc_ctx* cx, const uint8_t** row, uint32_t* len) {
  if (cx->result_pos + 4 > cx->results.size()) return false;
  std::uint32_t n = 0;
  std::memcpy(&n, cx->results.data() + cx->result_pos, 4);
  *row = as_u8(cx->results.data() + cx->result_pos + 4);
  *len = n;
  cx->result_pos += 4 + n;
  return true;
}

void bc_ctx_reset(bc_ctx* cx) { cx->drop_execution_state(); }

enum bc_error_code bc_error(const bc_ctx* cx) { return cx->error; }

const char* bc_error_message(const bc_ctx* cx) { return cx->message.c_str(); }

int bc_execute(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* ref, const bc_params* params, bc_budget budget,
               bool sync, uint32_t max_retries) {
  for (std::uint32_t attempt = 0;; ++attempt) {
    bc_ctx_begin(cx, params, budget);
    bc_ctx_run(cx, plan, ref);
    auto r = bc_ctx_commit(cx, sync);
    if (r != BC_CONFLICT || attempt >= max_retries) return r;
  }
}

// ------------------------------------------------- parameters and results

int64_t bc_param_i64(bc_ctx* cx, uint32_t i) {
  const auto* p = cx->param(i);
  if (p == nullptr) return 0;
  if (const auto* v = std::get_if<std::int64_t>(p)) return *v;
  cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not an integer");
  return 0;
}

double bc_param_f64(bc_ctx* cx, uint32_t) {
  cx->unimplemented("bc_param_f64");
  return 0;
}

bc_view bc_param_bytes(bc_ctx* cx, uint32_t i) {
  const auto* p = cx->param(i);
  if (p == nullptr) return {};
  if (const auto* v = std::get_if<Bytes>(p)) return cx->make_view(v->data(), v->size());
  cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not bytes");
  return {};
}

bool bc_param_null(bc_ctx* cx, uint32_t i) {
  const auto* p = cx->param(i);
  return p != nullptr && std::holds_alternative<std::monostate>(*p);
}

void bc_emit_begin(bc_ctx* cx, uint32_t layout) {
  if (!cx->ok()) return;
  const auto* types = cx->cat->result_types(layout);
  if (types == nullptr || cx->emit) {
    cx->fail(BC_E_LAYOUT, "bad result layout or a row already begun");
    return;
  }
  cx->emit.emplace(*cx->cat->row_layout(layout));
  cx->emit_types = types;
  cx->emit_col = 0;
}

namespace {
auto emit_slot(bc_ctx* cx, std::optional<PhysType> type) -> bool {
  if (!cx->ok()) return false;
  if (!cx->emit || cx->emit_col >= cx->emit_types->size() ||
      (type && (*cx->emit_types)[cx->emit_col] != *type)) {
    cx->fail(BC_E_LAYOUT, "emitted column does not match the result layout");
    return false;
  }
  return true;
}
}  // namespace

void bc_emit_i64(bc_ctx* cx, int64_t v) {
  if (!emit_slot(cx, PhysType::I64)) return;
  (void)cx->emit->set_i64(cx->emit_col++, v);
}

void bc_emit_f64(bc_ctx* cx, double) { cx->unimplemented("bc_emit_f64"); }

void bc_emit_bytes(bc_ctx* cx, bc_view v) {
  if (!emit_slot(cx, PhysType::Bytes)) return;
  auto b = cx->resolve(v);
  if (!b) return;
  (void)cx->emit->set_bytes(cx->emit_col++, *b);
}

void bc_emit_null(bc_ctx* cx) {
  if (!emit_slot(cx, std::nullopt)) return;
  (void)cx->emit->set_null(cx->emit_col++);
}

void bc_emit_end(bc_ctx* cx) {
  if (!cx->ok()) return;
  if (!cx->emit || cx->emit_col != cx->emit_types->size()) {
    cx->fail(BC_E_LAYOUT, "result row ended with the wrong number of columns");
    return;
  }
  BC_GUARD_BEGIN
  Bytes row;
  if (!cx->emit->finish(row)) {
    cx->fail(BC_E_LIMIT, "result row over the row format's limit");
    return;
  }
  // prototype: the result buffer is a heap vector, not part of the arena.
  auto n = static_cast<std::uint32_t>(row.size());
  auto at = cx->results.size();
  cx->results.resize(at + 4 + n);
  std::memcpy(cx->results.data() + at, &n, 4);
  std::memcpy(cx->results.data() + at + 4, row.data(), n);
  cx->emit.reset();
  BC_GUARD_END(cx)
}

void bc_emit_count(bc_ctx* cx, int64_t affected) {
  if (!cx->ok()) return;
  if (cx->affected >= 0) {
    cx->fail(BC_E_HANDLE, "bc_emit_count called twice");
    return;
  }
  cx->affected = affected;
}

// ---------------------------------------------------------------- arithmetic

int64_t bc_add(bc_ctx* cx, int64_t, int64_t) {
  cx->unimplemented("bc_add");
  return 0;
}

int64_t bc_sub(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  if (__builtin_sub_overflow(a, b, &r)) {
    cx->fail(BC_E_ARITH, "integer out of range");
    return 0;
  }
  return r;
}

int64_t bc_mul(bc_ctx* cx, int64_t, int64_t) {
  cx->unimplemented("bc_mul");
  return 0;
}
int64_t bc_div(bc_ctx* cx, int64_t, int64_t) {
  cx->unimplemented("bc_div");
  return 0;
}
int64_t bc_mod(bc_ctx* cx, int64_t, int64_t) {
  cx->unimplemented("bc_mod");
  return 0;
}
int64_t bc_neg(bc_ctx* cx, int64_t) {
  cx->unimplemented("bc_neg");
  return 0;
}
int64_t bc_f64_to_i64(bc_ctx* cx, double) {
  cx->unimplemented("bc_f64_to_i64");
  return 0;
}
double bc_i64_to_f64(int64_t v) { return static_cast<double>(v); }
bool bc_bytes_eq(bc_ctx* cx, bc_view, bc_view) {
  cx->unimplemented("bc_bytes_eq");
  return false;
}
int bc_bytes_cmp(bc_ctx* cx, bc_view, bc_view) {
  cx->unimplemented("bc_bytes_cmp");
  return 0;
}
bool bc_bytes_prefix(bc_ctx* cx, bc_view, bc_view) {
  cx->unimplemented("bc_bytes_prefix");
  return false;
}

// ---------------------------------------------------------------------- keys

bc_key bc_key_begin(bc_ctx* cx, uint32_t table_or_index) {
  if (!cx->ok()) return {};
  const auto* kl = cx->cat->key_layout(table_or_index);
  if (kl == nullptr) {
    cx->fail(BC_E_HANDLE, "no table or index " + std::to_string(table_or_index));
    return {};
  }
  BC_GUARD_BEGIN
  KeyBuilder b{kl, {}, 0, false};
  bytecask::sql::key_put_prefix(b.buf, kl->ns, table_or_index);
  cx->keys.push_back(std::move(b));
  return {static_cast<std::uint32_t>(cx->keys.size() - 1), cx->epoch};
  BC_GUARD_END(cx)
  return {};
}

void bc_key_i64(bc_ctx* cx, bc_key k, int64_t v) {
  auto* b = key_builder_part(cx, k, PhysType::I64, false);
  if (b != nullptr) bytecask::sql::key_put_i64(b->buf, v, b->layout->parts[b->next_part - 1].nullable);
}

void bc_key_f64(bc_ctx* cx, bc_key, double) { cx->unimplemented("bc_key_f64"); }

void bc_key_bytes(bc_ctx* cx, bc_key k, bc_view v) {
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  auto* b = key_builder_part(cx, k, PhysType::Bytes, false);
  if (b != nullptr) bytecask::sql::key_put_bytes(b->buf, *bytes, b->layout->parts[b->next_part - 1].nullable);
}

void bc_key_null(bc_ctx* cx, bc_key k) {
  auto* b = key_builder_part(cx, k, PhysType::I64, true);
  if (b != nullptr) bytecask::sql::key_put_null(b->buf);
}

void bc_key_pk(bc_ctx* cx, bc_key k, bc_view pk) {
  auto bytes = cx->resolve(pk);
  if (!bytes) return;
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return;
  if (b->done || b->layout->index == nullptr || b->next_part != b->layout->index_parts) {
    cx->fail(BC_E_HANDLE, "bc_key_pk outside an index key's primary-key suffix");
    return;
  }
  b->buf.insert(b->buf.end(), bytes->begin(), bytes->end());
  b->next_part = b->layout->parts.size();
}

bc_view bc_key_end(bc_ctx* cx, bc_key k) {
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return {};
  if (b->done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return {};
  }
  b->done = true;
  return cx->arena_view(b->buf);
}

bc_view bc_key_successor(bc_ctx* cx, bc_view key) {
  auto k = cx->resolve(key);
  if (!k) return {};
  BC_GUARD_BEGIN
  return cx->arena_view(bytecask::sql::key_successor(*k));
  BC_GUARD_END(cx)
  return {};
}

// --------------------------------------------------------------------- reads

bc_view bc_get(bc_ctx* cx, bc_view key, bool* found) {
  if (found != nullptr) *found = false;
  auto k = cx->resolve(key);
  if (!k || !cx->ok()) return {};
  BC_GUARD_BEGIN
  return read_record(cx, *k, found);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_lookup(bc_ctx* cx, uint32_t table, bc_view pk, bool* found) {
  if (found != nullptr) *found = false;
  auto p = cx->resolve(pk);
  if (!p || !cx->ok()) return {};
  if (cx->cat->table(table) == nullptr) {
    cx->fail(BC_E_HANDLE, "no table " + std::to_string(table));
    return {};
  }
  BC_GUARD_BEGIN
  Bytes key;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, table);
  key.insert(key.end(), p->begin(), p->end());
  return read_record(cx, key, found);
  BC_GUARD_END(cx)
  return {};
}

bool bc_exists(bc_ctx* cx, bc_view) {
  cx->unimplemented("bc_exists");
  return false;
}

bc_cursor bc_scan_open(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi, uint32_t flags) {
  auto lo_b = cx->resolve(lo);
  auto hi_b = cx->resolve(hi);
  if (!lo_b || !hi_b || !cx->ok()) return {};
  const auto* kl = cx->cat->key_layout(table_or_index);
  if (kl == nullptr) {
    cx->fail(BC_E_HANDLE, "no table or index " + std::to_string(table_or_index));
    return {};
  }
  BC_GUARD_BEGIN
  Cursor c;
  c.layout = kl;
  bytecask::sql::key_put_prefix(c.prefix, kl->ns, table_or_index);
  c.flags = flags;
  c.lo = lo_b->empty() ? c.prefix : Bytes(lo_b->begin(), lo_b->end());
  if ((flags & BC_SCAN_PREFIX) != 0) {
    c.hi = bytecask::sql::key_successor(c.lo);
    c.flags &= ~static_cast<std::uint32_t>(BC_SCAN_HI_INCLUSIVE);
  } else if (!hi_b->empty()) {
    c.hi.assign(hi_b->begin(), hi_b->end());
  }
  cx->cursors.push_back(std::move(c));
  return {static_cast<std::uint32_t>(cx->cursors.size() - 1), cx->epoch};
  BC_GUARD_END(cx)
  return {};
}

bool bc_scan_next(bc_ctx* cx, bc_cursor h) {
  auto* c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr || c->done) return false;
  ++c->step;  // every view of the previous entry is now stale
  BC_GUARD_BEGIN
  c->positioned = cursor_advance(cx, *c);
  if (!c->positioned) c->done = true;
  return c->positioned && cx->ok();
  BC_GUARD_END(cx)
  c->done = true;
  return false;
}

namespace {
auto positioned_cursor(bc_ctx* cx, bc_cursor h) -> Cursor* {
  auto* c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c != nullptr && !c->positioned) {
    cx->fail(BC_E_HANDLE, "cursor is not on an entry");
    return nullptr;
  }
  return c;
}
}  // namespace

bc_view bc_scan_key(bc_ctx* cx, bc_cursor h) {
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  return cx->make_view(c->key.data(), c->key.size(), h.i, c->step);
}

bc_view bc_scan_value(bc_ctx* cx, bc_cursor h) {
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  return cx->make_view(c->value.data(), c->value.size(), h.i, c->step);
}

bc_view bc_scan_pk(bc_ctx* cx, bc_cursor h) {
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  bytecask::sql::KeyReader r{c->key, bytecask::sql::kKeyPrefixBytes};
  for (std::size_t i = 0; i < c->layout->index_parts; ++i) {
    if (!r.skip(c->layout->parts[i])) {
      cx->fail(BC_E_CORRUPT, "unparseable index key");
      return {};
    }
  }
  return cx->make_view(c->key.data() + r.pos(), c->key.size() - r.pos(), h.i, c->step);
}

void bc_scan_close(bc_ctx* cx, bc_cursor h) {
  auto* c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr) return;
  ++c->step;
  c->fwd.reset();
  c->rev.reset();
  c->done = true;
  c->positioned = false;
}

int64_t bc_count_range(bc_ctx* cx, uint32_t, bc_view, bc_view, int64_t) {
  cx->unimplemented("bc_count_range");
  return 0;
}

// ------------------------------------------------------------ rows, columns

bool bc_col_null(bc_ctx* cx, bc_view row, uint32_t col) {
  auto r = cx->resolve(row);
  if (!r || !cx->ok()) return true;
  return bytecask::sql::row_null_bit(*r, col);
}

int64_t bc_col_i64(bc_ctx* cx, bc_view row, uint32_t off) {
  auto r = cx->resolve(row);
  if (!r || !cx->ok()) return 0;
  auto v = bytecask::sql::row_i64(*r, off);
  if (!v) {
    cx->fail(BC_E_HANDLE, "column offset outside the row");
    return 0;
  }
  return *v;
}

int64_t bc_col_i32(bc_ctx* cx, bc_view, uint32_t) {
  cx->unimplemented("bc_col_i32");
  return 0;
}

double bc_col_f64(bc_ctx* cx, bc_view, uint32_t) {
  cx->unimplemented("bc_col_f64");
  return 0;
}

bc_view bc_col_bytes(bc_ctx* cx, bc_view row, uint32_t slot) {
  auto r = cx->resolve(row);
  if (!r || !cx->ok()) return {};
  auto v = bytecask::sql::row_bytes(*r, slot);
  if (!v) {
    cx->fail(BC_E_HANDLE, "variable slot outside the row");
    return {};
  }
  auto [cursor, step] = cx->owner(row);
  return cx->make_view(v->data(), v->size(), cursor, step);
}

bc_view bc_row_pk(bc_ctx* cx, uint32_t, bc_view) {
  cx->unimplemented("bc_row_pk");
  return {};
}

bc_row bc_row_begin(bc_ctx* cx, uint32_t layout) {
  if (!cx->ok()) return {};
  const auto* t = cx->cat->table_of_layout(layout);
  if (t == nullptr) {
    cx->fail(BC_E_LAYOUT, "no table layout " + std::to_string(layout));
    return {};
  }
  BC_GUARD_BEGIN
  cx->rows.push_back(RowBuilder{&t->layout, RowWriter{t->layout}, false});
  return {static_cast<std::uint32_t>(cx->rows.size() - 1), cx->epoch};
  BC_GUARD_END(cx)
  return {};
}

bc_row bc_row_from(bc_ctx* cx, uint32_t layout, bc_view old) {
  auto o = cx->resolve(old);
  if (!o) return {};
  auto r = bc_row_begin(cx, layout);
  if (!cx->ok()) return {};
  if (!bytecask::sql::row_valid(*o) || bytecask::sql::row_generation(*o) != cx->rows[r.i].layout->generation) {
    cx->fail(BC_E_LAYOUT, "row is not of this layout");
    return {};
  }
  cx->rows[r.i].writer.copy_from(*o);
  return r;
}

void bc_row_set_i64(bc_ctx* cx, bc_row r, uint32_t col, int64_t v) {
  auto* b = row_builder(cx, r);
  if (b != nullptr && !b->writer.set_i64(col, v)) cx->fail(BC_E_LAYOUT, "column is not an integer of this layout");
}

void bc_row_set_f64(bc_ctx* cx, bc_row, uint32_t, double) { cx->unimplemented("bc_row_set_f64"); }

void bc_row_set_bytes(bc_ctx* cx, bc_row r, uint32_t col, bc_view v) {
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  auto* b = row_builder(cx, r);
  if (b == nullptr) return;
  BC_GUARD_BEGIN
  if (!b->writer.set_bytes(col, *bytes)) cx->fail(BC_E_LAYOUT, "column is not bytes of this layout");
  BC_GUARD_END(cx)
}

void bc_row_set_null(bc_ctx* cx, bc_row r, uint32_t col) {
  auto* b = row_builder(cx, r);
  if (b != nullptr && !b->writer.set_null(col)) cx->fail(BC_E_LAYOUT, "no such column in this layout");
}

bc_view bc_row_end(bc_ctx* cx, bc_row r) {
  auto* b = row_builder(cx, r);
  if (b == nullptr) return {};
  b->done = true;
  if (b->writer.missing_required()) {
    cx->fail(BC_E_LAYOUT, "a non-nullable column was not set");
    return {};
  }
  BC_GUARD_BEGIN
  Bytes row;
  if (!b->writer.finish(row)) {
    cx->fail(BC_E_LIMIT, "row over the row format's limit");
    return {};
  }
  return cx->arena_view(row);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_empty(bc_ctx*) { return {}; }

bc_view bc_pin(bc_ctx* cx, bc_view) {
  cx->unimplemented("bc_pin");
  return {};
}

// --------------------------------------------------- ephemeral structures

bc_sorter bc_sorter_open(bc_ctx* cx, uint32_t) {
  cx->unimplemented("bc_sorter_open");
  return {};
}
void bc_sorter_add(bc_ctx* cx, bc_sorter, bc_view, bc_view) { cx->unimplemented("bc_sorter_add"); }
int64_t bc_sorter_count(bc_ctx* cx, bc_sorter) {
  cx->unimplemented("bc_sorter_count");
  return 0;
}
bc_view bc_sorter_nth(bc_ctx* cx, bc_sorter, int64_t) {
  cx->unimplemented("bc_sorter_nth");
  return {};
}
bool bc_sorter_next(bc_ctx* cx, bc_sorter, bc_view*) {
  cx->unimplemented("bc_sorter_next");
  return false;
}
bc_hash bc_hash_open(bc_ctx* cx, uint32_t) {
  cx->unimplemented("bc_hash_open");
  return {};
}
void bc_hash_put(bc_ctx* cx, bc_hash, bc_view, bc_view) { cx->unimplemented("bc_hash_put"); }
bc_hash_iter bc_hash_find(bc_ctx* cx, bc_hash, bc_view) {
  cx->unimplemented("bc_hash_find");
  return {};
}
bool bc_hash_next(bc_ctx* cx, bc_hash_iter, bc_view*) {
  cx->unimplemented("bc_hash_next");
  return false;
}
bc_agg bc_agg_open(bc_ctx* cx, uint32_t, uint32_t) {
  cx->unimplemented("bc_agg_open");
  return {};
}
bc_group bc_agg_group(bc_ctx* cx, bc_agg, bc_view) {
  cx->unimplemented("bc_agg_group");
  return {};
}
void bc_agg_add_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_add_i64"); }
void bc_agg_add_f64(bc_ctx* cx, bc_group, uint32_t, double) { cx->unimplemented("bc_agg_add_f64"); }
void bc_agg_min_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_min_i64"); }
void bc_agg_max_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_max_i64"); }
void bc_agg_count(bc_ctx* cx, bc_group, uint32_t) { cx->unimplemented("bc_agg_count"); }
bool bc_agg_next(bc_ctx* cx, bc_agg, bc_view*, bc_group*) {
  cx->unimplemented("bc_agg_next");
  return false;
}
int64_t bc_agg_i64(bc_ctx* cx, bc_group, uint32_t) {
  cx->unimplemented("bc_agg_i64");
  return 0;
}
double bc_agg_f64(bc_ctx* cx, bc_group, uint32_t) {
  cx->unimplemented("bc_agg_f64");
  return 0;
}

// ------------------------------------------------------------------- effects

void bc_put(bc_ctx* cx, bc_view key, bc_view row) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::Put, key, row);
  BC_GUARD_END(cx)
}

void bc_del(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::Del, key, {});
  BC_GUARD_END(cx)
}

void bc_del_range(bc_ctx* cx, bc_view, bc_view) { cx->unimplemented("bc_del_range"); }
void bc_guard_unchanged(bc_ctx* cx, bc_view) { cx->unimplemented("bc_guard_unchanged"); }
void bc_guard_range(bc_ctx* cx, uint32_t, bc_view, bc_view) { cx->unimplemented("bc_guard_range"); }
void bc_guard_present(bc_ctx* cx, bc_view) { cx->unimplemented("bc_guard_present"); }

void bc_guard_absent(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::GuardAbsent, key, {});
  BC_GUARD_END(cx)
}

}  // extern "C"
