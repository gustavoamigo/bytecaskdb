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
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <map>
#include <system_error>
#include <type_traits>
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
using bytecask::sql::ColumnSlot;
using bytecask::sql::ConstraintDetail;
using bytecask::sql::has_prefix;
using bytecask::sql::Datum;
using bytecask::sql::Index;
using bytecask::sql::KeyLayout;
using bytecask::sql::KeyPart;
using bytecask::sql::KeyReader;
using bytecask::sql::LiveCatalog;
using bytecask::sql::PhysType;
using bytecask::sql::RowLayout;
using bytecask::sql::RowWriter;
using bytecask::sql::SqlKind;
using bytecask::sql::SqlType;
using bytecask::sql::Table;

// bc_type's codes are PhysType's values; a schema ref's result_types are read as such.
static_assert(static_cast<int>(PhysType::I64) == BC_T_I64);
static_assert(static_cast<int>(PhysType::F64) == BC_T_F64);
static_assert(static_cast<int>(PhysType::Bool) == BC_T_BOOL);
static_assert(static_cast<int>(PhysType::Bytes) == BC_T_BYTES);

namespace {

constexpr std::uint32_t kNoCursor = UINT32_MAX;
// What a transaction's bound counts for what it holds: a conservative bound,
// never less than the memory used. Each heap block costs its capacity plus
// the allocator's header and rounding (kAllocBytes, an over-estimate); a map
// node its links and colour, its key and value objects, and its own block.
// So a transaction of many small writes reaches its bound too.
constexpr std::uint64_t kAllocBytes = 4 * sizeof(void*);
constexpr std::uint64_t kNodeBytes =
    4 * sizeof(void*) + sizeof(std::pair<const Bytes, std::optional<Bytes>>) + kAllocBytes;
[[nodiscard]] constexpr auto held(std::size_t capacity) -> std::uint64_t {
  return capacity == 0 ? 0 : capacity + kAllocBytes;
}
constexpr std::uint32_t kScanFlagMask = BC_SCAN_REVERSE | BC_SCAN_PREFIX;

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

// A declared column, where it lives in its table's current layout.
struct ColSlot {
  std::uint32_t table{0};
  std::uint32_t col{0};
  std::uint16_t generation{0};  // the layout's: the generation pos is for
  PhysType type{PhysType::I64};
  std::uint32_t pos{0};
  // The declared type a value written to it is checked against: a view into
  // the pinned catalog's layout. Valid while the run's pin is held:
  // drop_execution_state clears every ColSlot before it releases the pin.
  const SqlType* sql{nullptr};
};

// The columns of a written table's keys, as layout slots: what bc_update
// compares in its old and new rows. Built once per run and table, on the
// first update, so the write path searches a layout once, not per row.
struct KeySlots {
  std::uint32_t table;
  std::vector<ColumnSlot> pk;
  std::vector<std::vector<ColumnSlot>> parts;  // per index, in the table's index order
};

// The plan's schema reference, verified against the pinned catalog version
// by every bc_ctx_run, with what the verification built: the declared
// objects, written tables and columns, sorted for a binary search per
// access, and the result layout. One per context, its buffers kept across
// runs so a run allocates nothing for them; the result layout is rebuilt
// only when the result types change.
struct Verified {
  std::vector<std::uint32_t> objects;  // the tables and indexes it names
  std::vector<std::uint32_t> written;  // the tables it writes
  std::vector<ColSlot> cols;           // by (table, col)
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

// A transaction's writes so far, as its later statements read them
// (docs/transaction_design.md, *The transaction's view*): point entries,
// a key mapped to its value or to a tombstone, and the ranges bc_del_range
// deleted. A point entry is always newer than any range over it, since a
// range delete erases the points inside it as it is recorded. So a key's
// answer is its point entry, else absent if a range covers it, else the
// snapshot's.
struct Overlay {
  std::map<Bytes, std::optional<Bytes>, BytesLess> points;
  std::map<Bytes, Bytes, BytesLess> ranges;  // lo -> hi: disjoint, none empty
  std::uint64_t bytes{0};                    // a conservative bound on all it holds: keys, values and nodes

  [[nodiscard]] auto empty() const -> bool { return points.empty() && ranges.empty(); }

  // The deleted range holding `key`, or null.
  [[nodiscard]] auto range_at(BytesView key) const -> const std::pair<const Bytes, Bytes>* {
    auto it = ranges.upper_bound(key);
    if (it == ranges.begin()) return nullptr;
    --it;
    return std::ranges::lexicographical_compare(key, it->second) ? &*it : nullptr;
  }
  [[nodiscard]] auto covered(BytesView key) const -> bool { return range_at(key) != nullptr; }

  // Whether the overlay decides `key`: nullopt if the snapshot does; else
  // the value, or nullptr for a key the transaction deleted.
  [[nodiscard]] auto find(BytesView key) const -> std::optional<const Bytes*> {
    if (auto it = points.find(key); it != points.end()) {
      return it->second ? &*it->second : nullptr;
    }
    if (covered(key)) return nullptr;
    return std::nullopt;
  }

  // Whether [lo, hi) meets a point entry or a range.
  [[nodiscard]] auto meets(BytesView lo, BytesView hi) const -> bool {
    auto p = points.lower_bound(lo);
    if (p != points.end() && std::ranges::lexicographical_compare(p->first, hi)) return true;
    if (covered(lo)) return true;
    auto r = ranges.lower_bound(lo);
    return r != ranges.end() && std::ranges::lexicographical_compare(r->first, hi);
  }

  void put(Bytes key, Bytes value) { set(std::move(key), std::move(value)); }
  void del(Bytes key) { set(std::move(key), std::nullopt); }

  // Records a deleted range, merged with every range it meets, and drops
  // the points inside it.
  void del_range(Bytes lo, Bytes hi) {
    for (auto it = points.lower_bound(BytesView{lo}); it != points.end() && std::ranges::lexicographical_compare(it->first, hi);) {
      bytes -= cost(it->first, it->second ? held(it->second->capacity()) : 0);
      it = points.erase(it);
    }
    auto it = ranges.upper_bound(BytesView{lo});
    if (it != ranges.begin() && !std::ranges::lexicographical_compare(std::prev(it)->second, lo)) --it;
    while (it != ranges.end() && !std::ranges::lexicographical_compare(hi, it->first)) {
      if (std::ranges::lexicographical_compare(it->first, lo)) lo = it->first;
      if (std::ranges::lexicographical_compare(hi, it->second)) hi = it->second;
      bytes -= cost(it->first, held(it->second.capacity()));
      it = ranges.erase(it);
    }
    auto add = cost(lo, held(hi.capacity()));
    ranges.emplace(std::move(lo), std::move(hi));
    bytes += add;
  }

private:
  static auto cost(const Bytes& key, std::uint64_t value_bytes) -> std::uint64_t {
    return held(key.capacity()) + value_bytes + kNodeBytes;
  }

  void set(Bytes key, std::optional<Bytes> value) {
    auto value_bytes = value ? held(value->capacity()) : 0;
    if (auto it = points.find(BytesView{key}); it != points.end()) {
      bytes -= it->second ? held(it->second->capacity()) : 0;
      it->second = std::move(value);
      bytes += value_bytes;
      return;
    }
    auto add = cost(key, value_bytes);
    points.emplace(std::move(key), std::move(value));
    bytes += add;
  }
};

// The entries a read sees through the view in [lo, hi), in key order or its
// reverse: the snapshot's, with the overlay over them. On a key both hold the
// overlay wins, and its tombstone hides the snapshot's entry. The snapshot's
// entries inside a deleted range are passed over by seeking past the range,
// not read one by one. Each record read is charged as it is made, the one
// that finds the end of [lo, hi) too, and so is each overlay entry stepped
// onto, so a read the overlay answers costs what a record read does. key()
// and value() stay valid until the next call to next().
class ViewEntries {
public:
  ViewEntries(const bytecask::Snapshot& snap, const Overlay* overlay, BytesView lo, BytesView hi, bool reverse)
      : snap_{&snap},
        overlay_{overlay != nullptr && !overlay->empty() ? overlay : nullptr},
        lo_(lo.begin(), lo.end()),
        hi_(hi.begin(), hi.end()),
        reverse_{reverse} {}

  // The next entry; false at the end, or with the context's error set.
  [[nodiscard]] auto next(bc_ctx* cx) -> bool;
  [[nodiscard]] auto key() const -> BytesView { return key_; }
  [[nodiscard]] auto value() const -> BytesView { return value_; }

private:
  using Point = std::map<Bytes, std::optional<Bytes>, BytesLess>::const_iterator;

  [[nodiscard]] auto snap_step(bc_ctx* cx) -> bool;
  [[nodiscard]] auto snap_seek(bc_ctx* cx, BytesView at) -> bool;
  [[nodiscard]] auto point_step(bc_ctx* cx) -> bool;
  [[nodiscard]] auto snap_key() const -> BytesView { return reverse_ ? (**rev_).key : (**fwd_).key; }
  [[nodiscard]] auto snap_value() const -> BytesView { return reverse_ ? (**rev_).value : (**fwd_).value; }

  const bytecask::Snapshot* snap_;
  const Overlay* overlay_;  // null when there is none to merge
  Bytes lo_, hi_;
  bool reverse_;
  std::optional<bytecask::EntryIterator> fwd_;
  std::optional<bytecask::ReverseEntryIterator> rev_;
  Point point_{};
  bool started_{false};
  bool snap_live_{false}, point_live_{false};
  bool snap_pending_{false}, point_pending_{false};  // the entry handed out: step past it next time
  BytesView key_, value_;
};

struct Cursor {
  const KeyLayout* layout{nullptr};
  Bytes prefix;  // every key of this table or index starts with it
  Bytes lo, hi;  // hi empty: to the end of the prefix
  std::uint32_t flags{0};
  std::optional<ViewEntries> entries;  // from the first bc_scan_next
  bool done{false};
  bool positioned{false};
  std::uint64_t step{0};
  Bytes key, value;
  Bytes row;      // on an index: the current entry's row, once bc_scan_row fetched it
  Bytes row_key;  // the row's key, rebuilt in place: no allocation per entry
  std::uint64_t row_step{0};  // the step `row` was fetched at; 0: none
};



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

// A context leaves the transaction it was begun on; defined with bc_txn.
void txn_detach(bc_txn* txn, bc_ctx* cx) noexcept;

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
  std::optional<bytecask::Snapshot> snap;  // an autocommit execution's own
  // What every read goes through: the snapshot the execution reads (its own,
  // or its transaction's) and, inside a transaction, the overlay of its
  // earlier statements' writes, which the execution reads but never changes.
  const bytecask::Snapshot* base{nullptr};
  const Overlay* overlay{nullptr};  // null outside a transaction
  bc_txn* txn{nullptr};
#ifdef BYTECASK_SQL_TESTING
  std::function<void()> before_commit;  // bc_ctx_before_commit_for_testing
#endif

