// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.cpp — the catalog's definitions; see catalog.cppm and
// docs/sql_layer_catalog_design.md.

module;
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
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
constexpr std::uint8_t kMaxDecimalPrecision = 18;

using DdlOp = Catalog::DdlOp;

auto counter_key(std::byte id) -> Bytes { return {std::byte{kNsCatalog}, kSubCounter, id}; }

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

auto has_prefix(BytesView key, BytesView prefix) -> bool {
  return key.size() >= prefix.size() && std::ranges::equal(key.first(prefix.size()), prefix);
}

// SQL folds unquoted identifiers; the catalog folds every name to lower case.
auto fold(std::string_view name) -> std::string {
  std::string s{name};
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

[[noreturn]] void corrupt(const std::string& what) { throw std::runtime_error("catalog: " + what); }

// Stored name bytes as the string they were written from: the one place the
// catalog reads bytes as chars.
auto as_string(BytesView b) -> std::string { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// Little-endian, append-only: a later format adds fields at the end of the
// record or of an entry.
class RecordWriter {
public:
  void u8(std::uint8_t v) { out_.push_back(std::byte{v}); }
  void u16(std::uint64_t v) { le(narrow<std::uint16_t>(v), 2); }
  void u32(std::uint64_t v) { le(narrow<std::uint32_t>(v), 4); }
  void u64(std::uint64_t v) { le(v, 8); }
  void str(std::string_view s) {
    u16(s.size());
    for (auto c : s) out_.push_back(static_cast<std::byte>(c));
  }
  void blob(BytesView b) {
    u32(b.size());
    out_.insert(out_.end(), b.begin(), b.end());
  }
  // An entry: its u32 length, then what `fill` writes.
  template <typename Fill>
  void entry(Fill&& fill) {
    RecordWriter sub;
    fill(sub);
    blob(sub.out_);
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
  auto u64() -> std::uint64_t { return le(8); }
  auto str() -> std::string {
    auto b = take(u16());
    return as_string(b);
  }
  auto blob() -> BytesView { return take(u32()); }
  [[nodiscard]] auto at_end() const -> bool { return pos_ == in_.size(); }
  void expect_end(const std::string& what) const {
    if (!at_end()) corrupt(what + " has trailing bytes");
  }

private:
  auto take(std::size_t n) -> BytesView {
    if (in_.size() - pos_ < n) corrupt("record truncated");
    auto b = in_.subspan(pos_, n);
    pos_ += n;
    return b;
  }
  auto le(std::size_t width) -> std::uint64_t {
    auto b = take(width);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) v |= std::to_integer<std::uint64_t>(b[i]) << (8 * i);
    return v;
  }
  BytesView in_;
  std::size_t pos_{0};
};

void write_datum(RecordWriter& w, const Datum& d) {
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          w.u8(0);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          w.u8(static_cast<std::uint8_t>(PhysType::I64));
          w.u64(static_cast<std::uint64_t>(v));
        } else if constexpr (std::is_same_v<T, double>) {
          w.u8(static_cast<std::uint8_t>(PhysType::F64));
          w.u64(std::bit_cast<std::uint64_t>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
          w.u8(static_cast<std::uint8_t>(PhysType::Bool));
          w.u8(v ? 1 : 0);
        } else {
          w.u8(static_cast<std::uint8_t>(PhysType::Bytes));
          w.blob(v);
        }
      },
      d);
}

auto read_datum(RecordReader& r) -> Datum {
  switch (r.u8()) {
    case 0: return std::monostate{};
    case static_cast<std::uint8_t>(PhysType::I64): return static_cast<std::int64_t>(r.u64());
    case static_cast<std::uint8_t>(PhysType::F64): return std::bit_cast<double>(r.u64());
    case static_cast<std::uint8_t>(PhysType::Bool): {
      auto b = r.u8();
      if (b > 1) corrupt("boolean default is neither 0 nor 1");
      return b == 1;
    }
    case static_cast<std::uint8_t>(PhysType::Bytes): {
      auto b = r.blob();
      return Bytes(b.begin(), b.end());
    }
    default: corrupt("unknown default tag");
  }
}

auto name_value(std::uint8_t kind, std::uint32_t id) -> Bytes {
  auto v = Bytes{std::byte{kind}};
  auto idb = be_bytes(id, 4);
  v.insert(v.end(), idb.begin(), idb.end());
  return v;
}

auto row_prefix(std::uint32_t table_id) -> Bytes {
  Bytes p;
  key_put_prefix(p, kNsRow, table_id);
  return p;
}

auto index_prefix(std::uint32_t index_id) -> Bytes {
  Bytes p;
  key_put_prefix(p, kNsIndex, index_id);
  return p;
}

