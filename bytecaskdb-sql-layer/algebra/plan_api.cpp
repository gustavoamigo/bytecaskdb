// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// plan_api.cpp — the plan API of bc_plan.h on the engine's public API: the
// extern "C" definitions, and bc_ctx itself.
//
// Every call resolves its handles against tables owned by the context. A
// handle carries the context's epoch, bumped by bc_ctx_reset, so a handle
// from an earlier execution is stale. A view owned by a cursor also records
// the cursor's step count, so stepping invalidates it; a view that is a row
// records its table. Errors are sticky: the first one is kept, and every
// later call returns its neutral value.
//
// The writes (bc_insert, bc_update, bc_delete, bc_del_range) derive every
// index entry from the row and the pinned catalog version, and run the
// unique checks; a plan never writes a key itself.

// Standard library headers before the module imports, for the reason
// bytecaskdb/bytecask_hpp.cpp gives (LLVM 22's abi_tag redeclaration error).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <map>
#include <set>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The C contract, and the engine through its public header.
#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "bytecaskdb-sql-layer/algebra/bc_plan_testing.h"
#include "include/bytecask.hpp"

// bc_db, bc_catalog and bc_params, which bc_plan.h leaves opaque, and the
// catalog and codecs through it.
import bytecask.sql.runtime;

using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::Catalog;
using bytecask::sql::Datum;
using bytecask::sql::Index;
using bytecask::sql::KeyLayout;
using bytecask::sql::KeyPart;
using bytecask::sql::KeyReader;
using bytecask::sql::LiveCatalog;
using bytecask::sql::PhysType;
using bytecask::sql::RowLayout;
using bytecask::sql::RowWriter;
using bytecask::sql::Table;

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
// Verified schema references a context keeps. Past this a miss evicts one
// by CLOCK, so memory stays bounded and the references in use survive a
// flood of others, a recompile's dead ids included.
constexpr std::size_t kMaxVerified = 1024;

// What a view is, beyond its bytes: a row read from the engine (a stored
// row, the only kind bc_update and bc_delete take as the old one), a row a
// builder made, or neither.
enum class RowKind : std::uint8_t { None, Stored, Built };

struct ViewEntry {
  const std::byte* data;
  std::uint32_t len;
  std::uint32_t cursor;  // kNoCursor: lives until reset
  std::uint64_t step;    // the cursor's step when the view was made
  std::uint32_t table;   // the row's table; 0 unless kind is a row
  RowKind kind;
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
  Bytes row;      // on an index: the current entry's row, once bc_scan_row fetched it
  Bytes row_key;  // the row's key, rebuilt in place: no allocation per entry
  std::uint64_t row_step{0};  // the step `row` was fetched at; 0: none
};

struct KeyBuilder {
  const KeyLayout* layout;
  Bytes buf;
  std::size_t next_part{0};
  bool done{false};
};

struct RowBuilder {
  std::uint32_t table;
  const RowLayout* layout;
  RowWriter writer;
  bool done{false};
};

enum class EffectKind { Put, Del, DelRange, GuardUnchanged, GuardRange, GuardPresent, GuardAbsent };

struct Effect {
  EffectKind kind;
  Bytes a, b;
};

// A declared column of a verified reference: where it is in its table's
// current layout.
struct ColSlot {
  bool declared{false};
  PhysType type{PhysType::I64};
  std::uint32_t pos{0};
};

// The declared columns of one table, indexed by column id.
struct TableCols {
  std::uint32_t table;
  std::uint16_t generation;  // the current layout's: the generation pos is for
  std::vector<ColSlot> by_col;
};

// A schema reference verified against one catalog version, with what the
// verification built: the column map and the result layout. Plain data, no
// pointer into the catalog, so an entry outlives the version it was built
// on. Invariants: an entry is served without verification only when it is
// valid, its ref_id and version match the reference and the pinned catalog,
// and the reference's arrays hold what the entry was verified from, so a
// reference rebuilt under a kept id is verified on its own. A cached
// entry's ref_id is an id bc_ref_id issued and the key slot_of maps to it;
// a reference with no issued id uses the context's scratch entry and is
// verified on every run. A failed verification leaves the entry invalid,
// so the next run verifies again.
struct Verified {
  bool valid{false};
  bool referenced{false};  // CLOCK: used since the hand last passed
  std::uint64_t ref_id{0};
  std::uint64_t version{0};
  // What the reference held, compared on every hit.
  std::vector<std::uint32_t> read_ids, written_ids;
  std::vector<bc_col_ref> col_refs;
  // Built from it: sorted, for a binary search per access.
  std::vector<std::uint32_t> objects;  // the tables and indexes it names
  std::vector<std::uint32_t> written;  // the tables it writes
  std::vector<TableCols> tables;       // by table id
  std::vector<std::uint8_t> result_codes;  // as given
  std::vector<PhysType> result_types;
  std::optional<RowLayout> result_layout;
};

// Byte order, for the set of row keys an execution wrote; transparent, so a
// view looks a key up without copying it.
struct BytesLess {
  using is_transparent = void;
  auto operator()(BytesView a, BytesView b) const -> bool { return std::ranges::lexicographical_compare(a, b); }
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

auto type_name(PhysType t) -> const char* {
  switch (t) {
    case PhysType::I64: return "an integer";
    case PhysType::F64: return "a double";
    case PhysType::Bool: return "a boolean";
    case PhysType::Bytes: return "bytes";
  }
  return "a value";
}

// The table or index id of an encoded key, and its namespace.
auto key_object(BytesView key) -> std::optional<std::pair<std::uint8_t, std::uint32_t>> {
  if (key.size() < bytecask::sql::kKeyPrefixBytes) return std::nullopt;
  std::uint32_t id = 0;
  for (std::size_t i = 1; i < bytecask::sql::kKeyPrefixBytes; ++i) id = (id << 8) | std::to_integer<std::uint32_t>(key[i]);
  return std::pair{std::to_integer<std::uint8_t>(key[0]), id};
}

// An error code other than BC_E_LAYOUT, checked at compile time: a layout
// error is set with its bc_layout_detail, through the other fail overload,
// so none goes without one.
// -Wc++20-compat: the project is C++23; consteval is what makes it a
// compile-time check.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++20-compat"
struct ErrorCode {
  consteval ErrorCode(bc_error_code c) : code{c} {  // implicit: fail(BC_E_IO, ...)
    if (c == BC_E_LAYOUT) throw "BC_E_LAYOUT is set with its bc_layout_detail";
  }
  bc_error_code code;
};
#pragma clang diagnostic pop

}  // namespace

