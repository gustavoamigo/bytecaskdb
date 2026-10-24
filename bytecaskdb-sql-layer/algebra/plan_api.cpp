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
// The writes (bc_insert, bc_update, bc_delete, bc_del_range) derive every
// index entry from the row and the pinned catalog version, and run the
// unique checks; a plan never writes a key itself.

// Standard library headers before the module imports, for the reason
// bytecaskdb/bytecask_hpp.cpp gives (LLVM 22's abi_tag redeclaration error).
#include <algorithm>
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
using bytecask::sql::kCursorEntryBytes;
using bytecask::sql::kAggBytes;
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
using bytecask::sql::kAllocBytes;
using bytecask::sql::kClaimSlotBytes;
using bytecask::sql::kEffectSlotBytes;
using bytecask::sql::kLockSlotBytes;
using bytecask::sql::kOverlayNodeBytes;
using bytecask::sql::kRangeNodeBytes;
using bytecask::sql::kTableSlotBytes;
using bytecask::sql::kUniqueNodeBytes;
using bytecask::sql::kWrittenRowNodeBytes;
using bytecask::sql::lock_bytes;
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
// A handle's generation never repeats within a context: one is drawn per
// handle, and at one a nanosecond 64 bits last five centuries. Step rollback
// reuses slots, so a counter that wrapped would let a stale handle resolve
// to another entry.
template <typename H>
constexpr bool kWideGeneration = std::is_same_v<decltype(H::g), std::uint64_t>;
static_assert(kWideGeneration<bc_view> && kWideGeneration<bc_cursor> && kWideGeneration<bc_sorter> &&
              kWideGeneration<bc_hash> && kWideGeneration<bc_hash_iter> && kWideGeneration<bc_agg> &&
              kWideGeneration<bc_group> && kWideGeneration<bc_key> && kWideGeneration<bc_row>);

namespace {

constexpr std::uint32_t kNoCursor = UINT32_MAX;
// A cursor or aggregate that is not in a loop's step.
constexpr std::uint32_t kNoStep = UINT32_MAX;
// A pinned view's handle: its index in the pinned views, with this bit set.
constexpr std::uint32_t kPinnedView = 1u << 31;
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
};

// The finished key's view reads buf, which a move of the builder keeps
// where it is: a key costs its bytes once, charged as its parts are added.
struct KeyBuilder {
  const KeyLayout* layout;
  Bytes buf;
  std::uint64_t gen;
  std::size_t next_part{0};
  bool done{false};
  std::uint64_t held{0};  // what buf is charged to the statement's memory budget
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
    if (chunks_.empty() || chunks_.back().size - used_ < b.size()) {
      if (spare_.data != nullptr && b.size() <= spare_.size) {
        chunks_.push_back(std::move(spare_));
        spare_ = {};
      } else {
        auto size = std::max(kArenaChunk, b.size());
        chunks_.push_back({std::make_unique_for_overwrite<std::byte[]>(size), size});
      }
      used_ = 0;
    }
    auto* p = chunks_.back().data.get() + used_;
    if (!b.empty()) std::memcpy(p, b.data(), b.size());
    used_ += b.size();
    bytes_ += b.size();
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

enum class EffectKind { Put, Del, DelRange, GuardUnchanged, GuardRange, GuardPresent, GuardAbsent };

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
    case EffectKind::GuardPresent:
    case EffectKind::GuardAbsent: return false;
  }
  return false;
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

[[nodiscard]] constexpr auto effect_bytes(EffectKind k, std::size_t a, std::size_t b) -> std::uint64_t {
  return bytecask::sql::effect_bytes(is_write(k), a, b);
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

  // Whether the overlay puts `key`.
  [[nodiscard]] auto puts(BytesView key) const -> bool {
    auto it = points.find(key);
    return it != points.end() && it->second;
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
      bytes -= cost(it->first, it->second ? buffer_bytes(it->second->capacity()) : 0);
      it = points.erase(it);
    }
    auto it = ranges.upper_bound(BytesView{lo});
    if (it != ranges.begin() && !std::ranges::lexicographical_compare(std::prev(it)->second, lo)) --it;
    while (it != ranges.end() && !std::ranges::lexicographical_compare(hi, it->first)) {
      if (std::ranges::lexicographical_compare(it->first, lo)) lo = it->first;
      if (std::ranges::lexicographical_compare(hi, it->second)) hi = it->second;
      bytes -= cost(it->first, buffer_bytes(it->second.capacity()));
      it = ranges.erase(it);
    }
    auto add = cost(lo, buffer_bytes(hi.capacity()));
    ranges.emplace(std::move(lo), std::move(hi));
    bytes += add;
  }

