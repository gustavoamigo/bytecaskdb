// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.cpp — the catalog's definitions; see catalog.cppm.

module;
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/bytecask.hpp"

module bytecask.sql.catalog;

namespace bytecask::sql {

namespace {

constexpr std::byte kSubCounter{0x01};
constexpr std::byte kSubTable{0x02};
constexpr std::byte kSubName{0x03};
constexpr std::byte kCounterNextId{0x01};
constexpr std::byte kCounterVersion{0x02};
constexpr std::uint8_t kKindTable = 1;
constexpr std::uint8_t kKindIndex = 2;
// 2: the row format of docs/sql_layer_types_design.md (12-byte header, the
// bitmap meaning present). Format 1 was the prototype's; its column
// positions and rows do not decode under format 2, so it is refused.
constexpr std::uint16_t kRecordFormat = 2;

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
  w.u16(narrow<std::uint16_t>(t.layout.bitmap_bytes));
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
  if (auto format = r.u16(); format != kRecordFormat) {
    throw std::runtime_error("catalog record format " + std::to_string(format) + " is not " +
                             std::to_string(kRecordFormat) +
                             (format < kRecordFormat ? ": the database was written by an earlier SQL-layer build "
                                                       "whose row format this build does not read"
                                                     : ": the database was written by a later SQL-layer build"));
  }
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
    if (is_fixed(c.type)) {
      t.layout.fixed_end = std::max(t.layout.fixed_end, pos + kFixedColumnBytes);
    } else {
      t.layout.nvar = std::max(t.layout.nvar, pos + 1);
    }
    t.columns.push_back(std::move(c));
  }
  if (!layout_valid(t.layout)) {
    throw std::runtime_error("catalog record of table " + t.name + " has a malformed row layout");
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
  // Every key column must be a column of the table.
  auto known = [&](std::uint32_t id) { return t.column(id) != nullptr; };
  auto index_ok = [&](const Index& ix) { return std::ranges::all_of(ix.col_ids, known); };
  if (t.pk_col_ids.empty() || !std::ranges::all_of(t.pk_col_ids, known) || !std::ranges::all_of(t.indexes, index_ok)) {
    throw std::runtime_error("catalog record of table " + t.name + " names a key column it does not have");
  }
  return t;
}

// Appends one column of `row` as a key part. False if the row is malformed.
auto put_part_from_row(const Table& t, std::uint32_t col_id, bool nullable, BytesView row, Bytes& out)
    -> bool {
  const auto* slot = t.layout.find(col_id);
  if (slot == nullptr) return false;
  KeyPart part{slot->type, nullable};
  if (row_is_null(row, *slot)) {
    if (!nullable) return false;
    key_put_null(out, part);
    return true;
  }
  switch (slot->type) {
    case PhysType::I64: {
      auto v = row_i64(row, slot->pos);
      if (!v) return false;
      key_put_i64(out, *v, part);
      return true;
    }
    case PhysType::F64: {
      auto v = row_f64(row, slot->pos);
      if (!v) return false;
      key_put_f64(out, *v, part);
      return true;
    }
    case PhysType::Bool: {
      auto v = row_bool(row, slot->pos);
      if (!v) return false;
      key_put_bool(out, *v, part);
      return true;
    }
    case PhysType::Bytes: {
      auto v = row_bytes(row, slot->pos);
      if (!v) return false;
      key_put_bytes(out, *v, part);
      return true;
    }
  }
  return false;
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
  if (first + n > kMaxObjectId) throw std::length_error("catalog ids exhausted");
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
    for (auto cid : t.pk_col_ids) pk_parts.push_back(KeyPart{t.column(cid)->type});
    key_layouts_[id] = KeyLayout{kNsRow, &t, nullptr, pk_parts, 0};
    for (const auto& ix : t.indexes) {
      KeyLayout kl{kNsIndex, &t, &ix, {}, ix.col_ids.size()};
      for (auto cid : ix.col_ids) {
        const auto* c = t.column(cid);
        kl.parts.push_back(KeyPart{c->type, c->nullable});
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
  const auto* t = table_of_layout(layout_id);
  return t == nullptr ? nullptr : &t->layout;
}

auto Catalog::table_of_layout(std::uint32_t layout_id) const -> const Table* {
  auto it = layout_tables_.find(layout_id);
  return it == layout_tables_.end() ? nullptr : table(it->second);
}

auto Catalog::constants(const Table& t) const -> std::vector<std::pair<std::string, std::uint32_t>> {
  std::vector<std::pair<std::string, std::uint32_t>> out;
  out.emplace_back("TBL_" + upper(t.name), t.id);
  out.emplace_back("LAY_" + upper(t.name), t.layout_id);
  for (const auto& ix : t.indexes) out.emplace_back("IDX_" + upper(ix.name), ix.id);
  for (const auto& c : t.columns) {
    const auto* slot = t.layout.find(c.id);
    auto suffix = is_fixed(c.type) ? "_OFF" : "_SLOT";
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
