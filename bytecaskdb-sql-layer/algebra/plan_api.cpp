// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// plan_api.cpp — the plan API of bc_plan.h on the engine's public API: the
// extern "C" definitions, and bc_ctx itself.
//
// Every call resolves its handles against tables owned by the context. A
// handle carries its entry's generation, drawn from a counter the context
// never resets, so a handle whose entry was released, by a loop's step or by
// bc_ctx_reset, is stale even once its slot is reused. A view owned by a
// cursor also records the cursor's step count, so stepping invalidates it; a
// view that is a row records its table. Errors are sticky: the first one is
// kept, and every later call returns its neutral value.
//
// A loop's step owns what the API allocated during it (docs/plan_api_design.md,
// *Memory*): the context marks its storage when a cursor or an aggregate
// moves to an entry, and rolls back to the mark when it moves again, ends or
// closes. Iterating a sorter, an aggregate's groups or a hash table probe is
// such a loop too. Pins, the contents of structures, the write set and the
// result rows are kept elsewhere, and outlive the step.
//
// The writes (bc_insert, bc_insert_on_conflict, bc_update, bc_delete,
// bc_del_range) derive every index entry from the row and the pinned
// catalog version, and run the unique checks; a plan never writes a key
// itself.

// Standard library headers before the module imports, for the reason
// bytecaskdb/bytecask_hpp.cpp gives (LLVM 22's abi_tag redeclaration error).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
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
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
#include "bytecaskdb-sql-layer/algebra/bc_proc.h"
#include "include/bytecask.hpp"

// bc_db, bc_catalog and bc_params, which bc_plan.h leaves opaque, and the
// catalog and codecs through it.
import bytecask.sql.runtime;
import bytecask.sql.text;
import bytecask.sql.jsonb;

using bytecask::sql::Bytes;
using bytecask::sql::BytesView;
using bytecask::sql::Catalog;
using bytecask::sql::ColumnSlot;
using bytecask::sql::ConstraintDetail;
using bytecask::sql::has_prefix;
using bytecask::sql::Datum;
using bytecask::sql::Index;
using bytecask::sql::kCursorEntryBytes;
using bytecask::sql::kAggBytes;
using bytecask::sql::kAggHeldBytes;
using bytecask::sql::kAggSlotBytes;
using bytecask::sql::kGroupEntryBytes;
using bytecask::sql::kHashBytes;
using bytecask::sql::kHashEntryBytes;
using bytecask::sql::kHashIterEntryBytes;
using bytecask::sql::kKeyEntryBytes;
using bytecask::sql::KeyLayout;
using bytecask::sql::KeyPart;
using bytecask::sql::KeyReader;
using bytecask::sql::LiveCatalog;
using bytecask::sql::kRowEntryBytes;
using bytecask::sql::kSortEntryBytes;
using bytecask::sql::kSorterBytes;
using bytecask::sql::kViewEntryBytes;
using bytecask::sql::buffer_bytes;
using bytecask::sql::claim_bytes;
using bytecask::sql::held_bytes;
using bytecask::sql::kAllocBytes;
using bytecask::sql::kClaimSlotBytes;
using bytecask::sql::kEffectSlotBytes;
using bytecask::sql::kLockSlotBytes;
using bytecask::sql::kOverlayNodeBytes;
using bytecask::sql::kPlanKeyGuardNodeBytes;
using bytecask::sql::kPlanRangeGuardSlotBytes;
using bytecask::sql::kPlanWriteSlotBytes;
using bytecask::sql::kRangeNodeBytes;
using bytecask::sql::kReadPointNodeBytes;
using bytecask::sql::kReadRangeNodeBytes;
using bytecask::sql::read_point_bytes;
using bytecask::sql::read_range_bytes;
using bytecask::sql::kTableSlotBytes;
using bytecask::sql::kUniqueNodeBytes;
using bytecask::sql::kWrittenRowNodeBytes;
using bytecask::sql::lock_bytes;
using bytecask::sql::PhysType;
using bytecask::sql::plan_entry_bytes;
using bytecask::sql::PlanEntry;
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
static_assert(static_cast<int>(PhysType::Num) == BC_T_NUM);
static_assert(static_cast<int>(PhysType::Jsonb) == BC_T_JSONB);
// A handle's generation never repeats within a context: one is drawn per
// handle, and at one a nanosecond 64 bits last five centuries. Step rollback
// reuses slots, so a counter that wrapped would let a stale handle resolve
// to another entry.
template <typename H>
constexpr bool kWideGeneration = std::is_same_v<decltype(H::g), std::uint64_t>;
static_assert(kWideGeneration<bc_view> && kWideGeneration<bc_cursor> && kWideGeneration<bc_sorter> &&
              kWideGeneration<bc_hash> && kWideGeneration<bc_hash_iter> && kWideGeneration<bc_agg> &&
              kWideGeneration<bc_group> && kWideGeneration<bc_key> && kWideGeneration<bc_row> &&
              kWideGeneration<bc_jsonb>);

namespace {

constexpr std::uint32_t kNoCursor = UINT32_MAX;
// A cursor or aggregate that is not in a loop's step.
constexpr std::uint32_t kNoStep = UINT32_MAX;
// A pinned view's handle: its index in the pinned views, with this bit set.
constexpr std::uint32_t kPinnedView = 1u << 31;
// A view of a parameter a subquery or bc_sub_arg_* set: its ViewEntry's
// cursor is the parameter's index with this bit, and its step the
// parameter's generation, so a rewrite of the parameter makes it stale.
constexpr std::uint32_t kParamOwner = 1u << 31;
// The arena grows in chunks of this size, or one chunk per copy larger than
// it.
constexpr std::size_t kArenaChunk = 64u << 10;
constexpr std::uint32_t kScanFlagMask = BC_SCAN_REVERSE | BC_SCAN_PREFIX;

// What a view is, beyond its bytes: a row read from the engine (a stored
// row, the only kind bc_update and bc_delete take as the old one), a row a
// builder made, or neither.
enum class RowKind : std::uint8_t { None, Stored, Built };

struct ViewEntry {
  const std::byte* data;
  std::uint64_t step;    // the cursor's step when the view was made
  std::uint64_t gen;     // the handle's generation
  std::uint32_t len;
  std::uint32_t cursor;  // kNoCursor: lives as long as its entry
  std::uint32_t table;   // the row's table; 0 unless kind is a row
  RowKind kind;
  // A jsonb node (a bc_jsonb's), never bytes: resolve() refuses it, and
  // resolve_jsonb() refuses every view that is not one.
  bool jsonb{false};
};

// The finished key's view reads buf, which a move of the builder keeps
// where it is: a key costs its bytes once, charged as its parts are added.
struct KeyBuilder {
  const KeyLayout* layout;
  Bytes buf;
  std::uint64_t gen;
  std::size_t next_part{0};
  bool done{false};
  // A part was given an integer its width does not hold (key_put_i64): no
  // key of the layout has it, so bc_key_fits is false and bc_key_end refuses it.
  bool unfit{false};
  std::uint64_t held{0};  // what buf is charged to the statement's memory budget
};

// A sort key (bc_sort_key_begin): parts the plan chooses, no layout. Its
// view reads buf, as a KeyBuilder's does, and it is charged the same way.
struct SortKeyBuilder {
  Bytes buf;
  std::uint64_t gen;
  bool done{false};
  std::uint64_t held{0};
};

struct RowBuilder {
  std::uint64_t gen;
  const RowLayout* layout;
  RowWriter writer;
  std::uint32_t table;
  bool done{false};
  // The finished row bc_row_end's view reads, and charges to the statement's
  // memory budget. Moving a builder keeps this buffer where it is.
  Bytes built;
  std::uint64_t held{0};  // what built is charged
};

// Copies in chunks that never move, charged by the caller. A mark is where
// the next copy goes: rolling back to one releases every copy made since.
// The last standard chunk released is kept as a spare, so a loop whose
// steps cross a chunk boundary does not allocate a chunk per step, and a
// statement that fits in one chunk allocates none after the first.
class Arena {
public:
  struct Mark {
    std::size_t chunks{0}, used{0};
    std::uint64_t bytes{0};
  };

  // Throws std::bad_alloc, with nothing changed: a push_back that cannot
  // grow leaves the spare it was given where it was.
  [[nodiscard]] auto copy(BytesView b) -> const std::byte* {
    auto* p = alloc(b.size());
    if (!b.empty()) std::memcpy(p, b.data(), b.size());
    return p;
  }

  // n bytes for the caller to fill, as copy() places them.
  [[nodiscard]] auto alloc(std::size_t n) -> std::byte* {
    if (chunks_.empty() || chunks_.back().size - used_ < n) {
      if (spare_.data != nullptr && n <= spare_.size) {
        chunks_.push_back(std::move(spare_));
        spare_ = {};
      } else {
        auto size = std::max(kArenaChunk, n);
        chunks_.push_back({std::make_unique_for_overwrite<std::byte[]>(size), size});
      }
      used_ = 0;
    }
    auto* p = chunks_.back().data.get() + used_;
    used_ += n;
    bytes_ += n;
    return p;
  }

  [[nodiscard]] auto mark() const -> Mark { return {chunks_.size(), used_, bytes_}; }

  // Releases every copy made since `m`, and returns the bytes they were.
  auto roll_back(Mark m) noexcept -> std::uint64_t {
    while (chunks_.size() > m.chunks) {
      if (chunks_.back().size == kArenaChunk) spare_ = std::move(chunks_.back());
      chunks_.pop_back();
    }
    used_ = m.used;
    auto released = bytes_ - m.bytes;
    bytes_ = m.bytes;
    return released;
  }

  // Releases every copy, and with `keep_spare` false the spare chunk too.
  void clear(bool keep_spare) noexcept {
    (void)roll_back({});
    if (!keep_spare) spare_ = {};
  }

private:
  struct Chunk {
    std::unique_ptr<std::byte[]> data;
    std::size_t size{0};
  };
  std::vector<Chunk> chunks_;
  Chunk spare_;
  std::size_t used_{0};     // of chunks_.back()
  std::uint64_t bytes_{0};  // the copies' bytes, as requested
};

// GuardUniqueRange is a unique check's range guard, over the checked value's
// prefix `a`. In a transaction it counts at commit only while the final
// writes still put an entry under that prefix (Overlay::puts_under), the
// rule the insert claims follow.
enum class EffectKind {
  Put,
  Del,
  DelRange,
  GuardUnchanged,
  GuardRange,
  GuardUniqueRange,
  GuardPresent,
  GuardAbsent
};

struct Effect {
  EffectKind kind;
  Bytes a, b;
};

[[nodiscard]] constexpr auto is_write(EffectKind k) -> bool {
  switch (k) {
    case EffectKind::Put:
    case EffectKind::Del:
    case EffectKind::DelRange: return true;
    case EffectKind::GuardUnchanged:
    case EffectKind::GuardRange:
    case EffectKind::GuardUniqueRange:
    case EffectKind::GuardPresent:
    case EffectKind::GuardAbsent: return false;
  }
  return false;
}

// The entry a commit's WritePlan takes for an effect of this kind.
[[nodiscard]] constexpr auto plan_entry(EffectKind k) -> PlanEntry {
  switch (k) {
    case EffectKind::Put:
    case EffectKind::Del:
    case EffectKind::DelRange: return PlanEntry::Write;
    case EffectKind::GuardUnchanged:
    case EffectKind::GuardPresent:
    case EffectKind::GuardAbsent: return PlanEntry::KeyGuard;
    case EffectKind::GuardRange:
    case EffectKind::GuardUniqueRange: return PlanEntry::RangeGuard;
  }
  return PlanEntry::Write;
}

// An insert claim (docs/transaction_design.md, *Commit and conflicts*): a
// primary key a write put into a key its snapshot held free, or a unique
// index prefix a write checked free there, with the table's or index's id.
// A commit that conflicts reports the first claim the latest committed
// state has taken as the duplicate it is, BC_C_UNIQUE, not a conflict.
struct Claim {
  std::uint32_t object;
  Bytes key;  // the primary key, or the index entry the write puts
  // An index entry's: taken by another row's entry with these first bytes,
  // never by this row's own entry, which a racing update of it also writes.
  std::optional<std::size_t> prefix_len;
  // How many of the transaction's locks came from statements up to and
  // including this claim's: one of them a concurrent commit changed is the
  // conflict PostgreSQL reports first, at the row, before the duplicate (an
  // UPDATE locks its old row before it checks the value it moves to).
  std::size_t locks_through{0};
};

// A row a statement locked (FOR UPDATE) or wrote (UPDATE, DELETE), as the
// transaction's snapshot held it: a fingerprint of its stored bytes, so a
// conflicting commit can tell whether a concurrent commit changed it.
struct Lock {
  Bytes key;
  std::uint64_t fingerprint;
};

// The write set's fixed charges cover what they stand for: a vector slot at
// twice its element, and a map or set node's links and colour, its value
// and its block.
template <typename V>
constexpr std::uint64_t kNodeBytesOf = 4 * sizeof(void*) + sizeof(V) + kAllocBytes;
static_assert(sizeof(void*) <= kAllocBytes);
static_assert(2 * sizeof(Effect) <= kEffectSlotBytes);
static_assert(2 * sizeof(Claim) <= kClaimSlotBytes);
static_assert(2 * sizeof(Lock) <= kLockSlotBytes);
static_assert(2 * sizeof(std::uint32_t) <= kTableSlotBytes);
static_assert(kNodeBytesOf<std::pair<const Bytes, std::optional<Bytes>>> <= kOverlayNodeBytes);
static_assert(kNodeBytesOf<std::pair<const Bytes, Bytes>> <= kOverlayNodeBytes);  // an overlay range
static_assert(kNodeBytesOf<std::pair<const Bytes, bool>> <= kWrittenRowNodeBytes);
static_assert(kNodeBytesOf<std::pair<std::uint32_t, Bytes>> <= kUniqueNodeBytes);
static_assert(kNodeBytesOf<std::pair<const Bytes, Bytes>> <= kRangeNodeBytes);
// The engine's WritePlan (bytecask.cppm) is behind the public header's
// pointer: these mirror its members' shapes, a variant of up to two keys
// per write, a guard map's one-enum value, and a range guard's two keys.
static_assert(2 * sizeof(std::variant<std::pair<Bytes, Bytes>, Bytes>) <= kPlanWriteSlotBytes);
static_assert(kNodeBytesOf<std::pair<const Bytes, std::uint32_t>> <= kPlanKeyGuardNodeBytes);
static_assert(2 * sizeof(std::pair<Bytes, Bytes>) <= kPlanRangeGuardSlotBytes);

[[nodiscard]] constexpr auto effect_bytes(bool in_txn, EffectKind k, std::size_t a, std::size_t b) -> std::uint64_t {
  return bytecask::sql::effect_bytes(in_txn, plan_entry(k), a, b);
}
[[nodiscard]] auto effect_bytes(bool in_txn, const Effect& e) -> std::uint64_t {
  return effect_bytes(in_txn, e.kind, e.a.capacity(), e.b.capacity());
}
// A table's charge, with the commit's guard on its record.
[[nodiscard]] auto table_bytes(std::uint32_t table) -> std::uint64_t {
  return bytecask::sql::table_bytes(bytecask::sql::table_record_key(table).size());
}

// Empties one of the write set's vectors at a reset: its storage is kept,
// uncharged, up to kKeptWriteSlots, and freed past it, so a pooled context
// keeps none of a large statement's slots.
template <typename T>
void clear_write_slots(std::vector<T>& v) noexcept {
  if (v.capacity() > bytecask::sql::kKeptWriteSlots) {
    std::vector<T>{}.swap(v);
  } else {
    v.clear();
  }
}

// FNV-1a: stable, and only ever compared within one process.
[[nodiscard]] auto row_fingerprint(BytesView row) -> std::uint64_t {
  std::uint64_t h = 14'695'981'039'346'656'037ULL;
  for (auto b : row) {
    h ^= std::to_integer<std::uint64_t>(b);
    h *= 1'099'511'628'211ULL;
  }
  return h;
}

// A declared column, where it lives in its table's current layout.
struct ColSlot {
  std::uint32_t table{0};
  std::uint32_t col{0};
  std::uint16_t generation{0};  // the layout's: the generation pos is for
  PhysType type{PhysType::I64};
  std::uint32_t pos{0};
  std::uint32_t width{0};  // a fixed column's bytes in the row; 0 for a variable one
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
  // A conservative bound on all it holds, keys, values and nodes, and on the
  // commit's WritePlan entry for each node.
  std::uint64_t bytes{0};

  [[nodiscard]] auto empty() const -> bool { return points.empty() && ranges.empty(); }

  // The deleted range holding `key`, or null.
  [[nodiscard]] auto range_at(BytesView key) const -> const std::pair<const Bytes, Bytes>* {
    auto it = ranges.upper_bound(key);
    if (it == ranges.begin()) return nullptr;
    --it;
    return std::ranges::lexicographical_compare(key, it->second) ? &*it : nullptr;
  }
  [[nodiscard]] auto covered(BytesView key) const -> bool { return range_at(key) != nullptr; }

  // Whether the overlay puts `key`.
  [[nodiscard]] auto puts(BytesView key) const -> bool {
    auto it = points.find(key);
    return it != points.end() && it->second;
  }

  // Whether the overlay puts a key that starts with `prefix`: the one test
  // that decides whether a unique value still counts at commit, for its
  // claims (conflict_or_duplicate) and its guards (bc_txn_commit). It looks
  // at the prefix, not the entry: an UPDATE that moves the row's primary key
  // rewrites the entry (prefix + primary key) without checking the value.
  // Relies on the entry-key layout, the index prefix first.
  [[nodiscard]] auto puts_under(BytesView prefix) const -> bool {
    for (auto it = points.lower_bound(prefix); it != points.end() && has_prefix(it->first, prefix); ++it) {
      if (it->second) return true;
    }
    return false;
  }

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
      bytes -= cost(it->first, it->second);
      it = points.erase(it);
    }
    auto it = ranges.upper_bound(BytesView{lo});
    if (it != ranges.begin() && !std::ranges::lexicographical_compare(std::prev(it)->second, lo)) --it;
    while (it != ranges.end() && !std::ranges::lexicographical_compare(hi, it->first)) {
      if (std::ranges::lexicographical_compare(it->first, lo)) lo = it->first;
      if (std::ranges::lexicographical_compare(hi, it->second)) hi = it->second;
      bytes -= cost(it->first, it->second);
      it = ranges.erase(it);
    }
    auto add = cost(lo, hi);
    ranges.emplace(std::move(lo), std::move(hi));
    bytes += add;
  }

private:
  // A node, its buffers, and its entry in the commit's WritePlan: no more
  // than the effect_bytes of the write it came from.
  static auto cost(const Bytes& key, std::size_t value_capacity) -> std::uint64_t {
    return held_bytes(key) + buffer_bytes(value_capacity) + kOverlayNodeBytes +
           plan_entry_bytes(PlanEntry::Write, key.capacity(), value_capacity);
  }
  static auto cost(const Bytes& key, const Bytes& value) -> std::uint64_t { return cost(key, value.capacity()); }
  static auto cost(const Bytes& key, const std::optional<Bytes>& value) -> std::uint64_t {
    return cost(key, value ? value->capacity() : 0);
  }

  void set(Bytes key, std::optional<Bytes> value) {
    if (auto it = points.find(BytesView{key}); it != points.end()) {
      bytes -= cost(it->first, it->second);
      it->second = std::move(value);
      bytes += cost(it->first, it->second);
      return;
    }
    auto add = cost(key, value);
    points.emplace(std::move(key), std::move(value));
    bytes += add;
  }
};

#ifdef BYTECASK_SQL_TESTING
// read_set_transitions_for_testing's counts: process-wide, constant-initialized.
std::array<std::atomic<std::uint64_t>, kReadSetTransitions> g_read_set_transitions{};
void count_transition(ReadSetTransition t) noexcept {
  g_read_set_transitions.at(static_cast<std::size_t>(t)).fetch_add(1, std::memory_order_relaxed);
}
#else
void count_transition(ReadSetTransition) noexcept {}
#endif

// The reads of a SERIALIZABLE transaction or autocommit execution
// (docs/transaction_design.md, *Mechanism: the transaction's read set*): the
// keys and key ranges its reads covered, which its commit submits as guards.
// A point inside a range is not kept, and ranges that overlap or touch are
// merged into their union, never across a gap, so the set grows with the
// distinct keys and ranges read, not with how often they were read. `bytes`
// is what its entries are charged (read_point_bytes, read_range_bytes, by
// their buffers' capacities).
struct ReadSet {
  using Ranges = std::map<Bytes, Bytes, BytesLess>;  // lo -> hi: disjoint, none touching, none empty
  using RangeNode = Ranges::node_type;
  std::set<Bytes, BytesLess> points;
  Ranges ranges;
  std::uint64_t bytes{0};

  [[nodiscard]] static auto cost(const RangeNode& n) -> std::uint64_t {
    return read_range_bytes(n.key().capacity(), n.mapped().capacity());
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return points.empty() && ranges.empty(); }

  // Whether `key` is inside a range or held as a point: adding it adds nothing.
  [[nodiscard]] auto holds(BytesView key) const -> bool {
    auto it = ranges.upper_bound(key);
    if (it != ranges.begin() && std::ranges::lexicographical_compare(key, std::prev(it)->second)) return true;
    return points.contains(key);
  }

  // Adds `key`, charged read_point_bytes(key.size()) by the caller, unless
  // it holds it already (false).
  [[nodiscard]] auto add_point(BytesView key) -> bool {
    if (holds(key)) return false;
    Bytes k(key.begin(), key.end());
    const auto n = read_point_bytes(k.capacity());
    points.insert(std::move(k));
    bytes += n;
    count_transition(ReadSetTransition::NewPoint);
    return true;
  }

  // Adds the range a node holds, merged with every range it overlaps or
  // touches, and drops the points inside it. Allocates nothing: the node is
  // the caller's, the ranges it merges are taken out whole, and whichever
  // buffers hold the union's bounds are kept by swapping. So `bytes` grows
  // by at most cost(nh) as it arrived; a merge only frees.
  void add_range(RangeNode nh) noexcept {
    auto lt = [](BytesView a, BytesView b) { return std::ranges::lexicographical_compare(a, b); };
    auto it = ranges.upper_bound(BytesView{nh.key()});
    if (it != ranges.begin() && !lt(std::prev(it)->second, nh.key())) --it;
    count_transition(ReadSetTransition::NewRange);
    while (it != ranges.end() && !lt(nh.mapped(), it->first)) {
      count_transition(ReadSetTransition::Merge);
      auto next = std::next(it);
      auto m = ranges.extract(it);
      bytes -= cost(m);
      if (lt(m.key(), nh.key())) std::swap(nh.key(), m.key());
      if (lt(nh.mapped(), m.mapped())) std::swap(nh.mapped(), m.mapped());
      it = next;
    }
    for (auto p = points.lower_bound(BytesView{nh.key()}); p != points.end() && lt(*p, nh.mapped());) {
      bytes -= read_point_bytes(p->capacity());
      p = points.erase(p);
      count_transition(ReadSetTransition::Swallow);
    }
    bytes += cost(nh);
    ranges.insert(std::move(nh));
  }

  void clear() noexcept {
    points.clear();
    ranges.clear();
    bytes = 0;
  }
};

// A range node not in any map: what a read set's range is built in, and
// what a cursor reserves at its open so that recording its range when it
// ends allocates nothing.
[[nodiscard]] auto range_node(Bytes lo, Bytes hi) -> ReadSet::RangeNode {
  ReadSet::Ranges one;
  one.emplace(std::move(lo), std::move(hi));
  return one.extract(one.begin());
}

static_assert(kNodeBytesOf<Bytes> <= kReadPointNodeBytes);
static_assert(kNodeBytesOf<std::pair<const Bytes, Bytes>> <= kReadRangeNodeBytes);

// Proof that a read is in the read set, or needs not be (snapshot
// isolation, a read-only transaction, a plan that writes nothing): what a
// read must show to reach the snapshot (bc_ctx::view). Only bc_ctx's
// recording calls make one, so a read that bypasses the recording does not
// compile.
class Recorded {
  friend struct ::bc_ctx;
  Recorded() noexcept {}  // NOLINT(modernize-use-equals-default): a defaulted one is never emitted (-Wunneeded-member-function)
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
  [[nodiscard]] auto bound_bytes() const -> std::uint64_t { return lo_.size() + hi_.size(); }

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
  std::uint64_t gen{0};
  std::uint32_t scope{kNoStep};  // its loop's step in bc_ctx::steps, while it is on an entry
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
  std::uint64_t held{0};  // what the buffers above are charged to the statement's memory budget
  std::optional<Recorded> recorded;  // bc_scan_open's: its range is recorded, or needs not be
  // SERIALIZABLE (docs/transaction_design.md, *What a read records for
  // phantoms*): the range it covers, as bc_ctx::finish_scan records it when
  // it closes, runs out or its statement ends. Reserved at its open: the
  // node, the bounds of its whole range, and a charge for the largest range
  // it can record, so the recording allocates and charges nothing.
  struct Read {
    ReadSet::RangeNode node;
    Bytes lo, hi;  // its whole range, inside its table's or index's keys
    Bytes last;    // the last key it handed out, with room for k⁺'s zero byte
    std::uint64_t charge{0};
    bool returned{false};  // it handed out a key
    bool ran_out{false};   // it reached its range's end without an error
  };
  std::unique_ptr<Read> read;
};

// A view's row-ness, kept with bytes a structure copied, so a row comes back
// out of it a row of its table, as bc_pin keeps one.
struct Held {
  std::uint32_t table{0};
  RowKind kind{RowKind::None};
};

// One sorter entry: its sort key and payload in one block, and the order it
// was added in, which breaks ties between equal keys.
struct SortEntry {
  Bytes data;  // the sort key, then the payload
  std::uint64_t seq;
  std::uint32_t key_len;
  Held held;

  [[nodiscard]] auto key() const -> BytesView { return BytesView{data}.first(key_len); }
  [[nodiscard]] auto payload() const -> BytesView { return BytesView{data}.subspan(key_len); }
};

auto bytes_cmp(BytesView a, BytesView b) -> std::strong_ordering {
  return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end(),
                                                [](std::byte x, std::byte y) {
                                                  return std::to_integer<int>(x) <=> std::to_integer<int>(y);
                                                });
}

// Byte order of the sort keys, then insertion order: a total order, so the
// sort is stable and a top-n sorter keeps the first n of equal keys.
[[nodiscard]] auto sort_before(const SortEntry& a, const SortEntry& b) -> bool {
  auto c = bytes_cmp(a.key(), b.key());
  return std::is_neq(c) ? std::is_lt(c) : a.seq < b.seq;
}

// A sorter's entries: with keep 0, every one added, in the order added; with
// keep n, the first n in sort order, as a heap whose top is the last of them,
// so an entry that sorts after it is refused without a copy. Sorted on the
// first bc_sorter_nth or bc_sorter_next, and never added to after.
struct Sorter {
  std::uint64_t gen;
  std::uint64_t keep;  // 0: every entry
  std::vector<SortEntry> entries;
  std::uint64_t added{0};
  bool sorted{false};
  std::size_t next{0};
  std::uint32_t scope{kNoStep};  // its loop's step in steps, while bc_sorter_next is on an entry
  bc_view nth{};                 // the last bc_sorter_nth's view, whose slot the next one takes
};

constexpr std::uint32_t kNoEntry = UINT32_MAX;

// One payload of a hash table, chained to the next payload of its key.
struct HashEntry {
  Bytes payload;
  std::uint32_t next{kNoEntry};
  Held held;
};

// A key's payloads: the first and last entries of its chain.
struct Chain {
  std::uint32_t first, last;
};

struct BytesHash {
  using is_transparent = void;
  auto operator()(BytesView b) const -> std::size_t { return static_cast<std::size_t>(row_fingerprint(b)); }
};
struct BytesEq {
  using is_transparent = void;
  auto operator()(BytesView a, BytesView b) const -> bool { return std::ranges::equal(a, b); }
};

// A multimap: each key once, its payloads chained in insertion order. An
// entry is never moved out of nor erased before reset, so a view of its
// payload stays where it is while the table grows.
struct Hash {
  std::uint64_t gen;
  std::unordered_map<Bytes, Chain, BytesHash, BytesEq> index;
  std::vector<HashEntry> entries;
};

// A probe of a hash table, a handle of the step it was made in.
struct HashIter {
  std::uint64_t gen;
  std::uint32_t hash;
  std::uint32_t next;
  std::uint32_t scope{kNoStep};  // its loop's step in steps, while bc_hash_next is on a payload
};

// What an aggregate's slot accumulates, fixed by its first fold.
enum class SlotKind : std::uint8_t { None, SumI64, SumF64, MinI64, MaxI64, MinF64, MaxF64, Count, MinBytes, MaxBytes };

struct Slot {
  std::int64_t i{0};  // a sum, a minimum or maximum, or a count; of bytes, its index in Agg::held
  double f{0};        // a double sum, minimum or maximum
  bool set{false};    // a value was folded in
};

// The fixed charges cover the entries they stand for. A group is a map node
// (its links and colour, its key and slot, its block) and its key's own
// block, both with the allocator's header, and its place in the order. A
// group's slot, a sorter and a hash table are each their place in a vector,
// at twice its size, as an aggregate is (below bc_ctx). A sorter entry is
// its slot, at twice its vector's size, and its block's
// header. A hash table entry is its slot likewise, and its payload block's
// header, with a key's node (its link and cached hash, its key and chain,
// its block), its key's block and its bucket, at twice the node count.
static_assert(sizeof(ViewEntry) <= kViewEntryBytes);
static_assert(sizeof(KeyBuilder) <= kKeyEntryBytes);
static_assert(sizeof(SortKeyBuilder) <= kKeyEntryBytes);
static_assert(sizeof(RowBuilder) <= kRowEntryBytes);
static_assert(sizeof(Cursor) <= kCursorEntryBytes);
static_assert(4 * sizeof(void*) + sizeof(std::pair<const Bytes, std::uint32_t>) + 2 * kAllocBytes +
                  sizeof(const Bytes*) <=
              kGroupEntryBytes);
static_assert(2 * sizeof(Slot) <= kAggSlotBytes);
// A held MIN or MAX of bytes: its block's header, and its place in the
// aggregate's vector, at twice its size.
static_assert(kAllocBytes + 2 * sizeof(Bytes) <= kAggHeldBytes);
static_assert(2 * sizeof(Sorter) <= kSorterBytes);
static_assert(2 * sizeof(Hash) <= kHashBytes);
static_assert(2 * sizeof(SortEntry) + kAllocBytes <= kSortEntryBytes);
static_assert(2 * sizeof(HashEntry) + kAllocBytes + 2 * sizeof(void*) + sizeof(std::pair<const Bytes, Chain>) +
                  2 * kAllocBytes + 2 * sizeof(void*) <=
              kHashEntryBytes);
static_assert(sizeof(HashIter) <= kHashIterEntryBytes);

// What a cursor holds: its bounds and its current entry's bytes.
auto cursor_bytes(const Cursor& c) -> std::uint64_t {
  std::uint64_t n = c.prefix.size() + c.lo.size() + c.hi.size() + c.key.size() + c.value.size() + c.row.size() +
                    c.row_key.size();
  if (c.entries) n += c.entries->bound_bytes();
  return n;
}



auto phys_type(std::uint8_t code) -> std::optional<PhysType> {
  switch (code) {
    case BC_T_I64: return PhysType::I64;
    case BC_T_F64: return PhysType::F64;
    case BC_T_BOOL: return PhysType::Bool;
    case BC_T_BYTES: return PhysType::Bytes;
    case BC_T_NUM: return PhysType::Num;
    case BC_T_JSONB: return PhysType::Jsonb;
    default: return std::nullopt;
  }
}