// Appends one column of `row` as a key part. False if the row is malformed
// or a non-nullable part is null; `was_null` receives whether it was null.
auto put_part_from_row(const Table& t, std::uint32_t col_id, KeyPart part, BytesView row, Bytes& out,
                       bool* was_null = nullptr) -> bool {
  const auto* slot = t.layout.find(col_id);
  if (slot == nullptr) return false;
  if (row_is_null(row, *slot)) {
    if (!part.nullable) return false;
    if (was_null != nullptr) *was_null = true;
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

// The rows of a table in the snapshot, in primary key order.
template <typename Visit>
void for_each_row(const Snapshot& snap, std::uint32_t table_id, Visit&& visit) {
  auto prefix = row_prefix(table_id);
  for (const auto& e : snap.iter_from({}, prefix)) {
    if (!has_prefix(e.key, prefix)) break;
    visit(e.key, e.value);
  }
}

auto put_op(std::vector<DdlOp>& ops, DdlOp::Kind kind, Bytes a, Bytes b = {}) -> void {
  ops.push_back(DdlOp{kind, std::move(a), std::move(b)});
}

// The guard every DDL carries: no other DDL committed since the snapshot.
// Also the bootstrap guard: the version key is absent until the first DDL.
void guard_catalog(std::vector<DdlOp>& ops) {
  put_op(ops, DdlOp::Kind::EnsureUnchanged, counter_key(kCounterVersion));
}

void put_counters(std::vector<DdlOp>& ops, std::uint64_t next_id, std::uint64_t version) {
  put_op(ops, DdlOp::Kind::Put, counter_key(kCounterNextId), be_bytes(next_id, 8));
  put_op(ops, DdlOp::Kind::Put, counter_key(kCounterVersion), be_bytes(version, 8));
}

// Every write to the table's rows since the snapshot makes the DDL a
// conflict: a backfill must see every row.
void guard_rows(std::vector<DdlOp>& ops, std::uint32_t table_id) {
  auto p = row_prefix(table_id);
  put_op(ops, DdlOp::Kind::EnsureRangeUnchanged, p, key_successor(p));
}

auto reserve_ids(std::uint64_t& next_id, std::uint32_t n) -> std::uint32_t {
  auto first = next_id;
  if (first + n - 1 > Catalog::kMaxObjectId) throw std::length_error("catalog object ids exhausted");
  next_id += n;
  return static_cast<std::uint32_t>(first);
}

void check_column_spec(const ColumnSpec& c) {
  if (!sql_type_valid(c.sql)) throw std::invalid_argument("column " + c.name + " has an invalid type");
  if (!datum_fits(c.default_value, phys_type(c.sql))) {
    throw std::invalid_argument("default of column " + c.name + " is not of its type");
  }
}

}  // namespace

// ------------------------------------------------------------------ types

auto set_datum(RowWriter& w, std::uint32_t col_id, const Datum& d) -> bool {
  return std::visit(
      [&](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return w.set_null(col_id);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return w.set_i64(col_id, v);
        } else if constexpr (std::is_same_v<T, double>) {
          return w.set_f64(col_id, v);
        } else if constexpr (std::is_same_v<T, bool>) {
          return w.set_bool(col_id, v);
        } else {
          return w.set_bytes(col_id, v);
        }
      },
      d);
}


auto sql_type_valid(SqlType t) -> bool {
  switch (t.kind) {
    case SqlKind::Varchar: return t.length > 0 && t.precision == 0 && t.scale == 0;
    case SqlKind::Decimal:
      return t.length == 0 && t.precision >= 1 && t.precision <= kMaxDecimalPrecision && t.scale <= t.precision;
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time: return t.length == 0 && t.precision == 0 && t.scale == 0;
  }
  return false;  // a kind no build knows
}

auto phys_type(SqlType t) -> PhysType {
  switch (t.kind) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Decimal:
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time: return PhysType::I64;
    case SqlKind::Real:
    case SqlKind::Double: return PhysType::F64;
    case SqlKind::Boolean: return PhysType::Bool;
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar: return PhysType::Bytes;
  }
  return PhysType::Bytes;
}

auto sql_type_name(SqlType t) -> std::string {
  switch (t.kind) {
    case SqlKind::SmallInt: return "smallint";
    case SqlKind::Integer: return "integer";
    case SqlKind::BigInt: return "bigint";
    case SqlKind::Real: return "real";
    case SqlKind::Double: return "double precision";
    case SqlKind::Boolean: return "boolean";
    case SqlKind::Bytea: return "bytea";
    case SqlKind::Text: return "text";
    case SqlKind::Varchar: return "varchar(" + std::to_string(t.length) + ")";
    case SqlKind::Decimal: return "numeric(" + std::to_string(t.precision) + "," + std::to_string(t.scale) + ")";
    case SqlKind::Timestamp: return "timestamp";
    case SqlKind::Date: return "date";
    case SqlKind::Time: return "time";
  }
  return "?";
}