  bc_error_code error{BC_OK};
  std::uint32_t detail{0};
  std::string message;

  std::vector<ViewEntry> views;
  std::vector<Cursor> cursors;
  std::vector<KeyBuilder> keys;
  std::vector<RowBuilder> rows;
  std::vector<Effect> effects;
  // The row keys this execution's writes touched, and whether the row is
  // there after the write: a second write of one row would derive its index
  // entries from the snapshot's row again, not from the first write's, and
  // leave the first write's entries behind. Kept across the runs of one
  // execution, which share its snapshot and write plan; reset by
  // bc_ctx_begin.
  std::map<Bytes, bool, BytesLess> written_rows;
  // Where each part of the last key a key-part decoder read starts, so the
  // next part of the same key is found without skipping the earlier ones
  // again. Keyed by the view handle, unique within an execution.
  struct {
    bc_view view{};
    std::uint32_t object{0};
    std::vector<std::size_t> at;
  } key_parts;
  std::map<Bytes, Bytes, BytesLess> deleted_ranges;  // lo -> hi of each bc_del_range, likewise; disjoint

  Verified verified;                 // this context's reference, rebuilt by every run
  bool has_ref{false};  // a run verified `verified` against the pinned version
  std::vector<KeySlots> key_slots;    // of the tables this run updated, built on the first update

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
  // Never throws: describing the error can itself fail to allocate, and then
  // the error is BC_E_MEMORY with no message.
  void fail_current_exception() noexcept {
    try {
      fail_classified();
    } catch (...) {
      // An error already set keeps its message.
      if (error == BC_OK) {
        error = BC_E_MEMORY;
        message.clear();
      }
    }
  }