private:
  static auto cost(const Bytes& key, std::uint64_t value_bytes) -> std::uint64_t {
    return buffer_bytes(key.capacity()) + value_bytes + kOverlayNodeBytes;
  }

  void set(Bytes key, std::optional<Bytes> value) {
    auto value_bytes = value ? buffer_bytes(value->capacity()) : 0;
    if (auto it = points.find(BytesView{key}); it != points.end()) {
      bytes -= it->second ? buffer_bytes(it->second->capacity()) : 0;
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
enum class SlotKind : std::uint8_t { None, SumI64, SumF64, MinI64, MaxI64, Count };

struct Slot {
  std::int64_t i{0};  // a sum, a minimum or maximum, or a count
  double f{0};        // a double sum
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
static_assert(sizeof(RowBuilder) <= kRowEntryBytes);
static_assert(sizeof(Cursor) <= kCursorEntryBytes);
static_assert(4 * sizeof(void*) + sizeof(std::pair<const Bytes, std::uint32_t>) + 2 * kAllocBytes +
                  sizeof(const Bytes*) <=
              kGroupEntryBytes);
static_assert(2 * sizeof(Slot) <= kAggSlotBytes);
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

// Where an error is recorded: a context's for its runs, a transaction's for
// its commit. fail() keeps the first error set; unknown() overrides.
struct ErrorSink {
  bc_error_code& error;
  std::uint32_t& detail;
  std::string& message;

  void clear() noexcept {
    error = BC_OK;
    detail = 0;
    message.clear();
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

  // The handle tables a step's storage is marked in: each grows as the plan
  // makes handles and is cut back to its mark when the step ends.
  std::vector<ViewEntry> views;
  std::vector<Cursor> cursors;
  std::vector<KeyBuilder> keys;
  std::vector<RowBuilder> rows;
  std::vector<HashIter> hash_iters;
  std::vector<ViewEntry> pinned_views;  // bc_pin's, kept until reset
  // Where each table and the arena stood when a loop moved to its current
  // entry: everything after is the step's.
  struct Mark {
    std::size_t views, keys, rows, cursors, hash_iters;
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
  // The aggregates bc_agg_open made: their groups' keys, in first-appearance
  // order, and their slots. Kept until reset; a group key's view reads its
  // Bytes, which a move of the vector keeps where it is.
  // Each group's key is held once, in `index`; `order` points at it, in
  // first-appearance order (a map node never moves). Group i's slots are
  // values[i * slots, (i + 1) * slots).
  struct Agg {
    std::uint64_t gen{0};
    std::uint32_t slots{0};
    std::map<Bytes, std::uint32_t, BytesLess> index;
    std::vector<const Bytes*> order;
    std::vector<SlotKind> kinds;  // per slot, the same in every group
    std::vector<Slot> values;
    std::size_t next{0};
    std::uint32_t scope{kNoStep};  // its loop's step in steps, while bc_agg_next is on a group
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

  bc_ctx(bytecask::DB* d, LiveCatalog* l) : db{d}, live{l} {}

  auto ok() const -> bool { return error == BC_OK; }

  [[nodiscard]] auto sink() -> ErrorSink { return {error, detail, message}; }

  void fail(ErrorCode code, std::string msg) { sink().fail(code, std::move(msg)); }

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
    mem_used += n;
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
  // false and BC_E_LIMIT, with nothing added and nothing charged; a `make`
  // or append that throws leaves nothing charged either.
  template <typename T, typename Make>
  [[nodiscard]] auto push_write(std::vector<T>& into, std::uint64_t n, Make&& make) -> bool {
    if (!ok()) return false;
    if (txn != nullptr && !txn_room(write_held + n)) return false;
    if (!take(n)) return false;
    try {
      into.push_back(make());
    } catch (...) {
      release(n);
      throw;
    }
    write_held += n;
    return true;
  }

  // Adds an effect, its buffers `a_bytes` and `b_bytes` long: a table the
  // writes had not touched is noted, before the effect, and its slot counted
  // with it.
  template <typename Make>
  void push_effect_made(EffectKind kind, BytesView key, std::size_t a_bytes, std::size_t b_bytes, Make&& make) {
    auto table = is_write(kind) ? written_table(key) : std::nullopt;
    auto n = effect_bytes(kind, a_bytes, b_bytes) + (table ? kTableSlotBytes : 0);
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
    if (push_charged(views, ViewEntry{p, step, gen, static_cast<std::uint32_t>(len), cursor, table, kind},
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
    auto gen = next_gen();
    views[v.i] = ViewEntry{b.data(), 0, gen, static_cast<std::uint32_t>(b.size()), kNoCursor, table, kind};
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
  // the sizes of its keys and values, before they are copied; each buffer
  // it keeps is that size, so the charge is the same on every build.
  void push_effect(EffectKind kind, BytesView a, BytesView b = {}) {
    push_effect_made(kind, a, a.size(), b.size(),
                     [&] { return Effect{kind, Bytes(a.begin(), a.end()), Bytes(b.begin(), b.end())}; });
  }
  // For keys the runtime built and no longer needs: moved, not copied, and
  // trimmed to their size.
  void push_effect(EffectKind kind, Bytes&& a, BytesView b = {}) {
    push_effect_made(kind, a, a.size(), b.size(), [&] {
      a.shrink_to_fit();
      return Effect{kind, std::move(a), Bytes(b.begin(), b.end())};
    });
  }
  void push_effect(EffectKind kind, Bytes&& a, Bytes&& b) {
    push_effect_made(kind, a, a.size(), b.size(), [&] {
      a.shrink_to_fit();
      b.shrink_to_fit();
      return Effect{kind, std::move(a), std::move(b)};
    });
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
  void raise(std::uint32_t constraint, std::uint32_t object_id, std::string msg = {}) {
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
    effects.clear();
    base = nullptr;
    overlay = nullptr;
    if (txn != nullptr) txn_detach(txn, this, abandoned);
    txn = nullptr;
  }

  [[nodiscard]] auto mark() const -> Mark {
    return {views.size(), keys.size(), rows.size(), cursors.size(), hash_iters.size(), arena.mark()};
  }

  // Releases what was allocated since `m`, and its charge. Allocates nothing.
  void roll_back(const Mark& m) noexcept {
    auto released = arena.roll_back(m.arena) + (views.size() - m.views) * kViewEntryBytes;
    if ((key_parts.view.i & kPinnedView) == 0 && key_parts.view.i >= m.views) key_parts.view = {};
    views.erase(views.begin() + static_cast<std::ptrdiff_t>(m.views), views.end());
    for (auto i = m.keys; i < keys.size(); ++i) released += keys[i].held + kKeyEntryBytes;
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(m.keys), keys.end());
    for (auto i = m.rows; i < rows.size(); ++i) released += rows[i].held + kRowEntryBytes;
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(m.rows), rows.end());
    for (auto i = m.cursors; i < cursors.size(); ++i) released += cursors[i].held + kCursorEntryBytes;
    cursors.erase(cursors.begin() + static_cast<std::ptrdiff_t>(m.cursors), cursors.end());
    released += (hash_iters.size() - m.hash_iters) * kHashIterEntryBytes;
    hash_iters.erase(hash_iters.begin() + static_cast<std::ptrdiff_t>(m.hash_iters), hash_iters.end());
    release(released);
  }

  // Starts a step of the loop `owner` drives: what is allocated from here
  // is the step's.
  void begin_step(std::uint32_t owner, Loop loop, std::uint32_t& scope) {
    steps.push_back({owner, loop, mark()});
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
    cursors.clear();
  }

  void drop_execution_state() {
    detach_from_txn();  // the snapshot's readers first, in the one leave order
    snap.reset();
    views.clear();
    pinned_views.clear();
    keys.clear();  // key and row builders point into the pinned catalog: before it
    rows.clear();
    verified.cols.clear();  // each ColSlot's sql points into the pinned catalog: before it
    cat = &unpinned;
    pinned.reset();  // an idle context keeps no superseded catalog alive
    written_rows.clear();
    unique_written.clear();
    aggs.clear();
    sorters.clear();
    hashes.clear();
    hash_iters.clear();
    deleted_ranges.clear();
    claims.clear();
    locks.clear();
    has_ref = false;  // set by the next run's verification
    key_slots.clear();
    key_parts.view = {};  // its view's slot is released
    key_parts.at.clear();
    results.clear();
    result_pos = 0;
    emit.reset();
    emit_col = 0;
    affected = -1;
    arena.clear(true);  // one chunk kept for the next statement
    pins.clear(false);
    mem_used = 0;
    write_held = 0;
    written_tables.clear();
    reads = 0;
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
  std::vector<Claim> claims;           // the statements' insert claims, in their order
  std::vector<Lock> locks;             // the statements' rows locked or written, in their order
  std::vector<std::uint32_t> written;  // the tables the writes are in
  // What the four above hold, each item as the statement counted it
  // (effect_bytes, claim_bytes, lock_bytes, kTableSlotBytes).
  std::uint64_t held{0};
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
  [[nodiscard]] auto sink() -> ErrorSink { return {error, detail, message}; }
  void fail_current_exception() noexcept { sink().fail_current_exception(); }
  // The one context begun on it and not yet reset, which points into its
  // snapshot and overlay; null when none is. A second is refused, and so
  // are a re-pin and a commit while one is. An abort and a free detach it.
  bc_ctx* attached{nullptr};

  bc_txn(bytecask::DB* d, LiveCatalog* l, std::uint64_t max) : db{d}, live{l}, max_bytes{max} {}
};

namespace {
void txn_abort(bc_txn* txn) noexcept;
}  // namespace

namespace {

// What the transaction holds: its overlay, which counts each node as its
// buffers and kOverlayNodeBytes, within the effect_bytes of the write it
// came from, and its guards, claims, locks and tables. A statement's write
// set would take it at most to this plus the write set's bytes: merging an
// effect into the overlay adds at most one node, and one that replaces or
// erases others only frees.
auto txn_holds(const bc_txn& t) -> std::uint64_t { return t.overlay.bytes + t.held; }

}  // namespace

// The one check both a statement's calls (push_write) and bc_txn_add make,
// on the write set as it would be and as it is.
auto bc_ctx::txn_room(std::uint64_t bytes) -> bool {
  if (txn_holds(*txn) + bytes <= txn->max_bytes) return true;
  fail(BC_E_LIMIT, "the transaction's writes would pass its bound of " + std::to_string(txn->max_bytes) + " bytes");
  return false;
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

// Aborts the transaction: a failed statement leaves only bc_txn_free, so
// what it holds goes now, the snapshot that holds vacuum back first. The
// contexts begun on it are detached; each keeps its error.
void txn_abort(bc_txn* txn) noexcept {
  txn->aborted = true;
  txn_drop_context(txn);
  txn->snap.reset();
  txn->pinned.reset();
  txn->overlay = {};
  txn->guards = {};
  txn->claims = {};
  txn->locks = {};
  txn->written = {};
  txn->held = 0;
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

// Positions a cursor's entries on the next one within bounds, which the
// caller copies; false at the end. Every entry is read through the view, and
// charged as it is read.
auto cursor_advance(bc_ctx* cx, Cursor& c) -> bool {
  if (!c.entries) {
    // The scan's bounds, inside its table's or index's keys.
    auto end = bytecask::sql::key_successor(c.prefix);
    BytesView lo = std::is_lt(bytes_cmp(c.lo, c.prefix)) ? BytesView{c.prefix} : BytesView{c.lo};
    BytesView hi = !c.hi.empty() && std::is_lt(bytes_cmp(c.hi, end)) ? BytesView{c.hi} : BytesView{end};
    c.entries.emplace(*cx->base, cx->overlay, lo, hi, (c.flags & BC_SCAN_REVERSE) != 0);
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
void key_held(bc_ctx* cx, KeyBuilder& b) { (void)cx->rehold(b.held, b.buf.size()); }

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

// A commit that conflicted: the duplicate it is when the latest committed
// state has taken one of its claims, the first in their order (BC_FAILED,
// BC_C_UNIQUE on the claim's table or index), else the conflict. Nothing was
// written either way; a check that cannot read reports the conflict.
// The first conflict in statement order decides, as in PostgreSQL, where an
// earlier statement's lock or write of a row a concurrent commit changed
// fails at that statement: before each claim, the locks of the statements
// up to its own are checked, and one whose row changed is the conflict. A
// row changed back to the same bytes counts as unchanged.
// A transaction's claim counts while its writes still put the key: one a
// later statement moved or deleted away claims nothing (`overlay`, null for
// an execution, whose one statement writes a row once). An index claim is
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
      if (overlay != nullptr && !overlay->puts(c.key)) continue;
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
  if (!cx->take(buffer_bytes(pk.size()) + kWrittenRowNodeBytes)) return false;
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
// A key free in the view is a claim when the snapshot holds it free too: one
// free only because an earlier statement deleted it conflicts with the row
// deleted, a 40001 in PostgreSQL as here.
[[nodiscard]] auto pk_free(bc_ctx* cx, const Table& t, BytesView pk) -> bool {
  if (!cx->charge()) return false;
  if (view_contains(cx, pk)) {
    cx->raise(BC_C_UNIQUE, t.id);
    return false;
  }
  // Free in the view; when the overlay decided it, ask the snapshot.
  if (cx->overlay != nullptr && cx->overlay->find(pk)) {
    if (!cx->charge()) return false;
    if (cx->base->contains_key({}, pk)) return true;
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

// No entry has this unique index prefix in the view, and no earlier write
// of this execution gave a row it, or BC_C_UNIQUE on the index; the range guard covers the race, since a duplicate is another
// row's entry, a key this plan does not write.
[[nodiscard]] auto unique_free(bc_ctx* cx, const Index& ix, const Entry& entry) -> bool {
  auto prefix = entry.prefix();
  if (!cx->charge()) return false;
  // Recorded, charged first; one this execution already gave a row, which
  // the view cannot see, is not inserted again, and its charge goes.
  const auto n = buffer_bytes(prefix.size()) + kUniqueNodeBytes;
  if (!cx->take(n)) return false;
  auto fresh = false;
  try {
    fresh = cx->unique_written.emplace(ix.id, Bytes(prefix.begin(), prefix.end())).second;
  } catch (...) {
    cx->release(n);
    throw;
  }
  if (!fresh) {
    cx->release(n);
    cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  auto in_snapshot = [&] { return holds_prefix(*cx->base, prefix); };
  // As pk_free: a claim when the snapshot holds the prefix free too.
  auto claim = true;
  if (cx->overlay != nullptr && cx->overlay->meets(prefix, bytecask::sql::key_successor(prefix))) {
    // The overlay holds entries with this prefix, or deleted some: the
    // first entry the view holds at or after it decides, each read charged.
    ViewEntries entries{*cx->base, cx->overlay, prefix, bytecask::sql::key_successor(prefix), false};
    auto taken = entries.next(cx);
    if (!cx->ok()) return false;
    if (taken) {
      cx->raise(BC_C_UNIQUE, ix.id);
      return false;
    }
    if (!cx->charge()) return false;
    claim = !in_snapshot();
  } else if (in_snapshot()) {
    cx->raise(BC_C_UNIQUE, ix.id);
    return false;
  }
  cx->push_effect(EffectKind::GuardRange, prefix, bytecask::sql::key_successor(prefix));
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

bc_ctx* bc_ctx_new(bc_db* db, const bc_catalog* cat) {
  try {
    return new bc_ctx(db->db, cat->live);
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
  cx->base = &*cx->snap;
  BC_GUARD_END(cx)
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
  cx->drop_cursors();
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
  for (auto id : cx->written_tables) plan.ensure_unchanged(bytecask::sql::table_record_key(id));
  cx->effects.clear();  // handed to the engine: a second commit has nothing to submit
#ifdef BYTECASK_SQL_TESTING
  if (cx->before_commit) cx->before_commit();
#endif
  return submit(cx->sink(), *cx->db, std::move(plan), sync);
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
std::uint64_t bc_ctx_held_for_testing(const bc_ctx* cx) { return cx->mem_used; }
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
  txn_drop_context(txn);
  delete txn;
}

uint64_t bc_txn_catalog_version(const bc_txn* txn) { return txn->pinned ? txn->pinned->version() : 0; }

bool bc_txn_repin(bc_txn* txn) {
  if (txn->statements != 0 || txn->aborted || txn->done || txn->attached != nullptr) return false;
  txn->snap.reset();
  txn->pinned.reset();
  return true;
}

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
  // The bound is checked before anything is merged, by the check each of the
  // statement's writes passed, on the write set they reached: a statement
  // that would pass it is refused without the transaction allocating for it.
  if (!cx->txn_room(cx->write_held)) {
    txn_abort(txn);
    return false;
  }
  // Aborted until the statement is wholly in: a failure part-way through
  // leaves an overlay no commit may use.
  txn->aborted = true;
  for (auto id : cx->written_tables) {
    if (std::ranges::find(txn->written, id) == txn->written.end()) {
      txn->written.push_back(id);
      txn->held += kTableSlotBytes;
    }
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
        txn->held += effect_bytes(e.kind, e.a.size(), e.b.size());
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
    txn->held += claim_bytes(c.key.size());
    txn->claims.push_back(std::move(c));
  }
  cx->claims.clear();
  for (auto& l : cx->locks) {
    txn->held += lock_bytes(l.key.size());
    txn->locks.push_back(std::move(l));
  }
  cx->locks.clear();
  // The write set is the transaction's now, and off the statement's budget.
  cx->release(cx->write_held);
  cx->write_held = 0;
  ++txn->statements;
  txn->aborted = false;
  return true;
  BC_GUARD_END(cx)
  if (txn != nullptr) txn_abort(txn);
  return false;
}

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
  const auto& ov = txn->overlay;
  // A transaction that neither writes nor guards commits nothing. One that
  // only guarded (SELECT … FOR UPDATE) is still checked: a guarded row a
  // concurrent commit changed is its conflict. The engine checks a
  // guard-only plan and writes nothing.
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
  auto r = submit(out, *txn->db, std::move(plan), sync);
  return r == BC_CONFLICT ? conflict_or_duplicate(out, *txn->db, txn->claims, txn->locks, &txn->overlay) : r;
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
  KeyBuilder b{kl, {}, cx->next_gen()};
  bytecask::sql::key_put_prefix(b.buf, kl->ns, table_or_index);
  b.held = b.buf.size();
  auto gen = b.gen;
  auto charge = kKeyEntryBytes + b.held;
  if (cx->push_charged(cx->keys, std::move(b), charge) == nullptr) return {};
  return {static_cast<std::uint32_t>(cx->keys.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}

void bc_key_i64(bc_ctx* cx, bc_key k, int64_t v) {
  BC_GUARD_BEGIN
  auto* b = key_builder_part(cx, k, PhysType::I64, false);
  if (b == nullptr) return;
  bytecask::sql::key_put_i64(b->buf, v, current_part(*b));
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
  if (b == nullptr) return {};
  if (b->done) {
    cx->fail(BC_E_HANDLE, "key builder already ended");
    return {};
  }
  b->done = true;
  if (!within_key_ceiling(cx, b->buf)) return {};
  return cx->make_view(b->buf.data(), b->buf.size());
  BC_GUARD_END(cx)
  return {};
}

bool bc_key_fits(bc_ctx* cx, bc_key k) {
  BC_GUARD_BEGIN
  const auto* b = cx->resolve_handle(k, cx->keys, "key builder");
  return b != nullptr && b->buf.size() <= bytecask::sql::kMaxKeyBytes;
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
  c.held = cursor_bytes(c);
  auto gen = c.gen;
  auto charge = kCursorEntryBytes + c.held;
  if (cx->push_charged(cx->cursors, std::move(c), charge) == nullptr) return {};
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
    c->done = true;
    return false;
  }
  auto k = c->entries->key();
  auto v = c->entries->value();
  c->key.assign(k.begin(), k.end());
  c->value.assign(v.begin(), v.end());
  cx->begin_step(h.i, bc_ctx::Loop::Cursor, c->scope);
  c->positioned = true;
  return cx->ok();
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

bc_view bc_col_key(bc_ctx* cx, bc_view row, uint32_t col) {
  BC_GUARD_BEGIN
  auto rc = row_col(cx, row, col, std::nullopt);
  if (!rc) return {};
  auto v = read_column(cx, *rc);
  if (!v) return {};
  // Nullable and ascending: NULL is the marker alone. A DOUBLE's -0.0 and
  // 0.0 encode alike, and every NaN as one, so equal values group as
  // PostgreSQL groups them.
  bytecask::sql::KeyPart part{rc->slot->type, true, false, false};
  Bytes key;
  if (v->state == bytecask::sql::ColumnState::Absent) {
    bytecask::sql::key_put_null(key, part);
  } else {
    switch (rc->slot->type) {
      case PhysType::I64: bytecask::sql::key_put_i64(key, v->i64(), part); break;
      case PhysType::F64: bytecask::sql::key_put_f64(key, v->f64(), part); break;
      case PhysType::Bool: bytecask::sql::key_put_bool(key, v->boolean(), part); break;
      case PhysType::Bytes: bytecask::sql::key_put_bytes(key, v->bytes, part); break;
    }
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
  if (!b->writer.finish(b->built)) {
    cx->fail(BC_E_LIMIT, "row over the engine's value limit");
    return {};
  }
  if (!cx->rehold(b->held, b->built.size())) return {};
  return cx->make_view(b->built.data(), b->built.size(), kNoCursor, 0, b->table, RowKind::Built);
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
                       ViewEntry{p, 0, gen, static_cast<std::uint32_t>(b->size()), kNoCursor, table, kind},
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
  a.slots = slots;
  a.kinds.assign(slots, SlotKind::None);
  auto gen = a.gen;
  cx->aggs.push_back(std::move(a));
  return {static_cast<std::uint32_t>(cx->aggs.size() - 1), gen};
  BC_GUARD_END(cx)
  return {};
}
bc_group bc_agg_group(bc_ctx* cx, bc_agg a, bc_view group_key) {
  BC_GUARD_BEGIN
  auto* agg = cx->resolve_handle(a, cx->aggs, "aggregate");
  if (agg == nullptr) return {};
  auto key = cx->resolve(group_key);
  if (!key || !cx->ok()) return {};
  if (auto it = agg->index.find(*key); it != agg->index.end()) return {it->second, agg->gen};
  if (!cx->take(key->size() + kGroupEntryBytes + std::uint64_t{agg->slots} * kAggSlotBytes)) return {};
  auto i = static_cast<std::uint32_t>(agg->order.size());
  agg->values.resize(agg->values.size() + agg->slots);
  auto it = agg->index.emplace(Bytes(key->begin(), key->end()), i).first;
  agg->order.push_back(&it->first);
  return {i, agg->gen};
  BC_GUARD_END(cx)
  return {};
}

extern "C++" {
namespace {

// The aggregate a group handle carries the generation of: the aggregates
// are kept in the order opened, so their generations ascend.
auto group_agg(bc_ctx* cx, bc_group g) -> bc_ctx::Agg* {
  if (!cx->ok()) return nullptr;
  auto it = std::ranges::lower_bound(cx->aggs, g.g, {}, &bc_ctx::Agg::gen);
  if (it == cx->aggs.end() || it->gen != g.g || g.i >= it->order.size()) {
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
    case SlotKind::Count: return "a count";
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

// The slot a read goes to, with its kind; nullptr and BC_E_HANDLE for a
// slot out of range, or, given `f64`, of the other type than the read's.
auto read_slot(bc_ctx* cx, bc_group g, std::uint32_t slot, std::optional<bool> f64) -> const Slot* {
  auto* agg = group_agg(cx, g);
  if (agg == nullptr) return nullptr;
  if (slot >= agg->slots) {
    cx->fail(BC_E_HANDLE, "slot " + std::to_string(slot) + " of an aggregate of " + std::to_string(agg->slots));
    return nullptr;
  }
  auto k = agg->kinds[slot];
  if (f64 && k != SlotKind::None && (k == SlotKind::SumF64) != *f64) {
    cx->fail(BC_E_HANDLE, std::string{"slot "} + std::to_string(slot) + " is " + slot_kind_name(k) + ", read as " +
                              (*f64 ? "a double" : "an integer"));
    return nullptr;
  }
  return &agg->values[std::size_t{g.i} * agg->slots + slot];
}

}  // namespace
}  // extern "C++"

void bc_agg_add_i64(bc_ctx* cx, bc_group g, uint32_t slot, int64_t v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::SumI64);
  if (s == nullptr) return;
  std::int64_t r = 0;
  if (__builtin_add_overflow(s->i, v, &r)) {
    cx->fail(BC_E_ARITH, "bigint out of range");
    return;
  }
  s->i = r;
  s->set = true;
  BC_GUARD_END(cx)
}
// PostgreSQL's float8 sum: a finite sum that overflows is an error, and an
// infinite or NaN input carries through.
void bc_agg_add_f64(bc_ctx* cx, bc_group g, uint32_t slot, double v) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::SumF64);
  if (s == nullptr) return;
  auto r = s->f + v;
  if (std::isinf(r) && !std::isinf(s->f) && !std::isinf(v)) {
    cx->fail(BC_E_ARITH, "value out of range: overflow");
    return;
  }
  s->f = r;
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
void bc_agg_count(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  auto* s = fold_slot(cx, g, slot, SlotKind::Count);
  if (s == nullptr) return;
  if (__builtin_add_overflow(s->i, 1, &s->i)) {
    cx->fail(BC_E_ARITH, "bigint out of range");
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
  if (g != nullptr) *g = {static_cast<std::uint32_t>(i), agg->gen};
  return cx->ok();
  BC_GUARD_END(cx)
  return {};
}
int64_t bc_agg_i64(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, false);
  return s == nullptr ? 0 : s->i;
  BC_GUARD_END(cx)
  return {};
}
double bc_agg_f64(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, true);
  return s == nullptr ? 0 : s->f;
  BC_GUARD_END(cx)
  return {};
}
// PostgreSQL's NULL for SUM, MIN, MAX or AVG of a group no value reached.
bool bc_agg_null(bc_ctx* cx, bc_group g, uint32_t slot) {
  BC_GUARD_BEGIN
  const auto* s = read_slot(cx, g, slot, std::nullopt);
  return s != nullptr && !s->set;
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
    if (t->indexes[i].unique && !entries[i].has_null && !unique_free(cx, t->indexes[i], entries[i])) return;
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
        !unique_free(cx, *m.ix, m.to)) {
      return;
    }
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

void bc_guard_row(bc_ctx* cx, uint32_t table, bc_view row) {
  BC_GUARD_BEGIN
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
  auto range = resolve_range(cx, table_or_index, lo, hi);
  if (!range) return;
  if (std::is_gteq(bytes_cmp(range->lo, range->hi))) return;  // an empty range cannot change
  cx->push_effect(EffectKind::GuardRange, std::move(range->lo), std::move(range->hi));
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