auto datum_fits(const Datum& d, PhysType t) -> bool {
  switch (t) {
    case PhysType::I64: return !std::holds_alternative<double>(d) && !std::holds_alternative<bool>(d) &&
                               !std::holds_alternative<Bytes>(d);
    case PhysType::F64: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<double>(d);
    case PhysType::Bool: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<bool>(d);
    case PhysType::Bytes: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<Bytes>(d);
  }
  return false;
}

// ------------------------------------------------------------------ records

auto table_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubTable};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto name_entry_key(std::string_view name) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, kSubName};
  for (auto c : name) k.push_back(static_cast<std::byte>(c));
  return k;
}

namespace {

void write_layout(RecordWriter& w, std::uint32_t layout_id, const RowLayout& l) {
  w.entry([&](RecordWriter& e) {
    e.u32(layout_id);
    e.u16(l.generation);
    e.u16(l.bitmap_bytes);
    e.u16(l.fixed_end);  // var_table: where this generation's fixed region ends
    e.u16(l.nvar);
    e.u16(l.columns.size());
    for (const auto& s : l.columns) {
      e.u32(s.col_id);
      e.u32(s.pos);
    }
  });
}

auto read_layout(RecordReader& r, const Table& t) -> std::pair<std::uint32_t, RowLayout> {
  RecordReader e{r.blob()};
  auto id = e.u32();
  RowLayout l;
  l.generation = static_cast<std::uint16_t>(e.u16());
  l.bitmap_bytes = e.u16();
  l.fixed_start = static_cast<std::uint32_t>(kRowHeaderBytes) + l.bitmap_bytes;
  l.fixed_end = e.u16();
  l.nvar = e.u16();
  auto n = e.u16();
  std::set<std::uint32_t> ids, fixed, slots;
  for (std::uint32_t i = 0; i < n; ++i) {
    auto col_id = e.u32();
    auto pos = e.u32();
    const auto* c = t.column(col_id);
    if (c == nullptr) corrupt("layout of table " + t.name + " names a column it does not have");
    auto& used = is_fixed(c->type) ? fixed : slots;
    if (!ids.insert(col_id).second || !used.insert(pos).second) {
      corrupt("layout of table " + t.name + " has a column or position twice");
    }
    l.columns.push_back({col_id, c->type, c->nullable, pos});
  }
  e.expect_end("layout entry of table " + t.name);
  if (!layout_valid(l)) corrupt("table " + t.name + " has a malformed row layout");
  return {id, std::move(l)};
}

}  // namespace

auto serialize_table(const Table& t) -> Bytes {
  RecordWriter w;
  w.u16(Catalog::kRecordFormat);
  w.u32(t.id);
  w.str(t.name);
  w.u32(t.version);
  w.u32(t.next_col_id);
  w.u16(t.columns.size());
  for (const auto& c : t.columns) {
    w.entry([&](RecordWriter& e) {
      e.u32(c.id);
      e.str(c.name);
      e.u8(static_cast<std::uint8_t>(c.sql.kind));
      e.u32(c.sql.length);
      e.u8(c.sql.precision);
      e.u8(c.sql.scale);
      e.u8(c.nullable ? 1 : 0);
      write_datum(e, c.default_value);
    });
  }
  w.u16(t.pk_col_ids.size());
  for (auto id : t.pk_col_ids) w.u32(id);
  w.u16(t.indexes.size());
  for (const auto& ix : t.indexes) {
    w.entry([&](RecordWriter& e) {
      e.u32(ix.id);
      e.str(ix.name);
      e.u8(ix.unique ? 1 : 0);
      e.u16(ix.parts.size());
      for (const auto& p : ix.parts) {
        e.u32(p.col_id);
        e.u8(p.desc ? 1 : 0);
      }
    });
  }
  w.u16(t.retired_layouts.size() + 1);
  for (const auto& [id, l] : t.retired_layouts) write_layout(w, id, l);
  write_layout(w, t.layout_id, t.layout);
  return w.bytes();
}