  void fail_classified() {
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
    } catch (...) {
      fail(BC_E_CORRUPT, "unknown exception");
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
  // a plan names, so the compatibility check covers each one. Outside a
  // run there is no reference, and nothing is declared.
  auto declared_object(std::uint32_t id, bool write) -> bool {
    if (!has_ref) {
      fail(BC_E_HANDLE, "no plan is running: bc_ctx_run sets the schema ref every object is checked against");
      return false;
    }
    const auto& list = write ? verified.written : verified.objects;
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
    if (t == nullptr) fail(BC_E_HANDLE, "no table " + std::to_string(id));
    return t;
  }

  void push_effect(EffectKind kind, BytesView a, BytesView b = {}) {
    effects.push_back({kind, Bytes(a.begin(), a.end()), Bytes(b.begin(), b.end())});
  }
  // For keys the runtime built and no longer needs: moved, not copied.
  void push_effect(EffectKind kind, Bytes&& a, BytesView b = {}) {
    effects.push_back({kind, std::move(a), Bytes(b.begin(), b.end())});
  }

  // Sets the constraint error: the kind in the detail's high byte, the
  // object id in its low 24 bits.
  void raise(std::uint32_t constraint, std::uint32_t object_id, std::string msg = {}) {
    if (!ok()) return;
    ConstraintDetail d{constraint, object_id};
    // A kind or id the detail cannot hold is a caller's bug, as in bc_raise:
    // BC_E_HANDLE, not a constraint and not corruption.
    if (!d.valid()) {
      fail(BC_E_HANDLE, "a constraint with an unknown kind or an object id past 2^24");
      return;
    }
    fail(BC_E_CONSTRAINT, msg.empty() ? d.message() : std::move(msg));
    detail = d.encode();
  }

  void drop_execution_state() {
    cursors.clear();  // iterators before the snapshot and the overlay they read
    if (txn != nullptr) txn_detach(txn, this);
    base = nullptr;
    overlay = nullptr;
    txn = nullptr;
    snap.reset();
    views.clear();
    keys.clear();  // key and row builders point into the pinned catalog: before it
    rows.clear();
    verified.cols.clear();  // each ColSlot's sql points into the pinned catalog: before it
    cat = &unpinned;
    pinned.reset();  // an idle context keeps no superseded catalog alive
    effects.clear();
    written_rows.clear();
    deleted_ranges.clear();
    has_ref = false;  // set by the next run's verification
    key_slots.clear();
    key_parts.view = {};  // view handles repeat across executions once the epoch wraps
    key_parts.at.clear();
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

  // For the one-line stubs, which hold no guard of their own.
  void unimplemented(const char* fn) noexcept {
    try {
      fail(BC_E_HANDLE, std::string{fn} + " is not implemented yet");
    } catch (...) {
      fail_current_exception();
    }
  }
};

// A transaction (docs/transaction_design.md): the snapshot and catalog
// version its statements read, pinned at its first statement; the log of
// every effect its statements made, in order; and the overlay that log
// builds, which later statements read through. Its statements run in
// contexts; bc_txn_add ends each one.
struct bc_txn {
  bytecask::DB* db;
  LiveCatalog* live;
  std::uint64_t max_bytes;

  std::shared_ptr<const Catalog> pinned;
  std::optional<bytecask::Snapshot> snap;  // engaged from the first statement to the commit
  // The writes: the commit is built from the overlay, its ranges first,
  // since every point in it is newer than any range over it.
  Overlay overlay;
  std::vector<Effect> guards;          // the statements' guards, in their order
  std::uint64_t guard_bytes{0};        // what the guards' keys hold
  std::vector<std::uint32_t> written;  // the tables the writes are in
  std::uint64_t statements{0};         // statements wholly added
  // A statement failed: nothing but bc_txn_free is left. Set before any
  // part of a failing statement could reach the overlay or the guards, so
  // an aborted transaction is never committed half-applied.
  bool aborted{false};
  bool done{false};  // committed, or its commit was attempted
  // The contexts begun on it and not yet reset: they point into its
  // snapshot and overlay. Re-pinning refuses while one is, and freeing it
  // detaches them.
  std::vector<bc_ctx*> attached;

  bc_txn(bytecask::DB* d, LiveCatalog* l, std::uint64_t max) : db{d}, live{l}, max_bytes{max} {}
};

void txn_detach(bc_txn* txn, bc_ctx* cx) noexcept { std::erase(txn->attached, cx); }

namespace {

// Aborts the transaction: a failed statement leaves only bc_txn_free, so
// what it holds goes now, the snapshot that holds vacuum back first. The
// contexts begun on it are detached; each keeps its error.
void txn_abort(bc_txn* txn) noexcept {
  txn->aborted = true;
  while (!txn->attached.empty()) txn->attached.back()->drop_execution_state();
  txn->snap.reset();
  txn->pinned.reset();
  txn->overlay = {};
  txn->guards = {};
  txn->guard_bytes = 0;
  txn->written = {};
}

}  // namespace

// The whole body of every extern "C" entry point a plan or the server calls
// is one guard: an exception, an allocation failure included, never crosses
// into a plan's C frames (TinyCC emits no unwind tables), and becomes the
// context's sticky error instead.
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

// Positions the snapshot's iterator on the first key >= at (forward) or the
// last key < at (reverse). A reverse seek lands on the last key <= at: a
// record at `at` itself is read, charged, and stepped past.
auto ViewEntries::snap_seek(bc_ctx* cx, BytesView at) -> bool {
  if (!reverse_) {
    fwd_.emplace(snap_->iter_from({}, at).begin());
    return true;
  }
  rev_.emplace(snap_->riter_from({}, at).begin());
  if (*rev_ != std::default_sentinel && std::is_eq(bytes_cmp((**rev_).key, at))) {
    if (!cx->charge()) return false;
    ++*rev_;
  }
  return true;
}

auto ViewEntries::snap_step(bc_ctx* cx) -> bool {
  if (!reverse_ ? !fwd_ : !rev_) {
    if (!snap_seek(cx, !reverse_ ? BytesView{lo_} : BytesView{hi_})) return false;
  } else if (!reverse_) {
    ++*fwd_;
  } else {
    ++*rev_;
  }
  for (;;) {
    snap_live_ = !reverse_ ? *fwd_ != std::default_sentinel : *rev_ != std::default_sentinel;
    if (!snap_live_) return true;
    if (!cx->charge()) return false;
    auto k = snap_key();
    if (!reverse_ ? !std::is_lt(bytes_cmp(k, hi_)) : std::is_lt(bytes_cmp(k, lo_))) {
      snap_live_ = false;
      return true;
    }
    const auto* range = overlay_ != nullptr ? overlay_->range_at(k) : nullptr;
    if (range == nullptr) return true;
    if (!snap_seek(cx, !reverse_ ? BytesView{range->second} : BytesView{range->first})) return false;
  }
}

auto ViewEntries::point_step(bc_ctx* cx) -> bool {
  const auto& points = overlay_->points;
  if (!started_) {
    if (!reverse_) {
      point_ = points.lower_bound(BytesView{lo_});
      point_live_ = point_ != points.end();
    } else {
      point_ = points.lower_bound(BytesView{hi_});
      point_live_ = point_ != points.begin();
      if (point_live_) --point_;
    }
  } else if (!reverse_) {
    ++point_;
    point_live_ = point_ != points.end();
  } else {
    point_live_ = point_ != points.begin();
    if (point_live_) --point_;
  }
  if (point_live_) {
    point_live_ = !reverse_ ? std::is_lt(bytes_cmp(point_->first, hi_)) : !std::is_lt(bytes_cmp(point_->first, lo_));
  }
  return !point_live_ || cx->charge();
}

auto ViewEntries::next(bc_ctx* cx) -> bool {
  if (!cx->ok()) return false;
  if (!started_) {
    if (!snap_step(cx) || (overlay_ != nullptr && !point_step(cx))) return false;
    started_ = true;
  } else {
    if (snap_pending_ && !snap_step(cx)) return false;
    if (point_pending_ && !point_step(cx)) return false;
  }
  snap_pending_ = point_pending_ = false;
  for (;;) {
    if (point_live_) {
      auto tie = false;
      auto point_first = !snap_live_;
      if (snap_live_) {
        auto c = bytes_cmp(point_->first, snap_key());
        tie = std::is_eq(c);
        point_first = reverse_ ? std::is_gt(c) : std::is_lt(c);
      }
      if (tie || point_first) {
        // The overlay decides this key; on a tie the snapshot's entry is
        // passed over.
        if (tie && !snap_step(cx)) return false;
        if (point_->second) {
          key_ = point_->first;
          value_ = *point_->second;
          point_pending_ = true;
          return true;
        }
        if (!point_step(cx)) return false;
        continue;
      }
    }
    if (!snap_live_) return false;
    key_ = snap_key();
    value_ = snap_value();
    snap_pending_ = true;
    return true;
  }
}

// Reads `key` through the view into `out`: the overlay's answer if it has
// one, else the snapshot's. Charged by the caller. Throws what the engine
// throws: an error is never taken as absent.
auto view_get(const bc_ctx* cx, BytesView key, Bytes& out) -> bool {
  if (cx->overlay != nullptr) {
    if (auto hit = cx->overlay->find(key)) {
      if (*hit == nullptr) return false;
      out.assign((*hit)->begin(), (*hit)->end());
      return true;
    }
  }
  return cx->base->get({}, key, out);
}

auto view_contains(const bc_ctx* cx, BytesView key) -> bool {
  if (cx->overlay != nullptr) {
    if (auto hit = cx->overlay->find(key)) return *hit != nullptr;
  }
  return cx->base->contains_key({}, key);
}

// The live keys in [lo, hi) the view holds, counted no further than
// `limit`, when the overlay meets the range: the snapshot's count, less the
// snapshot's keys in each deleted range, then corrected by one existence
// check per overlay point in the range. Each count is at most two record
// reads and each check one, all charged; the caller charged the first.
auto view_count(bc_ctx* cx, BytesView lo, BytesView hi, std::size_t limit) -> std::size_t {
  constexpr auto kAll = std::numeric_limits<std::size_t>::max();
  const auto& ov = *cx->overlay;
  auto n = cx->base->count_keys(lo, hi, kAll);
  auto r = ov.ranges.upper_bound(lo);
  if (r != ov.ranges.begin() && std::is_lt(bytes_cmp(lo, std::prev(r)->second))) --r;
  for (; r != ov.ranges.end() && std::is_lt(bytes_cmp(r->first, hi)); ++r) {
    auto a = std::is_lt(bytes_cmp(r->first, lo)) ? lo : BytesView{r->first};
    auto b = std::is_lt(bytes_cmp(hi, r->second)) ? hi : BytesView{r->second};
    if (!std::is_lt(bytes_cmp(a, b))) continue;
    if (!cx->charge(2)) return 0;
    n -= cx->base->count_keys(a, b, kAll);
  }
  for (auto p = ov.points.lower_bound(lo); p != ov.points.end() && std::is_lt(bytes_cmp(p->first, hi)); ++p) {
    if (!cx->charge()) return 0;
    auto in_view = p->second.has_value();
    auto in_snapshot = !ov.covered(p->first) && cx->base->contains_key({}, p->first);
    if (in_view && !in_snapshot) ++n;
    if (!in_view && in_snapshot) --n;
  }
  return std::min(n, limit);
}

// Positions a cursor on its next entry within bounds; false at the end.
// Every entry is read through the view, and charged as it is read.
auto cursor_advance(bc_ctx* cx, Cursor& c) -> bool {
  if (!c.entries) {
    // The scan's bounds, inside its table's or index's keys.
    auto end = bytecask::sql::key_successor(c.prefix);
    BytesView lo = std::is_lt(bytes_cmp(c.lo, c.prefix)) ? BytesView{c.prefix} : BytesView{c.lo};
    BytesView hi = !c.hi.empty() && std::is_lt(bytes_cmp(c.hi, end)) ? BytesView{c.hi} : BytesView{end};
    c.entries.emplace(*cx->base, cx->overlay, lo, hi, (c.flags & BC_SCAN_REVERSE) != 0);
  }
  if (!c.entries->next(cx)) return false;
  auto k = c.entries->key();
  auto v = c.entries->value();
  c.key.assign(k.begin(), k.end());
  c.value.assign(v.begin(), v.end());
  return true;
}

// Reads one record through the view into the arena. A record at a row key
// is a stored row of that key's table: in a transaction, the row as the
// transaction sees it.
auto read_record(bc_ctx* cx, BytesView key, bool* found) -> bc_view {
  if (found != nullptr) *found = false;
  if (!cx->charge()) return {};
  Bytes out;
  if (!view_get(cx, key, out)) return {};
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
[[nodiscard]] auto put_param_part(bc_ctx* cx, Bytes& buf, const KeyLayout& kl, std::size_t p, std::uint32_t i)
    -> bool {
  auto part = kl.parts[p];
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
auto written_tables(const Catalog& cat, const std::vector<Effect>& effects) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  auto note = [&](BytesView key) {
    auto obj = key_object(key);
    if (!obj || (obj->first != bytecask::sql::kNsRow && obj->first != bytecask::sql::kNsIndex)) return;
    const auto* kl = cat.key_layout(obj->second);
    if (kl == nullptr) return;
    if (std::ranges::find(out, kl->table->id) == out.end()) out.push_back(kl->table->id);
  };
  for (const auto& e : effects) {
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

// Submits a commit's batch. A failure that leaves the engine degraded, when
// it was not before, may have come after the batch reached the data file:
// its outcome is unknown (BC_E_UNKNOWN), never "nothing written". Any other
// failure wrote nothing.
auto submit(bc_ctx* cx, bytecask::DB& db, bytecask::WritePlan plan, bool sync) -> bc_commit_result {
  auto was_degraded = db.is_degraded();
  try {
    return db.apply_batch({.sync = sync}, std::move(plan)) ? BC_COMMITTED : BC_CONFLICT;
  } catch (...) {
    cx->fail_current_exception();
    if (!was_degraded && db.is_degraded()) cx->error = BC_E_UNKNOWN;
    return BC_FAILED;
  }
}

// A reference's arrays, empty where a pointer is null.
struct RefArrays {
  std::span<const std::uint32_t> read, written;
  std::span<const bc_col_ref> cols;
  std::span<const std::uint8_t> result_types;
};

auto ref_arrays(const bc_schema_ref& r) -> RefArrays {
  auto span = [](const auto* p, std::uint32_t n) { return std::span{p, p == nullptr ? 0 : n}; };
  return {span(r.objects_read, r.n_read), span(r.tables_written, r.n_written), span(r.cols, r.n_cols),
          span(r.result_types, r.n_result)};
}

auto key_slots_of(const Table& t) -> KeySlots {
  KeySlots k{t.id, {}, {}};
  for (auto id : t.pk_col_ids) k.pk.push_back(*t.layout.find(id));
  for (const auto& ix : t.indexes) {
    auto& parts = k.parts.emplace_back();
    for (const auto& p : ix.parts) parts.push_back(*t.layout.find(p.col_id));
  }
  return k;
}

// The key slots of table `t`, which the run updates: built on its first
// update in the run, then found. A run updates few tables.
auto key_slots(bc_ctx* cx, const Table& t) -> const KeySlots& {
  auto it = std::ranges::find(cx->key_slots, t.id, &KeySlots::table);
  if (it != cx->key_slots.end()) return *it;
  return cx->key_slots.emplace_back(key_slots_of(t));
}

// Builds `e` for `ref` on the pinned version: the compatibility check
// (docs/catalog_design.md, *Compatibility*), the result layout
// (rebuilt only when the codes change), the column map, in one walk of the
// reference: each column is looked up once, checked and mapped.
[[nodiscard]] auto verify(bc_ctx* cx, const bc_schema_ref* ref, Verified& e) -> bool {
  const auto& cat = *cx->cat;
  auto incompatible = [&](const std::string& why) {
    cx->fail(BC_L_INCOMPATIBLE, "plan for catalog v" + std::to_string(ref->catalog_version) +
                                    " is incompatible with v" + std::to_string(cat.version()) + ": " + why);
    return false;
  };
  auto [read, written, cols, codes] = ref_arrays(*ref);
  for (auto id : read) {
    if (cat.key_layout(id) == nullptr) return incompatible("table or index " + std::to_string(id) + " the plan reads no longer exists");
  }
  // An index where a table is required is a malformed reference, which no
  // recompile cures; a table that is gone is a stale one.
  auto is_index = [&](std::uint32_t id) { return cat.table(id) == nullptr && cat.key_layout(id) != nullptr; };
  auto not_a_table = [&](std::uint32_t id) {
    cx->fail(BC_L_PLAN, "object " + std::to_string(id) + " is an index where the plan's schema ref needs a table");
    return false;
  };
  for (auto id : written) {
    if (is_index(id)) return not_a_table(id);
    if (cat.table(id) == nullptr) return incompatible("table " + std::to_string(id) + " the plan writes no longer exists");
  }
  if (!e.result_layout || !std::ranges::equal(codes, e.result_codes)) {
    // Built aside and committed by moves, which cannot throw: a failure
    // leaves the entry's result layout, codes and types as they were.
    std::vector<PhysType> types;
    for (auto code : codes) {
      auto t = phys_type(code);
      if (!t) {
        cx->fail(BC_L_PLAN, "unknown result type code " + std::to_string(code));
        return false;
      }
      types.push_back(*t);
    }
    std::optional<RowLayout> layout;
    try {
      layout.emplace(bytecask::sql::make_result_layout(types));
    } catch (const std::length_error&) {
      cx->fail(BC_L_PLAN, "the plan's result row has too many columns for one row layout");
      return false;
    }
    std::vector<std::uint8_t> new_codes(codes.begin(), codes.end());
    static_assert(std::is_nothrow_move_assignable_v<std::optional<RowLayout>>);
    e.result_layout = std::move(layout);
    e.result_codes = std::move(new_codes);
    e.result_types = std::move(types);
  }
  auto sorted = [](std::vector<std::uint32_t>& v) {
    std::ranges::sort(v);
    v.erase(std::ranges::unique(v).begin(), v.end());
  };
  e.written.assign(written.begin(), written.end());
  sorted(e.written);
  e.objects.assign(read.begin(), read.end());
  e.objects.insert(e.objects.end(), e.written.begin(), e.written.end());
  sorted(e.objects);
  e.cols.clear();
  for (const auto& c : cols) {
    // A column's table is one the reference names, so every row a plan
    // reads or writes is of an object the compatibility check covers.
    if (!std::ranges::binary_search(e.objects, c.table)) {
      cx->fail(BC_L_PLAN, "column " + std::to_string(c.col) + " of table " + std::to_string(c.table) +
                              ", which the plan's schema ref does not name");
      return false;
    }
    if (is_index(c.table)) return not_a_table(c.table);
    const auto* t = cat.table(c.table);
    if (t == nullptr) return incompatible("table " + std::to_string(c.table) + " no longer exists");
    const auto* slot = t->layout.find(c.col);
    if (slot == nullptr || static_cast<std::uint8_t>(slot->type) != c.type) {
      return incompatible("column " + std::to_string(c.col) + " of table " + t->name +
                          " no longer exists with the type the plan was compiled with");
    }
    e.cols.push_back({c.table, c.col, t->layout.generation, slot->type, slot->pos, &slot->sql});
  }
  std::ranges::sort(e.cols, {}, [](const ColSlot& s) { return std::pair{s.table, s.col}; });
  return true;
}

// Makes `ref` the execution's reference, verified against the pinned
// catalog version.
[[nodiscard]] auto use_ref(bc_ctx* cx, const bc_schema_ref* ref) -> bool {
  if (ref == nullptr) {
    cx->fail(BC_E_HANDLE, "a plan runs against a bc_schema_ref: none was given");
    return false;
  }
  if (!verify(cx, ref, cx->verified)) return false;
  cx->has_ref = true;
  return true;
}

// Where declared column `col` of `table` is; nullptr and BC_E_HANDLE for a
// column the reference does not declare or not of `type`.
auto declared(bc_ctx* cx, std::uint32_t table, std::uint32_t col, std::optional<PhysType> type) -> const ColSlot* {
  if (!cx->ok()) return nullptr;
  if (cx->has_ref) {
    const auto& cols = cx->verified.cols;
    auto it = std::ranges::lower_bound(cols, std::pair{table, col}, {},
                                       [](const ColSlot& s) { return std::pair{s.table, s.col}; });
    if (it != cols.end() && it->table == table && it->col == col) {
      if (type && it->type != *type) {
        cx->fail(BC_E_HANDLE, "column " + std::to_string(col) + " of table " + std::to_string(table) + " is not " +
                                  type_name(*type));
        return nullptr;
      }
      return &*it;
    }
  }
  cx->fail(BC_E_HANDLE, "column " + std::to_string(col) + " of table " + std::to_string(table) +
                            " is not declared in the plan's schema ref");
  return nullptr;
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

// Whether `row`, a row of table `t` a write takes, can be written as one:
// its header parses (BC_E_CORRUPT otherwise) and it is of t's current
// layout generation (fail_generation otherwise). The one check of a row's
// layout on the write path, for row views and bc_del_range's stored rows.
[[nodiscard]] auto current_row(bc_ctx* cx, const Table& t, BytesView row) -> bool {
  if (!bytecask::sql::row_header_valid(row)) {
    cx->fail(BC_E_CORRUPT, "malformed row in table " + t.name);
    return false;
  }
  if (auto gen = bytecask::sql::row_generation(row); gen != t.layout.generation) {
    fail_generation(cx, &t, gen);
    return false;
  }
  return true;
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
  const auto* slot = declared(cx, r->table, col, type);
  if (slot == nullptr) return std::nullopt;
  auto gen = bytecask::sql::row_generation(r->bytes);
  if (gen != slot->generation) {
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
  if (!current_row(cx, t, r->bytes)) return std::nullopt;
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

// Records a row key this statement writes, and whether the row is there
// after the write (`present`). One statement affects a row at most once, so
// a second write of it, or a write into a range it deleted, is a wrong plan
// (BC_E_HANDLE), with one exception: an insert over a row the statement
// already put there is a duplicate key, as INSERT … VALUES (1, …), (1, …)
// is, and raises BC_C_UNIQUE. The keys and the deleted ranges are sorted:
// a lookup each per write.
[[nodiscard]] auto first_write(bc_ctx* cx, const Table& t, BytesView pk, bool present, bool insert = false) -> bool {
  if (auto it = cx->written_rows.find(pk); it != cx->written_rows.end()) {
    if (insert && it->second) {
      cx->raise(BC_C_UNIQUE, t.id);
    } else {
      cx->fail(BC_E_HANDLE, "a row of table " + t.name + " written twice in one statement");
    }
    return false;
  }
  if (meets_deleted(cx, pk)) {
    cx->fail(BC_E_HANDLE, "a row of table " + t.name + " written twice in one statement");
    return false;
  }
  cx->written_rows.emplace(Bytes(pk.begin(), pk.end()), present);
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

// The primary key is free in the view, or BC_C_UNIQUE on the table. Its
// race needs no guard: the key is in the write plan, and the engine checks
// every written key for a change since the snapshot.
[[nodiscard]] auto pk_free(bc_ctx* cx, const Table& t, BytesView pk) -> bool {
  if (!cx->charge()) return false;
  if (!view_contains(cx, pk)) return true;
  cx->raise(BC_C_UNIQUE, t.id);
  return false;
}

// No entry has this unique index prefix in the view, or BC_C_UNIQUE on
// the index; the range guard covers the race, since a duplicate is another
// row's entry, a key this plan does not write.
[[nodiscard]] auto unique_free(bc_ctx* cx, const Index& ix, BytesView prefix) -> bool {
  if (!cx->charge()) return false;
  auto taken = false;
  if (cx->overlay != nullptr && cx->overlay->meets(prefix, bytecask::sql::key_successor(prefix))) {
    // The overlay holds entries with this prefix, or deleted some: the
    // first entry the view holds at or after it decides, each read charged.
    ViewEntries entries{*cx->base, cx->overlay, prefix, bytecask::sql::key_successor(prefix), false};
    taken = entries.next(cx);
    if (!cx->ok()) return false;
  } else {
    auto keys = cx->base->keys_from({}, prefix);
    auto it = keys.begin();
    taken = it != std::default_sentinel && has_prefix(*it, prefix);
  }
  if (taken) {
    cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  cx->push_effect(EffectKind::GuardRange, prefix, bytecask::sql::key_successor(prefix));
  return true;
}


// Column `col` of a row view, read by row_column: nullopt, with the error
// set, for a bad handle or a malformed column (BC_E_CORRUPT). An absent
// column (NULL) reads with its word 0 and its bytes empty.
// Out of line: the message is built only on damage, off the read path.
[[gnu::cold, gnu::noinline]] void fail_malformed(bc_ctx* cx, std::uint32_t col) {
  cx->fail(BC_E_CORRUPT, "malformed row: column " + std::to_string(col) + " does not decode");
}

auto read_column(bc_ctx* cx, const RowCol& rc) -> std::optional<bytecask::sql::ColumnRead> {
  auto v = bytecask::sql::row_column(rc.row, rc.slot->col, rc.slot->type, rc.slot->pos);
  if (v.state == bytecask::sql::ColumnState::Malformed) {
    fail_malformed(cx, rc.slot->col);
    return std::nullopt;
  }
  return v;
}

// Loads a fixed column: its value, or the neutral value for NULL and on an error.
template <typename Get, typename T>
auto load_column(bc_ctx* cx, bc_view row, std::uint32_t col, PhysType type, Get&& get, T neutral) -> T {
  auto rc = row_col(cx, row, col, type);
  if (!rc) return neutral;
  auto v = read_column(cx, *rc);
  if (!v || v->state == bytecask::sql::ColumnState::Absent) return neutral;
  return get(*v);
}

// The builder a setter targets, and the declared column it sets.
struct SetTarget {
  RowBuilder* builder;
  const ColSlot* slot;
};

auto set_target(bc_ctx* cx, bc_row r, std::uint32_t col, std::optional<PhysType> type) -> std::optional<SetTarget> {
  auto* b = row_builder(cx, r);
  if (b == nullptr) return std::nullopt;
  const auto* slot = declared(cx, b->table, col, type);
  if (slot == nullptr) return std::nullopt;
  return SetTarget{b, slot};
}

// A value for column `col` of a row being built that is outside the
// column's declared type: refused with BC_E_CONSTRAINT, BC_C_RANGE and the
// table's id (the message names the column), and never set.
template <typename V>
[[nodiscard]] auto in_range(bc_ctx* cx, const SetTarget& at, std::uint32_t col, const V& v) -> bool {
  auto why = bytecask::sql::out_of_range(*at.slot->sql, v);
  if (!why) return true;
  const auto* t = cx->cat->table(at.builder->table);
  const auto* c = t == nullptr ? nullptr : t->column(col);
  cx->raise(BC_C_RANGE, at.builder->table,
            *why + " for column \"" + (c == nullptr ? std::to_string(col) : c->name) + "\" of table \"" +
                (t == nullptr ? std::to_string(at.builder->table) : t->name) + "\"");
  return false;
}

template <typename V, typename Set>
void set_column(bc_ctx* cx, bc_row r, std::uint32_t col, std::optional<PhysType> type, const V& v, Set&& set) {
  auto at = set_target(cx, r, col, type);
  if (!at || !in_range(cx, *at, col, v)) return;
  auto* b = at->builder;
  // The column is declared with this type and in the current layout: the
  // writer cannot refuse it.
  if (!set(b->writer)) cx->fail(BC_L_ROW, "column " + std::to_string(col) + " is not in the row's layout");
}
// A DOUBLE key part is canonicalised (-0.0 is 0.0, every NaN one NaN): the
// key does not hold the value the row does, so it is read from the row.
void refuse_f64_part(bc_ctx* cx) {
  cx->fail(BC_E_HANDLE, "a DOUBLE key part does not hold the column's value (-0.0 and NaN are canonicalised): read it "
                        "from the row");
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
  BC_GUARD_BEGIN
  cx->drop_execution_state();
  cx->error = BC_OK;
  cx->detail = 0;
  cx->message.clear();
  cx->params = params;
  cx->budget = budget;
  auto p = cx->live->pin();
  cx->pinned = std::move(p.cat);
  cx->cat = cx->pinned.get();
  cx->snap.emplace(std::move(p.snap));
  cx->base = &*cx->snap;
  BC_GUARD_END(cx)
}

void bc_ctx_begin_txn(bc_ctx* cx, bc_txn* txn, const bc_params* params, bc_budget budget) {
  BC_GUARD_BEGIN
  cx->drop_execution_state();
  // First, before anything that can fail: a statement that fails while it
  // begins must still abort its transaction at bc_txn_add.
  cx->txn = txn;
  txn->attached.push_back(cx);
  cx->error = BC_OK;
  cx->detail = 0;
  cx->message.clear();
  cx->params = params;
  cx->budget = budget;
  if (txn->aborted || txn->done) {
    cx->fail(BC_E_HANDLE, txn->done ? "the transaction has ended" : "the transaction is aborted");
    return;
  }
  // The snapshot and its catalog version are the first statement's: pinned
  // together, as an execution pins them.
  if (!txn->snap) {
    auto p = txn->live->pin();
    txn->pinned = std::move(p.cat);
    txn->snap.emplace(std::move(p.snap));
  }
  cx->pinned = txn->pinned;
  cx->cat = cx->pinned.get();
  cx->base = &*txn->snap;
  cx->overlay = &txn->overlay;
  BC_GUARD_END(cx)
}

bool bc_ctx_check(bc_ctx* cx, const bc_schema_ref* checked_against) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return false;
  if (cx->base == nullptr) {
    cx->fail(BC_E_HANDLE, "bc_ctx_check or bc_ctx_run without bc_ctx_begin");
    return false;
  }
  (void)use_ref(cx, checked_against);
  return cx->ok();
  BC_GUARD_END(cx)
  return {};
}

void bc_ctx_run(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* checked_against) {
  BC_GUARD_BEGIN
  cx->affected = -1;  // the count is the run's: a failed run reports none
  if (!bc_ctx_check(cx, checked_against)) return;
  plan(cx);
  if (cx->ok() && cx->emit) cx->fail(BC_L_PLAN, "result row begun and not ended");
  BC_GUARD_END(cx)
}

enum bc_commit_result bc_ctx_commit(bc_ctx* cx, bool sync) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return BC_FAILED;
  if (cx->txn != nullptr) {
    cx->fail(BC_E_HANDLE, "a statement of a transaction ends with bc_txn_add, not bc_ctx_commit");
    return BC_FAILED;
  }
  if (cx->effects.empty()) return BC_COMMITTED;
  if (!cx->snap) {
    // Committed already (the write plan took the snapshot), or never begun.
    cx->fail(BC_E_HANDLE, "bc_ctx_commit without a run to commit");
    return BC_FAILED;
  }
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
  for (auto id : written_tables(*cx->cat, cx->effects)) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
  cx->effects.clear();  // handed to the engine: a second commit has nothing to submit
#ifdef BYTECASK_SQL_TESTING
  if (cx->before_commit) cx->before_commit();
#endif
  return submit(cx, *cx->db, std::move(plan), sync);
  BC_GUARD_END(cx)
  return BC_FAILED;  // the guard caught a throw: never BC_COMMITTED
}

bool bc_ctx_next_result(bc_ctx* cx, const uint8_t** row, uint32_t* len) {
  BC_GUARD_BEGIN
  if (cx->result_pos + 4 > cx->results.size()) return false;
  std::uint32_t n = 0;
  std::memcpy(&n, cx->results.data() + cx->result_pos, 4);
  *row = as_u8(cx->results.data() + cx->result_pos + 4);
  *len = n;
  cx->result_pos += 4 + n;
  return true;
  BC_GUARD_END(cx)
  return {};
}

void bc_ctx_reset(bc_ctx* cx) { cx->drop_execution_state(); }

enum bc_error_code bc_error(const bc_ctx* cx) { return cx->error; }

const char* bc_error_message(const bc_ctx* cx) { return cx->message.c_str(); }

uint32_t bc_error_detail(const bc_ctx* cx) { return cx->detail; }

int64_t bc_ctx_affected(const bc_ctx* cx) { return cx->affected; }

}  // extern "C"

#ifdef BYTECASK_SQL_TESTING
void bc_ctx_before_commit_for_testing(bc_ctx* cx, std::function<void()> hook) { cx->before_commit = std::move(hook); }
#endif

extern "C" {

// ------------------------------------------------------------ transactions

bc_txn* bc_txn_new(bc_db* db, const bc_catalog* cat, uint64_t max_bytes) {
  try {
    return new bc_txn(db->db, cat->live, max_bytes);
  } catch (...) {
    return nullptr;
  }
}

void bc_txn_free(bc_txn* txn) {
  if (txn == nullptr) return;
  while (!txn->attached.empty()) txn->attached.back()->drop_execution_state();
  delete txn;
}

uint64_t bc_txn_catalog_version(const bc_txn* txn) { return txn->pinned ? txn->pinned->version() : 0; }

bool bc_txn_repin(bc_txn* txn) {
  if (txn->statements != 0 || txn->aborted || txn->done || !txn->attached.empty()) return false;
  txn->snap.reset();
  txn->pinned.reset();
  return true;
}

namespace {

// A conservative bound on what the transaction holds, and on what it would
// hold with the statement's effects in it, without merging them: a vector
// grows at most to twice what it holds, each effect adds at most one node,
// and an effect that replaces or erases others only frees.
auto txn_holds(const bc_txn& t, std::size_t more_guards, std::size_t more_written) -> std::uint64_t {
  auto vec = [](std::size_t cap, std::size_t size, std::size_t more, std::size_t elem) -> std::uint64_t {
    return held(std::max(cap, 2 * (size + more)) * elem);
  };
  return t.overlay.bytes + t.guard_bytes + vec(t.guards.capacity(), t.guards.size(), more_guards, sizeof(Effect)) +
         vec(t.written.capacity(), t.written.size(), more_written, sizeof(std::uint32_t));
}

}  // namespace

bool bc_txn_add(bc_ctx* cx) {
  bc_txn* txn = nullptr;
  BC_GUARD_BEGIN
  txn = cx->txn;
  if (txn == nullptr) {
    cx->fail(BC_E_HANDLE, "bc_txn_add on a context that is not running a statement of a transaction");
    return false;
  }
  // The statement's cursors read the overlay this changes.
  cx->cursors.clear();
  if (!cx->ok()) {
    txn_abort(txn);
    return false;
  }
  // The bound is checked before anything is merged, from the statement's
  // own effects: a statement that would pass it is refused without the
  // transaction allocating for it.
  std::uint64_t pending = 0;
  std::size_t guards = 0;
  for (const auto& e : cx->effects) {
    pending += held(e.a.capacity()) + held(e.b.capacity()) + kNodeBytes;
    switch (e.kind) {
      case EffectKind::Put:
      case EffectKind::Del:
      case EffectKind::DelRange: break;
      case EffectKind::GuardUnchanged:
      case EffectKind::GuardRange:
      case EffectKind::GuardPresent:
      case EffectKind::GuardAbsent: ++guards; break;
    }
  }
  auto tables = written_tables(*txn->pinned, cx->effects);
  if (txn_holds(*txn, guards, tables.size()) + pending > txn->max_bytes) {
    cx->fail(BC_E_LIMIT, "the transaction's writes would pass its bound of " + std::to_string(txn->max_bytes) +
                             " bytes");
    txn_abort(txn);
    return false;
  }
  // Aborted until the statement is wholly in: a failure part-way through
  // leaves an overlay no commit may use.
  txn->aborted = true;
  for (auto id : tables) {
    if (std::ranges::find(txn->written, id) == txn->written.end()) txn->written.push_back(id);
  }
  for (auto& e : cx->effects) {
    switch (e.kind) {
      case EffectKind::Put: txn->overlay.put(std::move(e.a), std::move(e.b)); break;
      case EffectKind::Del: txn->overlay.del(std::move(e.a)); break;
      case EffectKind::DelRange: txn->overlay.del_range(std::move(e.a), std::move(e.b)); break;
      case EffectKind::GuardUnchanged:
      case EffectKind::GuardRange:
      case EffectKind::GuardPresent:
      case EffectKind::GuardAbsent:
        txn->guard_bytes += held(e.a.capacity()) + held(e.b.capacity());
        txn->guards.push_back(std::move(e));
        break;
    }
  }
  cx->effects.clear();
  ++txn->statements;
  txn->aborted = false;
  return true;
  BC_GUARD_END(cx)
  if (txn != nullptr) txn_abort(txn);
  return false;
}

enum bc_commit_result bc_txn_commit(bc_ctx* cx, bc_txn* txn, bool sync) {
  BC_GUARD_BEGIN
  cx->drop_execution_state();  // the context holds nothing of a statement while the transaction commits
  cx->error = BC_OK;
  cx->detail = 0;
  cx->message.clear();
  if (txn->aborted || txn->done) {
    cx->fail(BC_E_HANDLE, txn->done ? "the transaction has ended" : "the transaction is aborted: roll it back");
    return BC_FAILED;
  }
  txn->done = true;
  const auto& ov = txn->overlay;
  if (ov.empty() && txn->guards.empty()) {
    txn->snap.reset();
    return BC_COMMITTED;
  }
  // The ranges before the points: a point is newer than any range over it,
  // and the engine applies a plan's operations in order.
  bytecask::WritePlan plan{std::move(*txn->snap)};
  txn->snap.reset();
  for (const auto& [lo, hi] : ov.ranges) plan.del_range(lo, hi);
  for (const auto& [key, value] : ov.points) {
    if (value) {
      plan.put(key, *value);
    } else {
      plan.del(key);
    }
  }
  for (const auto& e : txn->guards) {
    switch (e.kind) {
      case EffectKind::GuardUnchanged: plan.ensure_unchanged(e.a); break;
      case EffectKind::GuardRange: plan.ensure_range_unchanged(e.a, e.b); break;
      case EffectKind::GuardPresent: plan.ensure_present(e.a); break;
      case EffectKind::GuardAbsent: plan.ensure_absent(e.a); break;
      case EffectKind::Put:
      case EffectKind::Del:
      case EffectKind::DelRange: break;  // never kept as guards
    }
  }
  // As bc_ctx_commit: a DDL on a table the transaction wrote, committed
  // since its snapshot, makes the commit a conflict.
  for (auto id : txn->written) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
#ifdef BYTECASK_SQL_TESTING
  if (cx->before_commit) cx->before_commit();
#endif
  return submit(cx, *txn->db, std::move(plan), sync);
  BC_GUARD_END(cx)
  return BC_FAILED;  // the guard caught a throw: never BC_COMMITTED
}

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
  BC_GUARD_BEGIN
  const auto* v = cx->param_as<std::int64_t>(i, "an integer");
  return v == nullptr ? 0 : *v;
  BC_GUARD_END(cx)
  return {};
}

double bc_param_f64(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* v = cx->param_as<double>(i, "a double");
  return v == nullptr ? 0.0 : *v;
  BC_GUARD_END(cx)
  return {};
}

bool bc_param_bool(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* v = cx->param_as<bool>(i, "a boolean");
  return v != nullptr && *v;
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_param_bytes(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* v = cx->param_as<Bytes>(i, "bytes");
  if (v == nullptr) return {};
  return cx->make_view(v->data(), v->size());
  BC_GUARD_END(cx)
  return {};
}

bool bc_param_null(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* p = cx->param(i);
  return p != nullptr && std::holds_alternative<std::monostate>(*p);
  BC_GUARD_END(cx)
  return {};
}

void bc_emit_begin(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (!cx->has_ref || cx->verified.result_types.empty()) {
    cx->fail(BC_L_PLAN, "the plan's schema ref declares no result types");
    return;
  }
  if (cx->emit) {
    cx->fail(BC_L_PLAN, "result row already begun");
    return;
  }
  cx->emit.emplace(*cx->verified.result_layout);
  cx->emit_col = 0;
  BC_GUARD_END(cx)
}

// C++ linkage: overloads and a template, inside the extern "C" block.
extern "C++" {
namespace {
// The next result column, which must be of `type` (any, when nullopt).
auto emit_slot(bc_ctx* cx, std::optional<PhysType> type) -> bool {
  if (!cx->ok()) return false;
  if (!cx->emit || cx->emit_col >= cx->verified.result_types.size() ||
      (type && cx->verified.result_types[cx->emit_col] != *type)) {
    cx->fail(BC_L_PLAN, "emitted column does not match the result layout");
    return false;
  }
  return true;
}

void emit_bytes(bc_ctx* cx, BytesView b) { (void)cx->emit->set_bytes(cx->emit_col++, b); }

void emit_value(bc_ctx* cx, std::int64_t v) { (void)cx->emit->set_i64(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, double v) { (void)cx->emit->set_f64(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, bool v) { (void)cx->emit->set_bool(cx->emit_col++, v); }
void emit_value(bc_ctx* cx, BytesView v) { emit_bytes(cx, v); }

// The next result column from a decoded key part: BC_E_CORRUPT when the key
// did not parse, the value when there is one, NULL otherwise, whatever the
// type.
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
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::I64)) return;
  (void)cx->emit->set_i64(cx->emit_col++, v);
  BC_GUARD_END(cx)
}

void bc_emit_f64(bc_ctx* cx, double v) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::F64)) return;
  (void)cx->emit->set_f64(cx->emit_col++, v);
  BC_GUARD_END(cx)
}

void bc_emit_bool(bc_ctx* cx, bool v) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::Bool)) return;
  (void)cx->emit->set_bool(cx->emit_col++, v);
  BC_GUARD_END(cx)
}

void bc_emit_bytes(bc_ctx* cx, bc_view v) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::Bytes)) return;
  auto b = cx->resolve(v);
  if (!b) return;
  emit_bytes(cx, *b);
  BC_GUARD_END(cx)
}

void bc_emit_null(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, std::nullopt)) return;
  (void)cx->emit->set_null(cx->emit_col++);
  BC_GUARD_END(cx)
}

