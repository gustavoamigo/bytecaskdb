// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.cppm — tables, their permanent column ids, primary keys and
// secondary indexes, kept in memory and persisted under namespace 0x01.
//
// prototype: the minimum the end-to-end prototype needs (#497). Written from
// scratch: the plugin's catalog is GPL-2.0 and this directory is MIT. Its key
// space follows the plugin's where they overlap (counters under 01 01, table
// records under 01 02); records are keyed by id, with a name entry under
// 01 03, as #489 decided.
//
// Key space:
//   01 01 01        next object id (tables, indexes, layouts), u64 big-endian
//   01 01 02        catalog version, bumped by every DDL, u64 big-endian
//   01 02 <id:4>    table record (indexes included), append-only serialization
//   01 03 <name>    name entry: kind(1) id(4, big-endian)
//
// Not thread-safe: one Catalog per process, mutated under the caller's lock.

module;
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The engine through its public header, as the plugin and the bindings link
// it; the sql-layer imports no engine module.
#include "include/bytecask.hpp"

export module bytecask.sql.catalog;

export import bytecask.sql.key_codec;
export import bytecask.sql.row_codec;

export namespace bytecask::sql {

struct Column {
  std::uint32_t id;  // permanent; never reused
  std::string name;
  PhysType type;
  bool nullable;
};

// A non-unique secondary index: its columns, then the primary key's.
struct Index {
  std::uint32_t id;
  std::string name;
  std::vector<std::uint32_t> col_ids;
};

struct Table {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t version{1};  // bumped by every DDL on this table
  std::uint32_t layout_id{0};
  std::uint32_t next_col_id{0};
  std::vector<Column> columns;
  std::vector<std::uint32_t> pk_col_ids;
  std::vector<Index> indexes;
  RowLayout layout;

  [[nodiscard]] auto column(std::string_view name) const -> const Column*;
  [[nodiscard]] auto column(std::uint32_t id) const -> const Column*;
  [[nodiscard]] auto index(std::string_view name) const -> const Index*;
};

struct ColumnSpec {
  std::string name;
  PhysType type;
  bool nullable;
};

// What a key builder needs for a table (its primary key) or an index.
struct KeyLayout {
  std::uint8_t ns;
  const Table* table;
  const Index* index;  // nullptr for a table's primary key
  std::vector<KeyPart> parts;  // every part: index columns, then primary key
  std::size_t index_parts;     // how many of them precede the primary key
};

// A layout for result rows: the column types in emit order.
struct ResultLayout {
  std::vector<PhysType> types;
  RowLayout layout;
};

class Catalog {
public:
  // Reads every table record under namespace 0x01.
  [[nodiscard]] static auto load(DB& db) -> Catalog;

  // Each DDL is one atomic batch: the record, the name entry, the counters,
  // and for CREATE INDEX the entries for rows already in the table. Throws
  // std::invalid_argument for a duplicate name or a bad definition.
  auto create_table(DB& db, const std::string& name, const std::vector<ColumnSpec>& cols,
                    const std::vector<std::string>& pk) -> const Table&;
  auto create_index(DB& db, const std::string& name, const std::string& table,
                    const std::vector<std::string>& cols) -> const Index&;

  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  [[nodiscard]] auto table(std::string_view name) const -> const Table*;
  [[nodiscard]] auto table(std::uint32_t id) const -> const Table*;
  // A table or an index: both draw ids from one counter.
  [[nodiscard]] auto key_layout(std::uint32_t table_or_index) const -> const KeyLayout*;
  // A table's row layout, or a result layout.
  [[nodiscard]] auto row_layout(std::uint32_t layout_id) const -> const RowLayout*;
  [[nodiscard]] auto table_of_layout(std::uint32_t layout_id) const -> const Table*;

  // prototype: result layouts live in memory only, interned by their types,
  // with ids from kResultLayoutBase up in order of first use. A plan's result
  // layout id is therefore stable within a process, not across processes.
  // Below 2^31: a plan's constants are an enum, whose values are ints.
  static constexpr std::uint32_t kResultLayoutBase = 0x4000'0000u;
  auto result_layout(const std::vector<PhysType>& types) -> std::uint32_t;
  [[nodiscard]] auto result_types(std::uint32_t layout_id) const -> const std::vector<PhysType>*;

  // The constants a plan on this table needs, in the names generated plans
  // use: TBL_, LAY_, IDX_, and per column <COL>_OFF or <COL>_SLOT and <COL>_COL.
  [[nodiscard]] auto constants(const Table& t) const
      -> std::vector<std::pair<std::string, std::uint32_t>>;

private:
  void index_table(Table t);
  void rebuild_key_layouts();
  auto next_id(std::uint32_t n) -> std::uint32_t;  // reserves n ids, returns the first