auto parse_table(BytesView in) -> Table {
  RecordReader r{in};
  if (auto format = r.u16(); format != Catalog::kRecordFormat) {
    throw std::runtime_error(
        "catalog record format " + std::to_string(format) + " is not " + std::to_string(Catalog::kRecordFormat) +
        (format < Catalog::kRecordFormat
             ? ": the database was written by an earlier SQL-layer build whose catalog this build does not read"
             : ": the database was written by a later SQL-layer build"));
  }
  Table t;
  t.id = r.u32();
  t.name = r.str();
  t.version = r.u32();
  t.next_col_id = r.u32();
  auto ncols = r.u16();
  for (std::uint32_t i = 0; i < ncols; ++i) {
    RecordReader e{r.blob()};
    Column c;
    c.id = e.u32();
    c.name = e.str();
    c.sql.kind = static_cast<SqlKind>(e.u8());
    c.sql.length = e.u32();
    c.sql.precision = e.u8();
    c.sql.scale = e.u8();
    auto nullable = e.u8();
    c.default_value = read_datum(e);
    e.expect_end("column entry of table " + t.name);
    if (!sql_type_valid(c.sql)) corrupt("column " + c.name + " of table " + t.name + " has an invalid type");
    if (nullable > 1) corrupt("column " + c.name + " of table " + t.name + " has a bad nullable flag");
    c.type = phys_type(c.sql);
    c.nullable = nullable == 1;
    if (!datum_fits(c.default_value, c.type)) corrupt("default of column " + c.name + " is not of its type");
    if (c.id >= t.next_col_id || t.column(c.id) != nullptr || t.column(c.name) != nullptr) {
      corrupt("table " + t.name + " has a column id or name twice, or past next_col_id");
    }
    t.columns.push_back(std::move(c));
  }
  auto npk = r.u16();
  for (std::uint32_t i = 0; i < npk; ++i) {
    auto id = r.u32();
    if (std::ranges::find(t.pk_col_ids, id) != t.pk_col_ids.end()) corrupt("table " + t.name + " repeats a key column");
    t.pk_col_ids.push_back(id);
  }
  auto nidx = r.u16();
  for (std::uint32_t i = 0; i < nidx; ++i) {
    RecordReader e{r.blob()};
    Index ix;
    ix.id = e.u32();
    ix.name = e.str();
    auto unique = e.u8();
    auto n = e.u16();
    for (std::uint32_t j = 0; j < n; ++j) {
      IndexPart p;
      p.col_id = e.u32();
      auto desc = e.u8();
      if (desc > 1) corrupt("index " + ix.name + " has a bad direction flag");
      p.desc = desc == 1;
      if (std::ranges::find(ix.parts, p.col_id, &IndexPart::col_id) != ix.parts.end()) {
        corrupt("index " + ix.name + " repeats a column");
      }
      ix.parts.push_back(p);
    }
    e.expect_end("index entry of table " + t.name);
    if (unique > 1 || ix.parts.empty()) corrupt("index " + ix.name + " is malformed");
    ix.unique = unique == 1;
    t.indexes.push_back(std::move(ix));
  }
  auto nlayout = r.u16();
  if (nlayout == 0) corrupt("table " + t.name + " has no layout");
  for (std::uint32_t i = 0; i < nlayout; ++i) t.retired_layouts.push_back(read_layout(r, t));
  std::tie(t.layout_id, t.layout) = std::move(t.retired_layouts.back());
  t.retired_layouts.pop_back();
  r.expect_end("record of table " + t.name);

  // Every key column is a column of the table, and the current layout holds
  // every column exactly once.
  auto known = [&](std::uint32_t id) { return t.column(id) != nullptr; };
  auto index_ok = [&](const Index& ix) {
    return std::ranges::all_of(ix.parts, [&](const IndexPart& p) { return known(p.col_id); });
  };
  if (t.pk_col_ids.empty() || !std::ranges::all_of(t.pk_col_ids, known) || !std::ranges::all_of(t.indexes, index_ok)) {
    corrupt("record of table " + t.name + " names a key column it does not have");
  }
  if (std::ranges::any_of(t.pk_col_ids, [&](std::uint32_t id) { return t.column(id)->nullable; })) {
    corrupt("table " + t.name + " has a nullable primary key column");
  }
  if (t.layout.columns.size() != t.columns.size()) corrupt("current layout of table " + t.name + " misses a column");
  return t;
}

// ------------------------------------------------------------------ lookups

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