void bc_emit_col(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc || !emit_slot(cx, rc->slot->type)) return;
  auto v = read_column(cx, *rc);
  if (!v) return;
  if (v->state == bytecask::sql::ColumnState::Absent) {
    (void)cx->emit->set_null(cx->emit_col++);
    return;
  }
  switch (rc->slot->type) {
    case PhysType::I64: return emit_value(cx, v->i64());
    case PhysType::F64: return emit_value(cx, v->f64());
    case PhysType::Bool: return emit_value(cx, v->boolean());
    case PhysType::Bytes: return emit_value(cx, v->bytes);
  }
  BC_GUARD_END(cx)
}

void bc_emit_key_part(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto at = key_part_at(cx, table_or_index, key, part, std::nullopt);
  if (!at) return;
  if (at->part.type == PhysType::F64) {
    refuse_f64_part(cx);
    return;
  }
  if (!emit_slot(cx, at->part.type)) return;
  // A decoder's nullopt is a null part, or a malformed key when the reader failed.
  auto& r = at->reader;
  auto emit = [&](auto v) { emit_decoded(cx, v, !r.ok(), r.ok() && v.has_value(), "unparseable key"); };
  switch (at->part.type) {
    case PhysType::I64: return emit(r.read_i64(at->part));
    case PhysType::F64: return emit(r.read_f64(at->part));
    case PhysType::Bool: return emit(r.read_bool(at->part));
    case PhysType::Bytes: return emit(r.read_bytes(at->part));
  }
  BC_GUARD_END(cx)
}