auto type_name(PhysType t) -> const char* {
  switch (t) {
    case PhysType::I64: return "an integer";
    case PhysType::F64: return "a double";
    case PhysType::Bool: return "a boolean";
    case PhysType::Bytes: return "bytes";
    case PhysType::Num: return "a numeric";
    case PhysType::Jsonb: return "a jsonb";
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

// An error code other than BC_E_LAYOUT, BC_E_DATA and BC_E_ARITH, checked
// at compile time: a layout error is set with its bc_layout_detail, and a
// data or arithmetic error with its SQLSTATE, through the other fail
// overloads, so none goes without one.
// -Wc++20-compat: the project is C++23; consteval is what makes it a
// compile-time check.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++20-compat"
struct ErrorCode {
  consteval ErrorCode(bc_error_code c) : code{c} {  // implicit: fail(BC_E_IO, ...)
    if (c == BC_E_LAYOUT) throw "BC_E_LAYOUT is set with its bc_layout_detail";
    if (c == BC_E_DATA || c == BC_E_ARITH) throw "BC_E_DATA and BC_E_ARITH are set with their SQLSTATE";
  }
  bc_error_code code;
};
// BC_E_DATA or BC_E_ARITH, the codes whose detail is a SQLSTATE.
struct StateCode {
  consteval StateCode(bc_error_code c) : code{c} {  // implicit: fail(BC_E_ARITH, kDivisionByZero, ...)
    if (c != BC_E_DATA && c != BC_E_ARITH) throw "only BC_E_DATA and BC_E_ARITH carry a SQLSTATE";
  }
  bc_error_code code;
};
#pragma clang diagnostic pop

static_assert(bytecask::sql::kDivisionByZero.packed() == BC_SQLSTATE('2', '2', '0', '1', '2'));
static_assert(bytecask::sql::unpack_sqlstate(BC_SQLSTATE('2', '2', 'P', '0', '2')) ==
              std::array<char, 5>{'2', '2', 'P', '0', '2'});

// The lines an error carries beside its message, each "" for none: its
// DETAIL, HINT and CONTEXT (bc_error_note, bc_error_hint, bc_error_context).
struct ErrorNotes {
  std::string_view detail;
  std::string_view hint;
  std::string_view context;
};

// Where an error is recorded: a context's for its runs, a transaction's for
// its commit. fail() keeps the first error set; unknown() overrides.
struct ErrorSink {
  bc_error_code& error;
  std::uint32_t& detail;
  std::string& message;
  std::string& note;  // the error's DETAIL line (bc_error_note): a constraint with a row, or a data error
  std::string& hint;     // its HINT line (bc_error_hint)
  std::string& context;  // its CONTEXT line (bc_error_context)

  void clear() noexcept {
    error = BC_OK;
    detail = 0;
    clear_text();
  }

  void clear_text() noexcept {
    message.clear();
    note.clear();
    hint.clear();
    context.clear();
  }

  void fail(ErrorCode code, std::string msg, std::uint32_t with_detail = 0) {
    if (error != BC_OK) return;
    error = code.code;
    detail = with_detail;
    message = std::move(msg);
  }

  void fail(bc_layout_detail why, std::string msg) {
    if (error != BC_OK) return;
    error = BC_E_LAYOUT;
    detail = why;
    message = std::move(msg);
  }

  // A data or arithmetic error with its SQLSTATE in the detail. The code and
  // the SQLSTATE first, so text that cannot be allocated is dropped, all of
  // it, never the error or its SQLSTATE.
  void fail(StateCode code, bytecask::sql::SqlState state, std::string_view msg, ErrorNotes notes = {}) noexcept {
    if (error != BC_OK) return;
    error = code.code;
    detail = state.packed();
    try {
      message = msg;
      note = notes.detail;
      hint = notes.hint;
      context = notes.context;
    } catch (...) {
      clear_text();
    }
  }

  // A constraint the runtime found, not a plan: the code first, so a message
  // that cannot be allocated is dropped, never the constraint.
  void constraint(ConstraintDetail d) noexcept {
    if (error != BC_OK) return;
    error = BC_E_CONSTRAINT;
    detail = d.encode();
    try {
      message = d.message();
    } catch (...) {
      message.clear();
    }
  }

  // A commit whose outcome is unknown. It overrides whatever was recorded:
  // the batch may be in the data file, and no earlier error may stand for
  // "nothing written". The code first, so a message that cannot be
  // allocated is dropped, never the outcome.
  void unknown(const char* what) noexcept {
    error = BC_E_UNKNOWN;
    detail = 0;
    clear_text();
    try {
      message = what;
    } catch (...) {
      message.clear();
    }
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

};

}  // namespace

// A context leaves the transaction it was begun on, with its statement not
// added: when that statement failed or holds writes, the transaction
// aborts. Defined with bc_txn.
void txn_detach(bc_txn* txn, bc_ctx* cx, bool abandoned) noexcept;

// The step a statement reserves the memory cap in: one atomic operation per
// MiB a statement grows by, not per allocation. Each statement in progress
// may hold up to one step more than it uses, so the cap can refuse up to a
// step per running statement early, never late.
constexpr std::uint64_t kMemoryStep = 1u << 20;

// Reserves at least `need` bytes of `cap`, `want` where it leaves room: the
// bytes granted, or 0 (taking nothing) when `need` does not fit. Never waits.
[[nodiscard]] auto cap_reserve(bc_memory_cap& cap, std::uint64_t need, std::uint64_t want) noexcept -> std::uint64_t {
  auto used = cap.used.load(std::memory_order_relaxed);
  for (;;) {
    if (used > cap.cap || need > cap.cap - used) return 0;
    const auto grant = std::min(std::max(want, need), cap.cap - used);
    if (cap.used.compare_exchange_weak(used, used + grant, std::memory_order_relaxed)) return grant;
  }
}

auto cap_message(const bc_memory_cap& cap) -> std::string {
  return "out of memory: the statements and transactions in progress would hold more than the memory cap of " +
         std::to_string(cap.cap) + " bytes";
}

struct bc_ctx {
  bytecask::DB* db;
  LiveCatalog* live;
  // The catalog version this execution resolves against, pinned with the
  // snapshot by bc_ctx_begin and dropped with it; cat is pinned.get(), or
  // the empty catalog between executions, where every lookup fails.
  std::shared_ptr<const Catalog> pinned;
  Catalog unpinned;
  const Catalog* cat{&unpinned};
  // Copies a step owns (bc_get's records, keys, decoded parts), and the
  // copies bc_pin makes, which live until reset.
  Arena arena;
  Arena pins;
  // The bytes the statement holds, as requested: what budget.max_bytes bounds.
  std::uint64_t mem_used{0};
  // What of mem_used is the write set (effects, insert claims, locks and the
  // tables written), each item as its *_bytes function counts it: what
  // bc_txn_add moves from the statement budget to the transaction's bound.
  std::uint64_t write_held{0};
  // The database's memory cap (null: none), and what of its `used` this
  // statement reserved: at least mem_used while the statement runs, its
  // peak rounded up to kMemoryStep. Returned by drop_execution_state; what
  // the write set holds passes to the transaction in bc_txn_add.
  bc_memory_cap* memory{nullptr};
  std::uint64_t reserved{0};
  // The tables the writes are in, in first-write order.
  std::vector<std::uint32_t> written_tables;
  // The last handle generation drawn; 0 is the empty view's, never drawn.
  // 64 bits, so it never wraps: a generation never repeats within the
  // context's life, and a stale handle never matches a reused slot.
  std::uint64_t serial{0};

  const bc_params* params{nullptr};
  bc_budget budget{};
  std::uint64_t reads{0};
  std::optional<bytecask::Snapshot> snap;  // an autocommit execution's own
  // What every read goes through: the snapshot the execution reads (its own,
  // or its transaction's: base_, reached only through view()) and, inside a
  // transaction, the overlay of its earlier statements' writes, which the
  // execution reads but never changes.
  const Overlay* overlay{nullptr};  // null outside a transaction
  bc_txn* txn{nullptr};
  // The level of the context's autocommit executions (bc_ctx_set_isolation),
  // the read set of the one in progress, and where this run records its
  // reads (read_set): its transaction's read set, own_reads, or null when nothing is
  // recorded (docs/transaction_design.md, *Mechanism: the transaction's read
  // set*). Set by bc_ctx_run.
  bc_isolation isolation{BC_ISOLATION_SNAPSHOT};
  ReadSet own_reads;
  ReadSet* read_set{nullptr};
  // The last bc_ctx_commit conflicted at SERIALIZABLE with no key it wrote
  // changed since its snapshot: a read/write dependency, not an update.
  bool rw_conflict{false};
#ifdef BYTECASK_SQL_TESTING
  std::function<void()> before_commit;  // bc_ctx_before_commit_for_testing
#endif

  bc_error_code error{BC_OK};
  std::uint32_t detail{0};
  std::string message;
  std::string note;
  std::string hint;
  std::string context;

  // The handle tables a step's storage is marked in: each grows as the plan
  // makes handles and is cut back to its mark when the step ends.
  std::vector<ViewEntry> views;
  std::vector<Cursor> cursors;
  std::vector<KeyBuilder> keys;
  std::vector<SortKeyBuilder> sort_keys;
  std::vector<RowBuilder> rows;
  std::vector<HashIter> hash_iters;
  std::vector<ViewEntry> pinned_views;  // bc_pin's, kept until reset
  // Where each table and the arena stood when a loop moved to its current
  // entry: everything after is the step's.
  struct Mark {
    std::size_t views, keys, sort_keys, rows, cursors, hash_iters;
    Arena::Mark arena;
  };
  // What drives a loop: a cursor, an aggregate's groups, a sorter or a hash
  // table probe.
  enum class Loop : std::uint8_t { Cursor, Agg, Sorter, Probe };
  // The loops on an entry, innermost last: loops nest, so their steps are
  // a stack. Each names what drives it and where its step began.
  struct Step {
    std::uint32_t owner;
    Loop loop;
    Mark mark;
    std::uint64_t id;  // drawn by next_gen: a step that replaces another at its depth is told apart
  };
  std::vector<Step> steps;
  std::vector<Effect> effects;
  std::vector<Claim> claims;  // this statement's or execution's, in the order its writes made them
  std::vector<Lock> locks;    // a transaction statement's rows locked or written, read from the snapshot
  // The row keys this execution's writes touched, and whether the row is
  // there after the write: a second write of one row would derive its index
  // entries from the snapshot's row again, not from the first write's, and
  // leave the first write's entries behind. Kept across the runs of one
  // execution, which share its snapshot and write plan; reset by
  // bc_ctx_begin.
  std::map<Bytes, bool, BytesLess> written_rows;
  // The unique index prefixes this execution's writes gave a row, by index
  // id: the snapshot does not hold them, so without this two rows of one
  // statement could share one (#507). Kept and reset as written_rows.
  std::set<std::pair<std::uint32_t, Bytes>> unique_written;
  // The unique index prefixes this execution's updates took from a row: the
  // view still shows the row's entry, so without this ON CONFLICT DO UPDATE
  // could not tell a holder that left a prefix from one that kept it. Kept
  // and reset as written_rows.
  std::set<std::pair<std::uint32_t, Bytes>> unique_vacated;
  // The aggregates bc_agg_open made: their groups' keys, in first-appearance
  // order, and their slots. Kept until reset; a group key's view reads its
  // Bytes, which a move of the vector keeps where it is.
  // Each group's key is held once, in `index`; `order` points at it, in
  // first-appearance order (a map node never moves). Group i's slots are
  // values[i * slots, (i + 1) * slots). A group's handle is i and `groups`,
  // a generation of its own: bc_agg_clear draws a new one, so the indices
  // start again at 0 and a handle of a dropped group is stale, never
  // another group's. So 2^32 limits the groups held, not those ever made.
  struct Agg {
    std::uint64_t gen{0};
    std::uint64_t groups{0};
    std::uint32_t slots{0};
    std::uint32_t scope{kNoStep};  // its loop's step in steps, while bc_agg_next is on a group
    std::uint32_t next{0};         // the group bc_agg_next gives next
    std::map<Bytes, std::uint32_t, BytesLess> index;
    std::vector<const Bytes*> order;
    std::vector<SlotKind> kinds;  // per slot, the same in every group
    std::vector<Slot> values;
    // The value of each MinBytes or MaxBytes slot a value reached, in the
    // order they were first reached: the slot's `i` is its index. Made on
    // the first, so an aggregate of none keeps its size; charged as
    // held_value_bytes says, released by a clear or the reset.
    std::unique_ptr<std::vector<Bytes>> held;
  };
  // Sorters, hash tables and aggregates, in the order opened, so their
  // generations ascend; kept until reset.
  std::vector<Agg> aggs;
  std::vector<Sorter> sorters;
  std::vector<Hash> hashes;
  // Where each part of the last key a key-part decoder read starts, so the
  // next part of the same key is found without skipping the earlier ones
  // again. Keyed by the view handle, unique within an execution; dropped
  // when the view's slot is released, so it never outlives the view.
  struct {
    bc_view view{};
    std::uint32_t object{0};
    std::vector<std::size_t> at;
  } key_parts;
  // The same for a sort key's parts (bc_sort_key_part_*): the key carries no
  // layout, so each part's type is learned as it is read, in order. `type[i]`
  // is part i's, nullopt for a NULL; `at[i]` is where it starts, and `at`
  // holds one more, where the next unread part does.
  struct {
    bc_view view{};
    std::vector<std::size_t> at;
    std::vector<std::optional<PhysType>> type;
  } sort_parts;
  std::map<Bytes, Bytes, BytesLess> deleted_ranges;  // lo -> hi of each bc_del_range, likewise; disjoint

  Verified verified;                 // this context's reference, rebuilt by every run
  bool has_ref{false};  // a run verified `verified` against the pinned version
  std::vector<KeySlots> key_slots;    // of the tables this run updated, built on the first update

  // Results: rows in the row format of the plan's result_types, each
  // preceded by its u32 length.
  Bytes results;
  std::size_t result_pos{0};
  // bc_ctx_window: the result rows a run drops from its front and keeps
  // after them. `emitted` counts the rows a top-level bc_emit_end ended,
  // dropped ones included; `kept` those encoded into `results`.
  bool windowed{false};
  bool unwinding{false};  // the plan is running out after its last kept row
  bool stopped{false};    // bc_ctx_run ended that way
  std::uint64_t window_skip{0}, window_limit{0};
  std::uint64_t emitted{0}, kept{0};
  std::optional<RowWriter> emit;
  std::uint32_t emit_col{0};
  std::int64_t affected{-1};

  // A subquery's rows while it runs, bc_sub_begin to bc_sub_end: folded
  // here, never a result row. Then what the subqueries left until reset:
  // the parameters they and bc_sub_arg_* set, by index, over the caller's
  // NULLs, and each BC_SUB_SET's values, encoded by sub_key.
  struct Sub {
    std::uint32_t id{0};
    std::int32_t kind{BC_SUB_VALUE};
    PhysType type{PhysType::I64};
    std::uint32_t param{0};
    RowLayout layout;
    std::int64_t rows{0};
    bool saw_null{false};
    Datum value;                   // BC_SUB_VALUE: its row's
    std::optional<Datum> lo, hi;   // BC_SUB_BOUNDS: the non-NULL extremes
    // Where the handle tables and the blocking structures stood at
    // bc_sub_begin: bc_sub_end releases what the run made after. `steps` is
    // the loop depth then, and `top` the id of the innermost step (0: none),
    // so a loop outside the subquery that moved inside it is seen.
    Mark mark{};
    std::size_t steps{0}, aggs{0}, sorters{0}, hashes{0};
    std::uint64_t top{0};
  };
  std::optional<Sub> sub;
  // A parameter a subquery or bc_sub_arg_* set: its value, the generation
  // its views carry (a new one per write, so a view of the old value goes
  // stale), and what its bytes are charged.
  struct SubParam {
    std::optional<Datum> value;
    std::uint64_t gen{0};
    std::uint64_t held{0};
  };
  std::vector<SubParam> sub_params;  // sized once, to the caller's parameters
  // bc_param_jsonb's: each parameter's text parsed once per execution, a
  // pinned view of its node; the empty handle for one not parsed yet.
  std::vector<bc_jsonb> jsonb_params;
  struct SubSet {
    std::set<Bytes, BytesLess> values;
    std::uint64_t held{0};  // what its values are charged, released when its id runs again
  };
  std::map<std::uint32_t, SubSet> sub_sets;

  bc_ctx(bytecask::DB* d, LiveCatalog* l) : db{d}, live{l} {}

  // The snapshot the execution reads, for a read the token shows recorded.
  [[nodiscard]] auto view(Recorded) const -> const bytecask::Snapshot& { return *base_; }
  [[nodiscard]] auto has_view() const noexcept -> bool { return base_ != nullptr; }
  void set_view(const bytecask::Snapshot* s) noexcept { base_ = s; }

  // The recording reader (docs/transaction_design.md, *Mechanism: the
  // transaction's read set*): each read asks for its token first, and the
  // call records what the read will cover before it is made, charged, so a
  // read is never lost from the read set once its result is seen. Nullopt,
  // with BC_E_LIMIT (54000), BC_E_MEMORY or the context's error set, when it
  // cannot be recorded: the read is not made. Defined with bc_txn.
  [[nodiscard]] auto record_point(BytesView key) -> std::optional<Recorded>;
  [[nodiscard]] auto record_range(BytesView lo, BytesView hi) -> std::optional<Recorded>;
  // A cursor's range [lo, hi), reserved at its open; finish_scan records
  // what of it the cursor covered, allocating and charging nothing.
  [[nodiscard]] auto record_scan(Cursor& c, BytesView lo, BytesView hi) -> std::optional<Recorded>;
  void finish_scan(Cursor& c) noexcept;
  // Charges `n` bytes of the read set: to the transaction's bound and the
  // memory cap in a transaction, to the statement's budget in autocommit.
  [[nodiscard]] auto read_charge(std::uint64_t n) -> bool;
  void read_release(std::uint64_t n) noexcept;
  // False, with BC_E_READ_ONLY (25006), in a transaction begun read-only:
  // what a write or guard of `command` calls first.
  [[nodiscard]] auto writable(const char* command) -> bool;

  // A run that reached its window's end is unwound as a failed one is: every
  // call after the stop returns a neutral value. It is not an error.
  auto ok() const -> bool { return error == BC_OK && !unwinding; }

  [[nodiscard]] auto sink() -> ErrorSink { return {error, detail, message, note, hint, context}; }

  void fail(ErrorCode code, std::string msg) { sink().fail(code, std::move(msg)); }

  void fail(StateCode code, bytecask::sql::SqlState state, std::string_view msg, ErrorNotes notes = {}) noexcept {
    sink().fail(code, state, msg, notes);
  }

  void fail(bc_layout_detail why, std::string msg) { sink().fail(why, std::move(msg)); }

  void fail_current_exception() noexcept { sink().fail_current_exception(); }

  auto charge(std::uint64_t units = 1) -> bool {
    if (!ok()) return false;
    auto before = reads;
    reads += units;
    if (budget.max_reads != 0 && reads > budget.max_reads) {
      fail(BC_E_BUDGET, "work budget exhausted");
      return false;
    }
    // Every 64 units, whatever the step: a charge of several units must not
    // jump over the check. The messages are PostgreSQL's for 57014.
    if ((budget.deadline_ns != 0 || budget.cancel != nullptr) && before / 64 != reads / 64) {
      if (budget.cancel != nullptr && __atomic_load_n(budget.cancel, __ATOMIC_RELAXED) != 0) {
        fail(BC_E_BUDGET, "canceling statement due to user request");
        return false;
      }
      if (budget.deadline_ns != 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        if (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) >
            budget.deadline_ns) {
          fail(BC_E_BUDGET, "canceling statement due to statement timeout");
          return false;
        }
      }
    }
    return true;
  }

  // Charges n bytes to the statement's memory budget: false, and BC_E_LIMIT,
  // past it. Checked before the bytes are allocated wherever their size is
  // known first.
  [[nodiscard]] auto take(std::uint64_t n) -> bool {
    if (!ok()) return false;
    if (budget.max_bytes != 0 && n > budget.max_bytes - std::min(mem_used, budget.max_bytes)) {
      fail(BC_E_LIMIT, "the statement would hold more than its memory budget of " + std::to_string(budget.max_bytes) +
                           " bytes");
      return false;
    }
    if (memory != nullptr && n > reserved - mem_used && !reserve(n - (reserved - mem_used))) return false;
    mem_used += n;
    return true;
  }

  // Reserves at least `need` more bytes of the memory cap, a kMemoryStep
  // multiple where the cap leaves room for one: false, and BC_E_MEMORY
  // (SQLSTATE 53200), if the cap has no room for `need`. Never waits.
  [[nodiscard]] auto reserve(std::uint64_t need) -> bool {
    if (need == 0) return true;
    const auto want = need > UINT64_MAX - (kMemoryStep - 1) ? need : (need + kMemoryStep - 1) / kMemoryStep * kMemoryStep;
    const auto grant = cap_reserve(*memory, need, want);
    if (grant == 0) {
      fail(BC_E_MEMORY, cap_message(*memory));
      return false;
    }
    reserved += grant;
    return true;
  }

  // Moves what one buffer is charged, `held`, to its size now: a release
  // always succeeds, growth is take()n.
  [[nodiscard]] auto rehold(std::uint64_t& held, std::uint64_t now) -> bool {
    if (now <= held) {
      mem_used -= held - now;
      held = now;
      return true;
    }
    if (!take(now - held)) return false;
    held = now;
    return true;
  }

  void release(std::uint64_t n) noexcept { mem_used -= n; }

  // Whether the transaction has room for the statement's write set grown to
  // `bytes`: false, and BC_E_LIMIT, if not. Defined with bc_txn, whose bound
  // it reads.
  [[nodiscard]] auto txn_room(std::uint64_t bytes) -> bool;

  // Appends what `make` builds to `into`, an item of the write set of `n`
  // bytes: charged to the statement budget and, in a transaction, held to
  // the room its bound leaves, before `make` allocates it. Past either,
  // false and BC_E_LIMIT, with nothing added and nothing charged. A throw
  // fails the statement, and a failed statement's charges go at reset: no
  // charge is released on the way out of a throw, here or elsewhere.
  template <typename T, typename Make>
  [[nodiscard]] auto push_write(std::vector<T>& into, std::uint64_t n, Make&& make) -> bool {
    if (!ok()) return false;
    if (txn != nullptr && !txn_room(write_held + n)) return false;
    if (!take(n)) return false;
    write_held += n;
    into.push_back(make());
    return true;
  }

  // Adds an effect, its buffers of capacity `a_bytes` and `b_bytes`: a table
  // the writes had not touched is noted, before the effect, and counted with
  // it.
  template <typename Make>
  void push_effect_made(EffectKind kind, BytesView key, std::size_t a_bytes, std::size_t b_bytes, Make&& make) {
    auto table = is_write(kind) ? written_table(key) : std::nullopt;
    auto n = effect_bytes(txn != nullptr, kind, a_bytes, b_bytes) + (table ? table_bytes(*table) : 0);
    (void)push_write(effects, n, [&] {
      if (table) written_tables.push_back(*table);
      return make();
    });
  }

  // The table a write's key is in when it is one the writes have not yet
  // touched: a row key names its table, an index key its index. A range's
  // upper bound is often the next object's prefix: only its lower bound
  // names what it deletes.
  [[nodiscard]] auto written_table(BytesView key) const -> std::optional<std::uint32_t> {
    auto obj = key_object(key);
    if (!obj || (obj->first != bytecask::sql::kNsRow && obj->first != bytecask::sql::kNsIndex)) return std::nullopt;
    const auto* kl = cat->key_layout(obj->second);
    if (kl == nullptr || std::ranges::find(written_tables, kl->table->id) != written_tables.end()) return std::nullopt;
    return kl->table->id;
  }

  [[nodiscard]] auto next_gen() noexcept -> std::uint64_t { return ++serial; }

  // Appends an entry to a handle table and charges it, with what it holds:
  // nullptr, and nothing appended, past the budget.
  template <typename T>
  [[nodiscard]] auto push_charged(std::vector<T>& table, T&& e, std::uint64_t charge) -> T* {
    table.push_back(std::move(e));
    if (!take(charge)) {
      table.pop_back();
      return nullptr;
    }
    return &table.back();
  }

  // Copies bytes into `into`, charged; nullptr (and the error) past the
  // budget.
  auto arena_copy(Arena& into, BytesView b) -> const std::byte* {
    if (!take(b.size())) return nullptr;
    return into.copy(b);
  }

  auto make_view(const std::byte* p, std::size_t len, std::uint32_t cursor = kNoCursor, std::uint64_t step = 0,
                 std::uint32_t table = 0, RowKind kind = RowKind::None) -> bc_view {
    auto gen = next_gen();
    if (push_charged(views, ViewEntry{p, step, gen, static_cast<std::uint32_t>(len), cursor, table, kind, false},
                     kViewEntryBytes) == nullptr) {
      return {};
    }
    return {static_cast<std::uint32_t>(views.size() - 1), gen};
  }

  // A view of a jsonb node, tagged so that only bc_*jsonb* calls read it.
  auto make_jsonb(const std::byte* p, std::size_t len, std::uint32_t cursor = kNoCursor, std::uint64_t step = 0)
      -> bc_jsonb {
    auto gen = next_gen();
    if (push_charged(views, ViewEntry{p, step, gen, static_cast<std::uint32_t>(len), cursor, 0, RowKind::None, true},
                     kViewEntryBytes) == nullptr) {
      return {};
    }
    return {static_cast<std::uint32_t>(views.size() - 1), gen};
  }

  // Points the view `v` names, if it is still live, at other bytes under a
  // new generation, so its old handle goes stale and its slot and charge
  // are reused; otherwise makes a new view. Either way `v` names the result.
  auto remake_view(bc_view& v, BytesView b, std::uint32_t table, RowKind kind) -> bc_view {
    if (find_view(v) == nullptr) return v = make_view(b.data(), b.size(), kNoCursor, 0, table, kind);
    if (key_parts.view.i == v.i && key_parts.view.g == v.g) key_parts.view = {};
    if (sort_parts.view.i == v.i && sort_parts.view.g == v.g) sort_parts.view = {};
    auto gen = next_gen();
    views[v.i] = ViewEntry{b.data(), 0, gen, static_cast<std::uint32_t>(b.size()), kNoCursor, table, kind, false};
    v.g = gen;
    return v;
  }

  // A copy the current step owns, as a view.
  auto arena_view(BytesView b, std::uint32_t table = 0, RowKind kind = RowKind::None) -> bc_view {
    const auto* p = arena_copy(arena, b);
    if (p == nullptr) return {};
    return make_view(p, b.size(), kNoCursor, 0, table, kind);
  }

  // A view's entry, or nullptr if it is stale or foreign.
  auto find_view(bc_view v) const -> const ViewEntry* {
    const auto& table = (v.i & kPinnedView) != 0 ? pinned_views : views;
    auto i = v.i & ~kPinnedView;
    if (i >= table.size() || table[i].gen != v.g) return nullptr;
    return &table[i];
  }

  // A view's entry; nullptr (and BC_E_HANDLE) if it is stale or foreign. The
  // zero handle, the empty view every neutral return gives, has no entry.
  auto entry(bc_view v) -> const ViewEntry* {
    const auto* found = find_view(v);
    if (found == nullptr) {
      fail(BC_E_HANDLE, "stale or foreign view: its loop moved on, or its statement ended");
      return nullptr;
    }
    const auto& e = *found;
    if (e.cursor != kNoCursor && (e.cursor & kParamOwner) != 0) {
      auto i = e.cursor & ~kParamOwner;
      if (i >= sub_params.size() || sub_params[i].gen != e.step) {
        fail(BC_E_HANDLE, "view read after its parameter was set again");
        return nullptr;
      }
    } else if (e.cursor != kNoCursor && (e.cursor >= cursors.size() || cursors[e.cursor].step != e.step)) {
      fail(BC_E_HANDLE, "view read after its cursor moved");
      return nullptr;
    }
    return &e;
  }

  // The bytes of a view; nullopt (and BC_E_HANDLE) if it is stale or
  // foreign, or a jsonb node: a bc_jsonb forged into a bc_view.
  auto resolve(bc_view v) -> std::optional<BytesView> {
    if (v.g == 0 && v.i == 0) return BytesView{};
    const auto* e = entry(v);
    if (e == nullptr) return std::nullopt;
    if (e->jsonb) {
      fail(BC_E_HANDLE, "a jsonb value where bytes are read: only bc_*jsonb* calls take one");
      return std::nullopt;
    }
    return BytesView{e->data, e->len};
  }

  // A jsonb node; nullopt (and BC_E_HANDLE) for a stale or foreign handle,
  // the empty one, or a view that is not a jsonb's: a bc_view forged into a
  // bc_jsonb.
  auto resolve_jsonb(bc_jsonb j) -> std::optional<BytesView> {
    if (j.g == 0 && j.i == 0) {
      fail(BC_E_HANDLE, "the empty handle is no jsonb value");
      return std::nullopt;
    }
    const auto* e = entry(bc_view{j.i, j.g});
    if (e == nullptr) return std::nullopt;
    if (!e->jsonb) {
      fail(BC_E_HANDLE, "bytes where a jsonb value is read: a bc_jsonb comes from a bc_*jsonb* call");
      return std::nullopt;
    }
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
    const auto* e = find_view(v);
    if (e == nullptr) return {kNoCursor, 0};
    return {e->cursor, e->step};
  }

  template <typename H, typename T>
  auto resolve_handle(H h, std::vector<T>& table, const char* what) -> T* {
    if (!ok()) return nullptr;
    if (h.i >= table.size() || table[h.i].gen != h.g) {
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
    if (i < sub_params.size() && sub_params[i].value) return &*sub_params[i].value;  // a subquery's
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

  // Each adds to the write set, and past a bound adds nothing. Counted by
  // the capacities of the buffers it keeps (held_bytes), before a copy is
  // made: a copy of a view has the view's size as its capacity.
  void push_effect(EffectKind kind, BytesView a, BytesView b = {}) {
    push_effect_made(kind, a, a.size(), b.size(),
                     [&] { return Effect{kind, Bytes(a.begin(), a.end()), Bytes(b.begin(), b.end())}; });
  }
  // For keys the runtime built and no longer needs: moved, not copied, and
  // charged their capacity just after encoding. The budget can be passed by
  // the keys of the one write being made, before they are charged: a
  // bounded overshoot, since a key is bounded by the key ceiling
  // (kMaxKeyBytes; the engine refuses a longer one).
  void push_effect(EffectKind kind, Bytes&& a, BytesView b = {}) {
    push_effect_made(kind, a, a.capacity(), b.size(),
                     [&] { return Effect{kind, std::move(a), Bytes(b.begin(), b.end())}; });
  }
  void push_effect(EffectKind kind, BytesView a, Bytes&& b) {
    push_effect_made(kind, a, a.size(), b.capacity(),
                     [&] { return Effect{kind, Bytes(a.begin(), a.end()), std::move(b)}; });
  }
  void push_effect(EffectKind kind, Bytes&& a, Bytes&& b) {
    push_effect_made(kind, a, a.capacity(), b.capacity(), [&] { return Effect{kind, std::move(a), std::move(b)}; });
  }
  [[nodiscard]] auto push_claim(std::uint32_t object, BytesView key, std::optional<std::size_t> prefix_len) -> bool {
    return push_write(claims, claim_bytes(key.size()),
                      [&] { return Claim{object, Bytes(key.begin(), key.end()), prefix_len}; });
  }
  [[nodiscard]] auto push_lock(BytesView key, std::uint64_t fingerprint) -> bool {
    return push_write(locks, lock_bytes(key.size()), [&] { return Lock{Bytes(key.begin(), key.end()), fingerprint}; });
  }

  // Sets the constraint error: the kind in the detail's high byte, the
  // object id in its low 24 bits.
  // With `detail_note`, the error's DETAIL line (a DECIMAL cast's precision).
  void raise(std::uint32_t constraint, std::uint32_t object_id, std::string msg = {}, std::string_view detail_note = {}) {
    if (!ok()) return;
    ConstraintDetail d{constraint, object_id};
    // A kind or id the detail cannot hold is a caller's bug, as in bc_raise:
    // BC_E_HANDLE, not a constraint and not corruption.
    if (!d.valid()) {
      fail(BC_E_HANDLE, "a constraint with an unknown kind or an object id past 2^24");
      return;
    }
    if (msg.empty()) {
      sink().constraint(d);
    } else {
      sink().fail(BC_E_CONSTRAINT, std::move(msg), d.encode());
      if (!detail_note.empty()) {
        try {
          sink().note = detail_note;
        } catch (...) {  // the DETAIL is dropped, never the error
        }
      }
    }
  }

  // Leaves the transaction: drops all that points into it (its snapshot and
  // overlay, the cursors over them, the statement's pending effects) and
  // keeps the rest, a finished statement's results and count readable.
  // Idempotent, and the first step of drop_execution_state. The protocol
  // (bc_plan.h) adds every statement before its reset, but one reset
  // without it that failed or holds writes or guards aborts its
  // transaction rather than vanish. A plan refused as incompatible before
  // it ran did nothing, and may be recompiled and run again: the API's
  // one reset without bc_txn_add.
  void detach_from_txn() noexcept {
    auto refused_before_run = error == BC_E_LAYOUT && detail == BC_L_INCOMPATIBLE;
    auto abandoned = txn != nullptr && ((!ok() && !refused_before_run) || !effects.empty());
    drop_cursors();  // iterators before the snapshot and the overlay they read
    clear_write_slots(effects);  // as the write set's other vectors at reset
    base_ = nullptr;
    overlay = nullptr;
    if (txn != nullptr) txn_detach(txn, this, abandoned);
    read_set = nullptr;
    txn = nullptr;
  }

  [[nodiscard]] auto mark() const -> Mark {
    return {views.size(), keys.size(), sort_keys.size(), rows.size(), cursors.size(), hash_iters.size(), arena.mark()};
  }

  // Releases what was allocated since `m`, and its charge. Allocates nothing.
  void roll_back(const Mark& m) noexcept {
    auto released = arena.roll_back(m.arena) + (views.size() - m.views) * kViewEntryBytes;
    if ((key_parts.view.i & kPinnedView) == 0 && key_parts.view.i >= m.views) key_parts.view = {};
    if ((sort_parts.view.i & kPinnedView) == 0 && sort_parts.view.i >= m.views) sort_parts.view = {};
    views.erase(views.begin() + static_cast<std::ptrdiff_t>(m.views), views.end());
    for (auto i = m.keys; i < keys.size(); ++i) released += keys[i].held + kKeyEntryBytes;
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(m.keys), keys.end());
    for (auto i = m.sort_keys; i < sort_keys.size(); ++i) released += sort_keys[i].held + kKeyEntryBytes;
    sort_keys.erase(sort_keys.begin() + static_cast<std::ptrdiff_t>(m.sort_keys), sort_keys.end());
    for (auto i = m.rows; i < rows.size(); ++i) released += rows[i].held + kRowEntryBytes;
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(m.rows), rows.end());
    for (auto i = m.cursors; i < cursors.size(); ++i) {
      finish_scan(cursors[i]);
      released += cursors[i].held + kCursorEntryBytes;
    }
    cursors.erase(cursors.begin() + static_cast<std::ptrdiff_t>(m.cursors), cursors.end());
    released += (hash_iters.size() - m.hash_iters) * kHashIterEntryBytes;
    hash_iters.erase(hash_iters.begin() + static_cast<std::ptrdiff_t>(m.hash_iters), hash_iters.end());
    release(released);
  }

  // Starts a step of the loop `owner` drives: what is allocated from here
  // is the step's.
  void begin_step(std::uint32_t owner, Loop loop, std::uint32_t& scope) {
    steps.push_back({owner, loop, mark(), next_gen()});
    scope = static_cast<std::uint32_t>(steps.size() - 1);
  }

  // The loop of `st` is off its entry.
  void leave_scope(const Step& st) noexcept {
    switch (st.loop) {
      case Loop::Cursor: cursors[st.owner].scope = kNoStep; break;
      case Loop::Agg: aggs[st.owner].scope = kNoStep; break;
      case Loop::Sorter: sorters[st.owner].scope = kNoStep; break;
      case Loop::Probe: hash_iters[st.owner].scope = kNoStep; break;
    }
  }

  // Ends step `depth` and every step inside it: their loops are off their
  // entries, and what the steps allocated goes. Allocates nothing.
  void end_steps_from(std::uint32_t depth) noexcept {
    for (auto i = std::size_t{depth}; i < steps.size(); ++i) leave_scope(steps[i]);
    roll_back(steps[depth].mark);
    steps.erase(steps.begin() + depth, steps.end());
  }

  // Drops every cursor, after the plan has run: the loops' steps end, and
  // what they allocated stays until reset.
  void drop_cursors() noexcept {
    for (const auto& st : steps) leave_scope(st);
    steps.clear();
    for (auto& c : cursors) finish_scan(c);
    cursors.clear();
  }

  void drop_execution_state() {
    detach_from_txn();  // the snapshot's readers first, in the one leave order
    snap.reset();
    views.clear();
    pinned_views.clear();
    keys.clear();  // key and row builders point into the pinned catalog: before it
    sort_keys.clear();
    rows.clear();
    verified.cols.clear();  // each ColSlot's sql points into the pinned catalog: before it
    cat = &unpinned;
    pinned.reset();  // an idle context keeps no superseded catalog alive
    written_rows.clear();
    unique_written.clear();
    unique_vacated.clear();
    aggs.clear();
    sorters.clear();
    hashes.clear();
    hash_iters.clear();
    deleted_ranges.clear();
    clear_write_slots(claims);
    clear_write_slots(locks);
    has_ref = false;  // set by the next run's verification
    key_slots.clear();
    key_parts.view = {};  // its view's slot is released
    key_parts.at.clear();
    sort_parts.view = {};
    sort_parts.at.clear();
    sort_parts.type.clear();
    results.clear();
    result_pos = 0;
    windowed = false;
    unwinding = false;
    stopped = false;
    window_skip = window_limit = emitted = kept = 0;
    emit.reset();
    emit_col = 0;
    affected = -1;
    sub.reset();
    sub_params.clear();
    jsonb_params.clear();
    sub_sets.clear();
    arena.clear(true);  // one chunk kept for the next statement
    pins.clear(false);
    mem_used = 0;
    if (reserved != 0) memory->used.fetch_sub(std::exchange(reserved, 0), std::memory_order_relaxed);
    write_held = 0;
    clear_write_slots(written_tables);
    reads = 0;
    own_reads.clear();  // a new execution, or a retry's attempt, starts with an empty read set
    rw_conflict = false;
  }

private:
  const bytecask::Snapshot* base_{nullptr};
};

// A savepoint's mark (docs/transaction_design.md, *Savepoints*): the
// transaction as it stood between two statements. The overlay is shared
// with the transaction, copy-on-write as a cursor's pin is, so a mark costs
// nothing until a write follows it; the vectors only grow between marks,
// so their lengths are enough to cut them back.
struct TxnMark {
  std::shared_ptr<Overlay> overlay;
  std::size_t guards;
  std::size_t claims;
  std::size_t locks;
  std::size_t written;
  std::uint64_t held;
  // The transaction's catalog at the mark (docs/transaction_design.md,
  // *Mechanism: savepoint marks carry the catalog*): a rollback to the mark
  // undoes the DDL since in memory, as the overlay undoes it in the batch.
  // Null for the pinned catalog, which a mark taken before the first
  // statement, or a re-pin after it, has not seen.
  std::shared_ptr<const Catalog> catalog;
  bool backfill;
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
  bc_isolation isolation;
  bool read_only;
  // SERIALIZABLE (docs/transaction_design.md, *Serializable isolation*): what
  // its statements read, recorded as they read it, and what that is charged
  // (read_bytes, with the reservations of the cursors open). Not part of a
  // mark: a rollback to a savepoint keeps the reads, and their charge, since
  // the client saw what they returned. Empty in a read-only transaction.
  ReadSet reads;
  std::uint64_t read_bytes{0};
  // The commit conflicted with no key it wrote changed since its snapshot:
  // a read/write dependency (docs/transaction_design.md, commit step 5).
  bool rw_conflict{false};

  std::shared_ptr<const Catalog> pinned;
  // The catalog its statements are bound and run against: `pinned`, until
  // its first DDL that changes something; then that DDL's private next
  // version (bc_txn_ddl). The transaction has run DDL while the two differ.
  std::shared_ptr<const Catalog> catalog;
  // A DDL of it rewrote or indexed rows: its commit's engine refusal of the
  // batch is the backfill's.
  bool backfill{false};
  std::optional<bytecask::Snapshot> snap;  // engaged from the first statement to the commit
  // The writes: the commit is built from the overlay, its ranges first,
  // since every point in it is newer than any range over it. Shared with the
  // cursors that pinned it (bc_txn_overlay_pin) and copy-on-write: a
  // statement's writes go through own_overlay(), which clones it first if a
  // cursor holds it, so a pinned overlay never changes.
  std::shared_ptr<Overlay> overlay{std::make_shared<Overlay>()};
  // What the pins (bc_overlay) hold back: each reserves the overlay's bound
  // as of its pin, counted by txn_holds so the write that follows a clone
  // cannot pass max_bytes for the copy a cursor keeps.
  std::uint64_t pinned_bytes{0};
  // What of pinned_bytes hold cursors (bc_txn_hold) took over: each reserves
  // its pin's share of the memory cap itself, and keeps it past the
  // transaction's end, so the transaction's reservation leaves it out
  // (cap_holds).
  std::uint64_t pinned_by_holds{0};
  std::uint64_t pins{0};  // bc_overlay handles live
  // The savepoints' marks, oldest first (bc_txn_savepoint).
  std::vector<TxnMark> marks;
  // What the marks' overlays hold beyond the current one: each overlay a
  // write cloned away from, once (recount_marks). Charged when the clone is
  // made, at bc_txn_add; txn_room counts the copy a write would keep before
  // that, so the statement that passes the bound fails at its call.
  std::uint64_t marked_bytes{0};
  std::vector<Effect> guards;          // the statements' guards, in their order
  std::vector<Claim> claims;           // the statements' insert claims, in their order
  std::vector<Lock> locks;             // the statements' rows locked or written, in their order
  std::vector<std::uint32_t> written;  // the tables the writes are in
  // What the four above hold, each item as the statement counted it
  // (effect_bytes, claim_bytes, lock_bytes, table_bytes), its WritePlan
  // entry included.
  std::uint64_t held{0};
  // What the transaction's private plans are charged (bc_txn_charge_plans):
  // the API's plan map, compiled against the pinned catalog. Not part of
  // `held`, so a mark neither records nor restores it; an abort that drops
  // the pin zeroes it, with the plans its holder then frees.
  std::uint64_t plan_bytes{0};
  // What the catalogs it holds besides `pinned` are charged: its current
  // one and those its marks keep, each distinct one once, by
  // Catalog::estimated_bytes (recount_catalogs).
  std::uint64_t catalog_bytes{0};
  std::uint64_t statements{0};         // statements wholly added
  // A statement failed: nothing but bc_txn_free is left. Set before any
  // part of a failing statement could reach the overlay or the guards, so
  // an aborted transaction is never committed half-applied.
  bool aborted{false};
  bool done{false};  // committed, or its commit was attempted
  // The last bc_txn_commit's error. The commit that ends the transaction
  // records its outcome here, and a later call on the ended transaction
  // leaves it as it is.
  bc_error_code error{BC_OK};
  std::uint32_t detail{0};
  std::string message;
  std::string note;
  std::string hint;
  std::string context;
  [[nodiscard]] auto sink() -> ErrorSink { return {error, detail, message, note, hint, context}; }
  void fail_current_exception() noexcept { sink().fail_current_exception(); }
  // The one context begun on it and not yet reset, which points into its
  // snapshot and overlay; null when none is. A second is refused, and so
  // are a re-pin and a commit while one is. An abort and a free detach it.
  bc_ctx* attached{nullptr};

  // The database's memory cap (null: none), and what of its `used` the
  // transaction holds: at least txn_holds, taken over from its statements'
  // reservations by bc_txn_add, returned by an abort or the free.
  bc_memory_cap* memory{nullptr};
  std::uint64_t reserved{0};

  bc_txn(bytecask::DB* d, LiveCatalog* l, std::uint64_t max, bc_memory_cap* m, bc_isolation iso, bool ro)
      : db{d}, live{l}, max_bytes{max}, isolation{iso}, read_only{ro}, memory{m} {}

  // Its statements record their reads: SERIALIZABLE, not read-only.
  [[nodiscard]] auto records() const noexcept -> bool { return isolation == BC_ISOLATION_SERIALIZABLE && !read_only; }
  ~bc_txn() { release_memory(); }
  bc_txn(const bc_txn&) = delete;
  bc_txn& operator=(const bc_txn&) = delete;
  void release_memory() noexcept {
    if (reserved != 0) memory->used.fetch_sub(std::exchange(reserved, 0), std::memory_order_relaxed);
  }

  // The overlay to write to: cloned first when a pin or a mark shares it.
  // A throw leaves the transaction's overlay and the pin's as they were.
  auto own_overlay() -> Overlay& {
    if (overlay.use_count() > 1) overlay = std::make_shared<Overlay>(*overlay);
    return *overlay;
  }

  // The copy the next write would keep for the newest mark: the overlay's
  // bytes while that mark shares it, else nothing.
  [[nodiscard]] auto mark_pending() const noexcept -> std::uint64_t {
    return !marks.empty() && marks.back().overlay == overlay ? overlay->bytes : 0;
  }

  [[nodiscard]] auto has_ddl() const noexcept -> bool { return catalog != pinned; }

  // What the catalogs would be charged with `current` the transaction's
  // catalog. The marks' catalogs only move forward, mark to mark, to the
  // current one, so each distinct one is counted once by skipping repeats.
  [[nodiscard]] auto catalog_charge(const Catalog* current) const noexcept -> std::uint64_t {
    std::uint64_t out = 0;
    const Catalog* last = pinned.get();
    auto count = [&](const Catalog* c) {
      if (c != nullptr && c != last && c != pinned.get()) out += c->estimated_bytes();
      last = c;
    };
    for (const auto& m : marks) count(m.catalog.get());
    count(current);
    return out;
  }
  void recount_catalogs() noexcept { catalog_bytes = catalog_charge(catalog.get()); }

  // Counts what the marks keep. Marks that share an overlay are adjacent (no
  // write was made between them, and a rollback drops every mark above the
  // one it restores), so each overlay is counted once; the current one is
  // counted as the overlay.
  void recount_marks() noexcept {
    marked_bytes = 0;
    const Overlay* last = nullptr;
    for (const auto& m : marks) {
      if (m.overlay == overlay || m.overlay.get() == last) continue;
      last = m.overlay.get();
      marked_bytes += m.overlay->bytes;
    }
  }
};

// A transaction's overlay as it stood when pinned: what a cursor reads
// through (docs/cursor_design.md, *What a cursor sees*). Released before its
// transaction is freed.
struct bc_overlay {
  std::shared_ptr<const Overlay> view;
  bc_txn* owner;
  std::uint64_t reserved;
  // The transaction's catalog when pinned: a hold reads the view with it.
  std::shared_ptr<const Catalog> catalog;
  // A hold cursor took this pin over (bc_txn_hold), and with it `held_share`
  // of the memory cap, which the transaction's reservation leaves out
  // (pinned_by_holds) until the pin is released.
  bool held{false};
  std::uint64_t held_share{0};
};

// What a cursor that outlives its transaction reads: its own handle on the
// snapshot (a clone, so the transaction's commit may move the original into
// its WritePlan), the catalog version and the overlay as they were pinned.
//
// It outlives the transaction, and the transaction's reservation of the
// memory cap with it, so it takes over its pin's share of the cap at
// bc_txn_hold (the copy a pin may keep, which the transaction had reserved)
// and returns it when freed. Not counted: the snapshot's tree nodes and data
// files, which no transaction's reservation counts either.
struct bc_hold {
  bytecask::Snapshot snap;
  std::shared_ptr<const Catalog> pinned;
  std::shared_ptr<const Overlay> view;
  bc_memory_cap* memory{nullptr};
  std::uint64_t reserved{0};

  bc_hold(bytecask::Snapshot s, std::shared_ptr<const Catalog> c, std::shared_ptr<const Overlay> v, bc_memory_cap* m,
          std::uint64_t r)
      : snap{std::move(s)}, pinned{std::move(c)}, view{std::move(v)}, memory{m}, reserved{r} {}
  ~bc_hold() {
    if (reserved != 0) memory->used.fetch_sub(reserved, std::memory_order_relaxed);
  }
  bc_hold(const bc_hold&) = delete;
  bc_hold& operator=(const bc_hold&) = delete;
};

namespace {
void txn_abort(bc_txn* txn) noexcept;
}  // namespace

namespace {

// What the transaction holds, and its commit's WritePlan with it: its
// overlay, which counts each node as its buffers, kOverlayNodeBytes and its
// plan entry, within the effect_bytes of the write it came from, and its
// guards, claims, locks and tables, each with its plan entry. A statement's
// write set would take it at most to this plus the write set's bytes:
// merging an effect into the overlay adds at most one node, and one that
// replaces or erases others only frees. So the commit needs nothing its
// statements were not held to.
// The marks' overlays count as the pins' do.
auto txn_holds(const bc_txn& t) -> std::uint64_t {
  return t.overlay->bytes + t.held + t.pinned_bytes + t.marked_bytes + t.plan_bytes + t.catalog_bytes + t.read_bytes;
}

// What the transaction reserves of the memory cap for itself: txn_holds but
// the pins hold cursors reserve.
auto cap_holds(const bc_txn& t) -> std::uint64_t { return txn_holds(t) - t.pinned_by_holds; }

}  // namespace

// The check each of a statement's calls makes (push_write), on the write
// set as it would be: it bounds what bc_txn_add takes in.
auto bc_ctx::txn_room(std::uint64_t bytes) -> bool {
  if (txn_holds(*txn) + txn->mark_pending() + bytes <= txn->max_bytes) return true;
  fail(BC_E_LIMIT, "the transaction's writes would pass its bound of " + std::to_string(txn->max_bytes) + " bytes");
  return false;
}

auto bc_ctx::writable(const char* command) -> bool {
  if (txn == nullptr || !txn->read_only) return true;
  fail(BC_E_READ_ONLY, std::string{"cannot execute "} + command + " in a read-only transaction");
  return false;
}

auto bc_ctx::read_charge(std::uint64_t n) -> bool {
  if (!ok()) return false;
  if (txn != nullptr) {
    if (txn_holds(*txn) + txn->mark_pending() + write_held + n > txn->max_bytes) {
      fail(BC_E_LIMIT, "the SERIALIZABLE transaction's reads would pass its bound of " + std::to_string(txn->max_bytes) +
                           " bytes");
      return false;
    }
    // The transaction reserves what it holds of the memory cap (cap_holds):
    // its read set grows between its statements' bc_txn_add, so here.
    if (txn->memory != nullptr && cap_holds(*txn) + n > txn->reserved) {
      const auto need = cap_holds(*txn) + n - txn->reserved;
      const auto grant = cap_reserve(*txn->memory, need, need);
      if (grant == 0) {
        fail(BC_E_MEMORY, cap_message(*txn->memory));
        return false;
      }
      txn->reserved += grant;
    }
    txn->read_bytes += n;
    return true;
  }
  if (budget.max_bytes != 0 && n > budget.max_bytes - std::min(mem_used, budget.max_bytes)) {
    fail(BC_E_LIMIT, "the SERIALIZABLE statement's reads would pass its memory budget of " +
                         std::to_string(budget.max_bytes) + " bytes");
    return false;
  }
  return take(n);
}

void bc_ctx::read_release(std::uint64_t n) noexcept {
  if (txn != nullptr) {
    txn->read_bytes -= std::min(n, txn->read_bytes);
  } else {
    release(n);
  }
}

auto bc_ctx::record_point(BytesView key) -> std::optional<Recorded> {
  if (!ok()) return std::nullopt;
  if (read_set == nullptr || read_set->holds(key)) return Recorded{};
  const auto n = read_point_bytes(key.size());
  if (!read_charge(n)) return std::nullopt;
  try {
    if (!read_set->add_point(key)) read_release(n);
  } catch (...) {
    read_release(n);  // nothing was added; the read is not made
    throw;
  }
  return Recorded{};
}

auto bc_ctx::record_range(BytesView lo, BytesView hi) -> std::optional<Recorded> {
  if (!ok()) return std::nullopt;
  if (read_set == nullptr || !std::ranges::lexicographical_compare(lo, hi)) return Recorded{};
  const auto n = read_range_bytes(lo.size(), hi.size());
  if (!read_charge(n)) return std::nullopt;
  std::optional<ReadSet::RangeNode> node;
  try {
    node.emplace(range_node(Bytes(lo.begin(), lo.end()), Bytes(hi.begin(), hi.end())));
  } catch (...) {
    read_release(n);
    throw;
  }
  const auto before = read_set->bytes;
  read_set->add_range(std::move(*node));
  read_release(before + n - read_set->bytes);  // what a merge freed
  return Recorded{};
}

auto bc_ctx::record_scan(Cursor& c, BytesView lo, BytesView hi) -> std::optional<Recorded> {
  if (!ok()) return std::nullopt;
  if (read_set == nullptr || !std::ranges::lexicographical_compare(lo, hi)) return Recorded{};
  // The largest range it can record: its own bounds, or one of them and a
  // key it handed out with k⁺'s byte, no longer than the key ceiling's.
  constexpr auto kKey = bytecask::sql::kMaxKeyBytes + 1;
  const auto n = read_range_bytes(std::max(lo.size(), kKey), std::max(hi.size(), kKey));
  if (!read_charge(n)) return std::nullopt;
  try {
    auto r = std::make_unique<Cursor::Read>();
    r->node = range_node({}, {});
    r->lo.assign(lo.begin(), lo.end());
    r->hi.assign(hi.begin(), hi.end());
    r->charge = n;
    c.read = std::move(r);
  } catch (...) {
    read_release(n);
    throw;
  }
  return Recorded{};
}

namespace {

// k⁺ in place (docs/transaction_design.md, *What a read records for
// phantoms*): `k` followed by a zero byte, the least key above it; past the
// key ceiling, the successor of its longest prefix that does not end in
// 0xFF, the least key above every key that starts with it. Never longer
// than the key ceiling, and never past the object's keyspace, which every
// key's storage-id prefix bounds. Allocates nothing given capacity for one
// more byte.
void successor_in_place(Bytes& k) {
  if (k.size() < bytecask::sql::kMaxKeyBytes) {
    k.push_back(std::byte{0});
    return;
  }
  while (!k.empty() && k.back() == std::byte{0xFF}) k.pop_back();
  if (!k.empty()) k.back() = static_cast<std::byte>(std::to_integer<unsigned>(k.back()) + 1);
}

}  // namespace

void bc_ctx::finish_scan(Cursor& c) noexcept {
  if (!c.read) return;
  auto r = std::move(c.read);
  if (!r->ran_out && !r->returned) {
    read_release(r->charge);  // it handed out nothing: its result depends on no key
    return;
  }
  // Every key of its range, those absent included (the empty-range
  // phantom), when it ran out; else what it covered up to the last key it
  // handed out, [lo, k⁺) forward and [k, hi) in reverse.
  Bytes* lo = &r->lo;
  Bytes* hi = &r->hi;
  if (!r->ran_out) {
    if ((c.flags & BC_SCAN_REVERSE) != 0) {
      lo = &r->last;
    } else {
      successor_in_place(r->last);  // room for its byte was reserved with the key
      hi = &r->last;
    }
  }
  // Within the reservation by its construction; should a buffer have grown
  // past it, the whole range, which the reservation counted, is recorded.
  if (read_range_bytes(lo->capacity(), hi->capacity()) > r->charge ||
      !std::ranges::lexicographical_compare(*lo, *hi)) {
    lo = &r->lo;
    hi = &r->hi;
  }
  auto& nh = r->node;
  std::swap(nh.key(), *lo);
  std::swap(nh.mapped(), *hi);
  count_transition(ReadSetTransition::Fill);
  const auto before = read_set->bytes;
  read_set->add_range(std::move(nh));
  read_release(before + r->charge - read_set->bytes);
}

void txn_detach(bc_txn* txn, bc_ctx* cx, bool abandoned) noexcept {
  if (txn->attached != cx) return;
  txn->attached = nullptr;  // first: the abort below detaches nothing more
  if (abandoned && !txn->aborted && !txn->done) txn_abort(txn);
}

namespace {

// Detaches the attached context, if one is: it reads nothing of the
// transaction after, and keeps its error and a finished statement's results.
void txn_drop_context(bc_txn* txn) noexcept {
  if (txn->attached != nullptr) txn->attached->detach_from_txn();
}

// Returns what the transaction reserves of the memory cap beyond what it
// holds, after a rollback to a mark or a release let go of some.
void txn_trim_reservation(bc_txn* txn) noexcept {
  if (txn->memory == nullptr || txn->reserved <= cap_holds(*txn)) return;
  txn->memory->used.fetch_sub(txn->reserved - cap_holds(*txn), std::memory_order_relaxed);
  txn->reserved = cap_holds(*txn);
}

// Puts the transaction back as it stood at `m`: its overlay and catalog, and
// its guards, claims, locks and tables cut back to the mark's lengths. The
// snapshot, the pinned catalog, the pins and the marks stay.
void txn_restore(bc_txn* txn, const TxnMark& m) noexcept {
  txn->overlay = m.overlay;
  txn->catalog = m.catalog ? m.catalog : txn->pinned;
  txn->backfill = m.backfill;
  txn->recount_catalogs();
  txn->guards.erase(txn->guards.begin() + static_cast<std::ptrdiff_t>(m.guards), txn->guards.end());
  txn->claims.erase(txn->claims.begin() + static_cast<std::ptrdiff_t>(m.claims), txn->claims.end());
  txn->locks.erase(txn->locks.begin() + static_cast<std::ptrdiff_t>(m.locks), txn->locks.end());
  txn->written.erase(txn->written.begin() + static_cast<std::ptrdiff_t>(m.written), txn->written.end());
  txn->held = m.held;
  txn->recount_marks();
  txn_trim_reservation(txn);
}

// Aborts the transaction: a failed statement leaves only bc_txn_free, so
// what it holds goes now, the snapshot that keeps disk blocks first. The
// contexts begun on it are detached; each keeps its error. A transaction
// with marks may be rolled back to one (bc_txn_rollback_to): it keeps its
// snapshot and catalog, and goes back to its newest mark, so what the
// failed statements wrote goes now all the same.
void txn_abort(bc_txn* txn) noexcept {
  txn->aborted = true;
  txn_drop_context(txn);
  if (!txn->marks.empty()) {
    txn_restore(txn, txn->marks.back());
    return;
  }
  txn->snap.reset();
  txn->pinned.reset();
  txn->catalog.reset();  // the private one goes with the pin
  txn->catalog_bytes = 0;
  txn->backfill = false;
  txn->overlay = std::make_shared<Overlay>();  // a pin keeps the one it holds
  txn->guards = {};
  txn->claims = {};
  txn->locks = {};
  txn->written = {};
  txn->held = 0;
  txn->plan_bytes = 0;  // the plans were for the pin dropped above
  txn->reads.clear();   // it can no longer commit: no read of it is ever submitted
  txn->read_bytes = 0;
  txn->release_memory();
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
auto view_get(const bc_ctx* cx, Recorded rec, BytesView key, Bytes& out) -> bool {
  if (cx->overlay != nullptr) {
    if (auto hit = cx->overlay->find(key)) {
      if (*hit == nullptr) return false;
      out.assign((*hit)->begin(), (*hit)->end());
      return true;
    }
  }
  return cx->view(rec).get({}, key, out);
}

auto view_contains(const bc_ctx* cx, Recorded rec, BytesView key) -> bool {
  if (cx->overlay != nullptr) {
    if (auto hit = cx->overlay->find(key)) return *hit != nullptr;
  }
  return cx->view(rec).contains_key({}, key);
}

// A point read through the view, recorded first: nullopt, with the error
// set, when it cannot be recorded or its read cannot be charged.
auto read_contains(bc_ctx* cx, BytesView key) -> std::optional<bool> {
  auto rec = cx->record_point(key);
  if (!rec || !cx->charge()) return std::nullopt;
  return view_contains(cx, *rec, key);
}

// The live keys in [lo, hi) the view holds, counted no further than
// `limit`, when the overlay meets the range: the snapshot's count, less the
// snapshot's keys in each deleted range, then corrected by one existence
// check per overlay point in the range. Each count is at most two record
// reads and each check one, all charged; the caller charged the first.
auto view_count(bc_ctx* cx, Recorded rec, BytesView lo, BytesView hi, std::size_t limit) -> std::size_t {
  constexpr auto kAll = std::numeric_limits<std::size_t>::max();
  const auto& ov = *cx->overlay;
  const auto& base = cx->view(rec);
  auto n = base.count_keys(lo, hi, kAll);
  auto r = ov.ranges.upper_bound(lo);
  if (r != ov.ranges.begin() && std::is_lt(bytes_cmp(lo, std::prev(r)->second))) --r;
  for (; r != ov.ranges.end() && std::is_lt(bytes_cmp(r->first, hi)); ++r) {
    auto a = std::is_lt(bytes_cmp(r->first, lo)) ? lo : BytesView{r->first};
    auto b = std::is_lt(bytes_cmp(hi, r->second)) ? hi : BytesView{r->second};
    if (!std::is_lt(bytes_cmp(a, b))) continue;
    if (!cx->charge(2)) return 0;
    n -= base.count_keys(a, b, kAll);
  }
  for (auto p = ov.points.lower_bound(lo); p != ov.points.end() && std::is_lt(bytes_cmp(p->first, hi)); ++p) {
    if (!cx->charge()) return 0;
    auto in_view = p->second.has_value();
    auto in_snapshot = !ov.covered(p->first) && base.contains_key({}, p->first);
    if (in_view && !in_snapshot) ++n;
    if (!in_view && in_snapshot) --n;
  }
  return std::min(n, limit);
}

// Positions a cursor's entries on the next one within bounds, which the
// caller copies; false at the end. Every entry is read through the view, and
// charged as it is read.
auto cursor_advance(bc_ctx* cx, Cursor& c) -> bool {
  if (!c.entries) {
    // The scan's bounds, inside its table's or index's keys.
    auto end = bytecask::sql::key_successor(c.prefix);
    BytesView lo = std::is_lt(bytes_cmp(c.lo, c.prefix)) ? BytesView{c.prefix} : BytesView{c.lo};
    BytesView hi = !c.hi.empty() && std::is_lt(bytes_cmp(c.hi, end)) ? BytesView{c.hi} : BytesView{end};
    c.entries.emplace(cx->view(*c.recorded), cx->overlay, lo, hi, (c.flags & BC_SCAN_REVERSE) != 0);
  }
  return c.entries->next(cx);
}

// Moves a cursor off its entry: its views go stale, and its loop's step
// ends, releasing what the step allocated. The entry's buffers keep their
// capacity, and the caller reholds the cursor once for what it holds next.
void leave_entry(bc_ctx* cx, Cursor& c) noexcept {
  if (c.scope != kNoStep) cx->end_steps_from(c.scope);
  ++c.step;
  c.positioned = false;
  c.key.clear();
  c.value.clear();
  c.row.clear();
  c.row_key.clear();
}

// Reads one record through the view into the arena. A record at a row key
// is a stored row of that key's table: in a transaction, the row as the
// transaction sees it.
auto read_record(bc_ctx* cx, BytesView key, bool* found) -> bc_view {
  if (found != nullptr) *found = false;
  auto rec = cx->record_point(key);  // found absent or present, the key is read
  if (!rec || !cx->charge()) return {};
  Bytes out;
  if (!view_get(cx, *rec, key, out)) return {};
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
    case PhysType::I64:
      // No key of the part holds a value past its width: BC_E_LIMIT, as
      // bc_key_end reports a key built from one. The compiler's plans never
      // pass one: a parameter is checked against its column's type before
      // the plan runs, and a subquery's wider integer is no key bound.
      if (!bytecask::sql::key_put_i64(buf, std::get<std::int64_t>(*d), part)) {
        cx->fail(BC_E_LIMIT, "parameter $" + std::to_string(i + 1) + " is outside its key part's integer width: no key has it");
        return false;
      }
      break;
    case PhysType::F64: bytecask::sql::key_put_f64(buf, std::get<double>(*d), part); break;
    case PhysType::Bool: bytecask::sql::key_put_bool(buf, std::get<bool>(*d), part); break;
    case PhysType::Bytes: bytecask::sql::key_put_bytes(buf, std::get<Bytes>(*d), part); break;
    case PhysType::Num: throw std::logic_error("a key part that is a NUMERIC");
    case PhysType::Jsonb: throw std::logic_error("a key part that is a jsonb");
  }
  return true;
}

// Whether a finished key is within the engine's key ceiling; BC_E_LIMIT if not.
[[nodiscard]] auto within_key_ceiling(bc_ctx* cx, BytesView key) -> bool {
  if (key.size() <= bytecask::sql::kMaxKeyBytes) return true;
  cx->fail(BC_E_LIMIT, "key over the engine's key limit");
  return false;
}

// A finished key copied into the step's arena, as a view.
auto key_view(bc_ctx* cx, BytesView key) -> bc_view {
  if (!within_key_ceiling(cx, key)) return {};
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

// Charges a builder's key as it now stands, after a part was added.
template <typename B>
void key_held(bc_ctx* cx, B& b) {
  (void)cx->rehold(b.held, b.buf.size());
}

// Appends a key builder to `table`, charged `charge`: its handle, or the
// zero handle past the budget.
template <typename H, typename B>
auto push_key_builder(bc_ctx* cx, std::vector<B>& table, B&& b, std::uint64_t charge) -> H {
  auto gen = b.gen;
  if (cx->push_charged(table, std::move(b), charge) == nullptr) return {};
  return {static_cast<std::uint32_t>(table.size() - 1), gen};
}

// Ends a key builder, once: its key as a view, the step's.
template <typename B>
auto end_key_builder(bc_ctx* cx, B& b) -> bc_view {
  if (b.done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return {};
  }
  b.done = true;
  return cx->make_view(b.buf.data(), b.buf.size());
}

// The most a sort key holds: a view's length is 32 bits.
constexpr std::uint64_t kMaxSortKeyBytes = std::numeric_limits<std::uint32_t>::max();

// The most bytes a value of `type` takes as a sort key's part, `len` the
// length of a bytes value: every zero byte escaped, and the terminator.
auto sort_value_max_bytes(PhysType type, std::size_t len) -> std::uint64_t {
  switch (type) {
    case PhysType::I64:
    case PhysType::F64: return 8;
    case PhysType::Bool: return 1;
    case PhysType::Bytes: return 2 * std::uint64_t{len} + 2;
    case PhysType::Num: throw std::logic_error("a sort key part that is a NUMERIC");
    case PhysType::Jsonb: throw std::logic_error("a sort key part that is a jsonb");
  }
  return 0;
}

// Adds the next part of sort key k, placed by dir and nulls: its marker,
// then, for a value of `type` (nullopt: a NULL, its marker alone), what
// `put` writes as a non-nullable key part, inverted under DESC. The part's
// most bytes are charged, and checked against kMaxSortKeyBytes, before any
// is allocated; then the key is charged what it holds.
template <typename Put>
void add_sort_key_part(bc_ctx* cx, bc_sort_key k, std::uint32_t dir, std::uint32_t nulls,
                       std::optional<PhysType> type, std::size_t len, Put put) {
  auto* b = cx->resolve_handle(k, cx->sort_keys, "sort key builder");
  if (b == nullptr) return;
  if (b->done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return;
  }
  if (dir != BC_SORT_ASC && dir != BC_SORT_DESC) {
    cx->fail(BC_E_HANDLE, "unknown sort direction");
    return;
  }
  if (nulls != BC_SORT_NULLS_LAST && nulls != BC_SORT_NULLS_FIRST) {
    cx->fail(BC_E_HANDLE, "unknown NULL placement");
    return;
  }
  auto most = 1 + (type ? sort_value_max_bytes(*type, len) : 0);
  if (most > kMaxSortKeyBytes - b->buf.size()) {
    cx->fail(BC_E_LIMIT, "sort key over its limit of " + std::to_string(kMaxSortKeyBytes) + " bytes");
    return;
  }
  if (!cx->rehold(b->held, b->buf.size() + most)) return;
  b->buf.reserve(b->buf.size() + most);
  if (type) {
    bytecask::sql::sort_put_present(b->buf);
    put(b->buf, KeyPart{*type, false, dir == BC_SORT_DESC});
  } else {
    bytecask::sql::sort_put_null(b->buf, nulls == BC_SORT_NULLS_FIRST);
  }
  key_held(cx, *b);
}

// Appends v as a sort or group key's integer part, which is 8 bytes wide
// and holds every i64. A narrower part is a bug in its caller: it throws
// (the guard reports it) rather than leave the part out of the key.
void key_put_wide_i64(Bytes& out, std::int64_t v, KeyPart part) {
  if (!bytecask::sql::key_put_i64(out, v, part)) {
    throw std::logic_error("a sort or group key's integer part is narrower than 8 bytes");
  }
}

// Appends a column's value, read present, as `part`: a sort or group key's.
void key_put_column(Bytes& out, const bytecask::sql::ColumnRead& v, KeyPart part) {
  switch (part.type) {
    case PhysType::I64: key_put_wide_i64(out, v.i64(), part); break;
    case PhysType::F64: bytecask::sql::key_put_f64(out, v.f64(), part); break;
    case PhysType::Bool: bytecask::sql::key_put_bool(out, v.boolean(), part); break;
    case PhysType::Bytes: bytecask::sql::key_put_bytes(out, v.bytes, part); break;
    case PhysType::Num: throw std::logic_error("a sort or group key part that is a NUMERIC");
    case PhysType::Jsonb: throw std::logic_error("a sort or group key part that is a jsonb");
  }
}

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

// What bc_sort_key_part_* found at a part: nullopt if the key is unusable (a
// failure is set), else whether the part is a NULL, and the reader positioned
// on its value when it is not. `want` is the type the caller reads, nullopt
// for bc_sort_key_part_null, which can tell a NULL from a value but not read
// one, and so leaves a value's end unknown until a typed read of it.
struct SortPartAt {
  bool null;
  bool known;  // the part's end is known: a NULL, or a value already read
  std::optional<KeyReader> reader;
};

constexpr std::byte kSortMarkerPresent{0x01};

auto sort_part_at(bc_ctx* cx, bc_view key, std::uint32_t part, std::optional<PhysType> want)
    -> std::optional<SortPartAt> {
  auto k = cx->resolve(key);
  if (!k) return std::nullopt;
  auto& sp = cx->sort_parts;
  if (sp.view.i != key.i || sp.view.g != key.g) {
    sp.view = key;
    sp.at.assign(1, 0);
    sp.type.clear();
  }
  if (part > sp.type.size()) {
    cx->fail(BC_E_HANDLE, "sort key parts are read in order: part " + std::to_string(sp.type.size()) + " first");
    return std::nullopt;
  }
  if (part < sp.type.size()) {
    if (!sp.type[part]) return SortPartAt{true, true, std::nullopt};
    if (want && *want != *sp.type[part]) {
      cx->fail(BC_E_HANDLE, "sort key part of the wrong type");
      return std::nullopt;
    }
    return SortPartAt{false, true, want ? std::optional{KeyReader{*k, sp.at[part] + 1}} : std::nullopt};
  }
  auto at = sp.at[part];
  if (at >= k->size()) {
    cx->fail(BC_E_CORRUPT, "unparseable sort key");
    return std::nullopt;
  }
  if ((*k)[at] != kSortMarkerPresent) {
    // 0x00 and 0x02 are a NULL first and last; anything else is no marker.
    if ((*k)[at] != std::byte{0x00} && (*k)[at] != std::byte{0x02}) {
      cx->fail(BC_E_CORRUPT, "unparseable sort key");
      return std::nullopt;
    }
    sp.type.push_back(std::nullopt);
    sp.at.push_back(at + 1);
    return SortPartAt{true, true, std::nullopt};
  }
  if (!want) return SortPartAt{false, false, std::nullopt};
  return SortPartAt{false, false, KeyReader{*k, at + 1}};
}

// Records the type and end of part `part`, which a typed read of it just
// found; false, with BC_E_CORRUPT set, if the key did not parse there.
auto sort_part_read(bc_ctx* cx, const SortPartAt& at, PhysType type, bool ok) -> bool {
  if (!ok) {
    cx->fail(BC_E_CORRUPT, "unparseable sort key");
    return false;
  }
  auto& sp = cx->sort_parts;
  if (!at.known) {
    sp.type.push_back(type);
    sp.at.push_back(at.reader->pos());
  }
  return true;
}

// A typed read of sort key part `part`: the value, or the neutral one for a
// NULL part. `read` takes the reader and returns the optional value.
template <typename T, typename Read>
T sort_part_value(bc_ctx* cx, bc_view key, std::uint32_t part, PhysType type, T neutral, Read read) {
  auto at = sort_part_at(cx, key, part, type);
  if (!at) return neutral;
  if (at->null) return neutral;
  auto v = read(*at->reader);
  if (!sort_part_read(cx, *at, type, v.has_value())) return neutral;
  return std::move(*v);
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

// Whether `snap` holds a key that starts with `prefix`, other than `except`
// when one is given: a unique prefix holds at most one, and a row's own
// entry next to it, so the walk reads at most two keys.
[[nodiscard]] auto holds_prefix(const bytecask::Snapshot& snap, BytesView prefix, std::optional<BytesView> except = std::nullopt)
    -> bool {
  for (const auto& key : snap.keys_from({}, prefix)) {
    if (!has_prefix(key, prefix)) return false;
    if (!except || !std::ranges::equal(key, *except)) return true;
  }
  return false;
}

// Which claims count depends on Overlay::puts_under and the entry-key layout,
// as the pruning of the unique guards at bc_txn_commit does.
// A commit that conflicted: the duplicate it is when the latest committed
// state has taken one of its claims, the first in their order (BC_FAILED,
// BC_C_UNIQUE on the claim's table or index), else the conflict. Nothing was
// written either way; a check that cannot read reports the conflict.
// The first conflict in statement order decides, as in PostgreSQL, where an
// earlier statement's lock or write of a row a concurrent commit changed
// fails at that statement: before each claim, the locks of the statements
// up to its own are checked, and one whose row changed is the conflict. A
// row changed back to the same bytes counts as unchanged.
// A transaction's claim counts while its writes still put the key, or for an
// index any entry under its value (Overlay::puts_under, which prunes the
// unique guards at bc_txn_commit too: change the two together): one a later
// statement moved or deleted away claims nothing (`overlay`, null for an
// execution, whose one statement writes a row once). An index claim is
// taken only by another row's entry: a concurrent update of the same row to
// the same value is the conflict on that row, 40001.
[[nodiscard]] auto conflict_or_duplicate(ErrorSink out, bytecask::DB& db, std::span<const Claim> claims,
                                         std::span<const Lock> locks, const Overlay* overlay) -> bc_commit_result {
  if (claims.empty()) return BC_CONFLICT;
  try {
    auto latest = db.snapshot();
    Bytes now;
    std::size_t checked = 0;
    for (const auto& c : claims) {
      if (overlay != nullptr && !(c.prefix_len ? overlay->puts_under(BytesView{c.key}.first(*c.prefix_len))
                                               : overlay->puts(c.key))) {
        continue;
      }
      for (; checked < c.locks_through; ++checked) {
        const auto& l = locks[checked];
        if (!latest.get({}, l.key, now) || row_fingerprint(now) != l.fingerprint) return BC_CONFLICT;
      }
      auto taken = c.prefix_len ? holds_prefix(latest, BytesView{c.key}.first(*c.prefix_len), BytesView{c.key})
                                : latest.contains_key({}, c.key);
      if (taken) {
        out.constraint({BC_C_UNIQUE, c.object});
        return BC_FAILED;
      }
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): the conflict stands, unclassified
  }
  return BC_CONFLICT;
}

// Submits a commit's batch, under apply_batch_or_unknown's rule: an
// unknown outcome is BC_E_UNKNOWN, never "nothing written"; any other
// failure wrote nothing. The code is set before the message, which is
// dropped if it cannot be allocated.
auto submit(ErrorSink out, bytecask::DB& db, bytecask::WritePlan plan, bool sync) -> bc_commit_result {
  try {
    return bytecask::sql::apply_batch_or_unknown(db, {.sync = sync}, std::move(plan)) ? BC_COMMITTED : BC_CONFLICT;
  } catch (const bytecask::sql::CommitOutcomeUnknown& e) {
    out.unknown(e.what());
    return BC_FAILED;
  } catch (...) {
    out.fail_current_exception();
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
    e.cols.push_back({c.table, c.col, t->layout.generation, slot->type, slot->pos, slot->width, &slot->sql});
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
      // A NUMERIC is read and set as its text, as bytes are.
      if (type && it->type != *type && !(*type == PhysType::Bytes && it->type == PhysType::Num)) {
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
// (BC_E_HANDLE), with two exceptions for `insert`, a write that puts a new
// row at the key (an INSERT, or an UPDATE moving a row's primary key): over
// a row the statement already put there it is a duplicate key, as INSERT …
// VALUES (1, …), (1, …) is, and raises BC_C_UNIQUE; into a key the statement
// vacated (an UPDATE moved its row away) it takes the key, as PostgreSQL's
// row-by-row unique check lets UPDATE t SET id = id - 1 do. The keys and the
// deleted ranges are sorted: a lookup each per write.
[[nodiscard]] auto first_write(bc_ctx* cx, const Table& t, BytesView pk, bool present, bool insert = false) -> bool {
  if (auto it = cx->written_rows.find(pk); it != cx->written_rows.end()) {
    if (insert && !it->second) {
      it->second = true;
      return true;
    }
    if (insert) {
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
  if (!cx->take(buffer_bytes(pk.size()) + kWrittenRowNodeBytes)) return false;
  cx->written_rows.emplace(Bytes(pk.begin(), pk.end()), present);
  return true;
}

// Whether this statement vacated row key `pk` (moved or deleted its row). It
// is then free whatever the view holds: the vacating write is in this plan,
// and the engine checks it for a change since the snapshot.
[[nodiscard]] auto vacated_here(const bc_ctx* cx, BytesView pk) -> bool {
  auto it = cx->written_rows.find(pk);
  return it != cx->written_rows.end() && !it->second;
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
// A key free in the view is a claim when the snapshot holds it free too: one
// free only because an earlier statement deleted it conflicts with the row
// deleted, a 40001 in PostgreSQL as here.
[[nodiscard]] auto pk_free(bc_ctx* cx, const Table& t, BytesView pk) -> bool {
  auto rec = cx->record_point(pk);
  if (!rec || !cx->charge()) return false;
  if (view_contains(cx, *rec, pk)) {
    cx->raise(BC_C_UNIQUE, t.id);
    return false;
  }
  // Free in the view; when the overlay decided it, ask the snapshot.
  if (cx->overlay != nullptr && cx->overlay->find(pk)) {
    if (!cx->charge()) return false;
    if (cx->view(*rec).contains_key({}, pk)) return true;
  }
  return cx->push_claim(t.id, pk, std::nullopt);
}

// A row a transaction's statement locks or writes, kept for the commit's
// conflict order. One the overlay holds was an earlier statement's write,
// recorded then, or its insert, a claim; an execution's one statement has
// no statements before it.
void lock_row(bc_ctx* cx, BytesView pk, BytesView row) {
  if (cx->txn == nullptr || (cx->overlay != nullptr && cx->overlay->find(pk))) return;
  (void)cx->push_lock(pk, row_fingerprint(row));
}

// Records a unique prefix this execution gives a row, charged first: false,
// and its charge gone, when it already gave one a row, which the view cannot see.
[[nodiscard]] auto record_unique(bc_ctx* cx, const Index& ix, BytesView prefix) -> bool {
  const auto n = buffer_bytes(prefix.size()) + kUniqueNodeBytes;
  if (!cx->take(n)) return false;
  if (!cx->unique_written.emplace(ix.id, Bytes(prefix.begin(), prefix.end())).second) {
    cx->release(n);
    return false;
  }
  return true;
}

// Records a unique prefix an update takes from a row, charged first.
[[nodiscard]] auto vacate_unique(bc_ctx* cx, const Index& ix, BytesView prefix) -> bool {
  const auto n = buffer_bytes(prefix.size()) + kUniqueNodeBytes;
  if (!cx->take(n)) return false;
  if (!cx->unique_vacated.emplace(ix.id, Bytes(prefix.begin(), prefix.end())).second) cx->release(n);
  return true;
}

// Whether the view holds an entry with this unique prefix, nullopt with the
// error set; the caller charged the first read. `claim` is set to whether
// the snapshot holds it free too: as pk_free, a key free only because an
// earlier statement deleted it is no claim.
[[nodiscard]] auto prefix_taken(bc_ctx* cx, BytesView prefix, BytesView end, bool& claim) -> std::optional<bool> {
  claim = true;
  auto rec = cx->record_range(prefix, end);
  if (!rec) return std::nullopt;
  const auto& base = cx->view(*rec);
  if (cx->overlay != nullptr && cx->overlay->meets(prefix, end)) {
    // The overlay holds entries with this prefix, or deleted some: the
    // first entry the view holds at or after it decides, each read charged.
    ViewEntries entries{base, cx->overlay, prefix, end, false};
    auto taken = entries.next(cx);
    if (!cx->ok()) return std::nullopt;
    if (taken) return true;
    if (!cx->charge()) return std::nullopt;
    claim = !holds_prefix(base, prefix);
    return false;
  }
  return holds_prefix(base, prefix);
}

// No entry has this unique index prefix in the view, and no earlier write
// of this execution gave a row it, or BC_C_UNIQUE on the index; the range
// guard covers the race, since a duplicate is another row's entry, a key
// this plan does not write.
// The guard and the claim it records are pruned at commit by
// Overlay::puts_under (bc_txn_commit, conflict_or_duplicate): they count only
// while the transaction's final writes put an entry under this prefix. That
// depends on the entry-key layout, the index prefix first.
[[nodiscard]] auto unique_free(bc_ctx* cx, const Index& ix, const Entry& entry) -> bool {
  auto prefix = entry.prefix();
  if (!cx->charge()) return false;
  if (!record_unique(cx, ix, prefix)) {
    if (cx->ok()) cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  auto end = bytecask::sql::key_successor(prefix);
  auto claim = true;
  auto taken = prefix_taken(cx, prefix, end, claim);
  if (!taken) return false;
  if (*taken) {
    cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  cx->push_effect(EffectKind::GuardUniqueRange, prefix, std::move(end));
  return !claim || cx->push_claim(ix.id, entry.key, entry.prefix_len);
}


// Column `col` of a row view, read by row_column: nullopt, with the error
// set, for a bad handle or a malformed column (BC_E_CORRUPT). An absent
// column (NULL) reads with its word 0 and its bytes empty.
// Out of line: the message is built only on damage, off the read path.
[[gnu::cold, gnu::noinline]] void fail_malformed(bc_ctx* cx, std::uint32_t col) {
  cx->fail(BC_E_CORRUPT, "malformed row: column " + std::to_string(col) + " does not decode");
}

auto read_column(bc_ctx* cx, const RowCol& rc) -> std::optional<bytecask::sql::ColumnRead> {
  auto v = bytecask::sql::row_column(rc.row, rc.slot->col, rc.slot->type, rc.slot->pos, rc.slot->width);
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

namespace {

// Commit step 2 (docs/transaction_design.md, *Mechanism: the serializable
// commit*): every read range as ensure_range_unchanged and every read point
// as ensure_unchanged, but a point `written` says the final writes put or
// delete, which the engine checks already. A point a hand-written
// bc_guard_present or bc_guard_absent sits on goes as the one-key range
// [k, k⁺), which means the same: the engine takes one precondition per key.
template <class Written>
void guard_reads(bytecask::WritePlan& plan, const ReadSet& reads, std::span<const Effect> guards, Written&& written) {
  auto hand = [](const Effect& e) { return e.kind == EffectKind::GuardPresent || e.kind == EffectKind::GuardAbsent; };
  const auto any_hand = std::ranges::any_of(guards, hand);
  for (const auto& [lo, hi] : reads.ranges) plan.ensure_range_unchanged(lo, hi);
  for (const auto& k : reads.points) {
    if (written(BytesView{k})) continue;
    if (any_hand && std::ranges::any_of(guards, [&](const Effect& e) { return hand(e) && e.a == k; })) {
      Bytes hi = k;
      successor_in_place(hi);
      plan.ensure_range_unchanged(k, hi);
    } else {
      plan.ensure_unchanged(k);
    }
  }
}

// Commit step 1: whether the final writes only delete keys the snapshot does
// not hold, so write nothing. One lookup per tombstone, and none once a put
// or a range is found.
auto deletes_only_absent(const Overlay& ov, const bytecask::Snapshot& snap) -> bool {
  if (!ov.ranges.empty()) return false;
  if (std::ranges::any_of(ov.points, [](const auto& p) { return p.second.has_value(); })) return false;
  return std::ranges::none_of(ov.points, [&](const auto& p) { return snap.contains_key({}, p.first); });
}

// Commit step 5: whether a key the commit wrote changed between its
// snapshot, `before`, and the latest state: the "concurrent update" of a
// SERIALIZABLE conflict, else a read/write dependency. Run only after a
// conflict; `keys` visits the written keys. A probe that fails answers
// "changed", the message of any conflict.
template <class Keys>
auto written_changed(const bytecask::Snapshot& before, bytecask::DB& db, Keys&& keys) noexcept -> bool {
  try {
    auto latest = db.snapshot();
    auto changed = false;
    Bytes then, now;
    keys([&](BytesView k) {
      if (changed) return;
      const auto had = before.get({}, k, then);
      const auto has = latest.get({}, k, now);
      changed = had != has || (had && then != now);
    });
    return changed;
  } catch (...) {
    return true;
  }
}

}  // namespace

void bc_ctx_begin_frame(bc_ctx* cx, const bc_params* params, bc_budget budget) {
  BC_GUARD_BEGIN
  cx->drop_execution_state();
  cx->sink().clear();
  cx->params = params;
  cx->budget = budget;
  BC_GUARD_END(cx)
}

// ------------------------------------------------------------- procedures

namespace {

auto proc_failed(const bc_proc* px) -> bool { return px->error != nullptr || !px->frame->ok(); }

// The hook's exception is the procedure's error; the frame fails so that
// every later call does nothing.
void proc_fail(bc_proc* px) noexcept {
  if (px->error == nullptr) px->error = std::current_exception();
  px->frame->fail(BC_E_HANDLE, "the procedure failed");
}

template <typename T>
void proc_set(bc_proc* px, uint32_t var, T v) {
  if (proc_failed(px)) return;
  try {
    if (var >= px->slots.values.size()) {
      px->frame->fail(BC_E_HANDLE, "no variable " + std::to_string(var));
      return;
    }
    bc_value d{std::move(v)};
    if (auto why = bytecask::sql::out_of_range(px->types[var], d)) {
      px->frame->raise(BC_C_RANGE, 0, *why);
      return;
    }
    px->slots.values[var] = std::move(d);
  } catch (...) {
    proc_fail(px);
  }
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------- runtime

bc_ctx* bc_ctx_new(bc_db* db, const bc_catalog* cat) {
  try {
    auto* cx = new bc_ctx(db->db, cat->live);
    cx->memory = db->memory;
    return cx;
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
  cx->sink().clear();
  cx->params = params;
  cx->budget = budget;
  auto p = cx->live->pin();
  cx->pinned = std::move(p.cat);
  cx->cat = cx->pinned.get();
  cx->snap.emplace(std::move(p.snap));
  cx->set_view(&*cx->snap);
  BC_GUARD_END(cx)
}

bc_ctx* bc_proc_ctx(bc_proc* px) { return px->frame; }

bc_ctx* bc_stmt_begin(bc_proc* px, uint32_t stmt) {
  if (proc_failed(px)) return px->frame;
  try {
    return px->begin(stmt);
  } catch (...) {
    proc_fail(px);
    return px->frame;
  }
}

void bc_stmt_end(bc_proc* px, uint32_t stmt) {
  if (proc_failed(px)) return;
  try {
    px->end(stmt);
  } catch (...) {
    proc_fail(px);
  }
}

void bc_var_set_i64(bc_proc* px, uint32_t var, int64_t v) { proc_set(px, var, std::int64_t{v}); }
void bc_var_set_f64(bc_proc* px, uint32_t var, double v) { proc_set(px, var, v); }
void bc_var_set_bool(bc_proc* px, uint32_t var, bool v) { proc_set(px, var, v); }

void bc_var_set_bytes(bc_proc* px, uint32_t var, bc_view v) {
  if (proc_failed(px)) return;
  std::optional<BytesView> b;
  try {
    b = px->frame->resolve(v);
  } catch (...) {
    proc_fail(px);
    return;
  }
  if (!b) return;  // a stale view: the frame has failed
  proc_set(px, var, Bytes(b->begin(), b->end()));
}

void bc_var_set_null(bc_proc* px, uint32_t var) { proc_set(px, var, std::monostate{}); }

bool bc_rows_next(bc_proc* px, uint32_t stmt) {
  if (proc_failed(px) || !px->frame->charge()) return false;
  try {
    return px->next(stmt);
  } catch (...) {
    proc_fail(px);
    return false;
  }
}

void bc_rows_done(bc_proc* px, uint32_t stmt) {
  if (proc_failed(px)) return;
  try {
    px->done(stmt);
  } catch (...) {
    proc_fail(px);
  }
}

bool bc_loop(bc_proc* px) {
  if (proc_failed(px)) return false;
  return px->frame->charge();
}

void bc_range_open(bc_proc* px, uint32_t range, int64_t lo, bool lo_null, int64_t hi, bool hi_null, bool reverse) {
  if (proc_failed(px)) return;
  try {
    if (lo_null || hi_null) {
      throw bytecask::sql::ProcedureError(std::string{lo_null ? "lower" : "upper"} + " bound of FOR loop cannot be null",
                                          bytecask::sql::kNullValueNotAllowed);
    }
    if (px->ranges.size() <= range) px->ranges.resize(range + 1);
    // REVERSE counts from the first bound down to the second, as PostgreSQL's does.
    px->ranges[range] = {lo, hi, reverse, reverse ? lo < hi : lo > hi};
  } catch (...) {
    proc_fail(px);
  }
}

bool bc_range_next(bc_proc* px, uint32_t range, uint32_t var) {
  if (proc_failed(px) || range >= px->ranges.size()) return false;
  auto& r = px->ranges[range];
  if (r.done || !px->frame->charge()) return false;
  auto v = r.next;
  if (v == r.last) {
    r.done = true;
  } else {
    r.next = r.reverse ? v - 1 : v + 1;
  }
  bc_var_set_i64(px, var, v);
  return !proc_failed(px);
}

void bc_ctx_begin_txn(bc_ctx* cx, bc_txn* txn, const bc_params* params, bc_budget budget) {
  BC_GUARD_BEGIN
  cx->drop_execution_state();
  cx->sink().clear();
  cx->params = params;
  cx->budget = budget;
  if (txn->aborted || txn->done) {
    cx->fail(BC_E_HANDLE, txn->done ? "the transaction has ended" : "the transaction is aborted");
    return;
  }
  // One context per transaction: another one's statement is still open.
  if (txn->attached != nullptr) {
    cx->fail(BC_E_HANDLE, "another context is running a statement of this transaction");
    return;
  }
  // Attached before anything that can fail, and with nothing that can: a
  // statement that fails while it begins must still abort its transaction
  // at bc_txn_add.
  cx->txn = txn;
  txn->attached = cx;
  // The snapshot and its catalog version are the first statement's: pinned
  // together, as an execution pins them.
  if (!txn->snap) {
    auto p = txn->live->pin();
    txn->pinned = std::move(p.cat);
    txn->catalog = txn->pinned;
    txn->snap.emplace(std::move(p.snap));
  }
  // The transaction's catalog, its own DDL included: every check the run
  // makes (verify, layouts, index maintenance) reads it.
  cx->pinned = txn->catalog;
  cx->cat = cx->pinned.get();
  cx->set_view(&*txn->snap);
  cx->overlay = txn->overlay.get();
  // Its reads join the transaction's read set: those of a procedure's
  // statements too, which run without bc_ctx_run.
  cx->read_set = txn->records() ? &txn->reads : nullptr;
  BC_GUARD_END(cx)
}

bool bc_ctx_check(bc_ctx* cx, const bc_schema_ref* checked_against) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return false;
  if (!cx->has_view()) {
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
  // Where its reads are recorded (docs/transaction_design.md, *Mechanism:
  // the transaction's read set*): a SERIALIZABLE transaction's read set,
  // unless it was begun read-only (bc_ctx_begin_txn); an autocommit
  // execution's own, unless its plan writes no table, when its reads could
  // never be submitted.
  if (cx->txn != nullptr) {
    // A read-only transaction refuses a statement that writes before it
    // runs, whether or not it would reach a row, as PostgreSQL does.
    if (!cx->verified.written.empty() && !cx->writable("a statement that writes")) return;
  } else if (cx->isolation == BC_ISOLATION_SERIALIZABLE && cx->snap && !cx->verified.written.empty()) {
    cx->read_set = &cx->own_reads;
  } else {
    cx->read_set = nullptr;
  }
  // limit 0 wants no row: the plan, verified, reads nothing.
  cx->unwinding = cx->windowed && cx->window_limit == 0;
  plan(cx);
  cx->stopped = cx->unwinding;
  cx->unwinding = false;
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
  // An execution that writes nothing commits nothing: its guards protect
  // writes, and what its one statement read is fixed by its snapshot (a
  // SELECT … FOR UPDATE in autocommit). The engine, which may be refusing
  // writes, is not asked. A transaction checks a guard-only commit instead:
  // its later statements may have acted on what it read.
  if (std::ranges::none_of(cx->effects, is_write, &Effect::kind)) {
    cx->effects.clear();
    return BC_COMMITTED;
  }
  if (!cx->snap) {
    // Committed already (the write plan took the snapshot), or never begun.
    cx->fail(BC_E_HANDLE, "bc_ctx_commit without a run to commit");
    return BC_FAILED;
  }
  cx->drop_cursors();  // a cursor's range joins the read set as it closes
  // SERIALIZABLE: the snapshot as it was, which a conflict's message reads
  // (commit step 5); a reference count.
  std::optional<bytecask::Snapshot> before;
  if (cx->read_set != nullptr) before.emplace(cx->snap->clone());
  // The plan's copies were charged with the effects they copy, and the
  // tables' record guards with the tables (effect_bytes, table_bytes), and
  // the read set's with its entries (read_point_bytes, read_range_bytes).
  bytecask::WritePlan plan{std::move(*cx->snap)};
  cx->snap.reset();
  for (const auto& e : cx->effects) {
    switch (e.kind) {
      case EffectKind::Put: plan.put(e.a, e.b); break;
      case EffectKind::Del: plan.del(e.a); break;
      case EffectKind::DelRange: plan.del_range(e.a, e.b); break;
      case EffectKind::GuardUnchanged: plan.ensure_unchanged(e.a); break;
      case EffectKind::GuardRange:
      case EffectKind::GuardUniqueRange: plan.ensure_range_unchanged(e.a, e.b); break;
      case EffectKind::GuardPresent: plan.ensure_present(e.a); break;
      case EffectKind::GuardAbsent: plan.ensure_absent(e.a); break;
    }
  }
  // A DDL on a table this plan writes, committed since the snapshot,
  // changes the table's record: the commit is then a conflict, and the
  // retry pins the new catalog version, whose indexes it maintains.
  for (auto id : cx->written_tables) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
  // SERIALIZABLE (commit steps 2 and 4): what it read, unchanged at its
  // commit, and the catalog every object it used was bound through. A key
  // it writes is checked by the engine already, and its guard repeats that.
  if (cx->read_set != nullptr) {
    guard_reads(plan, *cx->read_set, cx->effects, [](BytesView) { return false; });
    plan.ensure_unchanged(bytecask::sql::catalog_version_key());
  }
#ifdef BYTECASK_SQL_TESTING
  if (cx->before_commit) cx->before_commit();
  if (!cx->ok()) return BC_FAILED;  // the hook failed the statement (bc_ctx_fail_for_testing)
#endif
  auto r = submit(cx->sink(), *cx->db, std::move(plan), sync);
  if (r == BC_CONFLICT && before) {
    cx->rw_conflict = !written_changed(*before, *cx->db, [&](auto&& visit) {
      for (const auto& e : cx->effects) {
        if (e.kind == EffectKind::Put || e.kind == EffectKind::Del) visit(BytesView{e.a});
      }
    });
  }
  cx->effects.clear();  // handed to the engine: a second commit has nothing to submit
  return r;
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

void bc_ctx_window(bc_ctx* cx, uint64_t skip, uint64_t limit) {
  BC_GUARD_BEGIN
  if (!cx->has_view() || cx->emitted != 0 || cx->windowed) {
    cx->fail(BC_E_HANDLE, "bc_ctx_window needs a begun context that has run nothing, once");
    return;
  }
  cx->windowed = true;
  cx->window_skip = skip;
  cx->window_limit = limit;
  BC_GUARD_END(cx)
}

uint64_t bc_ctx_emitted(const bc_ctx* cx) { return cx->emitted; }

bool bc_ctx_stopped(const bc_ctx* cx) { return cx->stopped; }

enum bc_error_code bc_error(const bc_ctx* cx) { return cx->error; }

const char* bc_error_message(const bc_ctx* cx) { return cx->message.c_str(); }

const char* bc_error_note(const bc_ctx* cx) { return cx->note.c_str(); }

const char* bc_error_hint(const bc_ctx* cx) { return cx->hint.c_str(); }

const char* bc_error_context(const bc_ctx* cx) { return cx->context.c_str(); }

uint32_t bc_error_detail(const bc_ctx* cx) { return cx->detail; }

int64_t bc_ctx_affected(const bc_ctx* cx) { return cx->affected; }

}  // extern "C"

bool bc_txn_conflict_read_write(const bc_txn* txn) noexcept { return txn->rw_conflict; }
bool bc_ctx_conflict_read_write(const bc_ctx* cx) noexcept { return cx->rw_conflict; }

#ifdef BYTECASK_SQL_TESTING
namespace {
auto contents_of(const ReadSet& r) -> ReadSetContents {
  ReadSetContents out;
  out.points.assign(r.points.begin(), r.points.end());
  for (const auto& [lo, hi] : r.ranges) out.ranges.emplace_back(lo, hi);
  out.bytes = r.bytes;
  out.charged = r.bytes;
  return out;
}
}  // namespace

ReadSetContents bc_txn_reads_for_testing(const bc_txn* txn) {
  auto out = contents_of(txn->reads);
  out.charged = txn->read_bytes;
  return out;
}

ReadSetContents bc_ctx_reads_for_testing(const bc_ctx* cx) { return contents_of(cx->own_reads); }

auto read_successor_for_testing(Bytes k) -> Bytes {
  k.reserve(k.size() + 1);
  successor_in_place(k);
  return k;
}

auto read_set_transitions_for_testing(bool reset) -> std::array<std::uint64_t, kReadSetTransitions> {
  std::array<std::uint64_t, kReadSetTransitions> out{};
  for (std::size_t i = 0; i < kReadSetTransitions; ++i) {
    out.at(i) = reset ? g_read_set_transitions.at(i).exchange(0) : g_read_set_transitions.at(i).load();
  }
  return out;
}

ReadSetContents read_set_apply_for_testing(std::span<const ReadSetOp> ops) {
  ReadSet r;
  for (const auto& op : ops) {
    if (op.hi.empty()) {
      (void)r.add_point(op.lo);
    } else {
      r.add_range(range_node(op.lo, op.hi));
    }
  }
  return contents_of(r);
}
#endif

#ifdef BYTECASK_SQL_TESTING
void bc_ctx_before_commit_for_testing(bc_ctx* cx, std::function<void()> hook) { cx->before_commit = std::move(hook); }
void bc_ctx_fail_for_testing(bc_ctx* cx, bc_error_code code, bytecask::sql::SqlState state, const char* message,
                             const char* note, const char* hint, const char* context) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  switch (code) {
    case BC_E_DATA: cx->fail(BC_E_DATA, state, message, {note, hint, context}); return;
    case BC_E_ARITH: cx->fail(BC_E_ARITH, state, message, {note, hint, context}); return;
    case BC_OK:
    case BC_E_IO:
    case BC_E_CORRUPT:
    case BC_E_MEMORY:
    case BC_E_BUDGET:
    case BC_E_LIMIT:
    case BC_E_HANDLE:
    case BC_E_CONSTRAINT:
    case BC_E_LAYOUT:
    case BC_E_DEGRADED:
    case BC_E_CLOSED:
    case BC_E_UNKNOWN:
    case BC_E_PATTERN:
    case BC_E_CARDINALITY:
    case BC_E_READ_ONLY: break;
  }
  cx->fail(BC_E_HANDLE, "bc_ctx_fail_for_testing takes BC_E_DATA or BC_E_ARITH");
  BC_GUARD_END(cx)
}
std::uint64_t bc_ctx_held_for_testing(const bc_ctx* cx) { return cx->mem_used; }
std::size_t bc_ctx_write_set_slots_for_testing(const bc_ctx* cx) {
  return cx->effects.capacity() + cx->claims.capacity() + cx->locks.capacity() + cx->written_tables.capacity();
}
#endif

extern "C" {

// ------------------------------------------------------------ transactions

bc_txn* bc_txn_new(bc_db* db, const bc_catalog* cat, uint64_t max_bytes, enum bc_isolation isolation, bool read_only) {
  try {
    return new bc_txn(db->db, cat->live, max_bytes, db->memory, isolation, read_only);
  } catch (...) {
    return nullptr;
  }
}

bool bc_txn_queried(const bc_txn* txn) {
  return txn->statements != 0 || txn->pins != 0 || txn->attached != nullptr || !txn->reads.empty();
}

bool bc_txn_set_mode(bc_txn* txn, enum bc_isolation isolation, bool read_only) {
  // Before anything has read: a read made at one level is never judged at another.
  if (bc_txn_queried(txn) || !txn->marks.empty() || txn->aborted || txn->done) return false;
  txn->isolation = isolation;
  txn->read_only = read_only;
  return true;
}

void bc_ctx_set_isolation(bc_ctx* cx, enum bc_isolation isolation) { cx->isolation = isolation; }

void bc_txn_free(bc_txn* txn) {
  if (txn == nullptr) return;
  txn_drop_context(txn);
  delete txn;
}

uint64_t bc_txn_catalog_version(const bc_txn* txn) { return txn->pinned ? txn->pinned->version() : 0; }

bool bc_txn_repin(bc_txn* txn) {
  // A pin reads the snapshot: moving it would change what the cursor sees.
  // A read made at the old snapshot is never guarded against a new one.
  if (txn->statements != 0 || txn->pins != 0 || txn->aborted || txn->done || txn->attached != nullptr ||
      !txn->reads.empty()) {
    return false;
  }
  txn->snap.reset();
  txn->pinned.reset();
  txn->catalog.reset();
  return true;
}

}  // extern "C"

namespace {

// Reserves what the transaction holds plus `extra` on the memory cap, when
// there is one; false, with the transaction's error set, if the cap has no room.
auto txn_reserve_with(bc_txn* txn, std::uint64_t extra) -> bool {
  if (txn->memory == nullptr || cap_holds(*txn) + extra <= txn->reserved) return true;
  const auto need = cap_holds(*txn) + extra - txn->reserved;
  const auto grant = cap_reserve(*txn->memory, need, need);
  if (grant == 0) {
    txn->sink().fail(BC_E_MEMORY, cap_message(*txn->memory));
    return false;
  }
  txn->reserved += grant;
  return true;
}

// The snapshot and catalog of the first statement, pinned together.
void txn_pin(bc_txn* txn) {
  if (txn->snap) return;
  auto p = txn->live->pin();
  txn->pinned = std::move(p.cat);
  txn->catalog = txn->pinned;
  txn->snap.emplace(std::move(p.snap));
}

}  // namespace

auto bc_txn_pinned_catalog(bc_txn* txn) -> std::shared_ptr<const bytecask::sql::Catalog> {
  if (txn->aborted || txn->done || txn->attached != nullptr) {
    throw std::logic_error("bc_txn_pinned_catalog needs an open transaction with no statement open");
  }
  txn_pin(txn);
  return txn->pinned;
}

auto bc_txn_catalog(bc_txn* txn) -> std::shared_ptr<const bytecask::sql::Catalog> {
  (void)bc_txn_pinned_catalog(txn);
  return txn->catalog;
}

auto bc_txn_has_ddl(const bc_txn* txn) noexcept -> bool { return txn->has_ddl(); }

auto bc_txn_current_catalog(const bc_txn* txn) noexcept -> const Catalog* { return txn->catalog.get(); }

auto bc_txn_first_statement(const bc_txn* txn) noexcept -> bool { return txn->statements == 0 && txn->pins == 0; }

auto bc_txn_charge_plans(bc_txn* txn, std::uint64_t bytes) noexcept -> bool {
  try {
    txn->sink().clear();
    if (txn_holds(*txn) + bytes > txn->max_bytes) {
      txn->sink().fail(BC_E_LIMIT, "the transaction's plans would pass its bound of " + std::to_string(txn->max_bytes) +
                                       " bytes");
      return false;
    }
    if (!txn_reserve_with(txn, bytes)) return false;
    txn->plan_bytes += bytes;
    return true;
  } catch (...) {
    txn->fail_current_exception();
    return false;
  }
}

void bc_txn_release_plans(bc_txn* txn, std::uint64_t bytes) noexcept {
  txn->plan_bytes -= std::min(bytes, txn->plan_bytes);
  txn_trim_reservation(txn);
}

#ifdef BYTECASK_SQL_TESTING
auto bc_txn_plan_bytes_for_testing(const bc_txn* txn) noexcept -> std::uint64_t { return txn->plan_bytes; }
auto bc_txn_cap_holds_for_testing(const bc_txn* txn) noexcept -> std::uint64_t { return cap_holds(*txn); }
#endif

extern "C" {

bc_overlay* bc_txn_overlay_pin(bc_txn* txn) { return bc_txn_overlay_pin_reserving(txn, 0); }

}  // extern "C"

auto bc_txn_overlay_pin_reserving(bc_txn* txn, std::uint64_t extra) -> bc_overlay* {
  BC_GUARD_BEGIN
  txn->sink().clear();
  if (txn->aborted || txn->done) {
    txn->sink().fail(BC_E_HANDLE, txn->done ? "the transaction has ended" : "the transaction is aborted");
    return nullptr;
  }
  if (txn->attached != nullptr) {
    txn->sink().fail(BC_E_HANDLE, "a statement of this transaction is still open on a context: reset it first");
    return nullptr;
  }
  // The snapshot and catalog are the transaction's from here, as a first
  // statement would pin them.
  txn_pin(txn);
  const auto reserve = txn->overlay->bytes + extra;
  if (txn_holds(*txn) + reserve > txn->max_bytes) {
    txn->sink().fail(BC_E_LIMIT, "a cursor's copy of the transaction's writes would pass its bound of " +
                                     std::to_string(txn->max_bytes) + " bytes");
    txn_abort(txn);
    return nullptr;
  }
  // The transaction holds the copy from here (txn_holds counts it): reserved
  // on the memory cap now, not at the next statement's bc_txn_add, which a
  // transaction that only fetches never runs.
  if (txn->memory != nullptr && cap_holds(*txn) + reserve > txn->reserved) {
    const auto need = cap_holds(*txn) + reserve - txn->reserved;
    const auto grant = cap_reserve(*txn->memory, need, need);
    if (grant == 0) {
      txn->sink().fail(BC_E_MEMORY, cap_message(*txn->memory));
      txn_abort(txn);
      return nullptr;
    }
    txn->reserved += grant;
  }
  auto pin = std::make_unique<bc_overlay>(bc_overlay{txn->overlay, txn, reserve, txn->catalog});
  txn->pinned_bytes += reserve;
  ++txn->pins;
  return pin.release();
  BC_GUARD_END(txn)
  if (txn != nullptr) txn_abort(txn);
  return nullptr;
}

extern "C" {

bc_hold* bc_txn_hold(bc_txn* txn, bc_overlay* pin) {
  BC_GUARD_BEGIN
  txn->sink().clear();
  // The pin made the snapshot and catalog the transaction's.
  if (txn->aborted || txn->done || !txn->snap || !txn->pinned || pin == nullptr || pin->owner != txn) {
    txn->sink().fail(BC_E_HANDLE, "bc_txn_hold needs a live transaction and an overlay pinned from it");
    return nullptr;
  }
  if (pin->held) {
    txn->sink().fail(BC_E_HANDLE, "bc_txn_hold: a hold cursor holds this pin already");
    return nullptr;
  }
  // The pin's share moves from the transaction's reservation (the pin
  // reserved it, so the transaction holds at least that much) to the hold's.
  // What moves is what the transaction stops counting: the two stay equal.
  const auto moved = txn->memory != nullptr ? std::min(pin->reserved, txn->reserved) : 0;
  // The catalog the cursor was declared under: a table the transaction
  // created is in it, read after the commit as the view holds its rows.
  auto* hold = new bc_hold{txn->snap->clone(), pin->catalog, pin->view, txn->memory, moved};
  txn->reserved -= moved;
  txn->pinned_by_holds += moved;
  pin->held = true;
  pin->held_share = moved;
  return hold;
  BC_GUARD_END(txn)
  if (txn != nullptr) txn_abort(txn);
  return nullptr;
}

void bc_txn_hold_free(bc_hold* hold) { delete hold; }

void bc_ctx_begin_hold(bc_ctx* cx, const bc_hold* hold, const bc_params* params, bc_budget budget) {
  BC_GUARD_BEGIN
  cx->drop_execution_state();
  cx->sink().clear();
  cx->params = params;
  cx->budget = budget;
  cx->pinned = hold->pinned;
  cx->cat = cx->pinned.get();
  cx->set_view(&hold->snap);
  cx->overlay = hold->view.get();
  BC_GUARD_END(cx)
}

void bc_txn_overlay_release(bc_overlay* pin) {
  if (pin == nullptr) return;
  pin->owner->pinned_bytes -= pin->reserved;
  if (pin->held) pin->owner->pinned_by_holds -= pin->held_share;
  --pin->owner->pins;
  delete pin;
}

void bc_ctx_use_overlay(bc_ctx* cx, const bc_overlay* pin) {
  BC_GUARD_BEGIN
  if (cx->txn == nullptr || pin->owner != cx->txn || !cx->ok()) {
    cx->fail(BC_E_HANDLE, "bc_ctx_use_overlay needs a context begun on the transaction the overlay was pinned from");
    return;
  }
  cx->overlay = pin->view.get();
  BC_GUARD_END(cx)
}

namespace {

// Whether a guard of the transaction goes to the engine. A unique check a
// later statement made stale guards nothing: the final writes hold no entry
// under its value. The same Overlay::puts_under decides the claims in
// conflict_or_duplicate.
auto guard_counts(const Effect& e, const Overlay& final_writes) -> bool {
  return e.kind != EffectKind::GuardUniqueRange || final_writes.puts_under(e.a);
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
  cx->drop_cursors();
  if (!cx->ok()) {
    txn_abort(txn);
    return false;
  }
  // The bound needs no check here: each of the statement's writes was held
  // to it, on the write set it reached (push_write).
  // Aborted until the statement is wholly in: a failure part-way through
  // leaves an overlay no commit may use.
  txn->aborted = true;
  for (auto id : cx->written_tables) {
    if (std::ranges::find(txn->written, id) == txn->written.end()) {
      txn->written.push_back(id);
      txn->held += table_bytes(id);
    }
  }
  auto writes = std::ranges::any_of(cx->effects, [](const Effect& e) {
    return e.kind == EffectKind::Put || e.kind == EffectKind::Del || e.kind == EffectKind::DelRange;
  });
  if (writes) {
    (void)txn->own_overlay();  // before the first write: a clone that fails changes nothing
    txn->recount_marks();      // a mark that shared the overlay keeps the copy: charged now
  }
  for (auto& e : cx->effects) {
    switch (e.kind) {
      case EffectKind::Put: txn->overlay->put(std::move(e.a), std::move(e.b)); break;
      case EffectKind::Del: txn->overlay->del(std::move(e.a)); break;
      case EffectKind::DelRange: txn->overlay->del_range(std::move(e.a), std::move(e.b)); break;
      case EffectKind::GuardUnchanged:
      case EffectKind::GuardRange:
      case EffectKind::GuardUniqueRange:
      case EffectKind::GuardPresent:
      case EffectKind::GuardAbsent:
        txn->held += effect_bytes(true, e);
        txn->guards.push_back(std::move(e));
        break;
    }
  }
  cx->effects.clear();
  // This statement's claims come after its own locks too: its old rows are
  // locked before the values it puts are checked.
  const auto locks_through = txn->locks.size() + cx->locks.size();
  for (auto& c : cx->claims) {
    c.locks_through = locks_through;
    txn->held += claim_bytes(c.key.capacity());
    txn->claims.push_back(std::move(c));
  }
  cx->claims.clear();
  for (auto& l : cx->locks) {
    txn->held += lock_bytes(l.key.capacity());
    txn->locks.push_back(std::move(l));
  }
  cx->locks.clear();
  // The write set is the transaction's now, and off the statement's budget.
  cx->release(cx->write_held);
  // What the transaction holds grew by at most the write set (cap_holds;
  // a cursor's pin reserved its own share when taken): the statement's
  // reservation, at least what it held, covers it and passes to the
  // transaction. The rest of it is returned when the statement ends. Should
  // it not cover it, the difference is reserved as any other, and past the
  // cap the statement fails with BC_E_MEMORY and aborts the transaction:
  // `used` never passes the cap unchecked, failing other sessions' statements.
  if (txn->memory != nullptr && cap_holds(*txn) > txn->reserved) {
    const auto grow = cap_holds(*txn) - txn->reserved;
    const auto spare = cx->reserved - cx->mem_used;
    if (grow > spare && !cx->reserve(grow - spare)) {
      txn_abort(txn);
      return false;
    }
    cx->reserved -= grow;
    txn->reserved += grow;
  }
  cx->write_held = 0;
  ++txn->statements;
  txn->aborted = false;
  return true;
  BC_GUARD_END(cx)
  if (txn != nullptr) txn_abort(txn);
  return false;
}

}  // extern "C"

namespace {

// A transaction's view of one key prefix: its snapshot's entries with its
// overlay over them, merged in key order, as a statement of it reads them.
// What a DDL's backfill reads in a transaction (Catalog::next_version), so
// a row the transaction wrote is indexed or rewritten and one it deleted is
// not. Holds the overlay it was made on, and the transaction's snapshot by
// reference: used while the DDL runs, before anything else of the
// transaction can.
class TxnRows final : public bytecask::sql::RowSource {
public:
  TxnRows(const bytecask::Snapshot& snap, std::shared_ptr<const Overlay> overlay)
      : snap_{snap}, overlay_{std::move(overlay)} {}
  ~TxnRows() override;
  TxnRows(const TxnRows&) = delete;
  TxnRows& operator=(const TxnRows&) = delete;

  void scan(BytesView prefix, const std::function<void(BytesView, BytesView)>& visit) const override {
    const auto& ov = *overlay_;
    auto entries = snap_.iter_from({}, prefix);
    auto s = entries.begin();
    auto p = ov.points.lower_bound(prefix);
    auto snap_live = [&] { return s != entries.end() && has_prefix((*s).key, prefix); };
    auto point_live = [&] { return p != ov.points.end() && has_prefix(p->first, prefix); };
    for (;;) {
      // A snapshot entry the overlay decides is the overlay's to give, or to hide.
      while (snap_live() && (ov.points.contains((*s).key) || ov.covered((*s).key))) ++s;
      const auto from_snap = snap_live();
      const auto from_point = point_live();
      if (!from_snap && !from_point) return;
      if (from_point && (!from_snap || std::ranges::lexicographical_compare(p->first, (*s).key))) {
        if (p->second) visit(p->first, *p->second);
        ++p;
      } else {
        visit((*s).key, (*s).value);
        ++s;
      }
    }
  }

private:
  const bytecask::Snapshot& snap_;
  std::shared_ptr<const Overlay> overlay_;
};

TxnRows::~TxnRows() = default;

}  // namespace

auto bc_txn_rows(bc_txn* txn) -> std::unique_ptr<bytecask::sql::RowSource> {
  (void)bc_txn_pinned_catalog(txn);  // the snapshot is the transaction's from here
  return std::make_unique<TxnRows>(*txn->snap, txn->overlay);
}

auto bc_txn_ddl(bc_txn* txn, std::shared_ptr<const Catalog> next, std::vector<Catalog::DdlOp>& ops,
                bool backfill) noexcept -> bool {
  using Kind = Catalog::DdlOp::Kind;
  try {
    txn->sink().clear();
    if (txn->aborted || txn->done || txn->attached != nullptr || !txn->snap || !next) {
      txn->sink().fail(BC_E_HANDLE, "bc_txn_ddl needs an open transaction with no statement open, and a catalog");
      return false;
    }
    if (txn->read_only) {
      txn->sink().fail(BC_E_READ_ONLY, "cannot execute DDL in a read-only transaction");
      txn_abort(txn);
      return false;
    }
    // The ops of a DDL that changed the catalog guard and bump its version
    // (docs/catalog_design.md, *DDL*): the commit's one version guard then
    // stands for the name and record guards dropped below.
    const auto version = bytecask::sql::catalog_version_key();
    const auto bumps = std::ranges::any_of(ops, [&](const auto& op) { return op.kind == Kind::Put && op.a == version; });
    const auto guards = std::ranges::any_of(ops, [&](const auto& op) {
      return op.kind == Kind::EnsureUnchanged && op.a == version;
    });
    if (!bumps || !guards) {
      txn->sink().fail(BC_E_HANDLE, "a DDL's batch that does not guard and bump the catalog version");
      return false;
    }
    // Aborted until the DDL is wholly in, as a statement's writes are.
    txn->aborted = true;
    (void)txn->own_overlay();  // before the first write: a clone that fails changes nothing
    txn->recount_marks();      // a mark that shared the overlay keeps the copy
    for (auto& op : ops) {
      switch (op.kind) {
        case Kind::Put: txn->overlay->put(std::move(op.a), std::move(op.b)); break;
        case Kind::Del: txn->overlay->del(std::move(op.a)); break;
        case Kind::DelRange: txn->overlay->del_range(std::move(op.a), std::move(op.b)); break;
        case Kind::EnsureRangeUnchanged: {
          // A backfill's rows: a write to them since the snapshot is the commit's conflict.
          Effect e{EffectKind::GuardRange, std::move(op.a), std::move(op.b)};
          const auto bytes = effect_bytes(true, e);
          txn->guards.push_back(std::move(e));
          txn->held += bytes;
          break;
        }
        // Checked against the head at commit, a name guard would fail a
        // transaction that drops a name and takes it again; the commit's
        // version guard covers what they guard.
        case Kind::EnsureAbsent:
        case Kind::EnsureUnchanged: break;
      }
      Bytes{}.swap(op.a);  // freed once the overlay holds it: a backfill is in memory about once
      Bytes{}.swap(op.b);
    }
    const auto charge = txn->catalog_charge(next.get());
    if (txn_holds(*txn) - txn->catalog_bytes + charge > txn->max_bytes) {
      txn->sink().fail(BC_E_LIMIT, "the transaction's DDL would pass its bound of " + std::to_string(txn->max_bytes) +
                                       " bytes; run the DDL outside a transaction");
      txn_abort(txn);
      return false;
    }
    if (!txn_reserve_with(txn, charge > txn->catalog_bytes ? charge - txn->catalog_bytes : 0)) {
      txn_abort(txn);
      return false;
    }
    // Last, and nothing here throws: the DDL is the transaction's.
    txn->catalog = std::move(next);
    txn->catalog_bytes = charge;
    txn->backfill = txn->backfill || backfill;
    ++txn->statements;
    txn->aborted = false;
    return true;
  } catch (...) {
    txn->fail_current_exception();
    txn_abort(txn);
    return false;
  }
}

extern "C" {

uint32_t bc_txn_savepoint(bc_txn* txn) {
  BC_GUARD_BEGIN
  txn->sink().clear();
  if (txn->aborted || txn->done || txn->attached != nullptr) {
    txn->sink().fail(BC_E_HANDLE, txn->done      ? "the transaction has ended"
                                  : txn->aborted ? "the transaction is aborted"
                                                 : "a statement of this transaction is still open on a context: reset it first");
    return 0;
  }
  if (txn->marks.size() >= std::numeric_limits<std::uint32_t>::max()) {
    txn->sink().fail(BC_E_LIMIT, "too many savepoints");
    return 0;
  }
  // The mark shares the overlay: it charges nothing until a write clones it.
  txn->marks.push_back(TxnMark{txn->overlay, txn->guards.size(), txn->claims.size(), txn->locks.size(),
                               txn->written.size(), txn->held, txn->has_ddl() ? txn->catalog : nullptr,
                               txn->backfill});
  return static_cast<std::uint32_t>(txn->marks.size());
  BC_GUARD_END(txn)
  return 0;  // nothing was pushed: the transaction is as it was
}

bool bc_txn_rollback_to(bc_txn* txn, uint32_t depth) {
  BC_GUARD_BEGIN
  txn->sink().clear();
  if (txn->done || depth == 0 || depth > txn->marks.size() || txn->attached != nullptr) {
    txn->sink().fail(BC_E_HANDLE, "bc_txn_rollback_to needs a live mark of a transaction with no statement open");
    return false;
  }
  // Allocates nothing: the marks above go, and the vectors are cut back.
  txn->marks.erase(txn->marks.begin() + depth, txn->marks.end());
  txn_restore(txn, txn->marks.back());
  txn->aborted = false;
  return true;
  BC_GUARD_END(txn)
  return false;  // only a refusal's message can throw, before anything changed
}

bool bc_txn_release(bc_txn* txn, uint32_t depth) {
  BC_GUARD_BEGIN
  txn->sink().clear();
  if (txn->aborted || txn->done || depth == 0 || depth > txn->marks.size() || txn->attached != nullptr) {
    txn->sink().fail(BC_E_HANDLE, "bc_txn_release needs a live mark of an open transaction with no statement open");
    return false;
  }
  txn->marks.erase(txn->marks.begin() + (depth - 1), txn->marks.end());
  txn->recount_marks();
  txn->recount_catalogs();
  txn_trim_reservation(txn);
  return true;
  BC_GUARD_END(txn)
  return false;  // only a refusal's message can throw, before anything changed
}

}  // extern "C"

namespace {

// The commit of a transaction that ran DDL (docs/transaction_design.md,
// *Mechanism: committing and publishing a transaction's catalog changes*):
// its plan, writes and guards built as any transaction's, published through
// LiveCatalog::commit, as an autocommit DDL is, so its catalog takes effect
// with its batch or not at all, synced. One guard on the catalog version
// stands for the catalog-record guards: every DDL published since the
// snapshot moved it.
auto commit_ddl(bc_txn* txn, ErrorSink out, bytecask::WritePlan plan) -> bc_commit_result {
  plan.ensure_unchanged(bytecask::sql::catalog_version_key());
  LiveCatalog::Prepared p{txn->pinned, txn->catalog, std::make_unique<bytecask::WritePlan>(std::move(plan)),
                          txn->backfill};
  bool applied = false;
  try {
    applied = txn->live->commit(std::move(p));
  } catch (const bytecask::sql::DdlInProgress&) {
    return BC_CONFLICT;  // another DDL is publishing: this one does not wait
  } catch (const bytecask::sql::CommitOutcomeUnknown& e) {
    out.unknown(e.what());
    return BC_FAILED;
  } catch (...) {
    out.fail_current_exception();
    return BC_FAILED;
  }
  if (applied) return BC_COMMITTED;
  // Which guard failed: with a DDL published since, a taken key may be a
  // private id's collision, not a duplicate, and the version guard failed
  // anyway: 40001. With none, it is classified as any transaction's.
  try {
    if (txn->live->head_version() != txn->pinned->version()) return BC_CONFLICT;
  } catch (...) {
    return BC_CONFLICT;
  }
  return conflict_or_duplicate(out, *txn->db, txn->claims, txn->locks, txn->overlay.get());
}

}  // namespace

extern "C" {

enum bc_commit_result bc_txn_commit(bc_txn* txn, bool sync) {
  // It touches no context, and reports through the transaction.
  BC_GUARD_BEGIN
  auto out = txn->sink();
  if (txn->done) {
    // The commit that ended it keeps its outcome: a BC_E_UNKNOWN stays one.
    if (txn->error == BC_OK) out.fail(BC_E_HANDLE, "the transaction has ended");
    return BC_FAILED;
  }
  out.clear();
  if (txn->aborted) {
    out.fail(BC_E_HANDLE, "the transaction is aborted: roll it back");
    return BC_FAILED;
  }
  // A statement still open reads the snapshot the commit would move: the
  // commit is refused, and the transaction is as it was.
  if (txn->attached != nullptr) {
    out.fail(BC_E_HANDLE, "a statement of this transaction is still open on a context: reset it first");
    return BC_FAILED;
  }
  txn->done = true;
  const auto& ov = *txn->overlay;
  // A transaction that neither writes nor guards commits nothing. One that
  // only guarded (SELECT … FOR UPDATE) is still checked: a guarded row a
  // concurrent commit changed is its conflict. The engine checks a
  // guard-only plan and writes nothing.
  // Guards a later statement made stale are pruned first: a transaction left
  // with none of them and no write sends the engine nothing.
  auto counts = [&](const Effect& e) { return guard_counts(e, ov); };
  // SERIALIZABLE (docs/transaction_design.md, *Mechanism: the serializable
  // commit*): a transaction whose final writes are empty and that ran no DDL
  // is a reader, serialized at its snapshot, and submits none of its reads
  // (step 1). One that writes submits them all (step 2).
  // Its final writes are the final overlay: at SERIALIZABLE, one that only
  // deletes keys its snapshot does not hold (an insert deleted again) writes
  // nothing, and is a reader too: its tombstones are dropped with its reads.
  const auto writes = !ov.empty() && !(txn->records() && !txn->has_ddl() && deletes_only_absent(ov, *txn->snap));
  const auto serial = txn->records() && (writes || txn->has_ddl());
  if (!writes && std::ranges::none_of(txn->guards, counts)) {
    txn->snap.reset();
    return BC_COMMITTED;
  }
  // The snapshot as it was, which a conflict's message reads (step 5).
  std::optional<bytecask::Snapshot> before;
  if (serial) before.emplace(txn->snap->clone());
  // The plan copies every write and guard while the overlay still holds
  // them, which a conflict reads back after the submit. Each copy was held
  // to max_bytes with the statement that made it (txn_holds): no check here.
  // A unique guard pruned here stays in txn->held: the count only
  // overstates what the plan copies, so it stays a bound.
  // The ranges before the points: a point is newer than any range over it,
  // and the engine applies a plan's operations in order.
  bytecask::WritePlan plan{std::move(*txn->snap)};
  txn->snap.reset();
  for (const auto& [lo, hi] : ov.ranges) {
    if (writes) plan.del_range(lo, hi);
  }
  for (const auto& [key, value] : ov.points) {
    if (!writes) break;
    if (value) {
      plan.put(key, *value);
    } else {
      plan.del(key);
    }
  }
  for (const auto& g : txn->guards) {
    if (!counts(g)) continue;
    switch (g.kind) {
      case EffectKind::GuardUnchanged: plan.ensure_unchanged(g.a); break;
      case EffectKind::GuardRange:
      case EffectKind::GuardUniqueRange: plan.ensure_range_unchanged(g.a, g.b); break;
      case EffectKind::GuardPresent: plan.ensure_present(g.a); break;
      case EffectKind::GuardAbsent: plan.ensure_absent(g.a); break;
      case EffectKind::Put:
      case EffectKind::Del:
      case EffectKind::DelRange: break;  // never kept as guards
    }
  }
  if (serial) guard_reads(plan, txn->reads, txn->guards, [&](BytesView k) { return ov.find(k).has_value(); });
  auto r = BC_FAILED;
  if (txn->has_ddl()) {
    r = commit_ddl(txn, out, std::move(plan));  // it guards the catalog version
  } else {
    // As bc_ctx_commit: a DDL on a table the transaction wrote, committed
    // since its snapshot, makes the commit a conflict.
    for (auto id : txn->written) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
    // SERIALIZABLE (step 4): every object it was bound through is as it was.
    if (serial) plan.ensure_unchanged(bytecask::sql::catalog_version_key());
    r = submit(out, *txn->db, std::move(plan), sync);
    if (r == BC_CONFLICT) r = conflict_or_duplicate(out, *txn->db, txn->claims, txn->locks, txn->overlay.get());
  }
  if (r == BC_CONFLICT && before) {
    txn->rw_conflict = !written_changed(*before, *txn->db, [&](auto&& visit) {
      for (const auto& [key, value] : ov.points) visit(BytesView{key});
    });
  }
  return r;
  BC_GUARD_END(txn)
  return BC_FAILED;  // the guard caught a throw: never BC_COMMITTED
}

enum bc_error_code bc_txn_error(const bc_txn* txn) { return txn->error; }

const char* bc_txn_error_message(const bc_txn* txn) { return txn->message.c_str(); }

uint32_t bc_txn_error_detail(const bc_txn* txn) { return txn->detail; }

int bc_execute(bc_ctx* cx, bc_plan_fn plan, const bc_schema_ref* ref, const bc_params* params, bc_budget budget,
               bool sync, uint32_t max_retries) {
  // Only a conflict is retried: an error, a raised BC_E_CONSTRAINT included,
  // is BC_FAILED and final.
  for (std::uint32_t attempt = 0;; ++attempt) {
    bc_ctx_begin(cx, params, budget);
    bc_ctx_run(cx, plan, ref);
    auto r = bc_ctx_commit(cx, sync);
    if (r != BC_CONFLICT) return r;
    // A retry's own checks see what the conflict was; the last is told the
    // way a transaction's commit is.
    if (attempt >= max_retries) return conflict_or_duplicate(cx->sink(), *cx->db, cx->claims, {}, nullptr);
  }
}

// ------------------------------------------------- parameters and results

// ------------------------------------------------------------------ jsonb
//
// docs/jsonb_design.md, *Plan API for jsonb*: a value is parsed and printed
// by sql/jsonb.cppm, every buffer charged to the statement's budget, every
// user error BC_E_DATA with its SQLSTATE, DETAIL and CONTEXT, and a stored
// value that breaks a decoding rule BC_E_CORRUPT.

extern "C++" {
namespace {

namespace jsonb = bytecask::sql::jsonb;

auto text_of(BytesView b) -> std::string_view { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

auto jsonb_memory(bc_ctx* cx) -> jsonb::Memory {
  return {cx, [](void* self, std::uint64_t n, jsonb::Site) { return static_cast<bc_ctx*>(self)->take(n); },
          [](void* self, std::uint64_t n) noexcept { static_cast<bc_ctx*>(self)->release(n); }};
}

// Charged bytes from one of the context's arenas.
struct ArenaOut {
  bc_ctx* cx;
  Arena* arena;
  static auto alloc(void* self, std::size_t n) -> std::byte* {
    auto& o = *static_cast<ArenaOut*>(self);
    if (!o.cx->take(n)) return nullptr;
    return o.arena->alloc(n);
  }
};

// Charged bytes in a buffer the caller frees and releases (`held`).
struct BufferOut {
  bc_ctx* cx;
  Bytes buf;
  std::uint64_t held{0};
  static auto alloc(void* self, std::size_t n) -> std::byte* {
    auto& o = *static_cast<BufferOut*>(self);
    if (!o.cx->take(n)) return nullptr;
    o.held += n;
    o.buf.resize(n);
    return o.buf.data();
  }
};

// A failed jsonb call's error. A refused charge has set it (BC_E_LIMIT).
void fail_jsonb(bc_ctx* cx, const jsonb::Outcome& r) {
  switch (r.status) {
    case jsonb::Status::Ok: return;
    case jsonb::Status::Data: {
      const auto& e = *r.error;
      cx->fail(BC_E_DATA, e.state, e.message, {e.detail, e.hint, e.context});
      return;
    }
    case jsonb::Status::Refused:
      if (cx->ok()) cx->fail(BC_E_LIMIT, "the statement would hold more than its memory budget");
      return;
    case jsonb::Status::Corrupt: cx->fail(BC_E_CORRUPT, std::string{"a stored jsonb value does not decode: "} + r.corrupt); return;
  }
}

// Text parsed into the arena: the node's handle, the step's (or the pins'
// for `pinned`, until reset).
auto parse_into(bc_ctx* cx, BytesView text, bool pinned) -> bc_jsonb {
  ArenaOut out{cx, pinned ? &cx->pins : &cx->arena};
  std::span<std::byte> value;
  auto r = jsonb::parse(text_of(text), jsonb_memory(cx), {&out, ArenaOut::alloc}, value);
  if (!r.ok()) {
    fail_jsonb(cx, r);
    return {};
  }
  auto node = jsonb::node_of(value);
  if (!pinned) return cx->make_jsonb(node->data(), node->size());
  auto gen = cx->next_gen();
  if (cx->push_charged(cx->pinned_views,
                       ViewEntry{node->data(), 0, gen, static_cast<std::uint32_t>(node->size()), kNoCursor, 0,
                                 RowKind::None, true},
                       kViewEntryBytes) == nullptr) {
    return {};
  }
  return {static_cast<std::uint32_t>(cx->pinned_views.size() - 1) | kPinnedView, gen};
}

// A node's text, charged, in a buffer the caller releases.
auto node_text(bc_ctx* cx, BytesView node, BufferOut& out) -> std::optional<std::span<char>> {
  std::span<char> text;
  auto r = jsonb::to_text(node, jsonb_memory(cx), {&out, BufferOut::alloc}, text);
  if (!r.ok()) {
    fail_jsonb(cx, r);
    return std::nullopt;
  }
  return text;
}

// The stored value of `node` (its format byte, then the node) set into a
// row's column, after the full check; its bytes stay charged to the
// builder until the row is ended.
void set_jsonb(bc_ctx* cx, const SetTarget& at, std::uint32_t col, BytesView node) {
  if (auto r = jsonb::check_node(node, jsonb_memory(cx)); !r.ok()) {
    fail_jsonb(cx, r);
    return;
  }
  if (node.size() >= jsonb::kMaxBytes) {
    cx->fail(BC_E_DATA, bytecask::sql::kProgramLimitExceeded, "a jsonb value exceeds the maximum of 268435455 bytes");
    return;
  }
  if (!cx->take(node.size() + 1)) return;
  at.builder->held += node.size() + 1;
  Bytes value;
  value.reserve(node.size() + 1);
  value.push_back(std::byte{jsonb::kFormat});
  value.insert(value.end(), node.begin(), node.end());
  if (!at.builder->writer.set_bytes(col, value)) cx->fail(BC_L_ROW, "column is not in the row's layout");
}

// A parameter's text parsed into a row's jsonb column.
void set_jsonb_text(bc_ctx* cx, const SetTarget& at, std::uint32_t col, BytesView text) {
  BufferOut out{cx, {}};
  std::span<std::byte> value;
  auto r = jsonb::parse(text_of(text), jsonb_memory(cx), {&out, BufferOut::alloc}, value);
  if (!r.ok()) {
    cx->release(out.held);
    fail_jsonb(cx, r);
    return;
  }
  // The parser's value passes the full check by construction: set as it is.
  at.builder->held += out.held;
  if (!at.builder->writer.set_bytes(col, value)) cx->fail(BC_L_ROW, "column is not in the row's layout");
}

}  // namespace
}  // extern "C++"

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
  // A parameter a subquery or bc_sub_arg_* set can be set again: its view
  // carries the generation it read, and goes stale with it.
  if (i < cx->sub_params.size() && cx->sub_params[i].value) {
    return cx->make_view(v->data(), v->size(), kParamOwner | i, cx->sub_params[i].gen);
  }
  return cx->make_view(v->data(), v->size());
  BC_GUARD_END(cx)
  return {};
}

bc_jsonb bc_param_jsonb(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  const auto* v = cx->param_as<Bytes>(i, "text");
  if (v == nullptr) return {};
  // One a subquery or bc_sub_arg_* set can be set again: parsed per call,
  // the step's. The caller's: once per execution, kept until reset.
  if (i < cx->sub_params.size() && cx->sub_params[i].value) return parse_into(cx, *v, false);
  if (cx->jsonb_params.size() <= i) cx->jsonb_params.resize(cx->params->values.size());
  auto& cached = cx->jsonb_params[i];
  if (cached.g == 0) cached = parse_into(cx, *v, true);
  return cached;
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
  if (!cx->has_ref) {
    cx->fail(BC_E_HANDLE, "no plan is running: bc_ctx_run sets the schema ref that declares the result layout");
    return;
  }
  // A SELECT may declare no result types (SELECT FROM t); a plan that writes
  // tables and declares none returns no rows, but for its subqueries'.
  if (!cx->sub && cx->verified.result_types.empty() && !cx->verified.written.empty()) {
    cx->fail(BC_L_PLAN, "a plan that writes declares no result types, so it returns no rows");
    return;
  }
  if (cx->emit) {
    cx->fail(BC_L_PLAN, "result row already begun");
    return;
  }
  cx->emit.emplace(cx->sub ? cx->sub->layout : *cx->verified.result_layout);
  cx->emit_col = 0;
  BC_GUARD_END(cx)
}

// C++ linkage: overloads and a template, inside the extern "C" block.
extern "C++" {
namespace {
// The next result column, which must be of `type` (any, when nullopt).
auto emit_slot(bc_ctx* cx, std::optional<PhysType> type) -> bool {
  if (!cx->ok()) return false;
  if (cx->sub) {  // a subquery's row: one column of its type, or EXISTS's of any, not kept
    if (cx->emit && cx->sub->kind == BC_SUB_EXISTS) return false;
    if (!cx->emit || cx->emit_col >= 1 || (type && cx->sub->type != *type)) {
      cx->fail(BC_L_PLAN, "emitted column does not match the subquery's type");
      return false;
    }
    return true;
  }
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

// A jsonb node as the next result column: its text, charged while it is
// written and released once the row holds it.
void emit_jsonb_text(bc_ctx* cx, BytesView node) {
  BufferOut out{cx, {}};
  auto text = node_text(cx, node, out);
  if (text) emit_bytes(cx, std::as_bytes(*text));
  cx->release(out.held);
}

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

// ---------------------------------------------------------------- subqueries

static auto group_bytes(const bc_ctx::Agg& a, std::size_t key_bytes) -> std::uint64_t;
// What the MIN and MAX values of bytes an aggregate holds are charged.
// The vector, once made: its block; and each value: its bytes and
// kAggHeldBytes.
static auto held_value_bytes(const bc_ctx::Agg& a) -> std::uint64_t {
  if (!a.held) return 0;
  std::uint64_t n = kAllocBytes + sizeof(std::vector<Bytes>);
  for (const auto& b : *a.held) n += b.size() + kAggHeldBytes;
  return n;
}

extern "C++" {
namespace {
// A BC_SUB_SET's value as its set holds it: the value's word or bytes, a
// DOUBLE's -0.0 as 0.0 and every NaN as one, as PostgreSQL's = compares them.
auto sub_key(const Datum& v) -> Bytes {
  auto word = [](std::uint64_t w) {
    Bytes out(8);
    std::memcpy(out.data(), &w, 8);
    return out;
  };
  if (const auto* i = std::get_if<std::int64_t>(&v)) return word(std::bit_cast<std::uint64_t>(*i));
  if (const auto* d = std::get_if<double>(&v)) {
    auto x = std::isnan(*d) ? std::numeric_limits<double>::quiet_NaN() : (*d == 0.0 ? 0.0 : *d);
    return word(std::bit_cast<std::uint64_t>(x));
  }
  if (const auto* b = std::get_if<bool>(&v)) return Bytes{std::byte{*b ? std::uint8_t{1} : std::uint8_t{0}}};
  return std::get<Bytes>(v);
}

// a < b, both non-NULL values of one type, in PostgreSQL's order: bytes
// bytewise, as the layer's strings compare, and NaN above every DOUBLE.
auto sub_less(const Datum& a, const Datum& b) -> bool {
  if (const auto* x = std::get_if<double>(&a)) {
    auto y = std::get<double>(b);
    return !std::isnan(*x) && (std::isnan(y) || *x < y);
  }
  if (const auto* x = std::get_if<Bytes>(&a)) return std::ranges::lexicographical_compare(*x, std::get<Bytes>(b));
  return a < b;  // int64 or bool, one alternative
}

// The row a subquery emitted, folded as its kind says.
void end_sub_row(bc_ctx* cx) {
  auto& s = *cx->sub;
  if (cx->emit && s.kind == BC_SUB_EXISTS) {
    cx->emit.reset();
    ++s.rows;
    return;
  }
  if (!cx->emit || cx->emit_col != 1) {
    cx->fail(BC_L_PLAN, "a subquery's row ended with the wrong number of columns");
    return;
  }
  Bytes row;
  if (!cx->emit->finish(row)) {
    cx->fail(BC_E_LIMIT, "subquery row over the row format's limit");
    return;
  }
  cx->emit.reset();
  auto r = bytecask::sql::row_column(row, s.layout.columns[0]);
  Datum v;
  if (r.state == bytecask::sql::ColumnState::Malformed) {
    cx->fail(BC_E_CORRUPT, "a subquery's row does not parse");
    return;
  }
  if (r.state == bytecask::sql::ColumnState::Value) {
    switch (s.type) {
      case PhysType::I64: v = r.i64(); break;
      case PhysType::F64: v = r.f64(); break;
      case PhysType::Bool: v = r.boolean(); break;
      case PhysType::Bytes:
      case PhysType::Num: v = Bytes(r.bytes.begin(), r.bytes.end()); break;
      case PhysType::Jsonb: throw std::logic_error("a subquery of jsonb");  // refused at bc_sub_begin
    }
  }
  ++s.rows;
  auto null = std::holds_alternative<std::monostate>(v);
  switch (s.kind) {
    case BC_SUB_VALUE:
      if (s.rows > 1) {
        cx->fail(BC_E_CARDINALITY, "more than one row returned by a subquery used as an expression");
        return;
      }
      s.value = std::move(v);
      return;
    case BC_SUB_EXISTS: return;
    case BC_SUB_BOUNDS:
      if (null) {
        s.saw_null = true;
        return;
      }
      if (!s.lo || sub_less(v, *s.lo)) s.lo = v;
      if (!s.hi || sub_less(*s.hi, v)) s.hi = std::move(v);
      return;
    case BC_SUB_SET: {
      if (null) {
        s.saw_null = true;
        return;
      }
      auto& set = cx->sub_sets[s.id];
      auto k = sub_key(v);
      auto bytes = k.size() + kHashEntryBytes;
      if (set.values.contains(k) || !cx->take(bytes)) return;
      set.values.insert(std::move(k));
      set.held += bytes;
      return;
    }
    default: cx->fail(BC_L_PLAN, "unknown subquery kind");
  }
}

// How many parameters a kind sets.
auto sub_params_of(std::int32_t kind) -> std::uint32_t {
  return kind == BC_SUB_BOUNDS ? 4 : kind == BC_SUB_SET ? 2 : 1;
}

auto sub_has(bc_ctx* cx, std::uint32_t sub, const Datum& v) -> bool {
  if (!cx->ok()) return false;
  auto it = cx->sub_sets.find(sub);
  if (it == cx->sub_sets.end()) {
    // A set of no values is never put: one not run is a plan bug, as
    // bc_sub_begin records each id it ran.
    cx->fail(BC_E_HANDLE, "no BC_SUB_SET subquery " + std::to_string(sub) + " has run");
    return false;
  }
  return it->second.values.contains(sub_key(v));
}

// Whether parameters [i, i + n) are all ones the caller passed as NULL: the
// only ones a plan may set, so it never overwrites a value it was given.
auto caller_null(const bc_ctx* cx, std::uint32_t i, std::uint32_t n) -> bool {
  if (cx->params == nullptr || std::uint64_t{i} + n > cx->params->values.size()) return false;
  for (auto j = i; j < i + n; ++j)
    if (!std::holds_alternative<std::monostate>(cx->params->values[j])) return false;
  return true;
}

// Sets parameter i, one the caller passed as NULL, for the rest of the plan
// to read: its bytes charged, and a new generation, so a view of the old
// value is stale. False, and the error, past the budget.
[[nodiscard]] auto set_sub_param(bc_ctx* cx, std::uint32_t i, Datum d) -> bool {
  if (cx->sub_params.empty()) cx->sub_params.resize(cx->params->values.size());
  auto& p = cx->sub_params[i];
  const auto* b = std::get_if<Bytes>(&d);
  if (!cx->rehold(p.held, b == nullptr ? 0 : b->size())) return false;
  p.value = std::move(d);
  p.gen = cx->next_gen();
  return true;
}

// What a sorter, a hash table and an aggregate are charged, as their opens
// and adds take it: released when a subquery that opened them ends.
auto held_bytes(const Sorter& s) -> std::uint64_t {
  std::uint64_t n = kSorterBytes;
  for (const auto& e : s.entries) n += e.data.size() + kSortEntryBytes;
  return n;
}
auto held_bytes(const Hash& h) -> std::uint64_t {
  std::uint64_t n = kHashBytes;
  for (const auto& e : h.entries) n += e.payload.size() + kHashEntryBytes;
  for (const auto& [key, chain] : h.index) n += key.size();
  return n;
}
auto held_bytes(const bc_ctx::Agg& a) -> std::uint64_t {
  std::uint64_t n = kAggBytes + std::uint64_t{a.slots} * sizeof(SlotKind);
  for (const auto& [key, i] : a.index) n += group_bytes(a, key.size());
  return n + held_value_bytes(a);
}

// Drops the structures of `v` past its first n, and releases their charge.
template <typename T>
void truncate_held(bc_ctx* cx, std::vector<T>& v, std::size_t n) noexcept {
  std::uint64_t released = 0;
  for (auto i = n; i < v.size(); ++i) released += held_bytes(v[i]);
  v.erase(v.begin() + static_cast<std::ptrdiff_t>(n), v.end());
  cx->release(released);
}

}  // namespace
}  // extern "C++"

void bc_sub_begin(bc_ctx* cx, uint32_t sub, int32_t kind, int32_t type, uint32_t param) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  auto t = type > 0 && type < 256 ? phys_type(static_cast<std::uint8_t>(type)) : std::nullopt;
  // A subquery of jsonb folds its sort key (stage 2): not built.
  if (cx->sub || cx->emit || !t || *t == PhysType::Jsonb || kind < BC_SUB_VALUE || kind > BC_SUB_SET ||
      !caller_null(cx, param, sub_params_of(kind))) {
    cx->fail(BC_E_HANDLE, "bc_sub_begin: a subquery running, a row begun, an unknown kind or type, or its "
                          "parameters past the given ones or ones the caller gave");
    return;
  }
  std::array<PhysType, 1> types{*t};
  bc_ctx::Sub s;
  s.id = sub;
  s.kind = kind;
  s.type = *t;
  s.param = param;
  s.layout = bytecask::sql::make_result_layout(types);
  s.mark = cx->mark();
  s.steps = cx->steps.size();
  s.top = cx->steps.empty() ? 0 : cx->steps.back().id;
  s.aggs = cx->aggs.size();
  s.sorters = cx->sorters.size();
  s.hashes = cx->hashes.size();
  // A run again, with new arguments: its set starts empty, and what the
  // last run's set held is released.
  if (auto it = cx->sub_sets.find(sub); it != cx->sub_sets.end()) {
    cx->release(it->second.held);
    cx->sub_sets.erase(it);
  }
  cx->sub.emplace(std::move(s));
  if (kind == BC_SUB_SET) cx->sub_sets[sub];  // run, so bc_sub_has_* knows it
  BC_GUARD_END(cx)
}

void bc_sub_end(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (!cx->sub || cx->emit) {
    cx->fail(BC_E_HANDLE, "bc_sub_end: no subquery running, or a row begun");
    return;
  }
  auto s = std::move(*cx->sub);
  cx->sub.reset();
  // What the run made goes, its sorters, hash tables and aggregates with
  // its handles, so a subquery run once per outer row holds one run's
  // worth. Its loops' steps end first. A loop outside it that moved inside
  // it already released what the run made after that loop's step began.
  auto outer_moved = cx->steps.size() < s.steps || (s.steps != 0 && cx->steps[s.steps - 1].id != s.top);
  if (outer_moved) {
    cx->fail(BC_E_HANDLE, "bc_sub_end: a loop outside the subquery moved inside it");
    return;
  }
  if (cx->steps.size() > s.steps) cx->end_steps_from(static_cast<std::uint32_t>(s.steps));
  cx->roll_back(s.mark);
  truncate_held(cx, cx->aggs, s.aggs);
  truncate_held(cx, cx->sorters, s.sorters);
  truncate_held(cx, cx->hashes, s.hashes);
  auto set = [&](std::uint32_t i, Datum d) { return set_sub_param(cx, s.param + i, std::move(d)); };
  auto flag = [&] { return s.saw_null ? Datum{} : Datum{false}; };
  switch (s.kind) {
    case BC_SUB_VALUE: (void)set(0, std::move(s.value)); break;
    case BC_SUB_EXISTS: (void)set(0, s.rows > 0); break;
    case BC_SUB_BOUNDS:
      (void)(set(0, s.hi ? std::move(*s.hi) : Datum{}) && set(1, s.lo ? std::move(*s.lo) : Datum{}) &&
             set(2, flag()) && set(3, s.rows > 0));
      break;
    case BC_SUB_SET: (void)(set(0, flag()) && set(1, s.rows > 0)); break;
    default: break;
  }
  BC_GUARD_END(cx)
}

bool bc_sub_has_i64(bc_ctx* cx, uint32_t sub, int64_t v) {
  BC_GUARD_BEGIN
  return sub_has(cx, sub, Datum{v});
  BC_GUARD_END(cx)
  return {};
}

bool bc_sub_has_f64(bc_ctx* cx, uint32_t sub, double v) {
  BC_GUARD_BEGIN
  return sub_has(cx, sub, Datum{v});
  BC_GUARD_END(cx)
  return {};
}

bool bc_sub_has_bool(bc_ctx* cx, uint32_t sub, bool v) {
  BC_GUARD_BEGIN
  return sub_has(cx, sub, Datum{v});
  BC_GUARD_END(cx)
  return {};
}

bool bc_sub_has_bytes(bc_ctx* cx, uint32_t sub, bc_view v) {
  BC_GUARD_BEGIN
  auto b = cx->resolve(v);
  if (!b) return false;
  return sub_has(cx, sub, Datum{Bytes(b->begin(), b->end())});
  BC_GUARD_END(cx)
  return {};
}

extern "C++" {
namespace {
// An argument of a subquery's next run: parameter i, which the caller must
// have passed as NULL, so a plan never overwrites a value it was given.
void sub_arg(bc_ctx* cx, std::uint32_t i, Datum d) {
  if (!cx->ok()) return;
  if (!caller_null(cx, i, 1)) {
    cx->fail(BC_E_HANDLE, "bc_sub_arg: parameter $" + std::to_string(i + 1) +
                              " is past the given ones or one the caller gave");
    return;
  }
  (void)set_sub_param(cx, i, std::move(d));
}
}  // namespace
}  // extern "C++"

void bc_sub_arg_null(bc_ctx* cx, uint32_t i) {
  BC_GUARD_BEGIN
  sub_arg(cx, i, Datum{});
  BC_GUARD_END(cx)
}

void bc_sub_arg_i64(bc_ctx* cx, uint32_t i, int64_t v) {
  BC_GUARD_BEGIN
  sub_arg(cx, i, Datum{v});
  BC_GUARD_END(cx)
}

void bc_sub_arg_f64(bc_ctx* cx, uint32_t i, double v) {
  BC_GUARD_BEGIN
  sub_arg(cx, i, Datum{v});
  BC_GUARD_END(cx)
}

void bc_sub_arg_bool(bc_ctx* cx, uint32_t i, bool v) {
  BC_GUARD_BEGIN
  sub_arg(cx, i, Datum{v});
  BC_GUARD_END(cx)
}

// The bytes are copied before the parameter is set: `v` may be a view of it.
void bc_sub_arg_bytes(bc_ctx* cx, uint32_t i, bc_view v) {
  BC_GUARD_BEGIN
  auto b = cx->resolve(v);
  if (!b) return;
  sub_arg(cx, i, Datum{Bytes(b->begin(), b->end())});
  BC_GUARD_END(cx)
}

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

void bc_emit_num(bc_ctx* cx, bc_view v) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::Num)) return;
  auto b = cx->resolve(v);
  if (!b) return;
  emit_bytes(cx, *b);
  BC_GUARD_END(cx)
}

void bc_emit_jsonb(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  if (!emit_slot(cx, PhysType::Jsonb)) return;
  auto node = cx->resolve_jsonb(j);
  if (!node) return;
  emit_jsonb_text(cx, *node);
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
    case PhysType::Bytes:
    case PhysType::Num: return emit_value(cx, v->bytes);
    case PhysType::Jsonb: {
      auto node = jsonb::node_of(v->bytes);
      if (!node) {
        cx->fail(BC_E_CORRUPT, "a stored jsonb value of no known format");
        return;
      }
      return emit_jsonb_text(cx, *node);
    }
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
    case PhysType::Num: throw std::logic_error("a key part that is a NUMERIC");
    case PhysType::Jsonb: throw std::logic_error("a key part that is a jsonb");
  }
  BC_GUARD_END(cx)
}

void bc_emit_end(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (cx->sub) {
    end_sub_row(cx);
    return;
  }
  if (!cx->emit || cx->emit_col != cx->verified.result_types.size()) {
    cx->fail(BC_L_PLAN, "result row ended with the wrong number of columns");
    return;
  }
  ++cx->emitted;
  if (cx->windowed && cx->emitted <= cx->window_skip) {
    cx->emit.reset();  // before the window: dropped unencoded and uncharged
    return;
  }
  // The row is encoded straight into the result buffer, after its length,
  // and charged with it.
  auto at = cx->results.size();
  cx->results.resize(at + 4);
  if (!cx->emit->finish(cx->results)) {
    cx->results.resize(at);
    cx->fail(BC_E_LIMIT, "result row over the row format's limit");
    return;
  }
  if (!cx->take(cx->results.size() - at)) {
    cx->results.resize(at);
    return;
  }
  auto n = static_cast<std::uint32_t>(cx->results.size() - at - 4);
  std::memcpy(cx->results.data() + at, &n, 4);
  cx->emit.reset();
  if (cx->windowed && ++cx->kept >= cx->window_limit) cx->unwinding = true;
  BC_GUARD_END(cx)
}

void bc_emit_count(bc_ctx* cx, int64_t affected) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (cx->sub) {
    cx->fail(BC_L_PLAN, "a subquery counts no rows written");
    return;
  }
  if (cx->affected >= 0) {
    cx->fail(BC_E_HANDLE, "bc_emit_count called twice");
    return;
  }
  cx->affected = affected;
  BC_GUARD_END(cx)
}

// ---------------------------------------------------------------- arithmetic

namespace {
auto arith_fail(bc_ctx* cx, const char* what, bytecask::sql::SqlState state = bytecask::sql::kNumericValueOutOfRange)
    -> std::int64_t {
  cx->fail(BC_E_ARITH, state, what);
  return 0;
}

// a + b or a - b. An overflow is BC_E_ARITH or, for a value stored in a
// BIGINT, that type's range error on `object_id`, as in PostgreSQL: the one
// the row builder and bc_check_int raise for the narrower types.
auto add_or_sub(bc_ctx* cx, bool subtract, std::int64_t a, std::int64_t b, bool bigint, std::uint32_t object_id)
    -> std::int64_t {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  std::int64_t r = 0;
  if (!(subtract ? __builtin_sub_overflow(a, b, &r) : __builtin_add_overflow(a, b, &r))) return r;
  if (bigint) {
    cx->raise(BC_C_RANGE, object_id, "bigint out of range");
    return 0;
  }
  return arith_fail(cx, "integer out of range");
  BC_GUARD_END(cx)
  return {};
}
}  // namespace

int64_t bc_add(bc_ctx* cx, int64_t a, int64_t b) { return add_or_sub(cx, false, a, b, false, 0); }
int64_t bc_sub(bc_ctx* cx, int64_t a, int64_t b) { return add_or_sub(cx, true, a, b, false, 0); }
int64_t bc_add_bigint(bc_ctx* cx, int64_t a, int64_t b, uint32_t object_id) {
  return add_or_sub(cx, false, a, b, true, object_id);
}
int64_t bc_sub_bigint(bc_ctx* cx, int64_t a, int64_t b, uint32_t object_id) {
  return add_or_sub(cx, true, a, b, true, object_id);
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
  if (b == 0) return arith_fail(cx, "division by zero", bytecask::sql::kDivisionByZero);
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return arith_fail(cx, "integer out of range");
  return a / b;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_mod(bc_ctx* cx, int64_t a, int64_t b) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  if (b == 0) return arith_fail(cx, "division by zero", bytecask::sql::kDivisionByZero);
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

bc_view bc_concat(bc_ctx* cx, bc_view a, bc_view b) {
  BC_GUARD_BEGIN
  auto v = cx->resolve2(a, b);
  if (!v) return {};
  Bytes out;
  out.reserve(v->first.size() + v->second.size());
  out.insert(out.end(), v->first.begin(), v->first.end());
  out.insert(out.end(), v->second.begin(), v->second.end());
  return cx->arena_view(out);
  BC_GUARD_END(cx)
  return {};
}

bc_jsonb bc_jsonb_in(bc_ctx* cx, bc_view text) {
  BC_GUARD_BEGIN
  auto t = cx->resolve(text);
  if (!t || !cx->ok()) return {};
  return parse_into(cx, *t, false);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_jsonb_text(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto node = cx->resolve_jsonb(j);
  if (!node) return {};
  // Written straight into the step's arena, charged before it is allocated.
  ArenaOut out{cx, &cx->arena};
  std::span<char> text;
  auto r = jsonb::to_text(*node, jsonb_memory(cx), {&out, ArenaOut::alloc}, text);
  if (!r.ok()) {
    fail_jsonb(cx, r);
    return {};
  }
  return cx->make_view(reinterpret_cast<const std::byte*>(text.data()), text.size());
  BC_GUARD_END(cx)
  return {};
}

// ------------------------------------------------------ jsonb accessors

extern "C++" {
namespace {

// A jsonb handle's node and its owner, for a slice that shares its lifetime.
struct JsonbIn {
  BytesView node;
  std::uint32_t cursor{kNoCursor};
  std::uint64_t step{0};
};

auto jsonb_arg(bc_ctx* cx, bc_jsonb j) -> std::optional<JsonbIn> {
  if (!cx->ok()) return std::nullopt;
  auto node = cx->resolve_jsonb(j);
  if (!node) return std::nullopt;
  auto [cursor, step] = cx->owner(bc_view{j.i, j.g});
  return JsonbIn{*node, cursor, step};
}

template <typename T>
auto out_arg(bc_ctx* cx, T* out) -> bool {
  if (out != nullptr) return true;
  cx->fail(BC_E_HANDLE, "a jsonb accessor's out pointer is NULL");
  return false;
}

// A found node into *out: a slice of the source, with its owner.
auto give_node(bc_ctx* cx, const JsonbIn& src, const std::optional<BytesView>& found, bc_jsonb* out) -> bool {
  if (!found) return false;
  auto h = cx->make_jsonb(found->data(), found->size(), src.cursor, src.step);
  if (!cx->ok()) return false;
  *out = h;
  return true;
}

// ->> of a found node into *out: NULL for a JSON null, a string's own bytes,
// or the node's text in the step's arena.
auto give_text(bc_ctx* cx, const JsonbIn& src, const std::optional<BytesView>& found, bc_view* out) -> bool {
  if (!found) return false;
  jsonb::TextForm form{};
  BytesView bytes;
  if (auto r = jsonb::text_form(*found, form, bytes); !r.ok()) {
    fail_jsonb(cx, r);
    return false;
  }
  switch (form) {
    case jsonb::TextForm::Null: return false;
    case jsonb::TextForm::Bytes: {
      auto v = cx->make_view(bytes.data(), bytes.size(), src.cursor, src.step);
      if (!cx->ok()) return false;
      *out = v;
      return true;
    }
    case jsonb::TextForm::Text: {
      ArenaOut arena{cx, &cx->arena};
      std::span<char> text;
      if (auto r = jsonb::to_text(*found, jsonb_memory(cx), {&arena, ArenaOut::alloc}, text); !r.ok()) {
        fail_jsonb(cx, r);
        return false;
      }
      auto v = cx->make_view(reinterpret_cast<const std::byte*>(text.data()), text.size());
      if (!cx->ok()) return false;
      *out = v;
      return true;
    }
  }
  return false;
}

auto lookup_field(bc_ctx* cx, const JsonbIn& src, bc_view key, std::optional<BytesView>& found) -> bool {
  auto k = cx->resolve(key);
  if (!k || !cx->ok()) return false;
  if (auto r = jsonb::field(src.node, text_of(*k), found); !r.ok()) {
    fail_jsonb(cx, r);
    return false;
  }
  return true;
}

auto lookup_element(bc_ctx* cx, const JsonbIn& src, std::int64_t i, std::optional<BytesView>& found) -> bool {
  if (auto r = jsonb::element(src.node, i, found); !r.ok()) {
    fail_jsonb(cx, r);
    return false;
  }
  return true;
}

auto lookup_path(bc_ctx* cx, const JsonbIn& src, bc_view path, std::optional<BytesView>& found) -> bool {
  auto p = cx->resolve(path);
  if (!p || !cx->ok()) return false;
  if (!jsonb::path_size(*p)) {
    cx->fail(BC_E_HANDLE, "a view that is no jsonb path: a path comes from bc_jsonb_path_in or bc_jsonb_path_add");
    return false;
  }
  if (auto r = jsonb::at_path(src.node, *p, found); !r.ok()) {
    fail_jsonb(cx, r);
    return false;
  }
  return true;
}

}  // namespace
}  // extern "C++"

bool bc_jsonb_field(bc_ctx* cx, bc_jsonb j, bc_view key, bc_jsonb* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_field(cx, *src, key, found) && give_node(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bool bc_jsonb_field_text(bc_ctx* cx, bc_jsonb j, bc_view key, bc_view* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_field(cx, *src, key, found) && give_text(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bool bc_jsonb_element(bc_ctx* cx, bc_jsonb j, int64_t i, bc_jsonb* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_element(cx, *src, i, found) && give_node(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bool bc_jsonb_element_text(bc_ctx* cx, bc_jsonb j, int64_t i, bc_view* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_element(cx, *src, i, found) && give_text(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bc_view bc_jsonb_path_in(bc_ctx* cx, bc_view text) {
  BC_GUARD_BEGIN
  auto t = cx->resolve(text);
  if (!t || !cx->ok()) return {};
  ArenaOut out{cx, &cx->arena};
  std::span<std::byte> path;
  if (auto r = jsonb::parse_path(text_of(*t), {&out, ArenaOut::alloc}, path); !r.ok()) {
    fail_jsonb(cx, r);
    return {};
  }
  return cx->make_view(path.data(), path.size());
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_jsonb_path_add(bc_ctx* cx, bc_view path, bc_view element, bool null) {
  BC_GUARD_BEGIN
  auto p = cx->resolve(path);
  auto e = null ? std::optional<BytesView>{BytesView{}} : cx->resolve(element);
  if (!p || !e || !cx->ok()) return {};
  auto n = jsonb::path_size(*p);
  if (!n || *n == std::numeric_limits<std::uint32_t>::max()) {
    cx->fail(BC_E_HANDLE, "a view that is no jsonb path: a path comes from bc_jsonb_path_in or bc_jsonb_path_add");
    return {};
  }
  if (!null && e->size() >= jsonb::kPathNull) {
    cx->fail(BC_E_DATA, bytecask::sql::kProgramLimitExceeded, "a path element of more than 4294967294 bytes");
    return {};
  }
  auto element_bytes = null ? std::nullopt : std::optional<BytesView>{*e};
  auto size = jsonb::path_append_size(*p, element_bytes);
  if (!cx->take(size)) return {};
  auto* d = cx->arena.alloc(size);
  jsonb::path_append(*p, *n, element_bytes, {d, size});
  return cx->make_view(d, size);
  BC_GUARD_END(cx)
  return {};
}

bool bc_jsonb_path(bc_ctx* cx, bc_jsonb j, bc_view path, bc_jsonb* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_path(cx, *src, path, found) && give_node(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bool bc_jsonb_path_text(bc_ctx* cx, bc_jsonb j, bc_view path, bc_view* out) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src || !out_arg(cx, out)) return false;
  std::optional<BytesView> found;
  return lookup_path(cx, *src, path, found) && give_text(cx, *src, found, out);
  BC_GUARD_END(cx)
  return false;
}

bc_view bc_jsonb_typeof(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return {};
  jsonb::Type t{};
  if (auto r = jsonb::type_of(src->node, t); !r.ok()) {
    fail_jsonb(cx, r);
    return {};
  }
  auto name = jsonb::typeof_name(t);
  return cx->arena_view(std::as_bytes(std::span{name.data(), name.size()}));
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_jsonb_array_length(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return 0;
  std::int64_t n = 0;
  if (auto r = jsonb::array_length(src->node, n); !r.ok()) {
    fail_jsonb(cx, r);
    return 0;
  }
  return n;
  BC_GUARD_END(cx)
  return 0;
}

bool bc_jsonb_to_bool(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return false;
  auto v = false;
  if (auto r = jsonb::to_bool(src->node, v); !r.ok()) {
    fail_jsonb(cx, r);
    return false;
  }
  return v;
  BC_GUARD_END(cx)
  return false;
}

int64_t bc_jsonb_to_i64(bc_ctx* cx, bc_jsonb j, uint32_t int_type, uint32_t object_id) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return 0;
  const char* target = nullptr;
  switch (int_type) {
    case BC_SMALLINT: target = "smallint"; break;
    case BC_INTEGER: target = "integer"; break;
    case BC_BIGINT: target = "bigint"; break;
    default: cx->fail(BC_E_HANDLE, "bc_jsonb_to_i64: an unknown integer type"); return 0;
  }
  std::optional<std::int64_t> v;
  if (auto r = jsonb::to_integer(src->node, target, v); !r.ok()) {
    fail_jsonb(cx, r);
    return 0;
  }
  if (!v) {
    cx->raise(BC_C_RANGE, object_id, std::string{target} + " out of range");
    return 0;
  }
  return bc_check_int(cx, *v, int_type, object_id);
  BC_GUARD_END(cx)
  return 0;
}

double bc_jsonb_to_f64(bc_ctx* cx, bc_jsonb j, bool real) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return 0.0;
  // The number's text, read and released: the value is all that is kept.
  BufferOut out{cx, {}};
  double v = 0;
  auto r = jsonb::to_float(src->node, real, jsonb_memory(cx), {&out, BufferOut::alloc}, v);
  cx->release(out.held);
  if (!r.ok()) {
    fail_jsonb(cx, r);
    return 0.0;
  }
  return v;
  BC_GUARD_END(cx)
  return 0.0;
}

bc_view bc_jsonb_to_num(bc_ctx* cx, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto src = jsonb_arg(cx, j);
  if (!src) return {};
  ArenaOut out{cx, &cx->arena};
  std::span<char> text;
  if (auto r = jsonb::to_numeric(src->node, "numeric", jsonb_memory(cx), {&out, ArenaOut::alloc}, text); !r.ok()) {
    fail_jsonb(cx, r);
    return {};
  }
  return cx->make_view(reinterpret_cast<const std::byte*>(text.data()), text.size());
  BC_GUARD_END(cx)
  return {};
}

extern "C++" {
namespace {

namespace sqlt = bytecask::sql;

static_assert(BC_TYPE_SMALLINT == static_cast<int>(sqlt::SqlKind::SmallInt) &&
              BC_TYPE_DECIMAL == static_cast<int>(sqlt::SqlKind::Decimal) &&
              BC_TYPE_NUMERIC == static_cast<int>(sqlt::SqlKind::Numeric) &&
              BC_TYPE_TIMESTAMPTZ == static_cast<int>(sqlt::SqlKind::TimestampTz) &&
              BC_TYPE_NAME == static_cast<int>(sqlt::SqlKind::Name) &&
              BC_TYPE_JSONB == static_cast<int>(sqlt::SqlKind::Jsonb));

// A bc_sql_type as its SqlKind; nullopt for a code outside the enum.
auto sql_kind_of(std::uint32_t code) -> std::optional<sqlt::SqlKind> {
  if (code < BC_TYPE_SMALLINT || code > BC_TYPE_JSONB) return std::nullopt;
  return static_cast<sqlt::SqlKind>(code);
}

auto integer_kind(sqlt::SqlKind k) -> bool {
  return k == sqlt::SqlKind::SmallInt || k == sqlt::SqlKind::Integer || k == sqlt::SqlKind::BigInt;
}

auto temporal_kind(sqlt::SqlKind k) -> bool {
  using K = sqlt::SqlKind;
  return k == K::Date || k == K::Time || k == K::TimeTz || k == K::Timestamp || k == K::TimestampTz;
}

// A bc_int_type as its SqlKind.
auto int_kind_of(std::uint32_t int_type) -> std::optional<sqlt::SqlKind> {
  switch (int_type) {
    case BC_SMALLINT: return sqlt::SqlKind::SmallInt;
    case BC_INTEGER: return sqlt::SqlKind::Integer;
    case BC_BIGINT: return sqlt::SqlKind::BigInt;
    default: return std::nullopt;
  }
}

// DECIMAL(precision, scale), when the layer holds one.
auto decimal_type(std::uint32_t precision, std::uint32_t scale) -> std::optional<sqlt::SqlType> {
  if (precision < 1 || precision > sqlt::kMaxDecimalPrecision || scale > precision) return std::nullopt;
  return sqlt::SqlType{sqlt::SqlKind::Decimal, 0, static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale)};
}

// A cast function's error as the plan API reports it: a value outside an
// integer or DECIMAL target BC_C_RANGE on object_id, with its DETAIL; any
// other BC_E_DATA with its SQLSTATE (never BC_E_CORRUPT, which an escaped
// TextError would be).
void fail_cast(bc_ctx* cx, const sqlt::TextError& e, sqlt::SqlKind to, std::uint32_t object_id) {
  auto state = sqlt::known_sqlstate(e.sqlstate()).value_or(sqlt::kInvalidTextRepresentation);
  if (state.view() == "22003" && (integer_kind(to) || to == sqlt::SqlKind::Decimal)) {
    cx->raise(BC_C_RANGE, object_id, e.what(), e.detail());
    return;
  }
  cx->fail(BC_E_DATA, state, e.what(), {e.detail(), "", e.context()});
}

// Runs `read` over the text of `text`, its size charged to the statement
// while it runs (`times` copies of it: from_text's temporaries) and
// released after, success or failure.
template <typename Read>
auto with_text(bc_ctx* cx, bc_view text, std::uint64_t times, Read&& read) -> bool {
  auto t = cx->resolve(text);
  if (!t || !cx->ok()) return false;
  auto charge = t->size() * times;
  if (!cx->take(charge)) return false;
  struct Release {
    bc_ctx* cx;
    std::uint64_t n;
    ~Release() { cx->release(n); }
  } release{cx, charge};
  read(text_of(*t));
  return cx->ok();
}

auto text_view(bc_ctx* cx, const std::string& text) -> bc_view {
  auto b = std::as_bytes(std::span{text.data(), text.size()});
  return cx->arena_view(BytesView{b});
}

}  // namespace
}  // extern "C++"

int64_t bc_text_in_i64(bc_ctx* cx, bc_view text, uint32_t sql_type, uint32_t precision, uint32_t scale, uint32_t object_id) {
  BC_GUARD_BEGIN
  auto kind = sql_kind_of(sql_type);
  std::optional<sqlt::SqlType> to;
  if (kind && (integer_kind(*kind) || temporal_kind(*kind))) to = sqlt::SqlType{*kind};
  if (kind == sqlt::SqlKind::Decimal) to = decimal_type(precision, scale);
  if (!to) {
    cx->fail(BC_E_HANDLE, "bc_text_in_i64: not an integer, DECIMAL or date or time type");
    return 0;
  }
  std::int64_t out = 0;
  (void)with_text(cx, text, 1, [&](std::string_view s) {
    try {
      out = std::get<std::int64_t>(sqlt::cast_from_text(s, *to));
    } catch (const sqlt::TextError& e) {
      fail_cast(cx, e, to->kind, object_id);
    }
  });
  return cx->ok() ? out : 0;
  BC_GUARD_END(cx)
  return 0;
}

double bc_text_in_f64(bc_ctx* cx, bc_view text, uint32_t sql_type) {
  BC_GUARD_BEGIN
  if (sql_type != BC_TYPE_REAL && sql_type != BC_TYPE_DOUBLE) {
    cx->fail(BC_E_HANDLE, "bc_text_in_f64: not REAL or DOUBLE PRECISION");
    return 0.0;
  }
  sqlt::SqlType to{static_cast<sqlt::SqlKind>(sql_type)};
  double out = 0.0;
  (void)with_text(cx, text, 1, [&](std::string_view s) {
    try {
      out = std::get<double>(sqlt::cast_from_text(s, to));
    } catch (const sqlt::TextError& e) {
      fail_cast(cx, e, to.kind, 0);
    }
  });
  return cx->ok() ? out : 0.0;
  BC_GUARD_END(cx)
  return 0.0;
}

bool bc_text_in_bool(bc_ctx* cx, bc_view text) {
  BC_GUARD_BEGIN
  auto out = false;
  (void)with_text(cx, text, 1, [&](std::string_view s) {
    try {
      out = std::get<bool>(sqlt::cast_from_text(s, sqlt::SqlType{sqlt::SqlKind::Boolean}));
    } catch (const sqlt::TextError& e) {
      fail_cast(cx, e, sqlt::SqlKind::Boolean, 0);
    }
  });
  return cx->ok() && out;
  BC_GUARD_END(cx)
  return false;
}

bc_view bc_text_in_bytes(bc_ctx* cx, bc_view text, uint32_t sql_type) {
  BC_GUARD_BEGIN
  if (sql_type != BC_TYPE_BYTEA && sql_type != BC_TYPE_NAME) {
    cx->fail(BC_E_HANDLE, "bc_text_in_bytes: not BYTEA or NAME");
    return {};
  }
  sqlt::SqlType to{static_cast<sqlt::SqlKind>(sql_type)};
  bc_view out{};
  // The temporary and its arena copy are held at once: twice the text.
  (void)with_text(cx, text, 2, [&](std::string_view s) {
    try {
      auto b = std::get<Bytes>(sqlt::cast_from_text(s, to));
      out = cx->arena_view(BytesView{b});
    } catch (const sqlt::TextError& e) {
      fail_cast(cx, e, to.kind, 0);
    }
  });
  return cx->ok() ? out : bc_view{};
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_value_text(bc_ctx* cx, int64_t v, uint32_t sql_type, uint32_t scale) {
  BC_GUARD_BEGIN
  auto kind = sql_kind_of(sql_type);
  auto ok_type = kind && (integer_kind(*kind) || temporal_kind(*kind) ||
                          (*kind == sqlt::SqlKind::Decimal && scale <= sqlt::kMaxDecimalPrecision) ||
                          (*kind == sqlt::SqlKind::Boolean && scale <= 1 && (v == 0 || v == 1)));
  if (!ok_type || (*kind != sqlt::SqlKind::Decimal && *kind != sqlt::SqlKind::Boolean && scale != 0)) {
    cx->fail(BC_E_HANDLE, "bc_value_text: not an integer, DECIMAL, BOOLEAN or date or time value");
    return {};
  }
  sqlt::SqlType from{*kind, 0, sqlt::kMaxDecimalPrecision, static_cast<std::uint8_t>(scale)};
  if (*kind == sqlt::SqlKind::Boolean) {
    // booltext's at scale 0, boolout's (the I/O cast to a name) at 1.
    auto to = sqlt::SqlType{scale == 0 ? sqlt::SqlKind::Text : sqlt::SqlKind::Name};
    return text_view(cx, sqlt::cast_to_text(v != 0, from, to));
  }
  return text_view(cx, sqlt::cast_to_text(v, from, sqlt::SqlType{sqlt::SqlKind::Text}));
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_bytea_text(bc_ctx* cx, bc_view b) {
  BC_GUARD_BEGIN
  auto t = cx->resolve(b);
  if (!t || !cx->ok()) return {};
  // \x and two hex digits a byte, written straight into the arena.
  static constexpr char kHex[] = "0123456789abcdef";
  auto n = 2 + 2 * t->size();
  if (!cx->take(n)) return {};
  auto* p = cx->arena.alloc(n);
  p[0] = std::byte{'\\'};
  p[1] = std::byte{'x'};
  for (std::size_t i = 0; i < t->size(); ++i) {
    auto x = std::to_integer<unsigned>((*t)[i]);
    p[2 + 2 * i] = static_cast<std::byte>(kHex[x >> 4U]);
    p[3 + 2 * i] = static_cast<std::byte>(kHex[x & 15U]);
  }
  return cx->make_view(p, n);
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_f64_to_int(bc_ctx* cx, double v, uint32_t int_type, uint32_t object_id) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  auto k = int_kind_of(int_type);
  if (!k) {
    cx->fail(BC_E_HANDLE, "bc_f64_to_int: an unknown integer type");
    return 0;
  }
  try {
    return sqlt::float_to_integer(v, sqlt::SqlType{*k});
  } catch (const sqlt::TextError& e) {
    fail_cast(cx, e, *k, object_id);
    return 0;
  }
  BC_GUARD_END(cx)
  return 0;
}

int64_t bc_decimal_rescale(bc_ctx* cx, int64_t v, uint32_t from_scale, uint32_t to_type, uint32_t precision, uint32_t to_scale,
                           uint32_t object_id) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  auto kind = sql_kind_of(to_type);
  std::optional<sqlt::SqlType> to;
  if (kind && integer_kind(*kind) && precision == 0 && to_scale == 0) to = sqlt::SqlType{*kind};
  if (kind == sqlt::SqlKind::Decimal) to = decimal_type(precision, to_scale);
  if (!to || from_scale > sqlt::kMaxDecimalPrecision) {
    cx->fail(BC_E_HANDLE, "bc_decimal_rescale: not an integer type or a DECIMAL, or a scale past 18");
    return 0;
  }
  try {
    return sqlt::rescale_decimal(v, static_cast<std::uint8_t>(from_scale), *to);
  } catch (const sqlt::TextError& e) {
    fail_cast(cx, e, to->kind, object_id);
    return 0;
  }
  BC_GUARD_END(cx)
  return 0;
}

double bc_decimal_to_f64(bc_ctx* cx, int64_t v, uint32_t scale, bool real) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0.0;
  if (scale > sqlt::kMaxDecimalPrecision) {
    cx->fail(BC_E_HANDLE, "bc_decimal_to_f64: a scale past 18");
    return 0.0;
  }
  return sqlt::decimal_to_float(v, static_cast<std::uint8_t>(scale), real);
  BC_GUARD_END(cx)
  return 0.0;
}

int64_t bc_f64_to_decimal(bc_ctx* cx, double v, bool real, uint32_t precision, uint32_t scale, uint32_t object_id) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  auto to = decimal_type(precision, scale);
  if (!to) {
    cx->fail(BC_E_HANDLE, "bc_f64_to_decimal: not a DECIMAL's precision and scale");
    return 0;
  }
  try {
    return sqlt::float_to_decimal(v, real, *to);
  } catch (const sqlt::TextError& e) {
    fail_cast(cx, e, sqlt::SqlKind::Decimal, object_id);
    return 0;
  }
  BC_GUARD_END(cx)
  return 0;
}

double bc_f64_to_f32(bc_ctx* cx, double v) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0.0;
  try {
    return sqlt::double_to_real(v);
  } catch (const sqlt::TextError& e) {
    fail_cast(cx, e, sqlt::SqlKind::Real, 0);
    return 0.0;
  }
  BC_GUARD_END(cx)
  return 0.0;
}

double bc_i64_to_f32(int64_t v) { return bytecask::sql::integer_to_real(v); }

int64_t bc_temporal_cast(bc_ctx* cx, int64_t v, uint32_t from_type, uint32_t to_type) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  auto from = sql_kind_of(from_type);
  auto to = sql_kind_of(to_type);
  if (!from || !to || !temporal_kind(*from) || !temporal_kind(*to) ||
      sqlt::cast_kind(sqlt::SqlType{*from}, sqlt::SqlType{*to}) != sqlt::CastKind::Function) {
    cx->fail(BC_E_HANDLE, "bc_temporal_cast: no cast between these date or time types");
    return 0;
  }
  try {
    return sqlt::convert_temporal(v, *from, *to);
  } catch (const sqlt::TextError& e) {
    fail_cast(cx, e, *to, 0);
    return 0;
  }
  BC_GUARD_END(cx)
  return 0;
}

bc_view bc_num_quotient(bc_ctx* cx, int64_t sum, int64_t count, uint32_t scale) {
  BC_GUARD_BEGIN
  if (count < 1 || scale > 18) {
    cx->fail(BC_E_HANDLE, "bc_num_quotient: a count below 1 or a scale past 18");
    return {};
  }
  auto text = bytecask::sql::average_text(sum, count, static_cast<std::uint8_t>(scale));
  auto b = std::as_bytes(std::span{text.data(), text.size()});
  return cx->arena_view(BytesView{b});
  BC_GUARD_END(cx)
  return {};
}

namespace {

// The bytes of the UTF-8 character at s[i]: 1 for a byte that starts none,
// which a TEXT, checked as UTF-8 when written or bound, does not hold.
auto char_len(BytesView s, std::size_t i) -> std::size_t {
  auto b = std::to_integer<unsigned>(s[i]);
  std::size_t n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
  return std::min(n, s.size() - i);
}

// The code point of the n-byte UTF-8 character at s[i].
auto code_point(BytesView s, std::size_t i, std::size_t n) -> std::int32_t {
  auto b = std::to_integer<std::int32_t>(s[i]);
  if (n == 1) return b;
  std::int32_t cp = b & (0x7F >> n);
  for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (std::to_integer<std::int32_t>(s[i + k]) & 0x3F);
  return cp;
}

enum class Like { Match, NoMatch, BadPattern };

// An ASCII letter's lower case; any other byte, a UTF-8 lead or continuation
// byte among them, as it is.
auto ascii_lower(std::byte b) -> std::byte {
  auto c = std::to_integer<unsigned>(b);
  return c >= 'A' && c <= 'Z' ? static_cast<std::byte>(c - 'A' + 'a') : b;
}

// PostgreSQL's LIKE (MatchText) of UTF-8 text, case-sensitive: % matches
// any run of characters, _ one, the escape the character after it as
// written, and any other character itself, byte for byte. Greedy, with one
// backtrack point: the last %, which absorbs one more character of the
// text at each retry. No allocation. An escape the match reaches with
// nothing after it is a bad pattern, as PostgreSQL raises it only when its
// match gets there.
auto like(BytesView s, BytesView p, std::int32_t escape, bool fold) -> Like {
  struct Token {
    enum class Kind { Any, One, Literal } kind;
    std::size_t at, len;   // the token in p: its bytes
    std::size_t lit, n;    // a literal's bytes in p
  };
  auto token = [&](std::size_t i) -> std::optional<Token> {
    auto n = char_len(p, i);
    auto cp = code_point(p, i, n);
    if (escape >= 0 && cp == escape) {
      if (i + n >= p.size()) return std::nullopt;
      auto m = char_len(p, i + n);
      return Token{Token::Kind::Literal, i, n + m, i + n, m};
    }
    if (cp == '%') return Token{Token::Kind::Any, i, n, 0, 0};
    if (cp == '_') return Token{Token::Kind::One, i, n, 0, 0};
    return Token{Token::Kind::Literal, i, n, i, n};
  };
  std::size_t si = 0, pi = 0;
  std::optional<std::size_t> star_p, star_s;
  while (si < s.size()) {
    if (pi < p.size()) {
      auto t = token(pi);
      if (!t) return Like::BadPattern;
      if (t->kind == Token::Kind::Any) {
        pi += t->len;
        star_p = pi;
        star_s = si;
        continue;
      }
      if (t->kind == Token::Kind::One) {
        si += char_len(s, si);
        pi += t->len;
        continue;
      }
      if (si + t->n <= s.size() && std::ranges::equal(s.subspan(si, t->n), p.subspan(t->lit, t->n), [&](std::byte a, std::byte b) {
            return fold ? ascii_lower(a) == ascii_lower(b) : a == b;
          })) {
        si += t->n;
        pi += t->len;
        continue;
      }
    }
    if (!star_p) return Like::NoMatch;
    *star_s += char_len(s, *star_s);
    si = *star_s;
    pi = *star_p;
  }
  // The text is done: what is left of the pattern must be %s alone.
  while (pi < p.size() && std::to_integer<char>(p[pi]) == '%') ++pi;
  return pi == p.size() ? Like::Match : Like::NoMatch;
}

}  // namespace

namespace {

auto like_of(bc_ctx* cx, bc_view text, bc_view pattern, int32_t escape, bool fold) -> bool {
  auto v = cx->resolve2(text, pattern);
  if (!v) return false;
  switch (like(v->first, v->second, escape, fold)) {
    case Like::Match: return true;
    case Like::NoMatch: return false;
    case Like::BadPattern:
      cx->fail(BC_E_PATTERN, "LIKE pattern must not end with escape character");
      return false;
  }
  return false;
}

}  // namespace

bool bc_like(bc_ctx* cx, bc_view text, bc_view pattern, int32_t escape) {
  BC_GUARD_BEGIN
  return like_of(cx, text, pattern, escape, false);
  BC_GUARD_END(cx)
  return {};
}

bool bc_ilike(bc_ctx* cx, bc_view text, bc_view pattern, int32_t escape) {
  BC_GUARD_BEGIN
  return like_of(cx, text, pattern, escape, true);
  BC_GUARD_END(cx)
  return {};
}

int32_t bc_like_escape(bc_ctx* cx, bc_view escape) {
  BC_GUARD_BEGIN
  auto e = cx->resolve(escape);
  if (!e) return -1;
  if (e->empty()) return -1;
  auto n = char_len(*e, 0);
  if (n != e->size()) {
    cx->fail(BC_E_PATTERN, "invalid escape string");
    return -1;
  }
  return code_point(*e, 0, n);
  BC_GUARD_END(cx)
  return {};
}

// ---------------------------------------------------------------------- keys

bc_key bc_key_begin(bc_ctx* cx, uint32_t table_or_index) {
  BC_GUARD_BEGIN
  const auto* kl = cx->key_layout(table_or_index);
  if (kl == nullptr) return {};
  KeyBuilder b{kl, {}, cx->next_gen()};
  bytecask::sql::key_put_prefix(b.buf, kl->ns, table_or_index);
  b.held = b.buf.size();
  auto charge = kKeyEntryBytes + b.held;
  return push_key_builder<bc_key>(cx, cx->keys, std::move(b), charge);
  BC_GUARD_END(cx)
  return {};
}

void bc_key_i64(bc_ctx* cx, bc_key k, int64_t v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::I64, false);
  if (b == nullptr) return;
  if (!bytecask::sql::key_put_i64(b->buf, v, current_part(*b))) b->unfit = true;
  key_held(cx, *b);
  BC_GUARD_END(cx)
}

void bc_key_f64(bc_ctx* cx, bc_key k, double v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::F64, false);
  if (b == nullptr) return;
  bytecask::sql::key_put_f64(b->buf, v, current_part(*b));
  key_held(cx, *b);
  BC_GUARD_END(cx)
}

void bc_key_bool(bc_ctx* cx, bc_key k, bool v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::Bool, false);
  if (b == nullptr) return;
  bytecask::sql::key_put_bool(b->buf, v, current_part(*b));
  key_held(cx, *b);
  BC_GUARD_END(cx)
}

void bc_key_bytes(bc_ctx* cx, bc_key k, bc_view v) {
  BC_GUARD_BEGIN
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  auto* b = key_builder_part(cx, k, PhysType::Bytes, false);
  if (b == nullptr) return;
  bytecask::sql::key_put_bytes(b->buf, *bytes, current_part(*b));
  key_held(cx, *b);
  BC_GUARD_END(cx)
}

void bc_key_null(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, std::nullopt, true);
  if (b == nullptr) return;
  bytecask::sql::key_put_null(b->buf, current_part(*b));
  key_held(cx, *b);
  BC_GUARD_END(cx)
}

void bc_key_param(bc_ctx* cx, bc_key k, uint32_t i) {
  BC_GUARD_BEGIN
  if (cx->param(i) == nullptr) return;
  auto* b = key_builder_part(cx, k, std::nullopt, false);
  if (b == nullptr) return;
  if (put_param_part(cx, b->buf, *b->layout, b->next_part - 1, i)) key_held(cx, *b);
  BC_GUARD_END(cx)
}

bc_view bc_key_end(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  if (b == nullptr || !within_key_ceiling(cx, b->buf)) return {};
  if (b->unfit) {
    cx->fail(BC_E_LIMIT, "an integer outside its key part's width: no key has it");
    return {};
  }
  return end_key_builder(cx, *b);
  BC_GUARD_END(cx)
  return {};
}

bool bc_key_fits(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  const auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  return b != nullptr && !b->unfit && b->buf.size() <= bytecask::sql::kMaxKeyBytes;
  BC_GUARD_END(cx)
  return false;
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

// ----------------------------------------------------------------- sort keys

// A sort key is the plan's, not a table's or index's: no prefix, no layout
// and no engine key ceiling, since it never reaches the engine. Charged and
// owned by its step as a key builder is.
bc_sort_key bc_sort_key_begin(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return {};
  return push_key_builder<bc_sort_key>(cx, cx->sort_keys, SortKeyBuilder{{}, cx->next_gen()}, kKeyEntryBytes);
  BC_GUARD_END(cx)
  return {};
}

void bc_sort_key_i64(bc_ctx* cx, bc_sort_key k, int64_t v, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  add_sort_key_part(cx, k, dir, nulls, PhysType::I64, 0,
                    [v](Bytes& out, KeyPart part) { key_put_wide_i64(out, v, part); });
  BC_GUARD_END(cx)
}

void bc_sort_key_f64(bc_ctx* cx, bc_sort_key k, double v, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  add_sort_key_part(cx, k, dir, nulls, PhysType::F64, 0,
                    [v](Bytes& out, KeyPart part) { bytecask::sql::key_put_f64(out, v, part); });
  BC_GUARD_END(cx)
}

void bc_sort_key_bool(bc_ctx* cx, bc_sort_key k, bool v, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  add_sort_key_part(cx, k, dir, nulls, PhysType::Bool, 0,
                    [v](Bytes& out, KeyPart part) { bytecask::sql::key_put_bool(out, v, part); });
  BC_GUARD_END(cx)
}

void bc_sort_key_bytes(bc_ctx* cx, bc_sort_key k, bc_view v, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  auto bytes = cx->resolve(v);
  if (!bytes) return;
  add_sort_key_part(cx, k, dir, nulls, PhysType::Bytes, bytes->size(),
                    [&](Bytes& out, KeyPart part) { bytecask::sql::key_put_bytes(out, *bytes, part); });
  BC_GUARD_END(cx)
}

void bc_sort_key_null(bc_ctx* cx, bc_sort_key k, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  add_sort_key_part(cx, k, dir, nulls, std::nullopt, 0, [](Bytes&, KeyPart) {});
  BC_GUARD_END(cx)
}

void bc_sort_key_col(bc_ctx* cx, bc_sort_key k, bc_view row, uint32_t col, uint32_t dir, uint32_t nulls) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return;
  auto v = read_column(cx, *rc);
  if (!v) return;
  auto type = v->state == bytecask::sql::ColumnState::Absent ? std::nullopt : std::optional{rc->slot->type};
  add_sort_key_part(cx, k, dir, nulls, type, v->bytes.size(),
                    [&](Bytes& out, KeyPart part) { key_put_column(out, *v, part); });
  BC_GUARD_END(cx)
}

bc_view bc_sort_key_end(bc_ctx* cx, bc_sort_key k) {
  BC_GUARD_BEGIN
  auto* b = cx->resolve_handle(k, cx->sort_keys, "sort key builder");
  if (b == nullptr) return {};
  return end_key_builder(cx, *b);
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
  if (!k) return false;
  return read_contains(cx, *k).value_or(false);
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
  c.gen = cx->next_gen();
  c.layout = kl;
  bytecask::sql::key_put_prefix(c.prefix, kl->ns, table_or_index);
  c.flags = flags;
  c.lo = lo_b->empty() ? c.prefix : Bytes(lo_b->begin(), lo_b->end());
  if ((flags & BC_SCAN_PREFIX) != 0) {
    c.hi = bytecask::sql::key_successor(c.lo);
  } else if (!hi_b->empty()) {
    c.hi.assign(hi_b->begin(), hi_b->end());
  }
  // The range it reads, inside its table's or index's keys: reserved in the
  // read set before it reads any of it.
  auto end = bytecask::sql::key_successor(c.prefix);
  BytesView rlo = std::is_lt(bytes_cmp(c.lo, c.prefix)) ? BytesView{c.prefix} : BytesView{c.lo};
  BytesView rhi = !c.hi.empty() && std::is_lt(bytes_cmp(c.hi, end)) ? BytesView{c.hi} : BytesView{end};
  c.recorded = cx->record_scan(c, rlo, rhi);
  if (!c.recorded) return {};
  c.held = cursor_bytes(c);
  auto gen = c.gen;
  auto charge = kCursorEntryBytes + c.held;
  // A cursor not opened read nothing: its reservation goes with it.
  try {
    cx->cursors.push_back(std::move(c));
  } catch (...) {
    cx->finish_scan(c);
    throw;
  }
  if (!cx->take(charge)) {
    cx->finish_scan(cx->cursors.back());
    cx->cursors.pop_back();
    return {};
  }
  return {static_cast<std::uint32_t>(cx->cursors.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}

bool bc_scan_next(bc_ctx* cx, bc_cursor h) {
  Cursor* c = nullptr;
  BC_GUARD_BEGIN
  c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr || c->done) return false;
  leave_entry(cx, *c);
  auto on_entry = cursor_advance(cx, *c);
  // One charge per step, before the copy: the next entry's bytes, or none.
  auto next = cursor_bytes(*c);
  if (on_entry) next += c->entries->key().size() + c->entries->value().size();
  if (!cx->rehold(c->held, next) || !on_entry) {
    if (c->read && !on_entry && cx->ok()) c->read->ran_out = true;
    c->done = true;
    cx->finish_scan(*c);
    return false;
  }
  auto k = c->entries->key();
  auto v = c->entries->value();
  if (c->read) {
    // The key it hands out, with room for k⁺'s byte: recorded as covered
    // when it closes early (finish_scan), allocating nothing then.
    c->read->last.reserve(k.size() + 1);
    c->read->last.assign(k.begin(), k.end());
    c->read->returned = true;
  }
  c->key.assign(k.begin(), k.end());
  c->value.assign(v.begin(), v.end());
  cx->begin_step(h.i, bc_ctx::Loop::Cursor, c->scope);
  c->positioned = true;
  return cx->ok();
  BC_GUARD_END(cx)
  if (c != nullptr) {
    c->done = true;  // a step that threw ends the scan
    cx->finish_scan(*c);
  }
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
  // The row is read at its key: an UPDATE of a column the index does not
  // hold changes no entry of the range, only this key.
  auto rec = cx->record_point(key);
  if (!rec || !cx->charge()) return {};
  if (!view_get(cx, *rec, key, c->row)) {
    // The runtime writes every entry with its row, so this is damage.
    cx->fail(BC_E_CORRUPT, "index " + c->layout->index->name + " has an entry whose row is missing");
    return {};
  }
  c->row_step = c->step;  // fetched once per entry, however often it is asked for
  if (!cx->rehold(c->held, cursor_bytes(*c))) return {};
  return cx->make_view(c->row.data(), c->row.size(), h.i, c->step, table, RowKind::Stored);
  BC_GUARD_END(cx)
  return {};
}

void bc_scan_close(bc_ctx* cx, bc_cursor h) {
  BC_GUARD_BEGIN
  auto* c = cx->resolve_handle(h, cx->cursors, "cursor");
  if (c == nullptr) return;
  leave_entry(cx, *c);
  c->entries.reset();
  c->done = true;
  cx->finish_scan(*c);
  (void)cx->rehold(c->held, cursor_bytes(*c));  // a release: it cannot fail
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

// ------------------------------------------------------- sort key decoders

int64_t bc_sort_key_part_i64(bc_ctx* cx, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  return sort_part_value<std::int64_t>(cx, key, part, PhysType::I64, 0,
                                       [](KeyReader& r) { return r.read_i64(KeyPart{PhysType::I64, false, false}); });
  BC_GUARD_END(cx)
  return {};
}

double bc_sort_key_part_f64(bc_ctx* cx, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  return sort_part_value<double>(cx, key, part, PhysType::F64, 0.0,
                                 [](KeyReader& r) { return r.read_f64(KeyPart{PhysType::F64, false, false}); });
  BC_GUARD_END(cx)
  return {};
}

bool bc_sort_key_part_bool(bc_ctx* cx, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  return sort_part_value<bool>(cx, key, part, PhysType::Bool, false,
                               [](KeyReader& r) { return r.read_bool(KeyPart{PhysType::Bool, false, false}); });
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_substr(bc_ctx* cx, bc_view s, int64_t from, int64_t count) {
  BC_GUARD_BEGIN
  auto text = cx->resolve(s);
  if (!text) return {};
  if (count < 0) {
    cx->fail(BC_E_ARITH, bytecask::sql::kSubstringError, "negative substring length not allowed");
    return {};
  }
  // Characters [from, from + count), 1-based, cut to the text.
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  auto end = count == kMax || from > kMax - count ? kMax : from + count;
  auto first = std::max<std::int64_t>(from, 1);
  if (end <= first) return cx->arena_view(BytesView{});
  std::size_t at = 0;
  std::int64_t pos = 1;
  while (at < text->size() && pos < first) {
    at += char_len(*text, at);
    ++pos;
  }
  auto start = std::min(at, text->size());
  while (at < text->size() && pos < end) {
    at += char_len(*text, at);
    ++pos;
  }
  return cx->arena_view(text->subspan(start, std::min(at, text->size()) - start));
  BC_GUARD_END(cx)
  return {};
}

namespace {

// Text as the characters the text module's functions take, and back as a
// view the statement holds. std::byte aliases as char.
auto chars(BytesView b) -> std::string_view { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

auto text_view(bc_ctx* cx, std::string_view s) -> bc_view { return cx->arena_view(std::as_bytes(std::span{s})); }

}  // namespace

bc_view bc_text_case(bc_ctx* cx, bc_view s, bool upper) {
  BC_GUARD_BEGIN
  auto text = cx->resolve(s);
  if (!text) return {};
  return text_view(cx, bytecask::sql::ascii_case(std::string{chars(*text)}, upper));
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_trim(bc_ctx* cx, bc_view s, bc_view set, uint32_t how) {
  BC_GUARD_BEGIN
  auto text = cx->resolve(s);
  if (!text) return {};
  std::string_view chosen{" "};
  if ((how & BC_TRIM_SPACES) == 0) {
    auto given = cx->resolve(set);
    if (!given) return {};
    chosen = chars(*given);
  }
  return text_view(cx, bytecask::sql::trimmed(chars(*text), chosen, (how & BC_TRIM_LEADING) != 0, (how & BC_TRIM_TRAILING) != 0));
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_char_length(bc_ctx* cx, bc_view s) {
  BC_GUARD_BEGIN
  auto text = cx->resolve(s);
  return text ? bytecask::sql::char_count(chars(*text)) : 0;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_octet_length(bc_ctx* cx, bc_view s) {
  BC_GUARD_BEGIN
  auto text = cx->resolve(s);
  return text ? std::ssize(*text) : 0;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_position(bc_ctx* cx, bc_view s, bc_view needle) {
  BC_GUARD_BEGIN
  auto v = cx->resolve2(s, needle);
  return v ? bytecask::sql::position_of(chars(v->second), chars(v->first)) : 0;
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_random(bc_ctx* cx, int64_t lo, int64_t hi) {
  BC_GUARD_BEGIN
  if (lo > hi) {
    cx->fail(BC_E_ARITH, bytecask::sql::kInvalidParameterValue, "lower bound must be less than or equal to upper bound");
    return 0;
  }
  // TLS: process — a generator per thread, holding no DB's state: draws are
  // not repeatable, as PostgreSQL's random() is not unless seeded.
  thread_local std::mt19937_64 gen{std::random_device{}()};
  return std::uniform_int_distribution<std::int64_t>{lo, hi}(gen);
  BC_GUARD_END(cx)
  return {};
}

// The codes bc_plan.h gives EXTRACT's fields are the binder's DateField.
static_assert(static_cast<uint32_t>(bytecask::sql::DateField::Microseconds) == BC_FIELD_MICROSECONDS);
static_assert(static_cast<uint32_t>(bytecask::sql::DateField::Year) == BC_FIELD_YEAR);
static_assert(static_cast<uint32_t>(bytecask::sql::DateField::Julian) == BC_FIELD_JULIAN);
static_assert(static_cast<uint32_t>(bytecask::sql::DateField::TimezoneMinute) == BC_FIELD_TIMEZONE_MINUTE);

int64_t bc_extract(bc_ctx* cx, int64_t v, uint32_t field, uint32_t source) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  auto kind = source == BC_SOURCE_DATE        ? SqlKind::Date
              : source == BC_SOURCE_TIME      ? SqlKind::Time
              : source == BC_SOURCE_TIMESTAMP ? SqlKind::Timestamp
                                              : SqlKind::TimestampTz;
  auto f = static_cast<bytecask::sql::DateField>(field);
  if (source < BC_SOURCE_DATE || source > BC_SOURCE_TIMESTAMPTZ || field < BC_FIELD_MICROSECONDS ||
      field > BC_FIELD_TIMEZONE_MINUTE || !bytecask::sql::date_field_built(f, kind)) {
    cx->fail(BC_E_HANDLE, "bc_extract: a field its source has no value for");
    return 0;
  }
  return bytecask::sql::extract_field(f, kind, v);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_sort_key_part_bytes(bc_ctx* cx, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto v = sort_part_value<Bytes>(cx, key, part, PhysType::Bytes, Bytes{},
                                  [](KeyReader& r) { return r.read_bytes(KeyPart{PhysType::Bytes, false, false}); });
  if (!cx->ok()) return {};
  return cx->arena_view(v);
  BC_GUARD_END(cx)
  return {};
}

bool bc_sort_key_part_null(bc_ctx* cx, bc_view key, uint32_t part) {
  BC_GUARD_BEGIN
  auto at = sort_part_at(cx, key, part, std::nullopt);
  return !at || at->null;
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
  // The whole range, even when the count stops at its limit: a superset,
  // since the engine's count does not name the key it stopped at.
  auto rec = cx->record_range(range->lo, range->hi);
  if (!rec) return 0;
  auto lim = static_cast<std::size_t>(limit);
  if (cx->overlay == nullptr || !cx->overlay->meets(range->lo, range->hi)) {
    return static_cast<std::int64_t>(cx->view(*rec).count_keys(range->lo, range->hi, lim));
  }
  return static_cast<std::int64_t>(view_count(cx, *rec, range->lo, range->hi, lim));
  BC_GUARD_END(cx)
  return {};
}

// ------------------------------------------------------------ rows, columns

bool bc_col_null(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return true;
  // The one decoder's verdict, damage included: the value is a view, not a copy.
  switch (bytecask::sql::row_column(rc->row, col, rc->slot->type, rc->slot->pos, rc->slot->width).state) {
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

bc_jsonb bc_col_jsonb(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, PhysType::Jsonb);
  if (!rc) return {};
  auto v = read_column(cx, *rc);
  if (!v || v->state == bytecask::sql::ColumnState::Absent) return {};
  // The format byte is checked once, here; the node shares the row's owner.
  auto node = jsonb::node_of(v->bytes);
  if (!node) {
    cx->fail(BC_E_CORRUPT, "a stored jsonb value of no known format in column " + std::to_string(col));
    return {};
  }
  auto [cursor, step] = cx->owner(row);
  return cx->make_jsonb(node->data(), node->size(), cursor, step);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_col_key(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return {};
  auto v = read_column(cx, *rc);
  if (!v) return {};
  // Nullable and ascending: NULL is the marker alone. A DOUBLE's -0.0 and
  // 0.0 encode alike, and every NaN as one, so equal values group as
  // PostgreSQL groups them.
  bytecask::sql::KeyPart part{rc->slot->type, true, false};
  Bytes key;
  if (v->state == bytecask::sql::ColumnState::Absent) {
    bytecask::sql::key_put_null(key, part);
  } else {
    key_put_column(key, *v, part);
  }
  // A group key, not an engine key: no key ceiling. It is the innermost
  // step's, whichever row it was read from: an outer row's key made in an
  // inner loop goes when that loop steps, not when the outer one does.
  return cx->arena_view(key);
  BC_GUARD_END(cx)
  return {};
}

int64_t bc_check_int(bc_ctx* cx, int64_t v, uint32_t int_type, uint32_t object_id) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return 0;
  switch (int_type) {
    case BC_SMALLINT:
      if (v >= INT16_MIN && v <= INT16_MAX) return v;
      cx->raise(BC_C_RANGE, object_id, "smallint out of range");
      return 0;
    case BC_INTEGER:
      if (v >= INT32_MIN && v <= INT32_MAX) return v;
      cx->raise(BC_C_RANGE, object_id, "integer out of range");
      return 0;
    case BC_BIGINT: return v;
    default: cx->fail(BC_E_HANDLE, "bc_check_int: an unknown integer type"); return 0;
  }
  BC_GUARD_END(cx)
  return {};
}

namespace {
// The row's primary key into `key`, and the row's bytes; nullopt on an error.
[[nodiscard]] auto row_pk(bc_ctx* cx, std::uint32_t table, bc_view row, Bytes& key) -> std::optional<BytesView> {
  const auto* t = cx->table(table);
  if (t == nullptr) return std::nullopt;
  auto r = write_row(cx, *t, row, false);
  if (!r || !encode_pk(cx, *t, *r, key)) return std::nullopt;
  return r;
}
}  // namespace

bc_view bc_row_key(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  Bytes key;
  if (!row_pk(cx, table, row, key)) return {};
  return cx->arena_view(key);
  BC_GUARD_END(cx)
  return {};
}

namespace {
// A builder of table t's current layout with every column null, pushed on
// the run's rows for the caller to fill. It is popped again unless the
// caller keeps it: a refusal, or an exception while filling it (bad_alloc
// in set_datum or copy_from), leaves no half-built row behind. Its entry is
// charged when it is kept.
class PushedBuilder {
public:
  PushedBuilder(bc_ctx* cx, const Table& t) : cx_{cx}, i_{cx->rows.size()} {
    cx_->rows.push_back(RowBuilder{cx->next_gen(), &t.layout, RowWriter{t.layout}, t.id, false, {}, 0});
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
    if (!cx_->take(kRowEntryBytes)) return {};
    kept_ = true;
    return {static_cast<std::uint32_t>(i_), cx_->rows[i_].gen};
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
  // A table without a primary key: its hidden row id, drawn now, so a retry
  // of the plan draws another.
  if (const auto* rid = t->row_id(); rid != nullptr && !b.writer().set_i64(rid->id, cx->live->next_row_id(*t))) {
    cx->fail(BC_E_CORRUPT, "the row id of table " + t->name + " does not fit its layout");
    return {};
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

extern "C++" {
namespace {

// A NUMERIC column's value: text as numeric_in reads it, stored as
// numeric_out writes it, so the column holds that form whatever spelling
// (1e2, 0x1F, " 1.50 ") a parameter or a view came in. Text that is no
// number is BC_C_RANGE, with numeric_in's message (22P02 or 22003); bytes
// that are not UTF-8 are refused first (22021), so that message quotes text.
void set_num(bc_ctx* cx, const SetTarget& at, std::uint32_t col, BytesView text) {
  if (!in_range(cx, at, col, text)) return;  // not UTF-8: 22021, as a TEXT column
  std::string canonical;
  try {
    canonical = bytecask::sql::numeric_in(chars(text));
  } catch (const bytecask::sql::TextError& e) {
    cx->raise(BC_C_RANGE, at.builder->table, e.what());
    return;
  }
  auto b = std::as_bytes(std::span{canonical.data(), canonical.size()});
  if (!at.builder->writer.set_bytes(col, BytesView{b})) cx->fail(BC_L_ROW, "column is not in the row's layout");
}

}  // namespace
}  // extern "C++"

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
  if (auto at = set_target(cx, r, col, PhysType::Bytes); at && at->slot->type == PhysType::Num) {
    set_num(cx, *at, col, *bytes);
    return;
  }
  set_column(cx, r, col, PhysType::Bytes, *bytes, [&](RowWriter& w) { return w.set_bytes(col, *bytes); });
  BC_GUARD_END(cx)
}

void bc_row_set_jsonb(bc_ctx* cx, bc_row r, uint32_t col, bc_jsonb j) {
  BC_GUARD_BEGIN
  auto node = cx->resolve_jsonb(j);
  if (!node) return;
  auto at = set_target(cx, r, col, PhysType::Jsonb);
  if (!at) return;
  set_jsonb(cx, *at, col, *node);
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
  if (const auto* text = std::get_if<Bytes>(d); text != nullptr && at->slot->type == PhysType::Num) {
    set_num(cx, *at, col, *text);
    return;
  }
  if (const auto* text = std::get_if<Bytes>(d); text != nullptr && at->slot->type == PhysType::Jsonb) {
    set_jsonb_text(cx, *at, col, *text);
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
  if (!b->writer.finish(b->built)) {
    cx->fail(BC_E_LIMIT, "row over the engine's value limit");
    return {};
  }
  if (!cx->rehold(b->held, b->built.size())) return {};
  return cx->make_view(b->built.data(), b->built.size(), kNoCursor, 0, b->table, RowKind::Built);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_null_row(bc_ctx* cx, uint32_t table) {
  BC_GUARD_BEGIN
  const auto* t = cx->table(table);
  if (t == nullptr) return {};
  // A writer given nothing: every column NULL, no default and no row id, as
  // bc_row_begin would give. Not a row any write takes: a NOT NULL column's
  // NULL is checked only by bc_row_end.
  Bytes row;
  if (!RowWriter{t->layout}.finish(row)) {
    cx->fail(BC_E_LIMIT, "row over the engine's value limit");
    return {};
  }
  return cx->arena_view(row, table, RowKind::Built);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_pin(bc_ctx* cx, bc_view v) {
  BC_GUARD_BEGIN
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return {};
  if (v.g == 0 && v.i == 0) return {};
  const auto* e = cx->find_view(v);
  auto table = e->table;
  auto kind = e->kind;
  const auto* p = cx->arena_copy(cx->pins, *b);
  if (p == nullptr) return {};
  auto gen = cx->next_gen();
  if (cx->push_charged(cx->pinned_views,
                       ViewEntry{p, 0, gen, static_cast<std::uint32_t>(b->size()), kNoCursor, table, kind, false},
                       kViewEntryBytes) == nullptr) {
    return {};
  }
  return {static_cast<std::uint32_t>(cx->pinned_views.size() - 1) | kPinnedView, gen};
  BC_GUARD_END(cx)
  return {};
}

// --------------------------------------------------- ephemeral structures
//
// Each copies what it is given, charged to the statement's memory budget
// (its bytes and a fixed amount for its node), and keeps it until reset.
// Iterating one is a loop: a view it hands out is the step's.

extern "C++" {
namespace {

// The bytes of a view, with what it is: a row stays one in a structure.
auto resolve_held(bc_ctx* cx, bc_view v) -> std::optional<std::pair<BytesView, Held>> {
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return std::nullopt;
  Held h;
  if (const auto* e = cx->find_view(v)) h = {e->table, e->kind};
  return std::pair{*b, h};
}

auto held_view(bc_ctx* cx, BytesView b, Held h) -> bc_view {
  return cx->make_view(b.data(), b.size(), kNoCursor, 0, h.table, h.kind);
}

auto sorter(bc_ctx* cx, bc_sorter s) -> Sorter* { return cx->resolve_handle(s, cx->sorters, "sorter"); }

// Sorts on the first read; a top-n sorter's heap becomes its first n in order.
void sort_once(Sorter& s) {
  if (s.sorted) return;
  std::ranges::sort(s.entries, sort_before);
  s.sorted = true;
}

}  // namespace
}  // extern "C++"

bc_sorter bc_sorter_open(bc_ctx* cx, int64_t keep) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return {};
  if (keep < 0) {
    cx->fail(BC_E_HANDLE, "a sorter keeps a negative number of entries");
    return {};
  }
  if (!cx->take(kSorterBytes)) return {};
  auto gen = cx->next_gen();
  cx->sorters.push_back({gen, static_cast<std::uint64_t>(keep), {}, 0, false, 0, kNoStep, {}});
  return {static_cast<std::uint32_t>(cx->sorters.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}

void bc_sorter_add(bc_ctx* cx, bc_sorter h, bc_view sort_key, bc_view payload) {
  BC_GUARD_BEGIN
  auto* s = sorter(cx, h);
  if (s == nullptr) return;
  if (s->sorted) {
    cx->fail(BC_E_HANDLE, "bc_sorter_add after the sorter was read");
    return;
  }
  auto key = cx->resolve(sort_key);
  auto p = resolve_held(cx, payload);
  if (!key || !p) return;
  SortEntry e{{}, s->added, 0, p->second};
  // A view's length, and so a sort key's, fits key_len.
  static_assert(std::is_same_v<decltype(ViewEntry::len), decltype(SortEntry::key_len)>);
  e.key_len = static_cast<std::uint32_t>(key->size());
  auto bytes = key->size() + p->first.size();
  auto full = s->keep != 0 && s->entries.size() == s->keep;
  // A top-n sorter that holds n: an entry after the last of them is never
  // among the first n, and is neither copied nor charged.
  if (full) {
    auto& last = s->entries.front();
    ++s->added;
    if (std::is_gteq(bytes_cmp(*key, last.key()))) return;  // equal keys: the earlier one stays
    // It takes the last one's place: only the difference is charged.
    auto old = last.data.size();
    if (bytes > old && !cx->take(bytes - old)) return;
    e.data.reserve(bytes);
    e.data.assign(key->begin(), key->end());
    e.data.insert(e.data.end(), p->first.begin(), p->first.end());
    std::ranges::pop_heap(s->entries, sort_before);
    s->entries.back() = std::move(e);
    std::ranges::push_heap(s->entries, sort_before);
    if (bytes < old) cx->release(old - bytes);
    return;
  }
  if (!cx->take(bytes + kSortEntryBytes)) return;
  e.data.reserve(bytes);
  e.data.assign(key->begin(), key->end());
  e.data.insert(e.data.end(), p->first.begin(), p->first.end());
  s->entries.push_back(std::move(e));
  ++s->added;
  if (s->keep != 0) std::ranges::push_heap(s->entries, sort_before);
  BC_GUARD_END(cx)
}

int64_t bc_sorter_count(bc_ctx* cx, bc_sorter h) {
  BC_GUARD_BEGIN
  const auto* s = sorter(cx, h);
  if (s == nullptr) return 0;
  return std::ssize(s->entries);
  BC_GUARD_END(cx)
  return {};
}

bc_view bc_sorter_nth(bc_ctx* cx, bc_sorter h, int64_t n) {
  BC_GUARD_BEGIN
  auto* s = sorter(cx, h);
  if (s == nullptr) return {};
  if (n < 0 || std::cmp_greater_equal(n, s->entries.size())) {
    cx->fail(BC_E_HANDLE, "bc_sorter_nth past the sorter's entries");
    return {};
  }
  sort_once(*s);
  const auto& e = s->entries[static_cast<std::size_t>(n)];
  // The view lives until the next bc_sorter_nth on this sorter, which takes
  // its slot: a loop over nth holds one view, not one per call.
  return cx->remake_view(s->nth, e.payload(), e.held.table, e.held.kind);
  BC_GUARD_END(cx)
  return {};
}

bool bc_sorter_next(bc_ctx* cx, bc_sorter h, bc_view* payload) {
  BC_GUARD_BEGIN
  auto* s = sorter(cx, h);
  if (s == nullptr) return false;
  // Iterating the entries is a loop: its step ends as it moves to the next.
  if (s->scope != kNoStep) cx->end_steps_from(s->scope);
  sort_once(*s);
  if (s->next >= s->entries.size() || !cx->charge()) return false;
  const auto& e = s->entries[s->next++];
  cx->begin_step(h.i, bc_ctx::Loop::Sorter, s->scope);
  auto v = held_view(cx, e.payload(), e.held);
  if (payload != nullptr) *payload = v;
  return cx->ok();
  BC_GUARD_END(cx)
  return {};
}

bc_hash bc_hash_open(bc_ctx* cx) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return {};
  if (!cx->take(kHashBytes)) return {};
  auto gen = cx->next_gen();
  cx->hashes.push_back({gen, {}, {}});
  return {static_cast<std::uint32_t>(cx->hashes.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}

// A payload charges its bytes and kHashEntryBytes, and a key new to the
// table its own bytes once.
void bc_hash_put(bc_ctx* cx, bc_hash h, bc_view key, bc_view payload) {
  BC_GUARD_BEGIN
  auto* t = cx->resolve_handle(h, cx->hashes, "hash table");
  if (t == nullptr) return;
  auto k = cx->resolve(key);
  auto p = resolve_held(cx, payload);
  if (!k || !p) return;
  if (t->entries.size() >= kNoEntry) {
    cx->fail(BC_E_LIMIT, "a hash table of 2^32 entries");
    return;
  }
  auto it = t->index.find(*k);
  auto fresh = it == t->index.end();
  if (!cx->take(p->first.size() + kHashEntryBytes + (fresh ? k->size() : 0))) return;
  auto at = static_cast<std::uint32_t>(t->entries.size());
  t->entries.push_back({Bytes(p->first.begin(), p->first.end()), kNoEntry, p->second});
  if (fresh) {
    try {
      t->index.emplace(Bytes(k->begin(), k->end()), Chain{at, at});
    } catch (...) {
      t->entries.pop_back();  // no chain reaches it
      throw;
    }
    return;
  }
  t->entries[it->second.last].next = at;
  it->second.last = at;
  BC_GUARD_END(cx)
}

bc_hash_iter bc_hash_find(bc_ctx* cx, bc_hash h, bc_view key) {
  BC_GUARD_BEGIN
  auto* t = cx->resolve_handle(h, cx->hashes, "hash table");
  if (t == nullptr) return {};
  auto k = cx->resolve(key);
  if (!k || !cx->ok()) return {};
  auto it = t->index.find(*k);
  auto gen = cx->next_gen();
  HashIter probe{gen, h.i, it == t->index.end() ? kNoEntry : it->second.first, kNoStep};
  if (cx->push_charged(cx->hash_iters, std::move(probe), kHashIterEntryBytes) == nullptr) return {};
  return {static_cast<std::uint32_t>(cx->hash_iters.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}

bool bc_hash_next(bc_ctx* cx, bc_hash_iter h, bc_view* payload) {
  BC_GUARD_BEGIN
  auto* probe = cx->resolve_handle(h, cx->hash_iters, "hash table probe");
  if (probe == nullptr) return false;
  // Iterating a key's payloads is a loop: its step ends as it moves to the
  // next. The probe was made before its first step, and outlives it.
  if (probe->scope != kNoStep) cx->end_steps_from(probe->scope);
  if (probe->next == kNoEntry || !cx->charge()) return false;
  const auto& e = cx->hashes[probe->hash].entries[probe->next];
  probe->next = e.next;
  cx->begin_step(h.i, bc_ctx::Loop::Probe, probe->scope);
  auto v = held_view(cx, e.payload, e.held);
  if (payload != nullptr) *payload = v;
  return cx->ok();
  BC_GUARD_END(cx)
  return {};
}

static_assert(2 * sizeof(bc_ctx::Agg) <= kAggBytes);

// What a group of `a` whose key is `key_bytes` long is charged, and
// released by bc_agg_clear.
static auto group_bytes(const bc_ctx::Agg& a, std::size_t key_bytes) -> std::uint64_t {
  return key_bytes + kGroupEntryBytes + std::uint64_t{a.slots} * kAggSlotBytes;
}

// A group's handle names its index in the aggregate, with the aggregate's
// generation. Opening one charges kAggBytes and its slots' kinds; a new
// group charges its key's bytes, kGroupEntryBytes, the
// aggregate's own node, and kAggSlotBytes per slot to the statement's
// memory budget.
bc_agg bc_agg_open(bc_ctx* cx, uint32_t slots) {
  BC_GUARD_BEGIN
  if (!cx->take(kAggBytes + std::uint64_t{slots} * sizeof(SlotKind))) return {};
  bc_ctx::Agg a;
  a.gen = cx->next_gen();
  a.groups = cx->next_gen();
  a.slots = slots;
  a.kinds.assign(slots, SlotKind::None);
  auto gen = a.gen;
  cx->aggs.push_back(std::move(a));
  return {static_cast<std::uint32_t>(cx->aggs.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}
// Drops every group, and releases what each was charged; the slots keep
// their kinds. Refused once bc_agg_next has started on it.
void bc_agg_clear(bc_ctx* cx, bc_agg a) {
  BC_GUARD_BEGIN
  auto* agg = cx->resolve_handle(a, cx->aggs, "aggregate");
  if (agg == nullptr || !cx->ok()) return;
  if (agg->next != 0) {
    cx->fail(BC_E_HANDLE, "an aggregate cleared after bc_agg_next");
    return;
  }
  std::uint64_t released = 0;
  for (const auto& [key, i] : agg->index) released += group_bytes(*agg, key.size());
  released += held_value_bytes(*agg);
  agg->groups = cx->next_gen();
  agg->index.clear();
  agg->order.clear();
  agg->values.clear();
  agg->held.reset();
  cx->release(released);
  BC_GUARD_END(cx)
}
bc_group bc_agg_group(bc_ctx* cx, bc_agg a, bc_view group_key) {
  BC_GUARD_BEGIN
  auto* agg = cx->resolve_handle(a, cx->aggs, "aggregate");
  if (agg == nullptr) return {};
  auto key = cx->resolve(group_key);
  if (!key || !cx->ok()) return {};
  if (auto it = agg->index.find(*key); it != agg->index.end()) return {it->second, agg->groups};
  if (agg->order.size() >= kNoEntry) {
    cx->fail(BC_E_LIMIT, "an aggregate of 2^32 groups");
    return {};
  }
  if (!cx->take(group_bytes(*agg, key->size()))) return {};
  agg->values.resize(agg->values.size() + agg->slots);
  auto i = static_cast<std::uint32_t>(agg->order.size());
  auto it = agg->index.emplace(Bytes(key->begin(), key->end()), i).first;
  agg->order.push_back(&it->first);
  return {i, agg->groups};
  BC_GUARD_END(cx)
  return {};
}

extern "C++" {
namespace {

// The aggregate whose groups' generation a group handle carries. A clear
// draws a new one, so they are not in the order opened: the search is
// linear, over the few aggregates a plan opens.
auto group_agg(bc_ctx* cx, bc_group g) -> bc_ctx::Agg* {
  if (!cx->ok()) return nullptr;
  auto it = std::ranges::find(cx->aggs, g.g, &bc_ctx::Agg::groups);
  if (it == cx->aggs.end() || g.i >= it->order.size()) {
    cx->fail(BC_E_HANDLE, "stale or foreign group");
    return nullptr;
  }
  return &*it;
}

auto slot_kind_name(SlotKind k) -> const char* {
  switch (k) {
    case SlotKind::None: return "unused";
    case SlotKind::SumI64: return "an integer sum";
    case SlotKind::SumF64: return "a double sum";
    case SlotKind::MinI64: return "a minimum";
    case SlotKind::MaxI64: return "a maximum";
    case SlotKind::MinF64: return "a double minimum";
    case SlotKind::MaxF64: return "a double maximum";
    case SlotKind::Count: return "a count";
    case SlotKind::MinBytes: return "a bytes minimum";
    case SlotKind::MaxBytes: return "a bytes maximum";
  }
  return "a slot";
}

// The slot a fold of `kind` goes to; nullptr and BC_E_HANDLE for a slot out
// of range, or one an earlier fold made another kind.
auto fold_slot(bc_ctx* cx, bc_group g, std::uint32_t slot, SlotKind kind) -> Slot* {
  auto* agg = group_agg(cx, g);
  if (agg == nullptr) return nullptr;
  if (slot >= agg->slots) {
    cx->fail(BC_E_HANDLE, "slot " + std::to_string(slot) + " of an aggregate of " + std::to_string(agg->slots));
    return nullptr;
  }
  auto& k = agg->kinds[slot];
  if (k != SlotKind::None && k != kind) {
    cx->fail(BC_E_HANDLE, "slot " + std::to_string(slot) + " is " + slot_kind_name(k) + ", not " + slot_kind_name(kind));
    return nullptr;
  }
  k = kind;
  return &agg->values[std::size_t{g.i} * agg->slots + slot];
}

// What a read of a slot takes it as.
enum class SlotRead : std::uint8_t { Any, I64, F64, Bytes };

// The slot a read goes to; nullptr and BC_E_HANDLE for a slot out of range,
// or one an earlier fold made of another type than the read's.
auto read_slot(bc_ctx* cx, bc_group g, std::uint32_t slot, SlotRead read) -> const Slot* {
  auto* agg = group_agg(cx, g);
  if (agg == nullptr) return nullptr;
  if (slot >= agg->slots) {
    cx->fail(BC_E_HANDLE, "slot " + std::to_string(slot) + " of an aggregate of " + std::to_string(agg->slots));
    return nullptr;
  }
  auto k = agg->kinds[slot];
  auto is_f64 = k == SlotKind::SumF64 || k == SlotKind::MinF64 || k == SlotKind::MaxF64;
  auto is_bytes = k == SlotKind::MinBytes || k == SlotKind::MaxBytes;
  auto kind = is_f64 ? SlotRead::F64 : is_bytes ? SlotRead::Bytes : SlotRead::I64;
  if (read != SlotRead::Any && k != SlotKind::None && kind != read) {
    const char* as = read == SlotRead::F64 ? "a double" : read == SlotRead::Bytes ? "bytes" : "an integer";
    cx->fail(BC_E_HANDLE, std::string{"slot "} + std::to_string(slot) + " is " + slot_kind_name(k) + ", read as " + as);
    return nullptr;
  }
  return &agg->values[std::size_t{g.i} * agg->slots + slot];
}

// Folds `v` into a MinBytes or MaxBytes slot of group g: held when it is
// the slot's first value, or orders before (MIN) or after (MAX) the one
// held, by bytes_cmp, the order of bc_bytes_cmp and of sort keys. The new
// value is charged before it is copied, and the old one released after: a
// failure (BC_E_LIMIT past the budget, BC_E_MEMORY) leaves the old value
// held and charged.
void fold_bytes(bc_ctx* cx, bc_group g, std::uint32_t slot, bc_view v, SlotKind kind) {
  auto* s = fold_slot(cx, g, slot, kind);
  if (s == nullptr) return;
  auto& agg = *group_agg(cx, g);
  auto b = cx->resolve(v);
  if (!b || !cx->ok()) return;
  if (s->set) {
    auto& old = (*agg.held)[static_cast<std::size_t>(s->i)];
    auto o = bytes_cmp(*b, old);
    if (kind == SlotKind::MinBytes ? !std::is_lt(o) : !std::is_gt(o)) return;
    if (!cx->take(b->size())) return;
    Bytes copy;
    try {
      copy.assign(b->begin(), b->end());
    } catch (...) {
      cx->release(b->size());
      throw;
    }
    cx->release(old.size());
    old = std::move(copy);
    return;
  }
  const auto made = agg.held ? 0 : kAllocBytes + sizeof(std::vector<Bytes>);
  const auto charge = b->size() + kAggHeldBytes + made;
  if (!cx->take(charge)) return;
  try {
    if (!agg.held) agg.held = std::make_unique<std::vector<Bytes>>();
    agg.held->emplace_back(b->begin(), b->end());
  } catch (...) {
    // A vector made empty is released whole.
    if (made != 0 && agg.held && agg.held->empty()) agg.held.reset();
    cx->release(agg.held ? charge - made : charge);
    throw;
  }
  s->i = static_cast<std::int64_t>(agg.held->size() - 1);
  s->set = true;
}

}  // namespace
}  // extern "C++"

void bc_agg_add_i64(bc_ctx* cx, bc_group g, uint32_t slot, int64_t v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::SumI64);
  if (s == nullptr) return;
  std::int64_t r = 0;
  if (__builtin_add_overflow(s->i, v, &r)) {
    cx->fail(BC_E_ARITH, bytecask::sql::kNumericValueOutOfRange, "bigint out of range");
    return;
  }
  s->i = r;
  s->set = true;
  BC_GUARD_END(cx)
}
// PostgreSQL's float8 sum: a finite sum that overflows is an error, and an
// infinite or NaN input carries through.
static auto sum_f64(bc_ctx* cx, double a, double b) -> double {
  auto r = a + b;
  if (std::isinf(r) && !std::isinf(a) && !std::isinf(b)) cx->fail(BC_E_ARITH, bytecask::sql::kNumericValueOutOfRange, "value out of range: overflow");
  return r;
}
// PostgreSQL's float8 order: NaN above every number, -0.0 equal to 0.0.
static auto less_f64(double a, double b) -> bool { return !std::isnan(a) && (std::isnan(b) || a < b); }

double bc_add_f64(bc_ctx* cx, double a, double b) {
  BC_GUARD_BEGIN
  return sum_f64(cx, a, b);
  BC_GUARD_END(cx)
  return {};
}
// As float8smaller and float8larger: of equal values (-0.0 and 0.0) the second.
double bc_min_f64(double a, double b) { return less_f64(a, b) ? a : b; }
double bc_max_f64(double a, double b) { return less_f64(b, a) ? a : b; }
// As float8_cmp_internal: NaN equals NaN and is above every other value.
int bc_cmp_f64(double a, double b) { return less_f64(a, b) ? -1 : less_f64(b, a) ? 1 : 0; }
void bc_agg_add_f64(bc_ctx* cx, bc_group g, uint32_t slot, double v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::SumF64);
  if (s == nullptr) return;
  auto r = sum_f64(cx, s->f, v);
  if (!cx->ok()) return;
  s->f = r;
  s->set = true;
  BC_GUARD_END(cx)
}
void bc_agg_min_f64(bc_ctx* cx, bc_group g, uint32_t slot, double v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::MinF64);
  if (s == nullptr) return;
  s->f = s->set ? bc_min_f64(s->f, v) : v;
  s->set = true;
  BC_GUARD_END(cx)
}
void bc_agg_max_f64(bc_ctx* cx, bc_group g, uint32_t slot, double v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::MaxF64);
  if (s == nullptr) return;
  s->f = s->set ? bc_max_f64(s->f, v) : v;
  s->set = true;
  BC_GUARD_END(cx)
}
void bc_agg_min_i64(bc_ctx* cx, bc_group g, uint32_t slot, int64_t v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::MinI64);
  if (s == nullptr) return;
  if (!s->set || v < s->i) s->i = v;
  s->set = true;
  BC_GUARD_END(cx)
}
void bc_agg_max_i64(bc_ctx* cx, bc_group g, uint32_t slot, int64_t v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::MaxI64);
  if (s == nullptr) return;
  if (!s->set || v > s->i) s->i = v;
  s->set = true;
  BC_GUARD_END(cx)
}
void bc_agg_min_bytes(bc_ctx* cx, bc_group g, uint32_t slot, bc_view v) {
  BC_GUARD_BEGIN
  fold_bytes(cx, g, slot, v, SlotKind::MinBytes);
  BC_GUARD_END(cx)
}
void bc_agg_max_bytes(bc_ctx* cx, bc_group g, uint32_t slot, bc_view v) {
  BC_GUARD_BEGIN
  fold_bytes(cx, g, slot, v, SlotKind::MaxBytes);
  BC_GUARD_END(cx)
}
void bc_agg_count(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::Count);
  if (s == nullptr) return;
  if (__builtin_add_overflow(s->i, 1, &s->i)) {
    cx->fail(BC_E_ARITH, bytecask::sql::kNumericValueOutOfRange, "bigint out of range");
    return;
  }
  s->set = true;
  BC_GUARD_END(cx)
}
bool bc_agg_next(bc_ctx* cx, bc_agg a, bc_view* group_key, bc_group* g) {
  BC_GUARD_BEGIN
  auto* agg = cx->resolve_handle(a, cx->aggs, "aggregate");
  if (agg == nullptr) return false;
  // Iterating the groups is a loop: its step ends as it moves to the next.
  if (agg->scope != kNoStep) cx->end_steps_from(agg->scope);
  if (agg->next >= agg->order.size() || !cx->charge()) return false;
  auto i = agg->next++;
  cx->begin_step(a.i, bc_ctx::Loop::Agg, agg->scope);
  const auto& k = *agg->order[i];
  if (group_key != nullptr) *group_key = cx->make_view(k.data(), k.size());
  if (g != nullptr) *g = {static_cast<std::uint32_t>(i), agg->groups};
  return cx->ok();
  BC_GUARD_END(cx)
  return {};
}
int64_t bc_agg_i64(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, SlotRead::I64);
  return s == nullptr ? 0 : s->i;
  BC_GUARD_END(cx)
  return {};
}
double bc_agg_f64(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, SlotRead::F64);
  return s == nullptr ? 0 : s->f;
  BC_GUARD_END(cx)
  return {};
}
// A copy in the step's arena: the held value changes when a later fold
// replaces it, and goes with a clear. The empty view when no value reached
// the slot (bc_agg_null).
bc_view bc_agg_bytes(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, SlotRead::Bytes);
  if (s == nullptr || !s->set) return {};
  return cx->arena_view((*group_agg(cx, g)->held)[static_cast<std::size_t>(s->i)]);
  BC_GUARD_END(cx)
  return {};
}
// PostgreSQL's NULL for SUM, MIN, MAX or AVG of a group no value reached.
bool bc_agg_null(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, SlotRead::Any);
  return s != nullptr && !s->set;
  BC_GUARD_END(cx)
  return {};
}

// -------------------------------------------------------------------- writes

namespace {

// A row an INSERT writes: its table, its bytes, its primary key and its
// index entries, encoded before anything is checked or recorded.
struct NewRow {
  const Table* t{nullptr};
  BytesView row;
  Bytes pk;
  std::vector<Entry> entries;
};

// (Results through `out` here: these functions have C linkage.)
[[nodiscard]] auto new_row(bc_ctx* cx, uint32_t table, bc_view row, NewRow& n) -> bool {
  n.t = cx->table(table, true);
  if (n.t == nullptr) return false;
  auto r = write_row(cx, *n.t, row, false);
  if (!r) return false;
  n.row = *r;
  n.entries.resize(n.t->indexes.size());
  if (!encode_pk(cx, *n.t, *r, n.pk)) return false;
  for (std::size_t i = 0; i < n.t->indexes.size(); ++i) {
    if (!encode_entry(cx, *n.t, n.t->indexes[i], *r, n.entries[i])) return false;
  }
  return true;
}

// The unique keys ON CONFLICT's `arbiter` covers, by object id, into
// `out`: the table's (its primary key) and its unique indexes'.
// BC_ARBITER_ANY covers each; a named key covers every unique key over its
// columns, as a set, as PostgreSQL makes each such index an arbiter. False
// when `arbiter` is no unique key of the table.
[[nodiscard]] auto arbiter_keys(const Table& t, std::uint32_t arbiter, std::vector<std::uint32_t>& out) -> bool {
  std::vector<std::pair<std::uint32_t, std::set<std::uint32_t>>> keys;
  keys.emplace_back(t.id, std::set<std::uint32_t>(t.pk_col_ids.begin(), t.pk_col_ids.end()));
  for (const auto& ix : t.indexes) {
    if (!ix.unique) continue;
    auto& [id, cols] = keys.emplace_back(ix.id, std::set<std::uint32_t>{});
    for (const auto& p : ix.parts) cols.insert(p.col_id);
  }
  const auto named = std::ranges::find_if(keys, [&](const auto& k) { return k.first == arbiter; });
  if (arbiter != BC_ARBITER_ANY && named == keys.end()) return false;
  for (const auto& [id, cols] : keys) {
    if (arbiter == BC_ARBITER_ANY || cols == named->second) out.push_back(id);
  }
  return true;
}

// Whether `arbiters`, arbiter_keys' (none for a plain INSERT), cover object `id`.
[[nodiscard]] auto arbiter_covers(std::span<const std::uint32_t> arbiters, std::uint32_t id) -> bool {
  return std::ranges::find(arbiters, id) != arbiters.end();
}

// An arbiter's unique prefix the row takes, checked free in the view: its
// range guard, recorded as unique_free records one, and no claim.
[[nodiscard]] auto take_arbiter_prefix(bc_ctx* cx, const Index& ix, const Entry& entry) -> bool {
  auto prefix = entry.prefix();
  if (!record_unique(cx, ix, prefix)) {
    if (cx->ok()) cx->fail(BC_E_HANDLE, "an arbiter prefix of index " + ix.name + " taken after it was checked free");
    return false;
  }
  cx->push_effect(EffectKind::GuardUniqueRange, prefix, bytecask::sql::key_successor(prefix));
  return true;
}

// Checks the row's keys and writes the row and its entries. A key under
// `arbiters` was checked free by bc_insert_on_conflict: it takes no insert
// claim, so a concurrent commit of it is a conflict, never BC_C_UNIQUE.
void insert_row(bc_ctx* cx, NewRow& n, std::span<const std::uint32_t> arbiters) {
  const auto& t = *n.t;
  auto vacated = vacated_here(cx, n.pk);
  if (!first_write(cx, t, n.pk, true, true)) return;
  if (!vacated && !arbiter_covers(arbiters, t.id) && !pk_free(cx, t, n.pk)) return;
  for (std::size_t i = 0; i < t.indexes.size(); ++i) {
    const auto& ix = t.indexes[i];
    // Nulls are distinct: an entry with a null part collides with nothing.
    if (!ix.unique || n.entries[i].has_null) continue;
    if (!(arbiter_covers(arbiters, ix.id) ? take_arbiter_prefix(cx, ix, n.entries[i]) : unique_free(cx, ix, n.entries[i]))) {
      return;
    }
  }
  cx->push_effect(EffectKind::Put, std::move(n.pk), n.row);
  for (auto& e : n.entries) cx->push_effect(EffectKind::Put, std::move(e.key));
}

// Whether ON CONFLICT DO NOTHING skips the row: a key under an arbiter is
// a row this execution wrote, or one the view holds that it did not vacate.
// Reads, charged; at Serializable each arbiter key or prefix it probes is
// recorded as a read; takes no claim. True too once an error is set.
[[nodiscard]] auto conflicts(bc_ctx* cx, const NewRow& n, std::span<const std::uint32_t> arbiters) -> bool {
  const auto& t = *n.t;
  if (arbiter_covers(arbiters, t.id)) {
    auto it = cx->written_rows.find(n.pk);
    if (it != cx->written_rows.end() && it->second) return true;
    if (it == cx->written_rows.end()) {
      // At SERIALIZABLE a skip is a read of the arbiter key, recorded
      // (docs/transaction_design.md, owner's question 1).
      auto taken = read_contains(cx, n.pk);
      if (!taken || *taken) return true;
    }
  }
  for (std::size_t i = 0; i < t.indexes.size(); ++i) {
    const auto& ix = t.indexes[i];
    if (!ix.unique || !arbiter_covers(arbiters, ix.id) || n.entries[i].has_null) continue;
    auto prefix = n.entries[i].prefix();
    if (cx->unique_written.contains({ix.id, Bytes(prefix.begin(), prefix.end())})) return true;
    if (!cx->charge()) return true;
    auto claim = true;
    auto taken = prefix_taken(cx, prefix, bytecask::sql::key_successor(prefix), claim);
    if (!taken || *taken) return true;
  }
  return false;
}

// ON CONFLICT DO UPDATE: the row key of the row that holds a key of `n`
// under `arbiters` into `pk`, the primary key first, then the unique
// indexes in order; false when none is taken, or with the error set. A key
// a row this execution wrote holds (written_rows, unique_written), or a
// unique prefix whose holder in the view it wrote and that kept it, is
// BC_E_CARDINALITY, as PostgreSQL refuses a command that affects a row
// twice. A primary key this execution vacated is free, as conflicts() has
// it, and so is a unique prefix its update took from the view's holder
// (unique_vacated): no other row holds it, or unique_written would. Reads,
// charged; at Serializable each arbiter key or range it probes is recorded
// as a read; takes no claim.
[[nodiscard]] auto holder(bc_ctx* cx, const NewRow& n, std::span<const std::uint32_t> arbiters, Bytes& pk) -> bool {
  const auto& t = *n.t;
  auto twice = [&] {
    cx->fail(BC_E_CARDINALITY, "ON CONFLICT DO UPDATE command cannot affect row a second time");
    return false;
  };
  if (arbiter_covers(arbiters, t.id)) {
    auto it = cx->written_rows.find(n.pk);
    if (it != cx->written_rows.end() && it->second) return twice();
    if (it == cx->written_rows.end()) {
      auto taken = read_contains(cx, n.pk);
      if (!taken) return false;
      if (*taken) {
        pk = n.pk;
        return true;
      }
    }
  }
  for (std::size_t i = 0; i < t.indexes.size(); ++i) {
    const auto& ix = t.indexes[i];
    if (!ix.unique || !arbiter_covers(arbiters, ix.id) || n.entries[i].has_null) continue;
    auto prefix = n.entries[i].prefix();
    if (cx->unique_written.contains({ix.id, Bytes(prefix.begin(), prefix.end())})) return twice();
    auto end = bytecask::sql::key_successor(prefix);
    auto rec = cx->record_range(prefix, end);
    if (!rec || !cx->charge()) return false;
    ViewEntries entries{cx->view(*rec), cx->overlay, prefix, end, false};
    if (!entries.next(cx)) {
      if (!cx->ok()) return false;
      continue;
    }
    // The holder's row key: the table's prefix and the entry's primary-key
    // suffix, found by skipping the index parts.
    const auto* kl = cx->cat->key_layout(ix.id);
    if (kl == nullptr) {
      cx->fail(BC_E_HANDLE, "no index " + std::to_string(ix.id));
      return false;
    }
    KeyReader r{entries.key(), bytecask::sql::kKeyPrefixBytes};
    for (std::size_t p = 0; p < kl->index_parts; ++p) {
      if (!r.skip(kl->parts[p])) {
        cx->fail(BC_E_CORRUPT, "unparseable index key");
        return false;
      }
    }
    pk.clear();
    bytecask::sql::key_put_prefix(pk, bytecask::sql::kNsRow, t.id);
    auto rest = r.rest();
    pk.insert(pk.end(), rest.begin(), rest.end());
    if (!cx->written_rows.contains(pk)) return true;
    if (!cx->unique_vacated.contains({ix.id, Bytes(prefix.begin(), prefix.end())})) return twice();
  }
  return false;
}

}  // namespace

void bc_insert(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  if (!cx->writable("INSERT")) return;
  NewRow n{};
  if (new_row(cx, table, row, n)) insert_row(cx, n, {});
  BC_GUARD_END(cx)
}

bool bc_insert_on_conflict(bc_ctx* cx, uint32_t table, bc_view row, uint32_t arbiter) {
  BC_GUARD_BEGIN
  if (!cx->writable("INSERT")) return false;
  NewRow n{};
  if (!new_row(cx, table, row, n)) return false;
  const auto& t = *n.t;
  std::vector<std::uint32_t> arbiters;
  if (!arbiter_keys(t, arbiter, arbiters)) {
    cx->fail(BC_E_HANDLE, "arbiter " + std::to_string(arbiter) + " is neither table " + t.name + " nor a unique index of it");
    return false;
  }
  if (conflicts(cx, n, arbiters)) return false;
  insert_row(cx, n, arbiters);
  return cx->ok();
  BC_GUARD_END(cx)
  return false;
}

bool bc_insert_or_existing(bc_ctx* cx, uint32_t table, bc_view row, uint32_t arbiter, bc_view* existing) {
  BC_GUARD_BEGIN
  if (existing != nullptr) *existing = {};
  if (!cx->writable("INSERT")) return false;
  NewRow n{};
  if (!new_row(cx, table, row, n)) return false;
  const auto& t = *n.t;
  std::vector<std::uint32_t> arbiters;
  if (!arbiter_keys(t, arbiter, arbiters)) {
    cx->fail(BC_E_HANDLE, "arbiter " + std::to_string(arbiter) + " is neither table " + t.name + " nor a unique index of it");
    return false;
  }
  if (existing == nullptr) {
    cx->fail(BC_E_HANDLE, "bc_insert_or_existing with no row to give");
    return false;
  }
  Bytes pk;
  if (holder(cx, n, arbiters, pk)) {
    bool found = false;
    auto v = read_record(cx, pk, &found);
    // An index entry whose row is missing is damage: the runtime writes every entry with its row.
    if (!found && cx->ok()) cx->fail(BC_E_CORRUPT, "table " + t.name + " has an index entry whose row is missing");
    if (cx->ok()) *existing = v;
    return false;
  }
  if (!cx->ok()) return false;
  insert_row(cx, n, arbiters);
  return cx->ok();
  BC_GUARD_END(cx)
  return false;
}

void bc_update(bc_ctx* cx, uint32_t table, bc_view old, bc_view row) {
  BC_GUARD_BEGIN
  if (!cx->writable("UPDATE")) return;
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
  auto vacated = moved_pk && vacated_here(cx, pk_new);
  if (!first_write(cx, *t, pk_old, !moved_pk) || (moved_pk && !first_write(cx, *t, pk_new, true, true))) return;
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
  if (moved_pk && !vacated && !pk_free(cx, *t, pk_new)) return;
  for (const auto& m : moves) {
    // A row never collides with itself: checked only when the prefix changed.
    if (!m.ix->unique || std::ranges::equal(m.from.prefix(), m.to.prefix())) continue;
    if (!m.to.has_null && !unique_free(cx, *m.ix, m.to)) return;
    if (!m.from.has_null && !vacate_unique(cx, *m.ix, m.from.prefix())) return;
  }
  lock_row(cx, pk_old, *o);
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
  if (!cx->writable("DELETE")) return;
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
  lock_row(cx, pk, *o);
  cx->push_effect(EffectKind::Del, std::move(pk));
  for (auto& e : entries) cx->push_effect(EffectKind::Del, std::move(e.key));
  BC_GUARD_END(cx)
}

void bc_del_range(bc_ctx* cx, uint32_t table, bc_view lo, bc_view hi) {
  BC_GUARD_BEGIN
  if (!cx->writable("DELETE")) return;
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
  if (!cx->take(buffer_bytes(range->lo.size()) + buffer_bytes(range->hi.size()) + kRangeNodeBytes)) {
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
    auto rec = cx->record_range(range->lo, range->hi);
    if (!rec) return;
    ViewEntries rows{cx->view(*rec), cx->overlay, range->lo, range->hi, false};
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
  if (!cx->writable("SELECT FOR UPDATE")) return;
  add_effect(cx, EffectKind::GuardUnchanged, key, {});
  BC_GUARD_END(cx)
}

void bc_guard_row(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  if (!cx->writable("SELECT FOR UPDATE")) return;
  // The key goes straight into the guard, not through the arena: a FOR
  // UPDATE scan guards every row it returns.
  Bytes key;
  auto r = row_pk(cx, table, row, key);
  if (!r) return;
  lock_row(cx, key, *r);
  cx->push_effect(EffectKind::GuardUnchanged, std::move(key));
  BC_GUARD_END(cx)
}

void bc_guard_range(bc_ctx* cx, uint32_t table_or_index, bc_view lo, bc_view hi) {
  BC_GUARD_BEGIN
  if (!cx->writable("SELECT FOR UPDATE")) return;
  auto range = resolve_range(cx, table_or_index, lo, hi);
  if (!range) return;
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return;  // an empty range cannot change
  cx->push_effect(EffectKind::GuardRange, std::move(range->lo), std::move(range->hi));
  BC_GUARD_END(cx)
}

void bc_guard_present(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  if (!cx->writable("SELECT FOR UPDATE")) return;
  add_effect(cx, EffectKind::GuardPresent, key, {});
  BC_GUARD_END(cx)
}

void bc_guard_absent(bc_ctx* cx, bc_view key) {
  BC_GUARD_BEGIN
  if (!cx->writable("SELECT FOR UPDATE")) return;
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

namespace {

// A value's text as the DETAIL writes it: at most 64 bytes, cut before a
// UTF-8 sequence it would split, then "..." where it was cut (PostgreSQL's
// ExecBuildSlotValueDescription).
auto clipped(std::string text) -> std::string {
  constexpr std::size_t kMaxFieldLen = 64;
  if (text.size() <= kMaxFieldLen) return text;
  auto end = kMaxFieldLen;
  // A continuation byte (10xxxxxx) at the cut means a sequence is split:
  // cut before its lead byte.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
  text.resize(end);
  return text + "...";
}

// The row's values as PostgreSQL's DETAIL writes a failing row: each column
// a statement can name, in the table's order, as its text output clipped,
// NULL as null.
auto failing_row(const Table& t, BytesView row) -> std::optional<std::string> {
  std::string out = "Failing row contains (";
  auto first = true;
  for (const auto& c : t.columns) {
    if (c.hidden) continue;
    if (!std::exchange(first, false)) out += ", ";
    const auto* slot = t.layout.find(c.id);
    if (slot == nullptr) return std::nullopt;
    auto v = bytecask::sql::row_column(row, *slot);
    switch (v.state) {
      case bytecask::sql::ColumnState::Malformed: return std::nullopt;
      case bytecask::sql::ColumnState::Absent: out += "null"; continue;
      case bytecask::sql::ColumnState::Value: break;
    }
    Datum d;
    switch (slot->type) {
      case PhysType::I64: d = v.i64(); break;
      case PhysType::F64: d = v.f64(); break;
      case PhysType::Bool: d = v.boolean(); break;
      case PhysType::Bytes:
      case PhysType::Num: d = Bytes(v.bytes.begin(), v.bytes.end()); break;
      case PhysType::Jsonb: {
        // Its text, as a result writes it; a value that does not decode is shown as such.
        std::string text;
        if (!bytecask::sql::jsonb::value_text(v.bytes, text).ok()) text = "?";
        out += clipped(text);
        continue;
      }
    }
    out += clipped(bytecask::sql::to_text(d, c.sql));
  }
  return out + ").";
}

}  // namespace

extern "C" {

void bc_raise_row(bc_ctx* cx, uint32_t constraint, uint32_t object_id, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
  if (!cx->ok()) return;
  if (!ConstraintDetail{constraint, object_id}.valid()) {
    cx->fail(BC_E_HANDLE, "bc_raise_row with an unknown constraint kind or an object id past 2^24");
    return;
  }
  const auto* t = cx->table(table);
  if (t == nullptr) return;
  auto bytes = write_row(cx, *t, row, false);
  if (!bytes) return;
  auto note = failing_row(*t, *bytes);
  if (!note) {
    cx->fail(BC_E_CORRUPT, "malformed row in table " + t->name);
    return;
  }
  std::string msg;
  if (constraint == BC_C_CHECK_OPTION) {
    const auto* v = cx->cat->view(object_id);
    msg = "new row violates check option for view \"" + (v == nullptr ? std::to_string(object_id) : v->name) + "\"";
  } else {
    msg = ConstraintDetail{constraint, object_id}.message();
  }
  cx->raise(constraint, object_id, std::move(msg));
  if (cx->error == BC_E_CONSTRAINT) cx->note = std::move(*note);
  BC_GUARD_END(cx)
}

}  // extern "C"