auto Catalog::load(const Snapshot& snap) -> Catalog {
  Catalog cat;
  Bytes v;
  // A counter is 8 bytes; the next id is past every id in use and at most
  // one past kMaxObjectId.
  auto counter = [&](std::byte id, std::uint64_t& out) {
    if (!snap.get({}, counter_key(id), v)) return;
    if (v.size() != 8) corrupt("a counter is not 8 bytes");
    out = be_value(v);
  };
  counter(kCounterNextId, cat.next_id_);
  counter(kCounterVersion, cat.version_);
  if (cat.next_id_ == 0 || cat.next_id_ > std::uint64_t{kMaxObjectId} + 1) corrupt("next object id out of range");
  auto tables = Bytes{std::byte{kNsCatalog}, kSubTable};
  for (const auto& e : snap.iter_from({}, tables)) {
    if (!has_prefix(e.key, tables)) break;
    auto t = parse_table(e.value);
    if (e.key.size() != tables.size() + 4 || be_value(e.key.subspan(tables.size())) != t.id) {
      corrupt("record of table " + t.name + " is under another id's key");
    }
    cat.tables_.emplace(t.id, std::move(t));
  }
  cat.rebuild_lookups();

  // Every id below the counter and used once; every name entry names its
  // object, and every object has its entry.
  std::set<std::uint32_t> ids;
  auto use = [&](std::uint32_t id) {
    if (id == 0 || id >= cat.next_id_ || !ids.insert(id).second) corrupt("object id " + std::to_string(id) + " reused");
  };
  std::size_t objects = 0;
  for (const auto& [id, t] : cat.tables_) {
    use(id);
    use(t.layout_id);
    for (const auto& [lid, l] : t.retired_layouts) use(lid);
    for (const auto& ix : t.indexes) use(ix.id);
    objects += 1 + t.indexes.size();
  }
  std::size_t entries = 0;
  auto names = Bytes{std::byte{kNsCatalog}, kSubName};
  for (const auto& e : snap.iter_from({}, names)) {
    if (!has_prefix(e.key, names)) break;
    auto name = as_string(e.key.subspan(names.size()));
    auto it = cat.names_.find(name);
    auto kind = e.value.empty() ? 0 : std::to_integer<std::uint8_t>(e.value[0]);
    if (it == cat.names_.end() || e.value.size() != 5 || be_value(e.value.subspan(1)) != it->second ||
        kind != (cat.tables_.contains(it->second) ? kKindTable : kKindIndex)) {
      corrupt("name entry " + name + " does not match the records");
    }
    ++entries;
  }
  if (entries != objects || cat.names_.size() != objects) corrupt("a table or index has no name entry");
  return cat;
}

auto Catalog::clone() const -> Catalog {
  Catalog c;
  c.next_id_ = next_id_;
  c.version_ = version_;
  c.tables_ = tables_;
  c.rebuild_lookups();
  return c;
}

void Catalog::put_table(Table t) {
  auto id = t.id;
  tables_.insert_or_assign(id, std::move(t));
}

void Catalog::erase_table(std::uint32_t id) { tables_.erase(id); }