  std::uint64_t next_id_{1};
  std::uint64_t version_{0};
  std::map<std::uint32_t, Table> tables_;
  std::map<std::string, std::uint32_t, std::less<>> names_;
  std::map<std::uint32_t, KeyLayout> key_layouts_;
  std::map<std::uint32_t, std::uint32_t> layout_tables_;  // layout id -> table id
  std::vector<std::unique_ptr<ResultLayout>> results_;
};

// Encodes the primary key of `t` from a row: kNsRow, the table id, the parts.
[[nodiscard]] auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::byte kSubCounter{0x01};
constexpr std::byte kSubTable{0x02};
constexpr std::byte kSubName{0x03};
constexpr std::byte kCounterNextId{0x01};
constexpr std::byte kCounterVersion{0x02};
constexpr std::uint8_t kKindTable = 1;
constexpr std::uint8_t kKindIndex = 2;
constexpr std::uint16_t kRecordFormat = 1;

auto counter_key(std::byte id) -> Bytes { return {std::byte{kNsCatalog}, kSubCounter, id}; }

auto table_key(std::uint32_t id) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, kSubTable};
  for (int shift = 24; shift >= 0; shift -= 8) k.push_back(static_cast<std::byte>((id >> shift) & 0xFF));
  return k;
}

auto name_key(std::string_view name) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, kSubName};
  for (auto c : name) k.push_back(static_cast<std::byte>(c));
  return k;
}

auto be_bytes(std::uint64_t v, std::size_t width) -> Bytes {
  Bytes b;
  for (auto i = width; i-- > 0;) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  return b;
}

auto be_value(BytesView b) -> std::uint64_t {
  std::uint64_t v = 0;
  for (auto x : b) v = (v << 8) | std::to_integer<std::uint64_t>(x);
  return v;
}

// SQL folds unquoted identifiers; the prototype folds every name to lower case.
auto fold(std::string_view name) -> std::string {
  std::string s{name};
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

auto upper(std::string_view name) -> std::string {
  std::string s{name};
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Little-endian, append-only: a later format adds fields at the end.
class RecordWriter {
public:
  void u8(std::uint8_t v) { out_.push_back(std::byte{v}); }
  void u16(std::uint32_t v) { le(v, 2); }
  void u32(std::uint32_t v) { le(v, 4); }
  void str(std::string_view s) {
    u16(static_cast<std::uint32_t>(s.size()));
    for (auto c : s) out_.push_back(static_cast<std::byte>(c));
  }
  [[nodiscard]] auto bytes() const -> const Bytes& { return out_; }

private:
  void le(std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  }
  Bytes out_;
};

class RecordReader {
public:
  explicit RecordReader(BytesView in) : in_{in} {}
  auto u8() -> std::uint8_t { return static_cast<std::uint8_t>(le(1)); }
  auto u16() -> std::uint32_t { return static_cast<std::uint32_t>(le(2)); }
  auto u32() -> std::uint32_t { return static_cast<std::uint32_t>(le(4)); }
  auto str() -> std::string {
    auto n = u16();
    if (in_.size() - pos_ < n) throw std::runtime_error("catalog record truncated");
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(in_[pos_ + i]));
    pos_ += n;
    return s;
  }

private:
  auto le(std::size_t width) -> std::uint64_t {
    if (in_.size() - pos_ < width) throw std::runtime_error("catalog record truncated");
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) v |= std::to_integer<std::uint64_t>(in_[pos_ + i]) << (8 * i);
    pos_ += width;
    return v;
  }
  BytesView in_;
  std::size_t pos_{0};
};

auto serialize(const Table& t) -> Bytes {
  RecordWriter w;
  w.u16(kRecordFormat);
  w.u32(t.id);
  w.str(t.name);
  w.u32(t.version);
  w.u32(t.layout_id);
  w.u16(t.layout.generation);
  w.u16(t.layout.bitmap_bytes);
  w.u32(t.next_col_id);
  w.u16(static_cast<std::uint32_t>(t.columns.size()));
  for (const auto& c : t.columns) {
    w.u32(c.id);
    w.str(c.name);
    w.u8(static_cast<std::uint8_t>(c.type));
    w.u8(c.nullable ? 1 : 0);
    w.u32(t.layout.find(c.id)->pos);
  }
  w.u16(static_cast<std::uint32_t>(t.pk_col_ids.size()));
  for (auto id : t.pk_col_ids) w.u32(id);
  w.u16(static_cast<std::uint32_t>(t.indexes.size()));
  for (const auto& ix : t.indexes) {
    w.u32(ix.id);
    w.str(ix.name);
    w.u16(static_cast<std::uint32_t>(ix.col_ids.size()));
    for (auto id : ix.col_ids) w.u32(id);
  }
  return w.bytes();
}