void bc_emit_end(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (!cx->emit || cx->emit_col != cx->verified.result_types.size()) {
    cx->fail(BC_L_PLAN, "result row ended with the wrong number of columns");
    return;
  }
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
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (cx->affected >= 0) {
    cx->fail(BC_E_HANDLE, "bc_emit_count called twice");
    return;
  }
  cx->affected = affected;
  BC_GUARD_END(cx)
}

// ---------------------------------------------------------------- arithmetic

namespace {
auto arith_fail(bc_ctx* cx, const char* what) -> std::int64_t {
  cx->fail(BC_E_ARITH, what);
  return 0;
}
}  // namespace

int64_t bc_add(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_add_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_sub(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_sub_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_mul(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  return __builtin_mul_overflow(a, b, &r) ? arith_fail(cx, "integer out of range") : r;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_div(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  if (b == 0) return arith_fail(cx, "division by zero");
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return arith_fail(cx, "integer out of range");
  return a / b;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_mod(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  if (b == 0) return arith_fail(cx, "division by zero");
  if (b == -1) return 0;  // INT64_MIN % -1 is undefined in C; the remainder is 0
  return a % b;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_neg(bc_ctx* cx, int64_t a) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  if (a == std::numeric_limits<std::int64_t>::min()) return arith_fail(cx, "integer out of range");
  return -a;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_f64_to_i64(bc_ctx* cx, double v) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  // Truncation toward zero lands in i64 exactly when v is in [-2^63, 2^63);
  // both bounds are exact doubles.
  if (v != v) return arith_fail(cx, "cannot convert NaN to an integer");
  if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return arith_fail(cx, "double out of integer range");
  return static_cast<std::int64_t>(v);
  BC_GUARD_END(cx)
  return {};
}

double bc_i64_to_f64(int64_t v) { return static_cast<double>(v); }

bool bc_bytes_eq(bc_ctx* cx, bc_view a, bc_view b) {
  BC_GUARD_BEGIN
  auto v = cx->resolve2(a, b);
  return v && std::ranges::equal(v->first, v->second);
  BC_GUARD_END(cx)
  return {};
}

int bc_bytes_cmp(bc_ctx* cx, bc_view a, bc_view b) {
  BC_GUARD_BEGIN
  auto v = cx->resolve2(a, b);
  if (!v) return 0;
  auto o = bytes_cmp(v->first, v->second);
  return std::is_lt(o) ? -1 : std::is_gt(o) ? 1 : 0;
  BC_GUARD_END(cx)
  return {};
}

bool bc_bytes_prefix(bc_ctx* cx, bc_view prefix, bc_view b) {
  BC_GUARD_BEGIN
  auto v = cx->resolve2(prefix, b);
  return v && has_prefix(v->second, v->first);
  BC_GUARD_END(cx)
  return {};
}

// ---------------------------------------------------------------------- keys

bc_key bc_key_begin(bc_ctx* cx, uint32_t table_or_index) {
  BC_GUARD_BEGIN
  const auto* kl = cx->key_layout(table_or_index);
  if (kl == nullptr) return {};
  KeyBuilder b{kl, {}, 0, false};
  bytecask::sql::key_put_prefix(b.buf, kl->ns, table_or_index);
  cx->keys.push_back(std::move(b));
  return {static_cast<std::uint32_t>(cx->keys.size() - 1), cx->epoch};
  BC_GUARD_END(cx)
  return {};
}

void bc_key_i64(bc_ctx* cx, bc_key k, int64_t v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::I64, false);
  if (b != nullptr) bytecask::sql::key_put_i64(b->buf, v, current_part(*b));
  BC_GUARD_END(cx)
}

void bc_key_f64(bc_ctx* cx, bc_key k, double v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::F64, false);
  if (b != nullptr) bytecask::sql::key_put_f64(b->buf, v, current_part(*b));
  BC_GUARD_END(cx)
}

void bc_key_bool(bc_ctx* cx, bc_key k, bool v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::Bool, false);
  if (b != nullptr) bytecask::sql::key_put_bool(b->buf, v, current_part(*b));
  BC_GUARD_END(cx)
}

void bc_key_bytes(bc_ctx* cx, bc_key k, bc_view v) {
  BC_GUARD_BEGIN
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  auto* b = key_builder_part(cx, k, PhysType::Bytes, false);
  if (b != nullptr) bytecask::sql::key_put_bytes(b->buf, *bytes, current_part(*b));
  BC_GUARD_END(cx)
}

void bc_key_null(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, std::nullopt, true);
  if (b != nullptr) bytecask::sql::key_put_null(b->buf, current_part(*b));
  BC_GUARD_END(cx)
}