void Catalog::rebuild_lookups() {
  names_.clear();
  key_layouts_.clear();
  layout_tables_.clear();
  for (const auto& [id, t] : tables_) {
    auto name_taken = [&](const std::string& n) {
      if (names_.contains(n)) corrupt("name " + n + " is used twice");
    };
    name_taken(t.name);
    names_[t.name] = id;
    layout_tables_[t.layout_id] = id;
    std::vector<KeyPart> pk_parts;
    for (auto cid : t.pk_col_ids) pk_parts.push_back(KeyPart{t.column(cid)->type});
    key_layouts_[id] = KeyLayout{kNsRow, &t, nullptr, pk_parts, 0};
    for (const auto& ix : t.indexes) {
      name_taken(ix.name);
      names_[ix.name] = ix.id;
      KeyLayout kl{kNsIndex, &t, &ix, {}, ix.parts.size()};
      for (const auto& p : ix.parts) {
        const auto* c = t.column(p.col_id);
        kl.parts.push_back(KeyPart{c->type, c->nullable, p.desc});
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

auto Catalog::index(std::string_view name) const -> std::pair<const Table*, const Index*> {
  auto it = names_.find(fold(name));
  if (it == names_.end()) return {nullptr, nullptr};
  const auto* kl = key_layout(it->second);
  if (kl == nullptr || kl->index == nullptr) return {nullptr, nullptr};
  return {kl->table, kl->index};
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

auto Catalog::constants(const Table& t) -> std::vector<PlanConstant> {
  std::vector<PlanConstant> out;
  auto id = [](std::uint32_t v) { return std::to_string(v); };
  out.push_back({"TBL_" + id(t.id), t.id, t.name});
  out.push_back({"LAY_" + id(t.layout_id), t.layout_id, t.name + " layout"});
  for (const auto& ix : t.indexes) out.push_back({"IDX_" + id(ix.id), ix.id, ix.name});
  for (const auto& c : t.columns) {
    const auto* slot = t.layout.find(c.id);
    auto prefix = "COL_" + id(t.id) + "_" + id(c.id);
    out.push_back({prefix + (is_fixed(c.type) ? "_OFF" : "_SLOT"), slot->pos, t.name + "." + c.name});
    out.push_back({prefix + "_COL", c.id, t.name + "." + c.name});
  }
  return out;
}

// --------------------------------------------------------------------- DDL

auto Catalog::next_version(const Ddl& ddl, const Snapshot& snap, std::vector<DdlOp>& ops, bool& backfill) const
    -> std::shared_ptr<const Catalog> {
  auto next = std::make_shared<Catalog>(clone());
  next->version_ = version_ + 1;
  auto finish = [&] {
    guard_catalog(ops);
    put_counters(ops, next->next_id_, next->version_);
    next->rebuild_lookups();
    return std::shared_ptr<const Catalog>{std::move(next)};
  };
  auto name_free = [&](const std::string& name) {
    if (names_.contains(name)) throw std::invalid_argument("relation \"" + name + "\" already exists");
  };
  auto find_table = [&](const std::string& name) -> const Table& {
    const auto* t = table(name);
    if (t == nullptr) throw std::invalid_argument("relation \"" + fold(name) + "\" does not exist");
    return *t;
  };
  backfill = false;

  if (const auto* ct = std::get_if<CreateTableDdl>(&ddl)) {
    auto name = fold(ct->name);
    name_free(name);
    if (ct->columns.empty() || ct->pk.empty()) throw std::invalid_argument("a table needs columns and a primary key");
    Table t;
    t.name = name;
    std::vector<ColumnDef> defs;
    for (const auto& spec : ct->columns) {
      check_column_spec(spec);
      auto cname = fold(spec.name);
      if (t.column(cname) != nullptr) throw std::invalid_argument("column \"" + cname + "\" specified more than once");
      auto in_pk = std::ranges::any_of(ct->pk, [&](const std::string& p) { return fold(p) == cname; });
      Column c{t.next_col_id++, cname, spec.sql, phys_type(spec.sql), spec.nullable && !in_pk, spec.default_value};
      defs.push_back({c.id, c.type, c.nullable});
      t.columns.push_back(std::move(c));
    }
    for (const auto& p : ct->pk) {
      const auto* c = t.column(p);
      if (c == nullptr) throw std::invalid_argument("primary key column \"" + fold(p) + "\" does not exist");
      if (std::ranges::find(t.pk_col_ids, c->id) != t.pk_col_ids.end()) {
        throw std::invalid_argument("column \"" + c->name + "\" appears twice in the primary key");
      }
      t.pk_col_ids.push_back(c->id);
    }
    t.layout = make_layout(defs);
    t.id = reserve_ids(next->next_id_, 2);
    t.layout_id = t.id + 1;
    put_op(ops, DdlOp::Kind::EnsureAbsent, name_entry_key(name));
    put_op(ops, DdlOp::Kind::Put, name_entry_key(name), name_value(kKindTable, t.id));
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));
    next->put_table(std::move(t));
    return finish();
  }

  if (const auto* ci = std::get_if<CreateIndexDdl>(&ddl)) {
    auto name = fold(ci->name);
    name_free(name);
    auto t = find_table(ci->table);
    if (ci->columns.empty()) throw std::invalid_argument("an index needs columns");
    Index ix;
    ix.name = name;
    ix.unique = ci->unique;
    for (const auto& c : ci->columns) {
      const auto* col = t.column(c.name);
      if (col == nullptr) throw std::invalid_argument("column \"" + fold(c.name) + "\" does not exist");
      if (std::ranges::find(ix.parts, col->id, &IndexPart::col_id) != ix.parts.end()) {
        throw std::invalid_argument("column \"" + col->name + "\" appears twice in the index");
      }
      ix.parts.push_back({col->id, c.desc});
    }
    ix.id = reserve_ids(next->next_id_, 1);
    t.indexes.push_back(ix);
    ++t.version;
    put_op(ops, DdlOp::Kind::EnsureAbsent, name_entry_key(name));
    put_op(ops, DdlOp::Kind::Put, name_entry_key(name), name_value(kKindIndex, ix.id));
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));

    // The entries for the rows already in the table, in the same batch.
    backfill = true;
    std::set<Bytes> prefixes;  // a unique index's non-null prefixes seen so far
    for_each_row(snap, t.id, [&](BytesView, BytesView row) {
      Bytes k;
      std::size_t prefix_len = 0;
      bool has_null = false;
      if (!encode_index_key_from_row(t, ix, row, k, &prefix_len, &has_null)) {
        throw std::runtime_error("malformed row in table " + t.name);
      }
      if (ix.unique && !has_null && !prefixes.emplace(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(prefix_len)).second) {
        throw std::invalid_argument("could not create unique index \"" + name + "\": table \"" + t.name +
                                    "\" holds a duplicate key");
      }
      put_op(ops, DdlOp::Kind::Put, std::move(k));
    });
    guard_rows(ops, t.id);
    next->put_table(std::move(t));
    return finish();
  }

  if (const auto* dt = std::get_if<DropTableDdl>(&ddl)) {
    const auto* found = table(dt->name);
    if (found == nullptr) {
      if (index(dt->name).first != nullptr) throw std::invalid_argument("\"" + fold(dt->name) + "\" is an index");
      if (dt->if_exists) return nullptr;
      throw std::invalid_argument("table \"" + fold(dt->name) + "\" does not exist");
    }
    const auto& t = *found;
    put_op(ops, DdlOp::Kind::Del, table_record_key(t.id));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(t.name));
    auto rows = row_prefix(t.id);
    put_op(ops, DdlOp::Kind::DelRange, rows, key_successor(rows));
    for (const auto& ix : t.indexes) {
      put_op(ops, DdlOp::Kind::Del, name_entry_key(ix.name));
      auto p = index_prefix(ix.id);
      put_op(ops, DdlOp::Kind::DelRange, p, key_successor(p));
    }
    next->erase_table(t.id);
    return finish();
  }

  if (const auto* di = std::get_if<DropIndexDdl>(&ddl)) {
    auto [owner, found] = index(di->name);
    if (found == nullptr) {
      if (table(di->name) != nullptr) throw std::invalid_argument("\"" + fold(di->name) + "\" is not an index");
      if (di->if_exists) return nullptr;
      throw std::invalid_argument("index \"" + fold(di->name) + "\" does not exist");
    }
    auto t = *owner;
    auto id = found->id;
    std::erase_if(t.indexes, [&](const Index& ix) { return ix.id == id; });
    ++t.version;
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(fold(di->name)));
    auto p = index_prefix(id);
    put_op(ops, DdlOp::Kind::DelRange, p, key_successor(p));
    next->put_table(std::move(t));
    return finish();
  }

  const auto& ac = std::get<AddColumnDdl>(ddl);
  auto t = find_table(ac.table);
  check_column_spec(ac.column);
  auto cname = fold(ac.column.name);
  if (t.column(cname) != nullptr) {
    throw std::invalid_argument("column \"" + cname + "\" of relation \"" + t.name + "\" already exists");
  }
  auto has_default = !std::holds_alternative<std::monostate>(ac.column.default_value);
  if (!ac.column.nullable && !has_default) {
    throw std::invalid_argument("column \"" + cname + "\" is NOT NULL and has no default for the existing rows");
  }
  Column c{t.next_col_id, cname, ac.column.sql, phys_type(ac.column.sql), ac.column.nullable, ac.column.default_value};
  auto old_layout = t.layout;
  if (!append_columns(t.layout, std::vector<ColumnDef>{{c.id, c.type, c.nullable}})) {
    throw std::length_error("table \"" + t.name + "\" has no room in its row layout for another column (needs REBUILD)");
  }
  ++t.next_col_id;
  t.columns.push_back(c);
  ++t.version;
  put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));
  if (has_default) {
    // Every existing row gets the default; the generation and every existing
    // position are unchanged, so the rewrite is a copy and one more column.
    backfill = true;
    RowWriter w{t.layout};
    for_each_row(snap, t.id, [&](BytesView key, BytesView row) {
      if (!row_valid(row) || row_generation(row) != old_layout.generation) {
        throw std::runtime_error("malformed row in table " + t.name);
      }
      w.reset();
      w.copy_from(row);
      Bytes out;
      if (!set_datum(w, c.id, c.default_value) || !w.finish(out)) {
        throw std::length_error("a row of table " + t.name + " is over the value limit with the new column");
      }
      put_op(ops, DdlOp::Kind::Put, Bytes(key.begin(), key.end()), std::move(out));
    });
    guard_rows(ops, t.id);
  }
  next->put_table(std::move(t));
  return finish();
}