auto deserialize(BytesView in) -> Table {
  RecordReader r{in};
  if (r.u16() != kRecordFormat) throw std::runtime_error("unknown catalog record format");
  Table t;
  t.id = r.u32();
  t.name = r.str();
  t.version = r.u32();
  t.layout_id = r.u32();
  t.layout.generation = static_cast<std::uint16_t>(r.u16());
  t.layout.bitmap_bytes = r.u16();
  t.layout.fixed_start = static_cast<std::uint32_t>(kRowHeaderBytes) + t.layout.bitmap_bytes;
  t.layout.fixed_end = t.layout.fixed_start;
  t.next_col_id = r.u32();
  auto ncols = r.u16();
  for (std::uint32_t i = 0; i < ncols; ++i) {
    Column c;
    c.id = r.u32();
    c.name = r.str();
    c.type = static_cast<PhysType>(r.u8());
    c.nullable = r.u8() != 0;
    auto pos = r.u32();
    t.layout.columns.push_back({c.id, c.type, c.nullable, pos});
    if (c.type == PhysType::Bytes) {
      t.layout.nvar = std::max(t.layout.nvar, pos + 1);
    } else {
      t.layout.fixed_end = std::max(t.layout.fixed_end, pos + 8);
    }
    t.columns.push_back(std::move(c));
  }
  auto npk = r.u16();
  for (std::uint32_t i = 0; i < npk; ++i) t.pk_col_ids.push_back(r.u32());
  auto nidx = r.u16();
  for (std::uint32_t i = 0; i < nidx; ++i) {
    Index ix;
    ix.id = r.u32();
    ix.name = r.str();
    auto n = r.u16();
    for (std::uint32_t j = 0; j < n; ++j) ix.col_ids.push_back(r.u32());
    t.indexes.push_back(std::move(ix));
  }
  return t;
}

// Appends one column of `row` as a key part. False if the row is malformed.
auto put_part_from_row(const Table& t, std::uint32_t col_id, bool nullable, BytesView row, Bytes& out)
    -> bool {
  const auto* slot = t.layout.find(col_id);
  if (slot == nullptr) return false;
  if (row_is_null(row, *slot)) {
    if (!nullable) return false;
    key_put_null(out);
    return true;
  }
  if (slot->type == PhysType::I64) {
    auto v = row_i64(row, slot->pos);
    if (!v) return false;
    key_put_i64(out, *v, nullable);
    return true;
  }
  auto v = row_bytes(row, slot->pos);
  if (!v) return false;
  key_put_bytes(out, *v, nullable);
  return true;
}

auto encode_index_key_from_row(const Table& t, const Index& ix, BytesView row, Bytes& out) -> bool {
  key_put_prefix(out, kNsIndex, ix.id);
  for (auto id : ix.col_ids) {
    if (!put_part_from_row(t, id, t.column(id)->nullable, row, out)) return false;
  }
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, false, row, out)) return false;
  }
  return true;
}

void commit_ddl(DB& db, WritePlan plan, const std::string& what) {
  if (!db.apply_batch({}, std::move(plan))) {
    throw std::invalid_argument(what + " already exists");
  }
}

}  // namespace

auto Table::column(std::string_view n) const -> const Column* {
  auto it = std::ranges::find(columns, fold(n), &Column::name);
  return it == columns.end() ? nullptr : &*it;
}

auto Table::column(std::uint32_t cid) const -> const Column* {
  auto it = std::ranges::find(columns, cid, &Column::id);
  return it == columns.end() ? nullptr : &*it;
}

auto Table::index(std::string_view n) const -> const Index* {
  auto it = std::ranges::find(indexes, fold(n), &Index::name);
  return it == indexes.end() ? nullptr : &*it;
}

auto Catalog::load(DB& db) -> Catalog {
  Catalog cat;
  Bytes v;
  if (db.get({}, counter_key(kCounterNextId), v)) cat.next_id_ = be_value(v);
  if (db.get({}, counter_key(kCounterVersion), v)) cat.version_ = be_value(v);
  auto snap = db.snapshot();
  auto prefix = Bytes{std::byte{kNsCatalog}, kSubTable};
  for (const auto& e : snap.iter_from({}, prefix)) {
    if (e.key.size() < 2 || !std::ranges::equal(e.key.first(2), prefix)) break;
    cat.index_table(deserialize(e.value));
  }
  cat.rebuild_key_layouts();
  return cat;
}

