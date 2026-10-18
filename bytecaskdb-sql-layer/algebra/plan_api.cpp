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

// Standard library headers before the module imports, for the reason
// bytecaskdb/bytecask_hpp.cpp gives (LLVM 22's abi_tag redeclaration error).
#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
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

// bc_db, bc_catalog and bc_params, which bc_plan.h leaves opaque, and the
// catalog and codecs through it.
import bytecask.sql.runtime;

using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::Catalog;
using bytecask::sql::KeyLayout;
using bytecask::sql::KeyPart;
using bytecask::sql::KeyReader;
using bytecask::sql::LiveCatalog;
using bytecask::sql::PhysType;
using bytecask::sql::RowLayout;
using bytecask::sql::RowWriter;

// bc_type's codes are PhysType's values; a schema ref's result_types are read as such.
static_assert(static_cast<int>(PhysType::I64) == BC_T_I64);
static_assert(static_cast<int>(PhysType::F64) == BC_T_F64);
static_assert(static_cast<int>(PhysType::Bool) == BC_T_BOOL);
static_assert(static_cast<int>(PhysType::Bytes) == BC_T_BYTES);
// bc_error_detail carries an object id in its low 24 bits.
static_assert(Catalog::kMaxObjectId <= 0x00FF'FFFFu);

namespace {

constexpr std::uint32_t kNoCursor = UINT32_MAX;
constexpr std::uint32_t kScanFlagMask = BC_SCAN_REVERSE | BC_SCAN_PREFIX;

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

enum class EffectKind { Put, Del, DelRange, GuardUnchanged, GuardRange, GuardPresent, GuardAbsent };

struct Effect {
  EffectKind kind;
  Bytes a, b;
};

auto as_u8(const std::byte* p) -> const std::uint8_t* {
  return reinterpret_cast<const std::uint8_t*>(p);  // the runtime hands raw rows to C callers
}

auto phys_type(std::uint8_t code) -> std::optional<PhysType> {
  switch (code) {
    case BC_T_I64: return PhysType::I64;
    case BC_T_F64: return PhysType::F64;
    case BC_T_BOOL: return PhysType::Bool;
    case BC_T_BYTES: return PhysType::Bytes;
    default: return std::nullopt;
  }
}

}  // namespace

struct bc_ctx {
  bytecask::DB* db;
  LiveCatalog* live;
  // The catalog version this execution resolves against, pinned with the
  // snapshot by bc_ctx_begin; cat is pinned.get().
  std::shared_ptr<const Catalog> pinned;
  const Catalog* cat;
  std::vector<std::byte> arena;
  std::size_t arena_used{0};
  std::uint32_t epoch{1};

  const bc_params* params{nullptr};
  bc_budget budget{};
  std::uint64_t reads{0};
  std::optional<bytecask::Snapshot> snap;

  bc_error_code error{BC_OK};
  std::uint32_t detail{0};
  std::string message;

  std::vector<ViewEntry> views;
  std::vector<Cursor> cursors;
  std::vector<KeyBuilder> keys;
  std::vector<RowBuilder> rows;
  std::vector<Effect> effects;

  // Results: rows in the row format of the plan's result_types, each
  // preceded by its u32 length.
  Bytes results;
  std::size_t result_pos{0};
  std::vector<PhysType> result_types;
  std::optional<RowLayout> result_layout;
  std::optional<RowWriter> emit;
  std::uint32_t emit_col{0};
  std::int64_t affected{-1};

  bc_ctx(bytecask::DB* d, LiveCatalog* l, std::uint32_t arena_bytes)
      : db{d}, live{l}, pinned{l->current()}, cat{pinned.get()}, arena(arena_bytes) {}

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
    } catch (const bytecask::DbFollowerMode& e) {
      fail(BC_E_DEGRADED, e.what());  // writes refused: the engine is a follower
    } catch (const std::length_error& e) {
      fail(BC_E_LIMIT, e.what());
    } catch (const std::overflow_error& e) {
      fail(BC_E_LIMIT, e.what());
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

  auto charge(std::uint64_t units = 1) -> bool {
    if (!ok()) return false;
    auto before = reads;
    reads += units;
    if (budget.max_reads != 0 && reads > budget.max_reads) {
      fail(BC_E_BUDGET, "work budget exhausted");
      return false;
    }
    // Every 64 units, whatever the step: a charge of several units must not
    // jump over the check.
    if (budget.deadline_ns != 0 && before / 64 != reads / 64) {
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

  // Two views at once; nullopt if either fails or the context has failed.
  auto resolve2(bc_view a, bc_view b) -> std::optional<std::pair<BytesView, BytesView>> {
    auto av = resolve(a);
    auto bv = resolve(b);
    if (!av || !bv || !ok()) return std::nullopt;
    return std::pair{*av, *bv};
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

  template <typename T>
  auto param_as(std::uint32_t i, const char* type_name) -> const T* {
    const auto* p = param(i);
    if (p == nullptr) return nullptr;
    if (const auto* v = std::get_if<T>(p)) return v;
    fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not " + type_name);
    return nullptr;
  }

  // The key layout of a table or index, or nullptr and BC_E_HANDLE.
  auto key_layout(std::uint32_t table_or_index) -> const KeyLayout* {
    if (!ok()) return nullptr;
    const auto* kl = cat->key_layout(table_or_index);
    if (kl == nullptr) fail(BC_E_HANDLE, "no table or index " + std::to_string(table_or_index));
    return kl;
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
    emit_col = 0;
    affected = -1;
    arena_used = 0;
    reads = 0;
    ++epoch;
    if (epoch == 0) epoch = 1;  // 0 is the empty view's generation
  }

  void unimplemented(const char* fn) { fail(BC_E_HANDLE, std::string{fn} + " is not implemented yet"); }
};

// Every entry point catches: an exception never crosses into a plan.
#define BC_GUARD_BEGIN try {
#define BC_GUARD_END(cx) \
  }                      \
  catch (...) {          \
    (cx)->fail_current_exception(); \
  }

namespace {

auto bytes_cmp(BytesView a, BytesView b) -> std::strong_ordering {
  return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end(),
                                                [](std::byte x, std::byte y) {
                                                  return std::to_integer<int>(x) <=> std::to_integer<int>(y);
                                                });
}

auto starts_with(BytesView k, BytesView prefix) -> bool {
  return k.size() >= prefix.size() && std::ranges::equal(k.first(prefix.size()), prefix);
}

// Positions a cursor on its next entry within bounds; false at the end.
auto cursor_advance(bc_ctx* cx, Cursor& c) -> bool {
  auto reverse = (c.flags & BC_SCAN_REVERSE) != 0;
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
    if (!starts_with(k, c.prefix)) {
      if (reverse && !c.hi.empty() && std::is_gt(bytes_cmp(k, c.prefix))) continue;  // the successor itself
      return false;
    }
    if (std::is_lt(bytes_cmp(k, c.lo))) {
      if (reverse) return false;
      continue;
    }
    if (!c.hi.empty() && std::is_gteq(bytes_cmp(k, c.hi))) {
      if (reverse) continue;
      return false;
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

// The range [lo, hi) of a table or index with empty bounds resolved: an empty
// lo is the start of its keys, an empty hi the end.
struct Range {
  Bytes prefix, lo, hi;
};

auto resolve_range(bc_ctx* cx, std::uint32_t table_or_index, bc_view lo, bc_view hi) -> std::optional<Range> {
  auto bounds = cx->resolve2(lo, hi);
  const auto* kl = cx->key_layout(table_or_index);
  if (!bounds || kl == nullptr) return std::nullopt;
  Range r;
  bytecask::sql::key_put_prefix(r.prefix, kl->ns, table_or_index);
  r.lo = bounds->first.empty() ? r.prefix : Bytes(bounds->first.begin(), bounds->first.end());
  r.hi = bounds->second.empty() ? bytecask::sql::key_successor(r.prefix)
                                : Bytes(bounds->second.begin(), bounds->second.end());
  return r;
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

// The part just reserved by key_builder_part.
auto current_part(const KeyBuilder& b) -> KeyPart { return b.layout->parts[b.next_part - 1]; }

// A reader positioned on part `part` of `key`, which must be a key of
// `table_or_index` and, when `want` is given, have a part of that type there.
struct KeyPartAt {
  KeyReader reader;
  KeyPart part;
};

auto key_part_at(bc_ctx* cx, std::uint32_t table_or_index, bc_view key, std::uint32_t part,
                 std::optional<PhysType> want) -> std::optional<KeyPartAt> {
  auto k = cx->resolve(key);
  const auto* kl = cx->key_layout(table_or_index);
  if (!k || kl == nullptr) return std::nullopt;
  if (part >= kl->parts.size()) {
    cx->fail(BC_E_HANDLE, "no key part " + std::to_string(part));
    return std::nullopt;
  }
  if (want && kl->parts[part].type != *want) {
    cx->fail(BC_E_HANDLE, "key part of the wrong type");
    return std::nullopt;
  }
  Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, kl->ns, table_or_index);
  if (!starts_with(*k, prefix)) {
    cx->fail(BC_E_HANDLE, "key is not of table or index " + std::to_string(table_or_index));
    return std::nullopt;
  }
  KeyReader r{*k, bytecask::sql::kKeyPrefixBytes};
  for (std::uint32_t i = 0; i < part; ++i) {
    if (!r.skip(kl->parts[i])) {
      cx->fail(BC_E_CORRUPT, "unparseable key");
      return std::nullopt;
    }
  }
  return KeyPartAt{r, kl->parts[part]};
}

// Unwraps a decoded part: the value, or the neutral one for a null part or an
// unparseable key (BC_E_CORRUPT).
template <typename T>
auto decoded(bc_ctx* cx, const KeyReader& r, std::optional<T> v, T neutral) -> T {
  if (v) return *v;
  if (!r.ok()) cx->fail(BC_E_CORRUPT, "unparseable key");
  return neutral;
}

auto add_effect(bc_ctx* cx, EffectKind kind, bc_view a, bc_view b) -> void {
  auto views = cx->resolve2(a, b);
  if (!views) return;
  cx->effects.push_back(
      {kind, Bytes(views->first.begin(), views->first.end()), Bytes(views->second.begin(), views->second.end())});
}

auto row_builder(bc_ctx* cx, bc_row r) -> RowBuilder* {
  auto* b = cx->resolve_handle(r, cx->rows, "row builder");
  if (b != nullptr && b->done) {
    cx->fail(BC_E_HANDLE, "row builder already ended");
    return nullptr;
  }
  return b;
}

// The tables whose rows or index entries the write plan touches, found from
// the keys: a row key names its table, an index key its index.
auto written_tables(const bc_ctx* cx) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  auto note = [&](BytesView key) {
    if (key.size() < bytecask::sql::kKeyPrefixBytes) return;
    auto ns = std::to_integer<std::uint8_t>(key[0]);
    if (ns != bytecask::sql::kNsRow && ns != bytecask::sql::kNsIndex) return;
    std::uint32_t id = 0;
    for (std::size_t i = 1; i < bytecask::sql::kKeyPrefixBytes; ++i) id = (id << 8) | std::to_integer<std::uint32_t>(key[i]);
    const auto* kl = cx->cat->key_layout(id);
    if (kl == nullptr) return;
    if (std::ranges::find(out, kl->table->id) == out.end()) out.push_back(kl->table->id);
  };
  for (const auto& e : cx->effects) {
    switch (e.kind) {
      case EffectKind::Put:
      case EffectKind::Del: note(e.a); break;
      case EffectKind::DelRange:
        note(e.a);
        note(e.b);
        break;
      case EffectKind::GuardUnchanged:
      case EffectKind::GuardRange:
      case EffectKind::GuardPresent:
      case EffectKind::GuardAbsent: break;
    }
  }
  return out;
}

auto check_schema(bc_ctx* cx, const bc_schema_ref* ref) -> bool {
  if (auto why = bytecask::sql::schema_incompatibility(*cx->cat, *ref)) {
    cx->fail(BC_E_LAYOUT, "plan compiled against catalog v" + std::to_string(ref->catalog_version) +
                              " is incompatible with v" + std::to_string(cx->cat->version()) + ": " + *why);
    return false;
  }
  std::vector<PhysType> types;
  for (std::uint32_t i = 0; i < ref->n_result; ++i) {
    auto t = phys_type(ref->result_types[i]);
    if (!t) {
      cx->fail(BC_E_LAYOUT, "unknown result type code " + std::to_string(ref->result_types[i]));
      return false;
    }
    types.push_back(*t);
  }
  // Kept across executions: a retry, or the next run of the same plan, does
  // not rebuild it.
  if (!cx->result_layout || types != cx->result_types) {
    try {
      cx->result_layout.emplace(bytecask::sql::make_result_layout(types));
    } catch (const std::length_error&) {
      cx->fail(BC_E_LAYOUT, "the plan's result row has too many columns for one row layout");
      return false;
    }
    cx->result_types = std::move(types);
  }
  return true;
}

// A view a column decoder reads: a row whose header is consistent with its
// length. The empty view is no row (a plan bug, BC_E_HANDLE); a stored row
// that does not parse is BC_E_CORRUPT, never read as null.
auto resolve_row(bc_ctx* cx, bc_view row) -> std::optional<BytesView> {
  auto r = cx->resolve(row);
  if (!r || !cx->ok()) return std::nullopt;
  if (r->empty()) {
    cx->fail(BC_E_HANDLE, "the empty view is not a row");
    return std::nullopt;
  }
  if (!bytecask::sql::row_header_valid(*r)) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return std::nullopt;
  }
  return r;
}

// The row builder a bc_row_set_* call targets, with the layout check's message.
template <typename Set>
void set_column(bc_ctx* cx, bc_row r, Set&& set, const char* what) {
  auto* b = row_builder(cx, r);
  if (b == nullptr) return;
  BC_GUARD_BEGIN
  if (!set(b->writer)) cx->fail(BC_E_LAYOUT, std::string{"column is not "} + what + " of this layout");
  BC_GUARD_END(cx)
}

template <typename Load, typename T>
auto load_column(bc_ctx* cx, bc_view row, std::uint32_t off, Load&& load, T neutral) -> T {
  auto r = resolve_row(cx, row);
  if (!r) return neutral;
  auto v = load(*r, off);
  if (!v) {
    cx->fail(BC_E_HANDLE, "column offset outside the row");
    return neutral;
  }
  return *v;
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------- runtime

bc_ctx* bc_ctx_new(bc_db* db, const bc_catalog* cat, uint32_t arena_bytes) {
  try {
    return new bc_ctx(db->db, cat->live, arena_bytes);
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
  cx->detail = 0;
  cx->message.clear();
  cx->params = params;
  cx->budget = budget;
  BC_GUARD_BEGIN
  auto p = cx->live->pin();
  cx->pinned = std::move(p.cat);
  cx->cat = cx->pinned.get();
  cx->snap.emplace(std::move(p.snap));
  BC_GUARD_END(cx)
}

void bc_ctx_run(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* compiled_against) {
  if (!cx->ok()) return;
  if (!cx->snap) {
    cx->fail(BC_E_HANDLE, "bc_ctx_run without bc_ctx_begin");
    return;
  }
  BC_GUARD_BEGIN
  if (compiled_against == nullptr) {
    cx->result_types.clear();
    cx->result_layout.reset();
  } else if (!check_schema(cx, compiled_against)) {
    return;
  }
  BC_GUARD_END(cx)
  if (!cx->ok()) return;
  plan(cx);
  if (cx->ok() && cx->emit) cx->fail(BC_E_LAYOUT, "result row begun and not ended");
}

enum bc_commit_result bc_ctx_commit(bc_ctx* cx, bool sync) {
  if (!cx->ok()) return BC_FAILED;
  if (cx->effects.empty()) return BC_COMMITTED;
  if (!cx->snap) {
    // Committed already (the write plan took the snapshot), or never begun.
    cx->fail(BC_E_HANDLE, "bc_ctx_commit without a run to commit");
    return BC_FAILED;
  }
  try {
    cx->cursors.clear();
    bytecask::WritePlan plan{std::move(*cx->snap)};
    cx->snap.reset();
    for (const auto& e : cx->effects) {
      switch (e.kind) {
        case EffectKind::Put: plan.put(e.a, e.b); break;
        case EffectKind::Del: plan.del(e.a); break;
        case EffectKind::DelRange: plan.del_range(e.a, e.b); break;
        case EffectKind::GuardUnchanged: plan.ensure_unchanged(e.a); break;
        case EffectKind::GuardRange: plan.ensure_range_unchanged(e.a, e.b); break;
        case EffectKind::GuardPresent: plan.ensure_present(e.a); break;
        case EffectKind::GuardAbsent: plan.ensure_absent(e.a); break;
      }
    }
    // A DDL on a table this plan writes, committed since the snapshot,
    // changes the table's record: the commit is then a conflict, and the
    // retry pins the new catalog version.
    for (auto id : written_tables(cx)) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
    cx->effects.clear();  // handed to the engine: a second commit has nothing to submit
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

uint32_t bc_error_detail(const bc_ctx* cx) { return cx->detail; }

int64_t bc_ctx_affected(const bc_ctx* cx) { return cx->affected; }

int bc_execute(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* ref, const bc_params* params, bc_budget budget,
               bool sync, uint32_t max_retries) {
  // Only a conflict is retried: an error, a raised BC_E_CONSTRAINT included,
  // is BC_FAILED and final.
  for (std::uint32_t attempt = 0;; ++attempt) {
    bc_ctx_begin(cx, params, budget);
    bc_ctx_run(cx, plan, ref);
    auto r = bc_ctx_commit(cx, sync);
    if (r != BC_CONFLICT || attempt >= max_retries) return r;
  }
}

// ------------------------------------------------- parameters and results

int64_t bc_param_i64(bc_ctx* cx, uint32_t i) {
  const auto* v = cx->param_as<std::int64_t>(i, "an integer");
  return v == nullptr ? 0 : *v;
}

double bc_param_f64(bc_ctx* cx, uint32_t i) {
  const auto* v = cx->param_as<double>(i, "a double");
  return v == nullptr ? 0.0 : *v;
}

bool bc_param_bool(bc_ctx* cx, uint32_t i) {
  const auto* v = cx->param_as<bool>(i, "a boolean");
  return v != nullptr && *v;
}

bc_view bc_param_bytes(bc_ctx* cx, uint32_t i) {
  const auto* v = cx->param_as<Bytes>(i, "bytes");
  if (v == nullptr) return {};
  return cx->make_view(v->data(), v->size());
}

bool bc_param_null(bc_ctx* cx, uint32_t i) {
  const auto* p = cx->param(i);
  return p != nullptr && std::holds_alternative<std::monostate>(*p);
}

void bc_emit_begin(bc_ctx* cx) {
  if (!cx->ok()) return;
  if (!cx->result_layout) {
    cx->fail(BC_E_LAYOUT, "the plan's schema ref declares no result types");
    return;
  }
  if (cx->emit) {
    cx->fail(BC_E_LAYOUT, "result row already begun");
    return;
  }
  BC_GUARD_BEGIN
  cx->emit.emplace(*cx->result_layout);
  cx->emit_col = 0;
  BC_GUARD_END(cx)
}

namespace {
auto emit_slot(bc_ctx* cx, std::optional<PhysType> type) -> bool {
  if (!cx->ok()) return false;
  if (!cx->emit || cx->emit_col >= cx->result_types.size() ||
      (type && cx->result_types[cx->emit_col] != *type)) {
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

void bc_emit_f64(bc_ctx* cx, double v) {
  if (!emit_slot(cx, PhysType::F64)) return;
  (void)cx->emit->set_f64(cx->emit_col++, v);
}

void bc_emit_bool(bc_ctx* cx, bool v) {
  if (!emit_slot(cx, PhysType::Bool)) return;
  (void)cx->emit->set_bool(cx->emit_col++, v);
}

void bc_emit_bytes(bc_ctx* cx, bc_view v) {
  if (!emit_slot(cx, PhysType::Bytes)) return;
  auto b = cx->resolve(v);
  if (!b) return;
  BC_GUARD_BEGIN
  (void)cx->emit->set_bytes(cx->emit_col++, *b);
  BC_GUARD_END(cx)
}

void bc_emit_null(bc_ctx* cx) {
  if (!emit_slot(cx, std::nullopt)) return;
  (void)cx->emit->set_null(cx->emit_col++);
}

void bc_emit_end(bc_ctx* cx) {
  if (!cx->ok()) return;
  if (!cx->emit || cx->emit_col != cx->result_types.size()) {
    cx->fail(BC_E_LAYOUT, "result row ended with the wrong number of columns");
    return;
  }
  BC_GUARD_BEGIN
  // prototype: the result buffer is a heap vector, not part of the arena.
  // The row is encoded straight into it, after its length.
  auto at = cx->results.size();
  cx->results.resize(at + 4);
  if (!cx->emit->finish(cx->results)) {
    cx->results.resize(at);
    cx->fail(BC_E_LIMIT, "result row over the row format's limit");
    return;
  }
  auto n = static_cast<std::uint32_t>(cx->results.size() - at - 4);
  std::memcpy(cx->results.data() + at, &n, 4);
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

namespace {
auto arith_fail(bc_ctx* cx, const char* what) -> std::int64_t {
  cx->fail(BC_E_ARITH, what);
  return 0;
}
}  // namespace

int64_t bc_add(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_add_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
}

int64_t bc_sub(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_sub_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
}

int64_t bc_mul(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_mul_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
}

int64_t bc_div(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  if (b == 0) return arith_fail(cx, "division by zero");
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return arith_fail(cx, "integer out of range");
  return a / b;
}

int64_t bc_mod(bc_ctx* cx, int64_t a, int64_t b) {
  if (!cx->ok()) return 0;
  if (b == 0) return arith_fail(cx, "division by zero");
  if (b == -1) return 0;  // INT64_MIN % -1 is undefined in C; the remainder is 0
  return a % b;
}

int64_t bc_neg(bc_ctx* cx, int64_t a) {
  if (!cx->ok()) return 0;
  if (a == std::numeric_limits<std::int64_t>::min()) return arith_fail(cx, "integer out of range");
  return -a;
}

int64_t bc_f64_to_i64(bc_ctx* cx, double v) {
  if (!cx->ok()) return 0;
  // Truncation toward zero lands in i64 exactly when v is in [-2^63, 2^63);
  // both bounds are exact doubles.
  if (v != v) return arith_fail(cx, "cannot convert NaN to an integer");
  if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return arith_fail(cx, "double out of integer range");
  return static_cast<std::int64_t>(v);
}

double bc_i64_to_f64(int64_t v) { return static_cast<double>(v); }

bool bc_bytes_eq(bc_ctx* cx, bc_view a, bc_view b) {
  auto v = cx->resolve2(a, b);
  return v && std::ranges::equal(v->first, v->second);
}

int bc_bytes_cmp(bc_ctx* cx, bc_view a, bc_view b) {
  auto v = cx->resolve2(a, b);
  if (!v) return 0;
  auto o = bytes_cmp(v->first, v->second);
  return std::is_lt(o) ? -1 : std::is_gt(o) ? 1 : 0;
}

bool bc_bytes_prefix(bc_ctx* cx, bc_view prefix, bc_view b) {
  auto v = cx->resolve2(prefix, b);
  return v && starts_with(v->second, v->first);
}

// ---------------------------------------------------------------------- keys

bc_key bc_key_begin(bc_ctx* cx, uint32_t table_or_index) {
  const auto* kl = cx->key_layout(table_or_index);
  if (kl == nullptr) return {};
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
  if (b != nullptr) bytecask::sql::key_put_i64(b->buf, v, current_part(*b));
}

void bc_key_f64(bc_ctx* cx, bc_key k, double v) {
  auto* b = key_builder_part(cx, k, PhysType::F64, false);
  if (b != nullptr) bytecask::sql::key_put_f64(b->buf, v, current_part(*b));
}

void bc_key_bool(bc_ctx* cx, bc_key k, bool v) {
  auto* b = key_builder_part(cx, k, PhysType::Bool, false);
  if (b != nullptr) bytecask::sql::key_put_bool(b->buf, v, current_part(*b));
}

void bc_key_bytes(bc_ctx* cx, bc_key k, bc_view v) {
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  auto* b = key_builder_part(cx, k, PhysType::Bytes, false);
  if (b != nullptr) bytecask::sql::key_put_bytes(b->buf, *bytes, current_part(*b));
}

void bc_key_null(bc_ctx* cx, bc_key k) {
  auto* b = key_builder_part(cx, k, PhysType::I64, true);
  if (b != nullptr) bytecask::sql::key_put_null(b->buf, current_part(*b));
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
  if (b->buf.size() > bytecask::sql::kMaxKeyBytes) {
    cx->fail(BC_E_LIMIT, "key over the engine's key limit");
    return {};
  }
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

bool bc_exists(bc_ctx* cx, bc_view key) {
  auto k = cx->resolve(key);
  if (!k || !cx->charge()) return false;
  BC_GUARD_BEGIN
  return cx->snap->contains_key({}, *k);
  BC_GUARD_END(cx)
  return false;
}

bc_cursor bc_scan_open(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi, uint32_t flags) {
  if ((flags & ~kScanFlagMask) != 0) {
    cx->fail(BC_E_HANDLE, "unknown scan flag");
    return {};
  }
  auto lo_b = cx->resolve(lo);
  auto hi_b = cx->resolve(hi);
  const auto* kl = cx->key_layout(table_or_index);
  if (!lo_b || !hi_b || kl == nullptr) return {};
  BC_GUARD_BEGIN
  Cursor c;
  c.layout = kl;
  bytecask::sql::key_put_prefix(c.prefix, kl->ns, table_or_index);
  c.flags = flags;
  c.lo = lo_b->empty() ? c.prefix : Bytes(lo_b->begin(), lo_b->end());
  if ((flags & BC_SCAN_PREFIX) != 0) {
    c.hi = bytecask::sql::key_successor(c.lo);
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
  KeyReader r{c->key, bytecask::sql::kKeyPrefixBytes};
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

bc_view bc_empty(bc_ctx*) { return {}; }

int64_t bc_key_part_i64(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::I64);
  if (!at) return 0;
  return decoded<std::int64_t>(cx, at->reader, at->reader.read_i64(at->part), 0);
}

double bc_key_part_f64(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::F64);
  if (!at) return 0.0;
  return decoded<double>(cx, at->reader, at->reader.read_f64(at->part), 0.0);
}

bool bc_key_part_bool(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::Bool);
  if (!at) return false;
  return decoded<bool>(cx, at->reader, at->reader.read_bool(at->part), false);
}

bc_view bc_key_part_bytes(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::Bytes);
  if (!at) return {};
  BC_GUARD_BEGIN
  auto v = at->reader.read_bytes(at->part);
  if (!v) {
    if (!at->reader.ok()) cx->fail(BC_E_CORRUPT, "unparseable key");
    return {};
  }
  return cx->arena_view(*v);
  BC_GUARD_END(cx)
  return {};
}

bool bc_key_part_null(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, std::nullopt);
  if (!at) return true;
  if (!at->reader.skip(at->part)) {
    cx->fail(BC_E_CORRUPT, "unparseable key");
    return true;
  }
  return at->reader.last_was_null();
}

int64_t bc_count_range(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi, int64_t limit) {
  auto range = resolve_range(cx, table_or_index, lo, hi);
  if (!range) return 0;
  if (limit < 0) {
    cx->fail(BC_E_HANDLE, "negative count limit");
    return 0;
  }
  if (!cx->charge(2)) return 0;  // the engine's bound: at most two record reads
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return 0;
  BC_GUARD_BEGIN
  return static_cast<std::int64_t>(cx->snap->count_keys(range->lo, range->hi, static_cast<std::size_t>(limit)));
  BC_GUARD_END(cx)
  return 0;
}

// ------------------------------------------------------------ rows, columns

bool bc_col_null(bc_ctx* cx, bc_view row, uint32_t col) {
  auto r = resolve_row(cx, row);
  if (!r) return true;
  return !bytecask::sql::row_present_bit(*r, col);
}

int64_t bc_col_i64(bc_ctx* cx, bc_view row, uint32_t off) {
  return load_column(cx, row, off, bytecask::sql::row_i64, std::int64_t{0});
}

double bc_col_f64(bc_ctx* cx, bc_view row, uint32_t off) {
  return load_column(cx, row, off, bytecask::sql::row_f64, 0.0);
}

bool bc_col_bool(bc_ctx* cx, bc_view row, uint32_t off) {
  return load_column(cx, row, off, bytecask::sql::row_bool, false);
}

bc_view bc_col_bytes(bc_ctx* cx, bc_view row, uint32_t slot) {
  auto r = resolve_row(cx, row);
  if (!r) return {};
  auto v = bytecask::sql::row_bytes(*r, slot);
  if (!v) {
    // The header parsed and a slot past nvar reads as empty, so this is a
    // var_end entry that runs past the row: the row is damaged.
    cx->fail(BC_E_CORRUPT, "malformed row");
    return {};
  }
  auto [cursor, step] = cx->owner(row);
  return cx->make_view(v->data(), v->size(), cursor, step);
}

bc_view bc_row_pk(bc_ctx* cx, uint32_t table, bc_view row) {
  auto r = resolve_row(cx, row);
  if (!r) return {};
  const auto* t = cx->cat->table(table);
  if (t == nullptr) {
    cx->fail(BC_E_HANDLE, "no table " + std::to_string(table));
    return {};
  }
  BC_GUARD_BEGIN
  Bytes key;
  if (!bytecask::sql::encode_pk_from_row(*t, *r, key)) {
    cx->fail(BC_E_LAYOUT, "row does not hold the table's primary key");
    return {};
  }
  return cx->arena_view(key);
  BC_GUARD_END(cx)
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
  // Every column at its default, so a plan compiled before ADD COLUMN …
  // DEFAULT writes the default without naming the column.
  auto& w = cx->rows.back().writer;
  for (const auto& c : t->columns) {
    if (!std::holds_alternative<std::monostate>(c.default_value) && !bytecask::sql::set_datum(w, c.id, c.default_value)) {
      cx->fail(BC_E_CORRUPT, "default of column " + c.name + " does not fit its layout");
      return {};
    }
  }
  return {static_cast<std::uint32_t>(cx->rows.size() - 1), cx->epoch};
  BC_GUARD_END(cx)
  return {};
}

bc_row bc_row_from(bc_ctx* cx, uint32_t layout, bc_view old) {
  auto o = resolve_row(cx, old);
  if (!o) return {};
  auto r = bc_row_begin(cx, layout);
  if (!cx->ok()) return {};
  if (!bytecask::sql::row_valid(*o)) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return {};
  }
  if (bytecask::sql::row_generation(*o) != cx->rows[r.i].layout->generation) {
    cx->fail(BC_E_LAYOUT, "row is not of this layout generation");
    return {};
  }
  BC_GUARD_BEGIN
  cx->rows[r.i].writer.copy_from(*o);
  BC_GUARD_END(cx)
  return r;
}

void bc_row_set_i64(bc_ctx* cx, bc_row r, uint32_t col, int64_t v) {
  set_column(cx, r, [&](RowWriter& w) { return w.set_i64(col, v); }, "an integer");
}

void bc_row_set_f64(bc_ctx* cx, bc_row r, uint32_t col, double v) {
  set_column(cx, r, [&](RowWriter& w) { return w.set_f64(col, v); }, "a double");
}

void bc_row_set_bool(bc_ctx* cx, bc_row r, uint32_t col, bool v) {
  set_column(cx, r, [&](RowWriter& w) { return w.set_bool(col, v); }, "a boolean");
}

void bc_row_set_bytes(bc_ctx* cx, bc_row r, uint32_t col, bc_view v) {
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  set_column(cx, r, [&](RowWriter& w) { return w.set_bytes(col, *bytes); }, "bytes");
}

void bc_row_set_null(bc_ctx* cx, bc_row r, uint32_t col) {
  set_column(cx, r, [&](RowWriter& w) { return w.set_null(col); }, "a column");
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
    cx->fail(BC_E_LIMIT, "row over the engine's value limit");
    return {};
  }
  return cx->arena_view(row);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_pin(bc_ctx* cx, bc_view v) {
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return {};
  return cx->arena_view(*b);
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

void bc_del_range(bc_ctx* cx, bc_view lo, bc_view hi) {
  auto bounds = cx->resolve2(lo, hi);
  if (!bounds) return;
  // No table id to resolve an open bound against: an empty bound would reach
  // past the table into the whole keyspace, the catalog included.
  if (bounds->first.empty() || bounds->second.empty()) {
    cx->fail(BC_E_HANDLE, "bc_del_range takes no open bound");
    return;
  }
  if (std::is_gteq(bytes_cmp(bounds->first, bounds->second))) return;  // an empty range deletes nothing
  BC_GUARD_BEGIN
  cx->effects.push_back({EffectKind::DelRange, Bytes(bounds->first.begin(), bounds->first.end()),
                         Bytes(bounds->second.begin(), bounds->second.end())});
  BC_GUARD_END(cx)
}

void bc_guard_unchanged(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::GuardUnchanged, key, {});
  BC_GUARD_END(cx)
}

void bc_guard_range(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi) {
  BC_GUARD_BEGIN
  auto range = resolve_range(cx, table_or_index, lo, hi);
  if (!range) return;
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return;  // an empty range cannot change
  cx->effects.push_back({EffectKind::GuardRange, std::move(range->lo), std::move(range->hi)});
  BC_GUARD_END(cx)
}

void bc_guard_present(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::GuardPresent, key, {});
  BC_GUARD_END(cx)
}

void bc_guard_absent(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  add_effect(cx, EffectKind::GuardAbsent, key, {});
  BC_GUARD_END(cx)
}

void bc_raise(bc_ctx* cx, uint32_t constraint, uint32_t object_id) {
  if (!cx->ok()) return;
  // The detail holds the kind in 8 bits and the id in 24: anything wider would
  // name another constraint or object.
  if (constraint < BC_C_UNIQUE || constraint > BC_C_RANGE || object_id > 0x00FF'FFFFu) {
    cx->fail(BC_E_HANDLE, "bc_raise with an unknown constraint kind or an object id past 2^24");
    return;
  }
  auto detail = (constraint << 24) | object_id;
  BC_GUARD_BEGIN
  cx->fail(BC_E_CONSTRAINT, constraint_message(detail));
  cx->detail = detail;
  BC_GUARD_END(cx)
}

}  // extern "C"