void bc_key_param(bc_ctx* cx, bc_key k, uint32_t i) {
  BC_GUARD_BEGIN
  if (cx->param(i) == nullptr) return;
  auto* b = key_builder_part(cx, k, std::nullopt, false);
  if (b == nullptr) return;
  (void)put_param_part(cx, b->buf, *b->layout, b->next_part - 1, i);
  BC_GUARD_END(cx)
}

bc_view bc_key_end(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr) return {};
  if (b->done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return {};
  }
  b->done = true;
  return key_view(cx, b->buf);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_key_params(bc_ctx* cx, uint32_t table_or_index, uint32_t first, uint32_t n) {
  BC_GUARD_BEGIN
  const auto* kl = cx->key_layout(table_or_index);
  if (kl == nullptr) return {};
  if (n > kl->parts.size()) {
    cx->fail(BC_E_HANDLE, "more key parts than the key has");
    return {};
  }
  Bytes key;
  bytecask::sql::key_put_prefix(key, kl->ns, table_or_index);
  for (std::uint32_t p = 0; p < n; ++p) {
    // first + p cannot wrap: a parameter index past the bound list fails first.
    if (first > UINT32_MAX - p || !put_param_part(cx, key, *kl, p, first + p)) {
      cx->fail(BC_E_HANDLE, "parameter index out of range");
      return {};
    }
  }
  return key_view(cx, key);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_key_successor(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  auto k = cx->resolve(key);
  if (!k) return {};
  return cx->arena_view(bytecask::sql::key_successor(*k));
  BC_GUARD_END(cx)
  return {};
}

// --------------------------------------------------------------------- reads

bc_view bc_get(bc_ctx* cx, bc_view key, bool* found) {
  BC_GUARD_BEGIN
  if (found != nullptr) *found = false;
  auto k = cx->resolve(key);
  if (!k || !cx->ok()) return {};
  return read_record(cx, *k, found);
  BC_GUARD_END(cx)
  return {};
}

bool bc_exists(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  auto k = cx->resolve(key);
  if (!k || !cx->charge()) return false;
  return view_contains(cx, *k);
  BC_GUARD_END(cx)
  return {};
}

bc_cursor bc_scan_open(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi, uint32_t flags) {
  BC_GUARD_BEGIN
  if ((flags & ~kScanFlagMask) != 0) {
    cx->fail(BC_E_HANDLE, "unknown scan flag");
    return {};
  }
  auto lo_b = cx->resolve(lo);
  auto hi_b = cx->resolve(hi);
  const auto* kl = cx->key_layout(table_or_index);
  if (!lo_b || !hi_b || kl == nullptr) return {};
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
  Cursor* c = nullptr;
  BC_GUARD_BEGIN
  c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr || c->done) return false;
  ++c->step;  // every view of the previous entry is now stale
  c->positioned = cursor_advance(cx, *c);
  if (!c->positioned) c->done = true;
  return c->positioned && cx->ok();
  BC_GUARD_END(cx)
  if (c != nullptr) c->done = true;  // a step that threw ends the scan
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
  BC_GUARD_BEGIN
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  return cx->make_view(c->key.data(), c->key.size(), h.i, c->step);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_scan_row(bc_ctx* cx, bc_cursor h) {
  BC_GUARD_BEGIN
  auto* c = positioned_cursor(cx, h);
  if (c == nullptr) return {};
  auto table = c->layout->table->id;
  if (c->layout->index == nullptr) return cx->make_view(c->value.data(), c->value.size(), h.i, c->step, table, RowKind::Stored);
  // An index cursor hands out its table's rows: the table must be declared too.
  if (!cx->declared_object(table, false)) return {};
  if (c->row_step == c->step) return cx->make_view(c->row.data(), c->row.size(), h.i, c->step, table, RowKind::Stored);
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
  if (!view_get(cx, key, c->row)) {
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
  BC_GUARD_BEGIN
  auto* c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr) return;
  ++c->step;
  c->entries.reset();
  c->done = true;
  c->positioned = false;
  BC_GUARD_END(cx)
}

bc_view bc_empty(bc_ctx*) { return {}; }

int64_t bc_key_part_i64(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::I64);
  if (!at) return 0;
  return decoded<std::int64_t>(cx, at->reader, at->reader.read_i64(at->part), 0);
  BC_GUARD_END(cx)
  return {};
}