auto Catalog::next_id(std::uint32_t n) -> std::uint32_t {
  auto first = next_id_;
  if (first + n >= kResultLayoutBase) throw std::length_error("catalog ids exhausted");
  next_id_ += n;
  return static_cast<std::uint32_t>(first);
}

auto Catalog::create_table(DB& db, const std::string& raw_name, const std::vector<ColumnSpec>& cols,
                           const std::vector<std::string>& pk) -> const Table& {
  auto name = fold(raw_name);
  if (names_.contains(name)) throw std::invalid_argument("relation " + name + " already exists");
  if (cols.empty() || pk.empty()) throw std::invalid_argument("a table needs columns and a primary key");

  Table t;
  t.name = name;
  std::vector<ColumnDef> defs;
  for (const auto& spec : cols) {
    auto cname = fold(spec.name);
    if (t.column(cname) != nullptr) throw std::invalid_argument("column " + cname + " specified more than once");
    auto pk_col = std::ranges::find(pk, cname, [](const std::string& s) { return fold(s); }) != pk.end();
    Column c{t.next_col_id++, cname, spec.type, spec.nullable && !pk_col};
    defs.push_back({c.id, c.type, c.nullable});
    t.columns.push_back(std::move(c));
  }
  for (const auto& p : pk) {
    const auto* c = t.column(p);
    if (c == nullptr) throw std::invalid_argument("primary key column " + p + " does not exist");
    t.pk_col_ids.push_back(c->id);
  }
  t.layout = make_layout(defs);

  auto saved_next = next_id_;
  t.id = next_id(2);
  t.layout_id = t.id + 1;

  WritePlan plan;
  plan.ensure_absent(name_key(name));
  plan.put(name_key(name), [&] {
    auto v = Bytes{std::byte{kKindTable}};
    auto id = be_bytes(t.id, 4);
    v.insert(v.end(), id.begin(), id.end());
    return v;
  }());
  plan.put(table_key(t.id), serialize(t));
  plan.put(counter_key(kCounterNextId), be_bytes(next_id_, 8));
  plan.put(counter_key(kCounterVersion), be_bytes(version_ + 1, 8));
  try {
    commit_ddl(db, std::move(plan), "relation " + name);
  } catch (...) {
    next_id_ = saved_next;
    throw;
  }
  ++version_;
  auto id = t.id;
  index_table(std::move(t));
  rebuild_key_layouts();
  return tables_.at(id);
}

auto Catalog::create_index(DB& db, const std::string& raw_name, const std::string& table_name,
                           const std::vector<std::string>& cols) -> const Index& {
  auto name = fold(raw_name);
  if (names_.contains(name)) throw std::invalid_argument("relation " + name + " already exists");
  const auto* found = table(table_name);
  if (found == nullptr) throw std::invalid_argument("relation " + fold(table_name) + " does not exist");
  auto t = *found;
  Index ix;
  ix.name = name;
  for (const auto& c : cols) {
    const auto* col = t.column(c);
    if (col == nullptr) throw std::invalid_argument("column " + fold(c) + " does not exist");
    ix.col_ids.push_back(col->id);
  }
  auto saved_next = next_id_;
  ix.id = next_id(1);
  t.indexes.push_back(ix);
  ++t.version;

  WritePlan plan;
  plan.ensure_absent(name_key(name));
  auto v = Bytes{std::byte{kKindIndex}};
  auto idb = be_bytes(ix.id, 4);
  v.insert(v.end(), idb.begin(), idb.end());
  plan.put(name_key(name), v);
  plan.put(table_key(t.id), serialize(t));
  plan.put(counter_key(kCounterNextId), be_bytes(next_id_, 8));
  plan.put(counter_key(kCounterVersion), be_bytes(version_ + 1, 8));
  // prototype: the backfill rides in the DDL's batch, so it is bounded by the
  // engine's 1 GiB plan limit and is not guarded against concurrent writers.
  {
    auto snap = db.snapshot();
    Bytes prefix;
    key_put_prefix(prefix, kNsRow, t.id);
    for (const auto& e : snap.iter_from({}, prefix)) {
      if (e.key.size() < prefix.size() || !std::ranges::equal(e.key.first(prefix.size()), prefix)) break;
      Bytes k;
      if (!encode_index_key_from_row(t, ix, e.value, k)) throw std::runtime_error("malformed row in " + t.name);
      plan.put(k, BytesView{});
    }
  }
  try {
    commit_ddl(db, std::move(plan), "relation " + name);
  } catch (...) {
    next_id_ = saved_next;
    throw;
  }
  ++version_;
  index_table(std::move(t));
  rebuild_key_layouts();
  const auto& stored = tables_.at(found->id);
  return *stored.index(name);
}