// ------------------------------------------------------------- LiveCatalog

LiveCatalog::LiveCatalog(DB& db) : db_{db} {
  auto snap = db_.snapshot();
  current_ = std::make_shared<const Catalog>(Catalog::load(snap));
}

auto LiveCatalog::current() const -> std::shared_ptr<const Catalog> {
  std::lock_guard<std::mutex> lock{mu_};
  return current_;
}

auto LiveCatalog::pin() const -> Pinned {
  for (;;) {
    auto g = gen_.load();
    auto cat = current();
    auto snap = db_.snapshot();
    if (gen_.load() != g) continue;  // a DDL commit began or ended in between
    if (g % 2 == 0) return Pinned{std::move(cat), std::move(snap)};
    // A DDL's commit is in flight. The engine publishes a batch only after
    // its sync, so for most of the commit the snapshot is still the old
    // catalog's; it holds the DDL only between the engine's publication and
    // the swap of current_. One read tells which, and only then is there a
    // wait, for the swap.
    Bytes v;
    auto version = snap.get({}, counter_key(kCounterVersion), v) ? be_value(v) : 0;
    if (version == cat->version()) return Pinned{std::move(cat), std::move(snap)};
    gen_.wait(g);
  }
}

auto LiveCatalog::prepare(const Ddl& ddl) const -> Prepared {
  auto [base, snap] = pin();
  std::vector<Catalog::DdlOp> ops;
  Prepared p{base, nullptr, nullptr, false};
  p.next = base->next_version(ddl, snap, ops, p.backfill);
  if (!p.next) return p;
  try {
    p.plan = std::make_unique<WritePlan>(std::move(snap));
    // Each op's bytes are freed once the plan holds its copy, so a backfill
    // near the batch limit is in memory about once, not twice.
    for (auto& op : ops) {
      switch (op.kind) {
        case Catalog::DdlOp::Kind::Put: p.plan->put(op.a, op.b); break;
        case Catalog::DdlOp::Kind::Del: p.plan->del(op.a); break;
        case Catalog::DdlOp::Kind::DelRange: p.plan->del_range(op.a, op.b); break;
        case Catalog::DdlOp::Kind::EnsureAbsent: p.plan->ensure_absent(op.a); break;
        case Catalog::DdlOp::Kind::EnsureUnchanged: p.plan->ensure_unchanged(op.a); break;
        case Catalog::DdlOp::Kind::EnsureRangeUnchanged: p.plan->ensure_range_unchanged(op.a, op.b); break;
      }
      Bytes{}.swap(op.a);
      Bytes{}.swap(op.b);
    }
  } catch (const std::invalid_argument& e) {
    if (!p.backfill) throw;
    throw std::length_error(std::string{"backfill refused by the engine (a key, or the 1 GiB batch, over its limit): "} +
                            e.what());
  }
  return p;
}