double bc_key_part_f64(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  if (key_part_at(cx, table_or_index, key, part, PhysType::F64)) refuse_f64_part(cx);
  BC_GUARD_END(cx)
  return {};
}

bool bc_key_part_bool(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::Bool);
  if (!at) return false;
  return decoded<bool>(cx, at->reader, at->reader.read_bool(at->part), false);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_key_part_bytes(bc_ctx* cx, uint32_t table_or_index, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto at = key_part_at(cx, table_or_index, key, part, PhysType::Bytes);
  if (!at) return {};
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
  BC_GUARD_BEGIN
  auto at = key_part_at(cx, table_or_index, key, part, std::nullopt);
  if (!at) return true;
  if (!at->reader.skip(at->part)) {
    cx->fail(BC_E_CORRUPT, "unparseable key");
    return true;
  }
  return at->reader.last_was_null();
  BC_GUARD_END(cx)
  return true;
}

int64_t bc_count_range(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi, int64_t limit) {
  BC_GUARD_BEGIN
  auto range = resolve_range(cx, table_or_index, lo, hi);
  if (!range) return 0;
  if (limit < 0) {
    cx->fail(BC_E_HANDLE, "negative count limit");
    return 0;
  }
  if (!cx->charge(2)) return 0;  // the engine's bound: at most two record reads
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return 0;
  auto lim = static_cast<std::size_t>(limit);
  if (cx->overlay == nullptr || !cx->overlay->meets(range->lo, range->hi)) {
    return static_cast<std::int64_t>(cx->base->count_keys(range->lo, range->hi, lim));
  }
  return static_cast<std::int64_t>(view_count(cx, range->lo, range->hi, lim));
  BC_GUARD_END(cx)
  return {};
}

// ------------------------------------------------------------ rows, columns

bool bc_col_null(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return true;
  // The one decoder's verdict, damage included: the value is a view, not a copy.
  switch (bytecask::sql::row_column(rc->row, col, rc->slot->type, rc->slot->pos).state) {
    case bytecask::sql::ColumnState::Absent: return true;
    case bytecask::sql::ColumnState::Value: return false;
    case bytecask::sql::ColumnState::Malformed: fail_malformed(cx, col); return true;
  }
  return true;
  BC_GUARD_END(cx)
  return true;
}

int64_t bc_col_i64(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  return load_column(cx, row, col, PhysType::I64, [](const auto& v) { return v.i64(); }, std::int64_t{0});
  BC_GUARD_END(cx)
  return {};
}

double bc_col_f64(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  return load_column(cx, row, col, PhysType::F64, [](const auto& v) { return v.f64(); }, 0.0);
  BC_GUARD_END(cx)
  return {};
}

bool bc_col_bool(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  return load_column(cx, row, col, PhysType::Bool, [](const auto& v) { return v.boolean(); }, false);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_col_bytes(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, PhysType::Bytes);
  if (!rc) return {};
  auto v = read_column(cx, *rc);
  if (!v) return {};
  auto [cursor, step] = cx->owner(row);
  return cx->make_view(v->bytes.data(), v->bytes.size(), cursor, step);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_row_key(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  auto r = write_row(cx, *t, row, false);
  if (!r) return {};
  Bytes key;
  if (!encode_pk(cx, *t, *r, key)) return {};
  return cx->arena_view(key);
  BC_GUARD_END(cx)
  return {};
}

namespace {
// A builder of table t's current layout with every column null, pushed on
// the run's rows for the caller to fill. It is popped again unless the
// caller keeps it: a refusal, or an exception while filling it (bad_alloc
// in set_datum or copy_from), leaves no half-built row behind.
class PushedBuilder {
public:
  PushedBuilder(bc_ctx* cx, const Table& t) : cx_{cx}, i_{cx->rows.size()} {
    cx_->rows.push_back(RowBuilder{t.id, &t.layout, RowWriter{t.layout}, false});
  }
  PushedBuilder(const PushedBuilder&) = delete;
  auto operator=(const PushedBuilder&) -> PushedBuilder& = delete;
  ~PushedBuilder() {
    if (kept_) return;
    if (cx_->rows.size() == i_ + 1) {
      cx_->rows.pop_back();
      return;
    }
    // Filling the builder pushed a row after it: a runtime bug. No row that
    // is not the builder's is removed; the run is refused instead, with the
    // sticky code alone: a message could fail to allocate while unwinding.
    if (cx_->error == BC_OK) cx_->error = BC_E_HANDLE;
  }
  auto writer() -> RowWriter& { return cx_->rows[i_].writer; }
  auto keep() -> bc_row {
    kept_ = true;
    return {static_cast<std::uint32_t>(i_), cx_->epoch};
  }

private:
  bc_ctx* cx_;
  std::size_t i_;  // the builder's index in cx->rows
  bool kept_{false};
};
}  // namespace

bc_row bc_row_begin(bc_ctx* cx, uint32_t table) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  // Every column at its default, so a plan compiled before ADD COLUMN …
  // DEFAULT writes the default without naming the column.
  PushedBuilder b{cx, *t};
  for (const auto& c : t->columns) {
    if (!std::holds_alternative<std::monostate>(c.default_value) &&
        !bytecask::sql::set_datum(b.writer(), c.id, c.default_value)) {
      cx->fail(BC_E_CORRUPT, "default of column " + c.name + " does not fit its layout");
      return {};
    }
  }
  return b.keep();
  BC_GUARD_END(cx)
  return {};
}

bc_row bc_row_from(bc_ctx* cx, uint32_t table, bc_view old) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  auto o = write_row(cx, *t, old, false);
  if (!o) return {};
  if (!bytecask::sql::row_valid(*o)) {
    cx->fail(BC_E_CORRUPT, "malformed row");
    return {};
  }
  // No defaults: the copy sets every column of the layout, present or not.
  PushedBuilder b{cx, *t};
  if (!b.writer().copy_from(*o)) {
    cx->fail(BC_E_CORRUPT, "malformed row: a column does not decode");
    return {};
  }
  return b.keep();
  BC_GUARD_END(cx)
  return {};
}

void bc_row_set_i64(bc_ctx* cx, bc_row r, uint32_t col, int64_t v) {
  BC_GUARD_BEGIN
  set_column(cx, r, col, PhysType::I64, v, [&](RowWriter& w) { return w.set_i64(col, v); });
  BC_GUARD_END(cx)
}

void bc_row_set_f64(bc_ctx* cx, bc_row r, uint32_t col, double v) {
  BC_GUARD_BEGIN
  set_column(cx, r, col, PhysType::F64, v, [&](RowWriter& w) { return w.set_f64(col, v); });
  BC_GUARD_END(cx)
}

void bc_row_set_bool(bc_ctx* cx, bc_row r, uint32_t col, bool v) {
  BC_GUARD_BEGIN
  set_column(cx, r, col, PhysType::Bool, v, [&](RowWriter& w) { return w.set_bool(col, v); });
  BC_GUARD_END(cx)
}

void bc_row_set_bytes(bc_ctx* cx, bc_row r, uint32_t col, bc_view v) {
  BC_GUARD_BEGIN
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  set_column(cx, r, col, PhysType::Bytes, *bytes, [&](RowWriter& w) { return w.set_bytes(col, *bytes); });
  BC_GUARD_END(cx)
}

void bc_row_set_null(bc_ctx* cx, bc_row r, uint32_t col) {
  BC_GUARD_BEGIN
  // A NOT NULL column is refused at bc_row_end, after every value of the
  // row is checked against its declared type, as PostgreSQL checks.
  auto at = set_target(cx, r, col, std::nullopt);
  if (!at) return;
  if (!at->builder->writer.set_null(col)) cx->fail(BC_L_ROW, "column is not in the row's layout");
  BC_GUARD_END(cx)
}