void Catalog::index_table(Table t) {
  names_[t.name] = t.id;
  for (const auto& ix : t.indexes) names_[ix.name] = ix.id;
  layout_tables_[t.layout_id] = t.id;
  tables_[t.id] = std::move(t);
}

void Catalog::rebuild_key_layouts() {
  key_layouts_.clear();
  for (const auto& [id, t] : tables_) {
    std::vector<KeyPart> pk_parts;
    for (auto cid : t.pk_col_ids) pk_parts.push_back({t.column(cid)->type, false});
    key_layouts_[id] = KeyLayout{kNsRow, &t, nullptr, pk_parts, 0};
    for (const auto& ix : t.indexes) {
      KeyLayout kl{kNsIndex, &t, &ix, {}, ix.col_ids.size()};
      for (auto cid : ix.col_ids) {
        const auto* c = t.column(cid);
        kl.parts.push_back({c->type, c->nullable});
      }
      kl.parts.insert(kl.parts.end(), pk_parts.begin(), pk_parts.end());
      key_layouts_[ix.id] = std::move(kl);
    }
  }
}

auto Catalog::table(std::string_view name) const -> const Table* {
  auto it = names_.find(fold(name));
  if (it == names_.end()) return nullptr;
  return table(it->second);
}

auto Catalog::table(std::uint32_t id) const -> const Table* {
  auto it = tables_.find(id);
  return it == tables_.end() ? nullptr : &it->second;
}

auto Catalog::key_layout(std::uint32_t id) const -> const KeyLayout* {
  auto it = key_layouts_.find(id);
  return it == key_layouts_.end() ? nullptr : &it->second;
}

auto Catalog::row_layout(std::uint32_t layout_id) const -> const RowLayout* {
  if (layout_id >= kResultLayoutBase) {
    auto i = std::size_t{layout_id - kResultLayoutBase};
    return i < results_.size() ? &results_[i]->layout : nullptr;
  }
  const auto* t = table_of_layout(layout_id);
  return t == nullptr ? nullptr : &t->layout;
}

auto Catalog::table_of_layout(std::uint32_t layout_id) const -> const Table* {
  auto it = layout_tables_.find(layout_id);
  return it == layout_tables_.end() ? nullptr : table(it->second);
}

auto Catalog::result_layout(const std::vector<PhysType>& types) -> std::uint32_t {
  for (std::size_t i = 0; i < results_.size(); ++i) {
    if (results_[i]->types == types) return kResultLayoutBase + static_cast<std::uint32_t>(i);
  }
  std::vector<ColumnDef> defs;
  for (std::uint32_t i = 0; i < types.size(); ++i) defs.push_back({i, types[i], true});
  results_.push_back(std::make_unique<ResultLayout>(ResultLayout{types, make_layout(defs)}));
  return kResultLayoutBase + static_cast<std::uint32_t>(results_.size() - 1);
}

auto Catalog::result_types(std::uint32_t layout_id) const -> const std::vector<PhysType>* {
  if (layout_id < kResultLayoutBase) return nullptr;
  auto i = std::size_t{layout_id - kResultLayoutBase};
  return i < results_.size() ? &results_[i]->types : nullptr;
}

auto Catalog::constants(const Table& t) const -> std::vector<std::pair<std::string, std::uint32_t>> {
  std::vector<std::pair<std::string, std::uint32_t>> out;
  out.emplace_back("TBL_" + upper(t.name), t.id);
  out.emplace_back("LAY_" + upper(t.name), t.layout_id);
  for (const auto& ix : t.indexes) out.emplace_back("IDX_" + upper(ix.name), ix.id);
  for (const auto& c : t.columns) {
    const auto* slot = t.layout.find(c.id);
    auto suffix = c.type == PhysType::Bytes ? "_SLOT" : "_OFF";
    out.emplace_back(upper(t.name) + "_" + upper(c.name) + suffix, slot->pos);
    out.emplace_back(upper(t.name) + "_" + upper(c.name) + "_COL", c.id);
  }
  return out;
}

auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool {
  key_put_prefix(out, kNsRow, t.id);
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, false, row, out)) return false;
  }
  return true;
}

}  // namespace bytecask::sql