struct bc_ctx {
  bytecask::DB* db;
  LiveCatalog* live;
  // The catalog version this execution resolves against, pinned with the
  // snapshot by bc_ctx_begin and dropped with it; cat is pinned.get(), or
  // the empty catalog between executions, where every lookup fails.
  std::shared_ptr<const Catalog> pinned;
  Catalog unpinned;
  const Catalog* cat{&unpinned};
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
  // The row keys this execution's writes touched: a second write of one row
  // would derive its index entries from the snapshot's row again, not from
  // the first write's, and leave the first write's entries behind.
  std::set<Bytes, BytesLess> written_rows;
  // Where each part of the last key a key-part decoder read starts, so the
  // next part of the same key is found without skipping the earlier ones
  // again. Keyed by the view handle, unique within an execution.
  struct {
    bc_view view{};
    std::uint32_t object{0};
    std::vector<std::size_t> at;
  } key_parts;
  std::map<Bytes, Bytes, BytesLess> deleted_ranges;  // lo -> hi of each bc_del_range, likewise; disjoint

  // References verified by bc_ctx_run, kept across executions: slots, a map
  // from reference id to slot, and the CLOCK hand. A context is bound to one
  // database, and a catalog version is never reused within one, so an entry
  // is matched by the reference's id and the version alone. An id of 0 is
  // verified into `scratch` on every run. `slots` grows by push_back, which
  // keeps every entry where it is, up to kMaxVerified, and only use_ref
  // reuses a slot, before it sets `current`, so `current` never dangles.
  std::deque<Verified> slots;
  std::unordered_map<std::uint64_t, std::uint32_t> slot_of;
  std::size_t hand{0};
  std::uint64_t verifications{0};  // runs the cache did not answer
  Verified scratch;
  const Verified* current{nullptr};  // this execution's reference; nullptr: none
  const TableCols* last_table{nullptr};  // of current: the table declared() found last

  // Results: rows in the row format of the plan's result_types, each
  // preceded by its u32 length.
  Bytes results;
  std::size_t result_pos{0};
  std::optional<RowWriter> emit;
  std::uint32_t emit_col{0};
  std::int64_t affected{-1};

  bc_ctx(bytecask::DB* d, LiveCatalog* l, std::uint32_t arena_bytes)
      : db{d}, live{l}, arena(arena_bytes) {}

  auto ok() const -> bool { return error == BC_OK; }

  void fail(ErrorCode code, std::string msg) {
    if (error != BC_OK) return;
    error = code.code;
    message = std::move(msg);
  }