void bc_row_set_param(bc_ctx* cx, bc_row r, uint32_t col, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* d = cx->param(i);
  if (d == nullptr) return;
  auto at = set_target(cx, r, col, std::nullopt);
  if (!at) return;
  if (!bytecask::sql::datum_fits(*d, at->slot->type)) {
    cx->fail(BC_E_HANDLE, "parameter $" + std::to_string(i + 1) + " is not " + type_name(at->slot->type));
    return;
  }
  if (!in_range(cx, *at, col, *d)) return;
  if (!bytecask::sql::set_datum(at->builder->writer, col, *d)) cx->fail(BC_L_ROW, "column is not in the row's layout");
  BC_GUARD_END(cx)
}

bc_view bc_row_end(bc_ctx* cx, bc_row r) {
  BC_GUARD_BEGIN
  auto* b = row_builder(cx, r);
  if (b == nullptr) return {};
  b->done = true;
  // A NOT NULL column set to NULL, or never set and with no default:
  // BC_E_CONSTRAINT, BC_C_NOT_NULL and the table's id (column ids are per
  // table; the message names the column). Checked last, after every value
  // the row was given passed its range check, so an out-of-range value is
  // BC_C_RANGE whatever else the row holds, as in PostgreSQL.
  if (auto col = b->writer.missing_required()) {
    not_null(cx, b->table, *col);
    return {};
  }
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
  BC_GUARD_BEGIN
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return {};
  if (v.g == 0 && v.i == 0) return {};
  const auto& e = cx->views[v.i];
  return cx->arena_view(*b, e.table, e.kind);
  BC_GUARD_END(cx)
  return {};
}

// --------------------------------------------------- ephemeral structures

bc_sorter bc_sorter_open(bc_ctx* cx, uint32_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_sorter_open");
  return {};
  BC_GUARD_END(cx)
  return {};
}
void bc_sorter_add(bc_ctx* cx, bc_sorter, bc_view, bc_view) { cx->unimplemented("bc_sorter_add"); }
int64_t bc_sorter_count(bc_ctx* cx, bc_sorter) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_sorter_count");
  return 0;
  BC_GUARD_END(cx)
  return {};
}
bc_view bc_sorter_nth(bc_ctx* cx, bc_sorter, int64_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_sorter_nth");
  return {};
  BC_GUARD_END(cx)
  return {};
}
bool bc_sorter_next(bc_ctx* cx, bc_sorter, bc_view*) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_sorter_next");
  return false;
  BC_GUARD_END(cx)
  return {};
}
bc_hash bc_hash_open(bc_ctx* cx, uint32_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_hash_open");
  return {};
  BC_GUARD_END(cx)
  return {};
}
void bc_hash_put(bc_ctx* cx, bc_hash, bc_view, bc_view) { cx->unimplemented("bc_hash_put"); }
bc_hash_iter bc_hash_find(bc_ctx* cx, bc_hash, bc_view) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_hash_find");
  return {};
  BC_GUARD_END(cx)
  return {};
}
bool bc_hash_next(bc_ctx* cx, bc_hash_iter, bc_view*) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_hash_next");
  return false;
  BC_GUARD_END(cx)
  return {};
}
bc_agg bc_agg_open(bc_ctx* cx, uint32_t, uint32_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_agg_open");
  return {};
  BC_GUARD_END(cx)
  return {};
}
bc_group bc_agg_group(bc_ctx* cx, bc_agg, bc_view) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_agg_group");
  return {};
  BC_GUARD_END(cx)
  return {};
}
void bc_agg_add_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_add_i64"); }
void bc_agg_add_f64(bc_ctx* cx, bc_group, uint32_t, double) { cx->unimplemented("bc_agg_add_f64"); }
void bc_agg_min_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_min_i64"); }
void bc_agg_max_i64(bc_ctx* cx, bc_group, uint32_t, int64_t) { cx->unimplemented("bc_agg_max_i64"); }
void bc_agg_count(bc_ctx* cx, bc_group, uint32_t) { cx->unimplemented("bc_agg_count"); }
bool bc_agg_next(bc_ctx* cx, bc_agg, bc_view*, bc_group*) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_agg_next");
  return false;
  BC_GUARD_END(cx)
  return {};
}
int64_t bc_agg_i64(bc_ctx* cx, bc_group, uint32_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_agg_i64");
  return 0;
  BC_GUARD_END(cx)
  return {};
}
double bc_agg_f64(bc_ctx* cx, bc_group, uint32_t) {
  BC_GUARD_BEGIN
  cx->unimplemented("bc_agg_f64");
  return 0;
  BC_GUARD_END(cx)
  return {};
}

// -------------------------------------------------------------------- writes

void bc_insert(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  auto r = write_row(cx, *t, row, false);
  if (!r) return;
  Bytes pk;
  if (!encode_pk(cx, *t, *r, pk) || !first_write(cx, *t, pk, true, true)) return;
  std::vector<Entry> entries(t->indexes.size());
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    if (!encode_entry(cx, *t, t->indexes[i], *r, entries[i])) return;
  }
  if (!pk_free(cx, *t, pk)) return;
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    // Nulls are distinct: an entry with a null part collides with nothing.
    if (t->indexes[i].unique && !entries[i].has_null && !unique_free(cx, t->indexes[i], entries[i].prefix())) return;
  }
  cx->push_effect(EffectKind::Put, std::move(pk), *r);
  for (auto& e : entries) cx->push_effect(EffectKind::Put, std::move(e.key));
  BC_GUARD_END(cx)
}

void bc_update(bc_ctx* cx, uint32_t table, bc_view old, bc_view row) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  auto o = write_row(cx, *t, old, true);
  auto n = write_row(cx, *t, row, false);
  if (!o || !n) return;
  // A key built only from columns whose encodings are the same in both rows
  // is the same key: it is encoded once, or not at all. A malformed row
  // counts as changed, and its encoding refuses it.
  const auto& keys = key_slots(cx, *t);
  auto same = [&](const ColumnSlot& slot) { return bytecask::sql::row_column_equal(*o, *n, slot).value_or(false); };
  Bytes pk_old, pk_new;
  if (!encode_pk(cx, *t, *o, pk_old)) return;
  auto moved_pk = !std::ranges::all_of(keys.pk, same);
  if (moved_pk) {
    if (!encode_pk(cx, *t, *n, pk_new)) return;
    moved_pk = pk_old != pk_new;
  }
  if (!first_write(cx, *t, pk_old, !moved_pk) || (moved_pk && !first_write(cx, *t, pk_new, true))) return;
  // Only the entries whose keys changed move; with the primary key, all do.
  // An index none of whose columns changed is not encoded.
  struct Move {
    const Index* ix;
    Entry from, to;
  };
  std::vector<Move> moves;
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    const auto& ix = t->indexes[i];
    if (!moved_pk && std::ranges::all_of(keys.parts[i], same)) continue;
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
  if (moved_pk) {
    cx->push_effect(EffectKind::Del, std::move(pk_old));
    cx->push_effect(EffectKind::Put, std::move(pk_new), *n);
  } else {
    cx->push_effect(EffectKind::Put, std::move(pk_old), *n);
  }
  for (auto& m : moves) {
    cx->push_effect(EffectKind::Del, std::move(m.from.key));
    cx->push_effect(EffectKind::Put, std::move(m.to.key));
  }
  BC_GUARD_END(cx)
}

void bc_delete(bc_ctx* cx, uint32_t table, bc_view old) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  auto o = write_row(cx, *t, old, true);
  if (!o) return;
  Bytes pk;
  if (!encode_pk(cx, *t, *o, pk) || !first_write(cx, *t, pk, false)) return;
  std::vector<Entry> entries(t->indexes.size());
  for (std::size_t i = 0; i < t->indexes.size(); ++i) {
    if (!encode_entry(cx, *t, t->indexes[i], *o, entries[i])) return;
  }
  cx->push_effect(EffectKind::Del, std::move(pk));
  for (auto& e : entries) cx->push_effect(EffectKind::Del, std::move(e.key));
  BC_GUARD_END(cx)
}

void bc_del_range(bc_ctx* cx, uint32_t table, bc_view lo, bc_view hi) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table, true);
  if (t == nullptr) return;
  auto range = resolve_range(cx, table, lo, hi);
  if (!range) return;
  // Both bounds inside the table's rows: a bound of another object would
  // reach past them into the rest of the keyspace.
  auto end = bytecask::sql::key_successor(range->prefix);
  if (!has_prefix(range->lo, range->prefix) || (!has_prefix(range->hi, range->prefix) && range->hi != end)) {
    cx->fail(BC_E_HANDLE, "bc_del_range bound is not a key of table " + t->name);
    return;
  }
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return;  // an empty range deletes nothing
  // Its entries come from the snapshot's rows, so a row this plan already
  // wrote in the range would keep its new entries; refused, as a second write.
  // So would a range this plan already deleted, whose entries it would
  // delete twice.
  auto written = cx->written_rows.lower_bound(BytesView{range->lo});
  if ((written != cx->written_rows.end() && std::is_lt(bytes_cmp(written->first, range->hi))) ||
      meets_deleted(cx, range->lo, BytesView{range->hi})) {
    cx->fail(BC_E_HANDLE, "bc_del_range over a row of table " + t->name + " this plan already wrote");
    return;
  }
  cx->deleted_ranges.emplace(range->lo, range->hi);
  if (!t->indexes.empty()) {
    // Each row in the range takes its entries with it. A row inserted into
    // the range after the snapshot makes the commit a conflict: the engine
    // checks a planned range delete's range for changes. Every record read
    // is charged, the one that finds the range's end too (ViewEntries).
    // The rows are the view's: in a transaction, an earlier statement's
    // row takes the entries that statement wrote.
    ViewEntries rows{*cx->base, cx->overlay, range->lo, range->hi, false};
    while (rows.next(cx)) {
      if (!current_row(cx, *t, rows.value())) return;
      for (const auto& ix : t->indexes) {
        Entry en;
        if (!encode_entry(cx, *t, ix, rows.value(), en)) return;
        cx->push_effect(EffectKind::Del, std::move(en.key));
      }
    }
    if (!cx->ok()) return;
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
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  // The detail holds the kind in 8 bits and the id in 24: anything wider would
  // name another constraint or object.
  if (!ConstraintDetail{constraint, object_id}.valid()) {
    cx->fail(BC_E_HANDLE, "bc_raise with an unknown constraint kind or an object id past 2^24");
    return;
  }
  cx->raise(constraint, object_id);
  BC_GUARD_END(cx)
}

}  // extern "C"