auto LiveCatalog::commit(Prepared p) -> bool {
  std::lock_guard<std::mutex> lock{ddl_mu_};
  return commit_locked(std::move(p));
}

auto LiveCatalog::commit_locked(Prepared p) -> bool {
  if (!p.next) return true;  // a no-op
  if (current() != p.base) return false;  // another DDL published since the prepare
  gen_.fetch_add(1);
  if (commit_hook_) commit_hook_();
  auto done = [&] {
    gen_.fetch_add(1);
    gen_.notify_all();
  };
  try {
    auto r = db_.apply_batch({.sync = true}, std::move(*p.plan));
    if (r) {
      std::lock_guard<std::mutex> swap{mu_};
      current_ = std::move(p.next);
    }
    done();
    return r.has_value();
  } catch (const std::invalid_argument& e) {
    done();
    if (!p.backfill) throw;
    throw std::length_error(std::string{"backfill refused by the engine (a key, or the 1 GiB batch, over its limit): "} +
                            e.what());
  } catch (...) {
    done();
    throw;
  }
}

auto LiveCatalog::apply(const Ddl& ddl) -> std::shared_ptr<const Catalog> {
  // One DDL at a time from prepare to publication, so a retry is only ever
  // for a write that raced a backfill, never for another DDL of this
  // catalog. Readers do not take this lock.
  std::lock_guard<std::mutex> lock{ddl_mu_};
  for (int attempt = 0; attempt < kDdlAttempts; ++attempt) {
    auto p = prepare(ddl);
    if (!p.next) return p.base;
    auto next = p.next;  // what this DDL publishes; current() may be a later DDL's once the lock is released
    if (commit_locked(std::move(p))) return next;
  }
  throw std::runtime_error("DDL conflicted with concurrent writes on every attempt");
}

// ------------------------------------------------------------------- keys

auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool {
  key_put_prefix(out, kNsRow, t.id);
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, KeyPart{t.column(id)->type}, row, out)) return false;
  }
  return true;
}

auto encode_index_key_from_row(const Table& t, const Index& ix, BytesView row, Bytes& out, std::size_t* prefix_len,
                               bool* has_null) -> bool {
  key_put_prefix(out, kNsIndex, ix.id);
  bool any_null = false;
  for (const auto& p : ix.parts) {
    const auto* c = t.column(p.col_id);
    bool was_null = false;
    if (!put_part_from_row(t, p.col_id, KeyPart{c->type, c->nullable, p.desc}, row, out, &was_null)) return false;
    any_null = any_null || was_null;
  }
  if (prefix_len != nullptr) *prefix_len = out.size();
  if (has_null != nullptr) *has_null = any_null;
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, KeyPart{t.column(id)->type}, row, out)) return false;
  }
  return true;
}

}  // namespace bytecask::sql