  void fail(bc_layout_detail why, std::string msg) {
    if (error != BC_OK) return;
    error = BC_E_LAYOUT;
    detail = why;
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

  auto make_view(const std::byte* p, std::size_t len, std::uint32_t cursor = kNoCursor, std::uint64_t step = 0,
                 std::uint32_t table = 0, RowKind kind = RowKind::None) -> bc_view {
    views.push_back({p, static_cast<std::uint32_t>(len), cursor, step, table, kind});
    return {static_cast<std::uint32_t>(views.size() - 1), epoch};
  }

  auto arena_view(BytesView b, std::uint32_t table = 0, RowKind kind = RowKind::None) -> bc_view {
    const auto* p = arena_copy(b);
    if (p == nullptr) return {};
    return make_view(p, b.size(), kNoCursor, 0, table, kind);
  }

  // A view's entry; nullptr (and BC_E_HANDLE) if it is stale or foreign. The
  // zero handle, the empty view every neutral return gives, has no entry.
  auto entry(bc_view v) -> const ViewEntry* {
    if (v.g != epoch || v.i >= views.size()) {
      fail(BC_E_HANDLE, "stale or foreign view");
      return nullptr;
    }
    const auto& e = views[v.i];
    if (e.cursor != kNoCursor && (e.cursor >= cursors.size() || cursors[e.cursor].step != e.step)) {
      fail(BC_E_HANDLE, "view read after its cursor moved");
      return nullptr;
    }
    return &e;
  }

  // The bytes of a view; nullopt (and BC_E_HANDLE) if it is stale or foreign.
  auto resolve(bc_view v) -> std::optional<BytesView> {
    if (v.g == 0 && v.i == 0) return BytesView{};
    const auto* e = entry(v);
    if (e == nullptr) return std::nullopt;
    return BytesView{e->data, e->len};
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

  // Whether the plan's reference names table or index `id` (and, for
  // `write`, lists it among the tables written), as it must for every object
  // a plan names, so the compatibility check covers each one. A run without
  // a reference is unchecked: it can name no column, only keys.
  auto declared_object(std::uint32_t id, bool write) -> bool {
    if (current == nullptr) return true;
    const auto& list = write ? current->written : current->objects;
    if (std::ranges::binary_search(list, id)) return true;
    fail(BC_E_HANDLE, std::string{write ? "table " : "table or index "} + std::to_string(id) +
                          (write ? " is not among the tables the plan's schema ref writes"
                                 : " is not declared in the plan's schema ref"));
    return false;
  }

  // The key layout of a table or index, or nullptr and BC_E_HANDLE.
  auto key_layout(std::uint32_t table_or_index) -> const KeyLayout* {
    if (!ok() || !declared_object(table_or_index, false)) return nullptr;
    const auto* kl = cat->key_layout(table_or_index);
    if (kl == nullptr) fail(BC_E_HANDLE, "no table or index " + std::to_string(table_or_index));
    return kl;
  }

  // A table of the pinned version the plan declares (among those it writes,
  // for `write`), or nullptr and an error.
  auto table(std::uint32_t id, bool write = false) -> const Table* {
    if (!ok() || !declared_object(id, write)) return nullptr;
    const auto* t = cat->table(id);
    if (t == nullptr) fail(BC_L_PLAN, "no table " + std::to_string(id));
    return t;
  }

  void push_effect(EffectKind kind, BytesView a, BytesView b = {}) {
    effects.push_back({kind, Bytes(a.begin(), a.end()), Bytes(b.begin(), b.end())});
  }

  // Sets the constraint error: the kind in the detail's high byte, the
  // object id in its low 24 bits.
  void raise(std::uint32_t constraint, std::uint32_t object_id, std::string msg = {}) {
    if (!ok()) return;
    auto d = (constraint << 24) | object_id;
    fail(BC_E_CONSTRAINT, msg.empty() ? constraint_message(d) : std::move(msg));
    detail = d;
  }

  void drop_execution_state() {
    cursors.clear();  // iterators before the snapshot they read
    snap.reset();
    views.clear();
    keys.clear();  // key and row builders point into the pinned catalog: before it
    rows.clear();
    cat = &unpinned;
    pinned.reset();  // an idle context keeps no superseded catalog alive
    effects.clear();
    written_rows.clear();
    deleted_ranges.clear();
    key_parts.view = {};  // view handles repeat across executions once the epoch wraps
    key_parts.at.clear();
    results.clear();
    result_pos = 0;
    emit.reset();
    emit_col = 0;
    current = nullptr;
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

// Reads one record through the snapshot into the arena. A record at a row
// key is a stored row of that key's table.
auto read_record(bc_ctx* cx, BytesView key, bool* found) -> bc_view {
  if (found != nullptr) *found = false;
  if (!cx->charge()) return {};
  Bytes out;
  if (!cx->snap->get({}, key, out)) return {};
  std::uint32_t table = 0;
  if (auto obj = key_object(key); obj && obj->first == bytecask::sql::kNsRow && cx->cat->table(obj->second) != nullptr) {
    table = obj->second;
  }
  auto v = cx->arena_view(out, table, table != 0 ? RowKind::Stored : RowKind::None);
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

// Appends parameter `i` as a key part: its value, or the null part for NULL.
[[nodiscard]] auto put_param_part(bc_ctx* cx, Bytes& buf, KeyPart part, std::uint32_t i) -> bool {
  const auto* d = cx->param(i);
  if (d == nullptr) return false;
  if (std::holds_alternative<std::monostate>(*d)) {
    if (!part.nullable) {
      cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is NULL for a key part that is not nullable");
      return false;
    }
    bytecask::sql::key_put_null(buf, part);
    return true;
  }
  if (!bytecask::sql::datum_fits(*d, part.type)) {
    cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not " + type_name(part.type));
    return false;
  }
  switch (part.type) {
    case PhysType::I64: bytecask::sql::key_put_i64(buf, std::get<std::int64_t>(*d), part); break;
    case PhysType::F64: bytecask::sql::key_put_f64(buf, std::get<double>(*d), part); break;
    case PhysType::Bool: bytecask::sql::key_put_bool(buf, std::get<bool>(*d), part); break;
    case PhysType::Bytes: bytecask::sql::key_put_bytes(buf, std::get<Bytes>(*d), part); break;
  }
  return true;
}

// A finished key as a view: BC_E_LIMIT past the engine's key ceiling.
auto key_view(bc_ctx* cx, BytesView key) -> bc_view {
  if (key.size() > bytecask::sql::kMaxKeyBytes) {
    cx->fail(BC_E_LIMIT, "key over the engine's key limit");
    return {};
  }
  return cx->arena_view(key);
}

auto key_builder_part(bc_ctx* cx, bc_key k, std::optional<PhysType> type, bool is_null) -> KeyBuilder* {
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return nullptr;
  if (b->done || b->next_part >= b->layout->parts.size()) {
    cx->fail(BC_E_HANDLE, "more key parts than the key has");
    return nullptr;
  }
  const auto& part = b->layout->parts[b->next_part];
  if (is_null ? !part.nullable : (type && part.type != *type)) {
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
  if (auto obj = key_object(*k); !obj || obj->first != kl->ns || obj->second != table_or_index) {
    cx->fail(BC_E_HANDLE, "key is not of table or index " + std::to_string(table_or_index));
    return std::nullopt;
  }
  auto& kp = cx->key_parts;
  if (kp.view.i != key.i || kp.view.g != key.g || kp.object != table_or_index) {
    kp.view = key;
    kp.object = table_or_index;
    kp.at.clear();
    KeyReader r{*k, bytecask::sql::kKeyPrefixBytes};
    kp.at.push_back(r.pos());
    for (const auto& p : kl->parts) {
      if (!r.skip(p)) break;  // the parts after a malformed one have no start
      kp.at.push_back(r.pos());
    }
  }
  if (part >= kp.at.size()) {
    cx->fail(BC_E_CORRUPT, "unparseable key");
    return std::nullopt;
  }
  return KeyPartAt{KeyReader{*k, kp.at[part]}, kl->parts[part]};
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
  cx->push_effect(kind, views->first, views->second);
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
    auto obj = key_object(key);
    if (!obj || (obj->first != bytecask::sql::kNsRow && obj->first != bytecask::sql::kNsIndex)) return;
    const auto* kl = cx->cat->key_layout(obj->second);
    if (kl == nullptr) return;
    if (std::ranges::find(out, kl->table->id) == out.end()) out.push_back(kl->table->id);
  };
  for (const auto& e : cx->effects) {
    switch (e.kind) {
      // A range's upper bound is often the next object's prefix: only its
      // lower bound names what it deletes.
      case EffectKind::Put:
      case EffectKind::Del:
      case EffectKind::DelRange: note(e.a); break;
      case EffectKind::GuardUnchanged:
      case EffectKind::GuardRange:
      case EffectKind::GuardPresent:
      case EffectKind::GuardAbsent: break;
    }
  }
  return out;
}

// A reference's arrays, empty where a pointer is null.
struct RefArrays {
  std::span<const std::uint32_t> read, written;
  std::span<const bc_col_ref> cols;
};

auto ref_arrays(const bc_schema_ref& r) -> RefArrays {
  auto span = [](const auto* p, std::uint32_t n) { return std::span{p, p == nullptr ? 0 : n}; };
  return {span(r.objects_read, r.n_read), span(r.tables_written, r.n_written), span(r.cols, r.n_cols)};
}

// Whether `ref` holds what `e` was verified from: its ids, columns and
// result types, compared by value, never by address.
auto same_contents(const Verified& e, const bc_schema_ref& ref) -> bool {
  auto [read, written, cols] = ref_arrays(ref);
  auto same_col = [](const bc_col_ref& a, const bc_col_ref& b) {
    return a.table == b.table && a.col == b.col && a.type == b.type;
  };
  return std::ranges::equal(read, e.read_ids) && std::ranges::equal(written, e.written_ids) &&
         std::ranges::equal(cols, e.col_refs, same_col) &&
         std::ranges::equal(std::span{ref.result_types, ref.result_types == nullptr ? 0 : ref.n_result},
                            e.result_codes);
}

// Builds `e` for `ref` on the pinned version: the compatibility check, the
// result layout (rebuilt only when the codes change), the column map.
[[nodiscard]] auto verify(bc_ctx* cx, const bc_schema_ref* ref, Verified& e) -> bool {
  ++cx->verifications;
  e.valid = false;
  if (auto why = bytecask::sql::schema_incompatibility(*cx->cat, *ref)) {
    cx->fail(BC_L_INCOMPATIBLE, "plan compiled against catalog v" + std::to_string(ref->catalog_version) +
                                    " is incompatible with v" + std::to_string(cx->cat->version()) + ": " + *why);
    return false;
  }
  auto codes = std::span{ref->result_types, ref->result_types == nullptr ? 0 : ref->n_result};
  if (!e.result_layout || !std::ranges::equal(codes, e.result_codes)) {
    std::vector<PhysType> types;
    for (auto code : codes) {
      auto t = phys_type(code);
      if (!t) {
        cx->fail(BC_L_PLAN, "unknown result type code " + std::to_string(code));
        return false;
      }
      types.push_back(*t);
    }
    try {
      e.result_layout.emplace(bytecask::sql::make_result_layout(types));
    } catch (const std::length_error&) {
      e.result_layout.reset();
      cx->fail(BC_L_PLAN, "the plan's result row has too many columns for one row layout");
      return false;
    }
    e.result_codes.assign(codes.begin(), codes.end());
    e.result_types = std::move(types);
  }
  auto [read, written, cols] = ref_arrays(*ref);
  e.read_ids.assign(read.begin(), read.end());
  e.written_ids.assign(written.begin(), written.end());
  e.col_refs.assign(cols.begin(), cols.end());
  auto sorted = [](std::vector<std::uint32_t>& v) {
    std::ranges::sort(v);
    v.erase(std::ranges::unique(v).begin(), v.end());
  };
  e.written = e.written_ids;
  sorted(e.written);
  e.objects = e.read_ids;
  e.objects.insert(e.objects.end(), e.written.begin(), e.written.end());
  sorted(e.objects);
  e.tables.clear();
  for (const auto& c : cols) {
    const auto* t = cx->cat->table(c.table);  // the check above found it
    const auto* slot = t->layout.find(c.col);
    auto it = std::ranges::lower_bound(e.tables, c.table, {}, &TableCols::table);
    if (it == e.tables.end() || it->table != c.table) it = e.tables.insert(it, {c.table, t->layout.generation, {}});
    if (it->by_col.size() <= c.col) it->by_col.resize(c.col + 1);
    it->by_col[c.col] = {true, slot->type, slot->pos};
  }
  e.ref_id = ref->id;
  e.version = cx->cat->version();
  e.valid = true;
  return true;
}

// The cache slot of reference `id`: its own, or a free one, or the one CLOCK
// evicts: the first the hand finds unused since it last passed.
auto cache_slot(bc_ctx* cx, std::uint64_t id) -> Verified& {
  if (auto it = cx->slot_of.find(id); it != cx->slot_of.end()) {
    auto& e = cx->slots[it->second];
    e.referenced = true;
    return e;
  }
  std::size_t at = cx->slots.size();
  if (at < kMaxVerified) {
    cx->slots.emplace_back();
  } else {
    while (cx->slots[cx->hand].referenced) {
      cx->slots[cx->hand].referenced = false;
      cx->hand = (cx->hand + 1) % kMaxVerified;
    }
    at = cx->hand;
    cx->hand = (cx->hand + 1) % kMaxVerified;
    cx->slot_of.erase(cx->slots[at].ref_id);
  }
  auto& e = cx->slots[at];
  e.valid = false;
  e.ref_id = id;
  e.referenced = true;
  cx->slot_of[id] = static_cast<std::uint32_t>(at);
  return e;
}

// The highest reference id the runtime has issued (bc_ref_id). Process-wide,
// as the ids are: a reference built in one context runs in any.
auto issued_ref_ids() -> std::atomic<std::uint64_t>& {
  static std::atomic<std::uint64_t> issued{0};
  return issued;
}

// Makes `ref` the execution's reference: from the cache when this context
// verified it against the pinned version before, verified otherwise.
[[nodiscard]] auto use_ref(bc_ctx* cx, const bc_schema_ref* ref) -> bool {
  if (ref == nullptr) {
    cx->current = nullptr;
    cx->last_table = nullptr;
    return true;
  }
  // An id the runtime never issued is no id: verified on every run.
  auto id = ref->id <= issued_ref_ids().load(std::memory_order_relaxed) ? ref->id : 0;
  auto& e = id == 0 ? cx->scratch : cache_slot(cx, id);
  if (id == 0 || !e.valid || e.ref_id != id || e.version != cx->cat->version() || !same_contents(e, *ref)) {
    if (!verify(cx, ref, e)) return false;
  }
  cx->current = &e;
  cx->last_table = nullptr;
  return true;
}

// Where declared column `col` of `table` is; nullptr and BC_E_HANDLE for a
// column the reference does not declare or not of `type`.
auto declared(bc_ctx* cx, std::uint32_t table, std::uint32_t col, std::optional<PhysType> type)
    -> std::pair<const TableCols*, const ColSlot*> {
  if (!cx->ok()) return {};
  if (cx->current != nullptr) {
    // Consecutive accesses mostly name one table: the last one found is
    // tried before the search.
    const auto* tc = cx->last_table;
    if (tc == nullptr || tc->table != table) {
      const auto& tables = cx->current->tables;
      auto it = std::ranges::lower_bound(tables, table, {}, &TableCols::table);
      tc = it != tables.end() && it->table == table ? &*it : nullptr;
      cx->last_table = tc;
    }
    if (tc != nullptr && col < tc->by_col.size() && tc->by_col[col].declared) {
      const auto& s = tc->by_col[col];
      if (type && s.type != *type) {
        cx->fail(BC_E_HANDLE, "column " + std::to_string(col) + " of table " + std::to_string(table) + " is not " +
                                  type_name(*type));
        return {};
      }
      return {tc, &s};
    }
  }
  cx->fail(BC_E_HANDLE, "column " + std::to_string(col) + " of table " + std::to_string(table) +
                            " is not declared in the plan's schema ref");
  return {};
}

// A row view's bytes and table. Not a row is BC_E_HANDLE; a row whose
// header does not parse is BC_E_CORRUPT, never read as null.
struct RowRef {
  BytesView bytes;
  std::uint32_t table;
  RowKind kind;
};

auto resolve_row(bc_ctx* cx, bc_view row) -> std::optional<RowRef> {
  if (!cx->ok()) return std::nullopt;
  if (row.g == 0 && row.i == 0) {
    cx->fail(BC_E_HANDLE, "the empty view is not a row");
    return std::nullopt;
  }
  const auto* e = cx->entry(row);
  if (e == nullptr) return std::nullopt;
  if (e->kind == RowKind::None) {
    cx->fail(BC_E_HANDLE, "the view is not a row");
    return std::nullopt;
  }
  BytesView b{e->data, e->len};
  if (!bytecask::sql::row_header_valid(b)) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return std::nullopt;
  }
  return RowRef{b, e->table, e->kind};
}

// A row of generation `gen`, not its table's current one. Only REBUILD
// (#503) makes a second generation: a retired one is a layout refusal, and
// one the table never had is damage.
void fail_generation(bc_ctx* cx, const Table* t, std::uint16_t gen) {
  auto retired = t != nullptr && std::ranges::any_of(t->retired_layouts, [&](const auto& l) {
                   return l.second.generation == gen;
                 });
  auto msg = "row of layout generation " + std::to_string(gen);
  if (retired) {
    cx->fail(BC_L_RETIRED, msg + ", which REBUILD retired");
  } else {
    cx->fail(BC_E_CORRUPT, msg + ", which its table never had");
  }
}

// BC_E_CONSTRAINT with BC_C_NOT_NULL: column `col` of `table` is null.
void not_null(bc_ctx* cx, std::uint32_t table, std::uint32_t col) {
  const auto* t = cx->cat->table(table);
  const auto* c = t == nullptr ? nullptr : t->column(col);
  cx->raise(BC_C_NOT_NULL, table,
            "null value in column \"" + (c == nullptr ? std::to_string(col) : c->name) + "\" of table \"" +
                (t == nullptr ? std::to_string(table) : t->name) + "\" violates its not-null constraint");
}

// Column `col` of a row view, where a decoder reads it: the row, and the
// column's position in the row's generation.
struct RowCol {
  BytesView row;
  const ColSlot* slot;
};

auto row_col(bc_ctx* cx, bc_view row, std::uint32_t col, std::optional<PhysType> type) -> std::optional<RowCol> {
  auto r = resolve_row(cx, row);
  if (!r) return std::nullopt;
  auto [tc, slot] = declared(cx, r->table, col, type);
  if (slot == nullptr) return std::nullopt;
  auto gen = bytecask::sql::row_generation(r->bytes);
  if (gen != tc->generation) {
    fail_generation(cx, cx->cat->table(r->table), gen);
    return std::nullopt;
  }
  return RowCol{r->bytes, slot};
}

// A row a write takes: a row view of `table`, of its current layout
// generation, and a stored one when `stored` (the old row of an update or
// delete, whose index entries are derived from it).
auto write_row(bc_ctx* cx, const Table& t, bc_view row, bool stored) -> std::optional<BytesView> {
  auto r = resolve_row(cx, row);
  if (!r) return std::nullopt;
  if (stored && r->kind != RowKind::Stored) {
    cx->fail(BC_E_HANDLE, "the old row must be a row read in this execution");
    return std::nullopt;
  }
  if (r->table != t.id) {
    cx->fail(BC_L_ROW, "row of table " + std::to_string(r->table) + " written to table " + t.name);
    return std::nullopt;
  }
  if (auto gen = bytecask::sql::row_generation(r->bytes); gen != t.layout.generation) {
    fail_generation(cx, &t, gen);
    return std::nullopt;
  }
  return r->bytes;
}

// Whether a range this execution deleted holds key `lo` or, given `hi`,
// meets [lo, hi). The ranges are sorted and disjoint: only the last one
// starting at or before lo can hold lo, and only the first after it can
// start inside [lo, hi).
[[nodiscard]] auto meets_deleted(const bc_ctx* cx, BytesView lo, std::optional<BytesView> hi = std::nullopt) -> bool {
  const auto& ranges = cx->deleted_ranges;
  auto next = ranges.upper_bound(lo);
  if (next != ranges.begin() && std::is_lt(bytes_cmp(lo, std::prev(next)->second))) return true;
  return hi && next != ranges.end() && std::is_lt(bytes_cmp(next->first, *hi));
}

// Records a row key this execution writes; a second write of it, or a write
// into a range this execution deleted, is a plan bug (BC_E_HANDLE). The keys
// and the deleted ranges are sorted sets: a lookup each per write.
[[nodiscard]] auto first_write(bc_ctx* cx, const Table& t, BytesView pk) -> bool {
  if (cx->written_rows.contains(pk) || meets_deleted(cx, pk)) {
    cx->fail(BC_E_HANDLE, "a row of table " + t.name + " written twice in one plan");
    return false;
  }
  cx->written_rows.emplace(pk.begin(), pk.end());
  return true;
}

[[nodiscard]] auto encode_pk(bc_ctx* cx, const Table& t, BytesView row, Bytes& out) -> bool {
  if (bytecask::sql::encode_pk_from_row(t, row, out)) return true;
  cx->fail(BC_E_CORRUPT, "row does not hold table " + t.name + "'s primary key");
  return false;
}

// An index entry of a row, with the length of its index-part prefix and
// whether a part is null.
struct Entry {
  Bytes key;
  std::size_t prefix_len{0};
  bool has_null{false};
  [[nodiscard]] auto prefix() const -> BytesView { return BytesView{key}.first(prefix_len); }
};

[[nodiscard]] auto encode_entry(bc_ctx* cx, const Table& t, const Index& ix, BytesView row, Entry& out) -> bool {
  if (bytecask::sql::encode_index_key_from_row(t, ix, row, out.key, &out.prefix_len, &out.has_null)) return true;
  cx->fail(BC_E_CORRUPT, "row does not hold the parts of index " + ix.name);
  return false;
}

// The primary key is free on the snapshot, or BC_C_UNIQUE on the table.
// Its race needs no guard: the key is in the write plan, and the engine
// checks every written key for a change since the snapshot.
[[nodiscard]] auto pk_free(bc_ctx* cx, const Table& t, BytesView pk) -> bool {
  if (!cx->charge()) return false;
  if (!cx->snap->contains_key({}, pk)) return true;
  cx->raise(BC_C_UNIQUE, t.id);
  return false;
}

// No entry has this unique index prefix on the snapshot, or BC_C_UNIQUE on
// the index; the range guard covers the race, since a duplicate is another
// row's entry, a key this plan does not write.
[[nodiscard]] auto unique_free(bc_ctx* cx, const Index& ix, BytesView prefix) -> bool {
  if (!cx->charge()) return false;
  auto keys = cx->snap->keys_from({}, prefix);
  if (auto it = keys.begin(); it != std::default_sentinel && starts_with(*it, prefix)) {
    cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  cx->push_effect(EffectKind::GuardRange, prefix, bytecask::sql::key_successor(prefix));
  return true;
}


// Loads a fixed column; a malformed row is BC_E_CORRUPT and the neutral value.
template <typename Load, typename T>
auto load_column(bc_ctx* cx, bc_view row, std::uint32_t col, PhysType type, Load&& load, T neutral) -> T {
  auto rc = row_col(cx, row, col, type);
  if (!rc) return neutral;
  auto v = load(rc->row, rc->slot->pos);
  if (!v) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return neutral;
  }
  return *v;
}

// The builder a setter targets, and the declared column it sets.
auto set_target(bc_ctx* cx, bc_row r, std::uint32_t col, std::optional<PhysType> type) -> RowBuilder* {
  auto* b = row_builder(cx, r);
  if (b == nullptr) return nullptr;
  if (declared(cx, b->table, col, type).second == nullptr) return nullptr;
  return b;
}

// A NULL for column `col` of a row being built: refused for a NOT NULL
// column with BC_E_CONSTRAINT, BC_C_NOT_NULL and the table's id (column ids
// are per table; the message names the column), and never set, so a
// column's default survives the refusal.
[[nodiscard]] auto null_allowed(bc_ctx* cx, const RowBuilder& b, std::uint32_t col) -> bool {
  if (b.layout->find(col)->nullable) return true;
  not_null(cx, b.table, col);
  return false;
}

template <typename Set>
void set_column(bc_ctx* cx, bc_row r, std::uint32_t col, std::optional<PhysType> type, Set&& set) {
  auto* b = set_target(cx, r, col, type);
  if (b == nullptr) return;
  BC_GUARD_BEGIN
  // The column is declared with this type and in the current layout: the
  // writer cannot refuse it.
  if (!set(b->writer)) cx->fail(BC_L_ROW, "column " + std::to_string(col) + " is not in the row's layout");
  BC_GUARD_END(cx)
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

bool bc_ctx_check(bc_ctx* cx, const bc_schema_ref* compiled_against) {
  if (!cx->ok()) return false;
  if (!cx->snap) {
    cx->fail(BC_E_HANDLE, "bc_ctx_check or bc_ctx_run without bc_ctx_begin");
    return false;
  }
  BC_GUARD_BEGIN
  (void)use_ref(cx, compiled_against);
  BC_GUARD_END(cx)
  return cx->ok();
}

void bc_ctx_run(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* compiled_against) {
  if (!bc_ctx_check(cx, compiled_against)) return;
  plan(cx);
  if (cx->ok() && cx->emit) cx->fail(BC_L_PLAN, "result row begun and not ended");
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
    // retry pins the new catalog version, whose indexes it maintains.
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

uint64_t bc_ctx_verifications(const bc_ctx* cx) { return cx->verifications; }

uint64_t bc_ref_id(void) { return issued_ref_ids().fetch_add(1, std::memory_order_relaxed) + 1; }

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
  if (cx->current == nullptr) {
    cx->fail(BC_L_PLAN, "the plan's schema ref declares no result types");
    return;
  }
  if (cx->emit) {
    cx->fail(BC_L_PLAN, "result row already begun");
    return;
  }
  BC_GUARD_BEGIN
  cx->emit.emplace(*cx->current->result_layout);
  cx->emit_col = 0;
  BC_GUARD_END(cx)
}

// C++ linkage: overloads and a template, inside the extern "C" block.
extern "C++" {
namespace {
// The next result column, which must be of `type` (any, when nullopt).
auto emit_slot(bc_ctx* cx, std::optional<PhysType> type) -> bool {
  if (!cx->ok()) return false;
  if (!cx->emit || cx->emit_col >= cx->current->result_types.size() ||
      (type && cx->current->result_types[cx->emit_col] != *type)) {
    cx->fail(BC_L_PLAN, "emitted column does not match the result layout");
    return false;
  }
  return true;
}

void emit_bytes(bc_ctx* cx, BytesView b) {
  BC_GUARD_BEGIN
  (void)cx->emit->set_bytes(cx->emit_col++, b);
  BC_GUARD_END(cx)
}

void emit_value(bc_ctx* cx, std::int64_t v) { (void)cx->emit->set_i64(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, double v) { (void)cx->emit->set_f64(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, bool v) { (void)cx->emit->set_bool(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, BytesView v) { emit_bytes(cx, v); }

// The next result column from a decoded value: BC_E_CORRUPT when the source
// did not parse, the value when there is one, NULL otherwise. One path for
// bc_emit_col and bc_emit_key_part, whatever the type.
template <typename T>
void emit_decoded(bc_ctx* cx, const std::optional<T>& v, bool corrupt, bool has_value, const char* what) {
  if (corrupt) {
    cx->fail(BC_E_CORRUPT, what);
  } else if (!has_value) {
    (void)cx->emit->set_null(cx->emit_col++);
  } else if constexpr (std::is_same_v<T, Bytes>) {
    emit_value(cx, BytesView{*v});
  } else {
    emit_value(cx, *v);
  }
}
}  // namespace
}  // extern "C++"

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
  emit_bytes(cx, *b);
}

void bc_emit_null(bc_ctx* cx) {
  if (!emit_slot(cx, std::nullopt)) return;
  (void)cx->emit->set_null(cx->emit_col++);
}

void bc_emit_col(bc_ctx* cx, bc_view row, uint32_t col) {
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc || !emit_slot(cx, rc->slot->type)) return;
  // A decoder's nullopt here is a malformed row: null is the bitmap's to say.
  auto present = bytecask::sql::row_present_bit(rc->row, col);
  auto emit = [&](auto v) { emit_decoded(cx, v, present && !v, present, "malformed row"); };
  const auto& r = rc->row;
  auto pos = rc->slot->pos;
  switch (rc->slot->type) {
    case PhysType::I64: return emit(bytecask::sql::row_i64(r, pos));
    case PhysType::F64: return emit(bytecask::sql::row_f64(r, pos));
    case PhysType::Bool: return emit(bytecask::sql::row_bool(r, pos));
    case PhysType::Bytes: return emit(bytecask::sql::row_bytes(r, pos));
  }
}

void bc_emit_key_part(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  auto at = key_part_at(cx, table_or_index, key, part, std::nullopt);
  if (!at || !emit_slot(cx, at->part.type)) return;
  // A decoder's nullopt is a null part, or a malformed key when the reader failed.
  auto& r = at->reader;
  auto emit = [&](auto v) { emit_decoded(cx, v, !r.ok(), r.ok() && v.has_value(), "unparseable key"); };
  BC_GUARD_BEGIN
  switch (at->part.type) {
    case PhysType::I64: return emit(r.read_i64(at->part));
    case PhysType::F64: return emit(r.read_f64(at->part));
    case PhysType::Bool: return emit(r.read_bool(at->part));
    case PhysType::Bytes: return emit(r.read_bytes(at->part));
  }
  BC_GUARD_END(cx)
}

void bc_emit_end(bc_ctx* cx) {
  if (!cx->ok()) return;
  if (!cx->emit || cx->emit_col != cx->current->result_types.size()) {
    cx->fail(BC_L_PLAN, "result row ended with the wrong number of columns");
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
  auto* b = key_builder_part(cx, k, std::nullopt, true);
  if (b != nullptr) bytecask::sql::key_put_null(b->buf, current_part(*b));
}

void bc_key_param(bc_ctx* cx, bc_key k, uint32_t i) {
  if (cx->param(i) == nullptr) return;
  auto* b = key_builder_part(cx, k, std::nullopt, false);
  if (b == nullptr) return;
  BC_GUARD_BEGIN
  (void)put_param_part(cx, b->buf, current_part(*b), i);
  BC_GUARD_END(cx)
}

bc_view bc_key_end(bc_ctx* cx, bc_key k) {
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return {};
  if (b->done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return {};
  }
  b->done = true;
  return key_view(cx, b->buf);
}

bc_view bc_key_params(bc_ctx* cx, uint32_t table_or_index, uint32_t first, uint32_t n) {
  const auto* kl = cx->key_layout(table_or_index);
  if (kl == nullptr) return {};
  if (n > kl->parts.size()) {
    cx->fail(BC_E_HANDLE, "more key parts than the key has");
    return {};
  }
  BC_GUARD_BEGIN
  Bytes key;
  bytecask::sql::key_put_prefix(key, kl->ns, table_or_index);
  for (std::uint32_t p = 0; p < n; ++p) {
    // first + p cannot wrap: a parameter index past the bound list fails first.
    if (first > UINT32_MAX - p || !put_param_part(cx, key, kl->parts[p], first + p)) {
      cx->fail(BC_E_HANDLE, "parameter index out of range");
      return {};
    }
  }
  return key_view(cx, key);
  BC_GUARD_END(cx)
  return {};
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

bc_view bc_scan_row(bc_ctx* cx, bc_cursor h) {
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  auto table = c->layout->table->id;
  if (c->layout->index == nullptr) return cx->make_view(c->value.data(), c->value.size(), h.i, c->step, table, RowKind::Stored);
  if (c->row_step == c->step) return cx->make_view(c->row.data(), c->row.size(), h.i, c->step, table, RowKind::Stored);
  BC_GUARD_BEGIN
  // The row key is the table's prefix and the entry's primary-key suffix,
  // found by skipping the index parts.
  KeyReader r{c->key, bytecask::sql::kKeyPrefixBytes};
  for (std::size_t i = 0; i < c->layout->index_parts; ++i) {
    if (!r.skip(c->layout->parts[i])) {
      cx->fail(BC_E_CORRUPT, "unparseable index key");
      return {};
    }
  }
  auto& key = c->row_key;
  key.clear();
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, table);
  auto pk = r.rest();
  key.insert(key.end(), pk.begin(), pk.end());
  if (!cx->charge()) return {};
  if (!cx->snap->get({}, key, c->row)) {
    // The runtime writes every entry with its row, so this is damage.
    cx->fail(BC_E_CORRUPT, "index " + c->layout->index->name + " has an entry whose row is missing");
    return {};
  }
  c->row_step = c->step;  // fetched once per entry, however often it is asked for
  return cx->make_view(c->row.data(), c->row.size(), h.i, c->step, table, RowKind::Stored);
  BC_GUARD_END(cx)
  return {};
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
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return true;
  return !bytecask::sql::row_present_bit(rc->row, col);
}

int64_t bc_col_i64(bc_ctx* cx, bc_view row, uint32_t col) {
  return load_column(cx, row, col, PhysType::I64, bytecask::sql::row_i64, std::int64_t{0});
}

double bc_col_f64(bc_ctx* cx, bc_view row, uint32_t col) {
  return load_column(cx, row, col, PhysType::F64, bytecask::sql::row_f64, 0.0);
}

bool bc_col_bool(bc_ctx* cx, bc_view row, uint32_t col) {
  return load_column(cx, row, col, PhysType::Bool, bytecask::sql::row_bool, false);
}

bc_view bc_col_bytes(bc_ctx* cx, bc_view row, uint32_t col) {
  auto rc = row_col(cx, row, col, PhysType::Bytes);
  if (!rc) return {};
  auto v = bytecask::sql::row_bytes(rc->row, rc->slot->pos);
  if (!v) {
    // The header parsed and a slot past nvar reads as empty, so this is a
    // var_end entry that runs past the row: the row is damaged.
    cx->fail(BC_E_CORRUPT, "malformed row");
    return {};
  }
  auto [cursor, step] = cx->owner(row);
  return cx->make_view(v->data(), v->size(), cursor, step);
}

bc_view bc_row_key(bc_ctx* cx, uint32_t table, bc_view row) {
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  BC_GUARD_BEGIN
  auto r = write_row(cx, *t, row, false);
  if (!r) return {};
  Bytes key;
  if (!encode_pk(cx, *t, *r, key)) return {};
  return cx->arena_view(key);
  BC_GUARD_END(cx)
  return {};
}

bc_row bc_row_begin(bc_ctx* cx, uint32_t table) {
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  BC_GUARD_BEGIN
  cx->rows.push_back(RowBuilder{t->id, &t->layout, RowWriter{t->layout}, false});
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

bc_row bc_row_from(bc_ctx* cx, uint32_t table, bc_view old) {
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  auto o = write_row(cx, *t, old, false);
  if (!o) return {};
  if (!bytecask::sql::row_valid(*o)) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return {};
  }
  auto r = bc_row_begin(cx, table);
  if (!cx->ok()) return {};
  BC_GUARD_BEGIN
  cx->rows[r.i].writer.copy_from(*o);
  BC_GUARD_END(cx)
  return r;
}

void bc_row_set_i64(bc_ctx* cx, bc_row r, uint32_t col, int64_t v) {
  set_column(cx, r, col, PhysType::I64, [&](RowWriter& w) { return w.set_i64(col, v); });
}

void bc_row_set_f64(bc_ctx* cx, bc_row r, uint32_t col, double v) {
  set_column(cx, r, col, PhysType::F64, [&](RowWriter& w) { return w.set_f64(col, v); });
}

void bc_row_set_bool(bc_ctx* cx, bc_row r, uint32_t col, bool v) {
  set_column(cx, r, col, PhysType::Bool, [&](RowWriter& w) { return w.set_bool(col, v); });
}

void bc_row_set_bytes(bc_ctx* cx, bc_row r, uint32_t col, bc_view v) {
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  set_column(cx, r, col, PhysType::Bytes, [&](RowWriter& w) { return w.set_bytes(col, *bytes); });
}

void bc_row_set_null(bc_ctx* cx, bc_row r, uint32_t col) {
  auto* b = set_target(cx, r, col, std::nullopt);
  if (b == nullptr || !null_allowed(cx, *b, col)) return;
  if (!b->writer.set_null(col)) cx->fail(BC_L_ROW, "column is not in the row's layout");
}

void bc_row_set_param(bc_ctx* cx, bc_row r, uint32_t col, uint32_t i) {
  const auto* d = cx->param(i);
  if (d == nullptr) return;
  auto* b = set_target(cx, r, col, std::nullopt);
  if (b == nullptr) return;
  const auto* slot = b->layout->find(col);
  if (!bytecask::sql::datum_fits(*d, slot->type)) {
    cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not " + type_name(slot->type));
    return;
  }
  if (std::holds_alternative<std::monostate>(*d) && !null_allowed(cx, *b, col)) return;
  BC_GUARD_BEGIN
  if (!bytecask::sql::set_datum(b->writer, col, *d)) cx->fail(BC_L_ROW, "column is not in the row's layout");
  BC_GUARD_END(cx)
}

bc_view bc_row_end(bc_ctx* cx, bc_row r) {
  auto* b = row_builder(cx, r);
  if (b == nullptr) return {};
  b->done = true;
  // A NOT NULL column the plan never set and that has no default.
  if (auto col = b->writer.missing_required()) {
    not_null(cx, b->table, *col);
    return {};
  }
  BC_GUARD_BEGIN
  Bytes row;
  if (!b->writer.finish(row)) {
    cx->fail(BC_E_LIMIT, "row over the engine's value limit");
    return {};
  }
  return cx->arena_view(row, b->table, RowKind::Built);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_pin(bc_ctx* cx, bc_view v) {
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return {};
  if (v.g == 0 && v.i == 0) return {};
  const auto& e = cx->views[v.i];
  return cx->arena_view(*b, e.table, e.kind);
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

// -------------------------------------------------------------------- writes

void bc_insert(bc_ctx* cx, uint32_t table, bc_view row) {
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  BC_GUARD_BEGIN
  auto r = write_row(cx, *t, row, false);
  if (!r) return;
  Bytes pk;
  if (!encode_pk(cx, *t, *r, pk) || !first_write(cx, *t, pk)) return;
  std::vector<Entry> entries(t->indexes.size());
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    if (!encode_entry(cx, *t, t->indexes[i], *r, entries[i])) return;
  }
  if (!pk_free(cx, *t, pk)) return;
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    // Nulls are distinct: an entry with a null part collides with nothing.
    if (t->indexes[i].unique && !entries[i].has_null && !unique_free(cx, t->indexes[i], entries[i].prefix())) return;
  }
  cx->push_effect(EffectKind::Put, pk, *r);
  for (const auto& e : entries) cx->push_effect(EffectKind::Put, e.key);
  BC_GUARD_END(cx)
}

void bc_update(bc_ctx* cx, uint32_t table, bc_view old, bc_view row) {
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  BC_GUARD_BEGIN
  auto o = write_row(cx, *t, old, true);
  auto n = write_row(cx, *t, row, false);
  if (!o || !n) return;
  // A key built only from columns whose encodings are the same in both rows
  // is the same key: it is encoded once, or not at all. A malformed row
  // counts as changed, and its encoding refuses it.
  auto same = [&](std::uint32_t col) {
    const auto* slot = t->layout.find(col);
    return slot != nullptr && bytecask::sql::row_column_equal(*o, *n, *slot).value_or(false);
  };
  Bytes pk_old, pk_new;
  if (!encode_pk(cx, *t, *o, pk_old)) return;
  auto moved_pk = !std::ranges::all_of(t->pk_col_ids, same);
  if (moved_pk) {
    if (!encode_pk(cx, *t, *n, pk_new)) return;
    moved_pk = pk_old != pk_new;
  }
  if (!first_write(cx, *t, pk_old) || (moved_pk && !first_write(cx, *t, pk_new))) return;
  // Only the entries whose keys changed move; with the primary key, all do.
  // An index none of whose columns changed is not encoded.
  struct Move {
    const Index* ix;
    Entry from, to;
  };
  std::vector<Move> moves;
  for (const auto& ix : t->indexes) {
    if (!moved_pk && std::ranges::all_of(ix.parts, [&](const auto& p) { return same(p.col_id); })) continue;
    Move m{&ix, {}, {}};
    if (!encode_entry(cx, *t, ix, *o, m.from) || !encode_entry(cx, *t, ix, *n, m.to)) return;
    if (m.from.key != m.to.key) moves.push_back(std::move(m));
  }
  if (moved_pk && !pk_free(cx, *t, pk_new)) return;
  for (const auto& m : moves) {
    // A row never collides with itself: checked only when the prefix changed.
    if (m.ix->unique && !m.to.has_null && !std::ranges::equal(m.from.prefix(), m.to.prefix()) &&
        !unique_free(cx, *m.ix, m.to.prefix())) {
      return;
    }
  }
  if (moved_pk) cx->push_effect(EffectKind::Del, pk_old);
  cx->push_effect(EffectKind::Put, moved_pk ? pk_new : pk_old, *n);
  for (const auto& m : moves) {
    cx->push_effect(EffectKind::Del, m.from.key);
    cx->push_effect(EffectKind::Put, m.to.key);
  }
  BC_GUARD_END(cx)
}

void bc_delete(bc_ctx* cx, uint32_t table, bc_view old) {
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  BC_GUARD_BEGIN
  auto o = write_row(cx, *t, old, true);
  if (!o) return;
  Bytes pk;
  if (!encode_pk(cx, *t, *o, pk) || !first_write(cx, *t, pk)) return;
  std::vector<Entry> entries(t->indexes.size());
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    if (!encode_entry(cx, *t, t->indexes[i], *o, entries[i])) return;
  }
  cx->push_effect(EffectKind::Del, pk);
  for (const auto& e : entries) cx->push_effect(EffectKind::Del, e.key);
  BC_GUARD_END(cx)
}

void bc_del_range(bc_ctx* cx, uint32_t table, bc_view lo, bc_view hi) {
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  BC_GUARD_BEGIN
  auto range = resolve_range(cx, table, lo, hi);
  if (!range) return;
  // Both bounds inside the table's rows: a bound of another object would
  // reach past them into the rest of the keyspace.
  auto end = bytecask::sql::key_successor(range->prefix);
  if (!starts_with(range->lo, range->prefix) || (!starts_with(range->hi, range->prefix) && range->hi != end)) {
    cx->fail(BC_E_HANDLE, "bc_del_range bound is not a key of table " + t->name);
    return;
  }
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return;  // an empty range deletes nothing
  // Its entries come from the snapshot's rows, so a row this plan already
  // wrote in the range would keep its new entries; refused, as a second write.
  // So would a range this plan already deleted, whose entries it would
  // delete twice.
  auto written = cx->written_rows.lower_bound(BytesView{range->lo});
  if ((written != cx->written_rows.end() && std::is_lt(bytes_cmp(*written, range->hi))) ||
      meets_deleted(cx, range->lo, BytesView{range->hi})) {
    cx->fail(BC_E_HANDLE, "bc_del_range over a row of table " + t->name + " this plan already wrote");
    return;
  }
  cx->deleted_ranges.emplace(range->lo, range->hi);
  if (!t->indexes.empty()) {
    // Each row in the range takes its entries with it. A row inserted into
    // the range after the snapshot makes the commit a conflict: the engine
    // checks a planned range delete's range for changes. Every read is
    // charged before it is made, the one that finds the range's end too:
    // begin() reads the first record, each ++ the next.
    if (!cx->charge()) return;
    auto rows = cx->snap->iter_from({}, range->lo);
    for (auto it = rows.begin(); it != std::default_sentinel;) {
      const auto& e = *it;
      if (!std::is_lt(bytes_cmp(e.key, range->hi))) break;
      if (!bytecask::sql::row_header_valid(e.value)) {
        cx->fail(BC_E_CORRUPT, "malformed row in table " + t->name);
        return;
      }
      if (auto gen = bytecask::sql::row_generation(e.value); gen != t->layout.generation) {
        fail_generation(cx, t, gen);
        return;
      }
      for (const auto& ix : t->indexes) {
        Entry en;
        if (!encode_entry(cx, *t, ix, e.value, en)) return;
        cx->push_effect(EffectKind::Del, en.key);
      }
      if (!cx->charge()) return;
      ++it;
    }
  }
  cx->push_effect(EffectKind::DelRange, range->lo, range->hi);
  BC_GUARD_END(cx)
}

// -------------------------------------------------------- guards and raise

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
  BC_GUARD_BEGIN
  cx->raise(constraint, object_id);
  BC_GUARD_END(cx)
}

}  // extern "C"
