// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// row_codec.cppm — the row format of docs/types_design.md, *Row
// format*:
//
//   u16   layout_generation
//   u16   bitmap_bytes       ; a multiple of 8
//   u16   var_table          ; offset of var_end: where the fixed region ends
//   u16   nvar               ; entries in var_end
//   u32   row_bytes          ; the whole row
//   u8[]  null_bitmap        ; bit set = present, indexed by permanent column id
//         fixed columns      ; one 8-byte word each, at permanent offsets
//   u32[] var_end            ; end offsets, relative to the start of var data
//   u8[]  var data
//
// The bitmap says present: a column set null and a column the row predates
// both have the bit clear. The sizes in the header let a reader with an older
// layout's constants find every region. All integers are little-endian. Every
// accessor is bounds-checked against the row: a column past the end of a
// shorter row reads as zero or empty, a malformed row is reported, never read
// past.

module;
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module bytecask.sql.row_codec;

export import bytecask.sql.key_codec;

export namespace bytecask::sql {

inline constexpr std::size_t kRowHeaderBytes = 12;
inline constexpr std::uint32_t kFixedColumnBytes = 8;
// The engine's hard ceiling on a value; its configured limit may be lower.
inline constexpr std::size_t kMaxRowBytes = (std::size_t{1} << 28) - 1;
// The bitmap is sized at create: the smallest multiple of 64 bits that holds
// every column id with kBitmapSpareBits to spare.
inline constexpr std::uint32_t kBitmapSpareBits = 16;

// A checked narrowing conversion: throws std::overflow_error if v does not
// fit To. For the header's and the catalog record's fixed-width fields.
template <typename To, typename From>
[[nodiscard]] constexpr auto narrow(From v) -> To {
  if (!std::in_range<To>(v)) throw std::overflow_error("value does not fit its field");
  return static_cast<To>(v);
}

// The stage-1 SQL types of docs/types_design.md. The values are
// the record's; never renumber one.
enum class SqlKind : std::uint8_t {
  SmallInt = 1,
  Integer,
  BigInt,
  Real,
  Double,
  Boolean,
  Bytea,
  Text,
  Varchar,
  Decimal,
  Timestamp,
  Date,
  Time,
};

// DECIMAL(p, s) is a scaled i64: p of at most 18 digits fits.
inline constexpr std::uint8_t kMaxDecimalPrecision = 18;

struct SqlType {
  SqlKind kind;
  std::uint32_t length{0};    // VARCHAR(n): n, in code points
  std::uint8_t precision{0};  // DECIMAL(p, s): p, 1..18
  std::uint8_t scale{0};      // DECIMAL(p, s): s, 0..p
  auto operator==(const SqlType&) const -> bool = default;
};

[[nodiscard]] auto phys_type(SqlType t) -> PhysType;
// The SQL type a column of only a physical type is declared as: BIGINT,
// DOUBLE PRECISION, BOOLEAN or BYTEA, none with a range narrower than its
// physical type. A result row's columns, and tests', state theirs with it.
[[nodiscard]] auto plain_sql_type(PhysType t) -> SqlType;

struct ColumnSlot {
  // The SQL type is required, and the physical type follows from it: a
  // layout cannot hold a column without the declared type its values are
  // checked against.
  ColumnSlot(std::uint32_t id, SqlType declared, bool null_ok, std::uint32_t at)
      : col_id{id}, type{phys_type(declared)}, nullable{null_ok}, pos{at}, sql{declared} {}

  std::uint32_t col_id;  // permanent; the bitmap bit
  PhysType type;
  bool nullable;
  std::uint32_t pos;  // fixed: byte offset in the row; variable: slot
  // The column's declared type, carried beside its slot: the runtime checks
  // a value written against it (BC_C_RANGE), and set_f64 rounds a REAL by it.
  SqlType sql;
};

// Where each column of one layout generation lives.
struct RowLayout {
  std::uint16_t generation{1};
  std::uint32_t bitmap_bytes{0};
  std::uint32_t fixed_start{0};
  std::uint32_t fixed_end{0};  // the fixed region is [fixed_start, fixed_end)
  std::uint32_t nvar{0};
  std::vector<ColumnSlot> columns;

  [[nodiscard]] auto find(std::uint32_t col_id) const -> const ColumnSlot*;
  // One past the largest column id the bitmap has a bit for.
  [[nodiscard]] auto id_ceiling() const noexcept -> std::uint32_t { return bitmap_bytes * 8; }
};

struct ColumnDef {
  ColumnDef(std::uint32_t id, SqlType declared, bool null_ok)
      : col_id{id}, type{phys_type(declared)}, nullable{null_ok}, sql{declared} {}

  std::uint32_t col_id;
  PhysType type;
  bool nullable;
  SqlType sql;
};

// Assigns positions in column order: fixed columns one word each after the
// bitmap, variable columns the next slot. Throws std::length_error if the
// bitmap or the fixed region would outgrow the header's 16-bit fields.
[[nodiscard]] auto make_layout(std::span<const ColumnDef> cols, std::uint16_t generation = 1)
    -> RowLayout;
// Appends columns to a layout, keeping every existing position (ADD COLUMN).
// False, and nothing appended, if a column id has no bit in the bitmap or
// the fixed region would outgrow the header's 16-bit var_table: the layout
// needs a REBUILD.
[[nodiscard]] auto append_columns(RowLayout& layout, std::span<const ColumnDef> cols) -> bool;
// Whether a layout keeps the format's invariants: the bitmap a multiple of 8
// bytes, every column id below its ceiling, fixed columns on word boundaries
// inside the fixed region, slots below nvar, the header's 16-bit fields, a
// known type. What a layout read from the catalog is checked against.
[[nodiscard]] auto layout_valid(const RowLayout& layout) -> bool;
// The layout of a plan's result rows: column i has id i and is nullable.
[[nodiscard]] auto make_result_layout(std::span<const PhysType> types) -> RowLayout;

// Encodes rows of one layout. Every column starts null.
class RowWriter {
public:
  explicit RowWriter(const RowLayout& layout);

  void reset();
  void copy_from(BytesView old);  // every column of old that this layout knows
  // false if col_id is not in the layout or has another type.
  [[nodiscard]] auto set_i64(std::uint32_t col_id, std::int64_t v) -> bool;
  [[nodiscard]] auto set_f64(std::uint32_t col_id, double v) -> bool;
  [[nodiscard]] auto set_bool(std::uint32_t col_id, bool v) -> bool;
  [[nodiscard]] auto set_bytes(std::uint32_t col_id, BytesView v) -> bool;
  [[nodiscard]] auto set_null(std::uint32_t col_id) -> bool;
  // The id of a non-nullable column that is null, if there is one.
  [[nodiscard]] auto missing_required() const -> std::optional<std::uint32_t>;
  // Appends the row to out; false, and nothing appended, if it exceeds
  // max_bytes or the layout is not valid.
  [[nodiscard]] auto finish(Bytes& out, std::size_t max_bytes = kMaxRowBytes) const -> bool;

private:
  auto set_word(std::uint32_t col_id, PhysType type, std::uint64_t word) -> bool;
  // The column's index in the layout, if it has one of `type` (any type when nullopt).
  [[nodiscard]] auto index_of(std::uint32_t col_id, std::optional<PhysType> type) const -> std::optional<std::size_t>;

  const RowLayout* layout_;
  std::vector<bool> present_;
  std::vector<std::uint64_t> fixed_;  // the column's word, by column index
  std::vector<Bytes> var_;
};

// Raw accessors on an encoded row. nullopt means the row is malformed, or the
// position is not a column position of any layout (inside the header, or not
// a word boundary).
[[nodiscard]] auto row_valid(BytesView row) -> bool;
// The header alone is consistent with the row's length: what every accessor
// checks before it reads. Constant time, unlike row_valid.
[[nodiscard]] auto row_header_valid(BytesView row) -> bool;
[[nodiscard]] auto row_generation(BytesView row) -> std::uint16_t;
// The bitmap bit. Clear for a malformed row or a bit past the bitmap.
[[nodiscard]] auto row_present_bit(BytesView row, std::uint32_t col_id) -> bool;
// The word at `off`; 0 if past the row's fixed region (the row predates the column).
[[nodiscard]] auto row_word(BytesView row, std::uint32_t off) -> std::optional<std::uint64_t>;
[[nodiscard]] auto row_i64(BytesView row, std::uint32_t off) -> std::optional<std::int64_t>;
[[nodiscard]] auto row_f64(BytesView row, std::uint32_t off) -> std::optional<double>;
[[nodiscard]] auto row_bool(BytesView row, std::uint32_t off) -> std::optional<bool>;  // any non-zero word is true
// Variable slot; empty if past the row's var_end table.
[[nodiscard]] auto row_bytes(BytesView row, std::uint32_t slot) -> std::optional<BytesView>;
// Whether `row`'s regions hold a column's slot: its fixed word before the
// var_end table, or its variable slot below nvar. A column the row predates
// is outside them and reads as 0 or empty; with its present bit set, the row
// is damaged.
[[nodiscard]] auto row_holds(BytesView row, const ColumnSlot& col) -> bool;
// Whether a column of a layout is null in `row`: its present bit is clear.
[[nodiscard]] auto row_is_null(BytesView row, const ColumnSlot& col) -> bool;
// Whether a column holds the same encoding in two rows of one layout: null
// in both, or the same word or bytes. Equal values with different encodings
// (-0.0 and 0.0) compare unequal. nullopt for a malformed row.
[[nodiscard]] auto row_column_equal(BytesView a, BytesView b, const ColumnSlot& col) -> std::optional<bool>;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kBitmapGranuleBits = 64;
constexpr std::uint32_t kMaxVarTable = 0xFFFF;

auto load_le(BytesView b, std::size_t at, std::size_t width) -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < width; ++i) v |= std::to_integer<std::uint64_t>(b[at + i]) << (8 * i);
  return v;
}

auto load_u16(BytesView b, std::size_t at) -> std::uint32_t { return static_cast<std::uint32_t>(load_le(b, at, 2)); }
auto load_u32(BytesView b, std::size_t at) -> std::uint32_t { return static_cast<std::uint32_t>(load_le(b, at, 4)); }

void store_le(Bytes& out, std::size_t at, std::uint64_t v, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }
}

struct Header {
  std::uint32_t bitmap_bytes;
  std::uint32_t var_table;
  std::uint32_t nvar;
  std::uint32_t row_bytes;
  [[nodiscard]] auto fixed_start() const -> std::uint32_t {
    return static_cast<std::uint32_t>(kRowHeaderBytes) + bitmap_bytes;
  }
  [[nodiscard]] auto var_data() const -> std::size_t { return std::size_t{var_table} + 4 * std::size_t{nvar}; }
};

// The header, if it is consistent with the row's length and its regions nest.
auto header(BytesView row) -> std::optional<Header> {
  if (row.size() < kRowHeaderBytes) return std::nullopt;
  Header h{load_u16(row, 2), load_u16(row, 4), load_u16(row, 6), load_u32(row, 8)};
  if (h.row_bytes != row.size() || h.bitmap_bytes % 8 != 0 || h.var_table < h.fixed_start() ||
      (h.var_table - h.fixed_start()) % kFixedColumnBytes != 0 || h.var_data() > h.row_bytes) {
    return std::nullopt;
  }
  return h;
}

auto bitmap_bytes_for(std::uint32_t id_ceiling) -> std::uint32_t {
  auto bits = ((id_ceiling + kBitmapSpareBits + kBitmapGranuleBits - 1) / kBitmapGranuleBits) * kBitmapGranuleBits;
  return bits / 8;
}

}  // namespace

auto RowLayout::find(std::uint32_t col_id) const -> const ColumnSlot* {
  auto it = std::ranges::find(columns, col_id, &ColumnSlot::col_id);
  return it == columns.end() ? nullptr : &*it;
}

auto make_layout(std::span<const ColumnDef> cols, std::uint16_t generation) -> RowLayout {
  RowLayout l;
  l.generation = generation;
  auto id_ceiling = std::uint32_t{0};
  for (const auto& c : cols) id_ceiling = std::max(id_ceiling, c.col_id + 1);
  l.bitmap_bytes = bitmap_bytes_for(id_ceiling);
  l.fixed_start = static_cast<std::uint32_t>(kRowHeaderBytes) + l.bitmap_bytes;
  // bitmap_bytes and var_table (at least fixed_start) are 16-bit header fields.
  if (l.fixed_start > kMaxVarTable) throw std::length_error("column ids too large for one row layout");
  l.fixed_end = l.fixed_start;
  // Every id is below the ceiling by construction; only the 16-bit bound on
  // the fixed region can refuse.
  if (!append_columns(l, cols)) throw std::length_error("too many columns for one row layout");
  return l;
}

auto append_columns(RowLayout& layout, std::span<const ColumnDef> cols) -> bool {
  auto fixed_end = layout.fixed_end;
  auto nvar = layout.nvar;
  for (const auto& c : cols) {
    if (c.col_id >= layout.id_ceiling()) return false;
    // The header stores var_table and nvar in 16 bits.
    if (is_fixed(c.type)) {
      if (fixed_end + kFixedColumnBytes > kMaxVarTable) return false;
      fixed_end += kFixedColumnBytes;
    } else {
      if (nvar + 1 > kMaxVarTable) return false;
      ++nvar;
    }
  }
  for (const auto& c : cols) {
    ColumnSlot s{c.col_id, c.sql, c.nullable, 0};
    if (is_fixed(c.type)) {
      s.pos = layout.fixed_end;
      layout.fixed_end += kFixedColumnBytes;
    } else {
      s.pos = layout.nvar++;
    }
    layout.columns.push_back(s);
  }
  return true;
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

auto plain_sql_type(PhysType t) -> SqlType {
  switch (t) {
    case PhysType::I64: return {SqlKind::BigInt};
    case PhysType::F64: return {SqlKind::Double};
    case PhysType::Bool: return {SqlKind::Boolean};
    case PhysType::Bytes: return {SqlKind::Bytea};
  }
  return {SqlKind::Bytea};
}

auto make_result_layout(std::span<const PhysType> types) -> RowLayout {
  std::vector<ColumnDef> defs;
  defs.reserve(types.size());
  for (std::uint32_t i = 0; i < types.size(); ++i) defs.emplace_back(i, plain_sql_type(types[i]), true);
  return make_layout(defs);
}

RowWriter::RowWriter(const RowLayout& layout)
    : layout_{&layout},
      present_(layout.columns.size(), false),
      fixed_(layout.columns.size(), 0),
      var_(layout.columns.size()) {}

void RowWriter::reset() {
  std::ranges::fill(present_, false);
  std::ranges::fill(fixed_, 0);
  for (auto& v : var_) v.clear();
}

void RowWriter::copy_from(BytesView old) {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    const auto& c = layout_->columns[i];
    if (row_is_null(old, c)) {
      present_[i] = false;
      continue;
    }
    present_[i] = true;
    if (is_fixed(c.type)) {
      fixed_[i] = row_word(old, c.pos).value_or(0);
    } else {
      auto v = row_bytes(old, c.pos).value_or(BytesView{});
      var_[i].assign(v.begin(), v.end());
    }
  }
}

auto RowWriter::index_of(std::uint32_t col_id, std::optional<PhysType> type) const -> std::optional<std::size_t> {
  const auto* c = layout_->find(col_id);
  if (c == nullptr || (type && c->type != *type)) return std::nullopt;
  return static_cast<std::size_t>(c - layout_->columns.data());
}

auto RowWriter::set_word(std::uint32_t col_id, PhysType type, std::uint64_t word) -> bool {
  auto i = index_of(col_id, type);
  if (!i) return false;
  present_[*i] = true;
  fixed_[*i] = word;
  return true;
}

auto RowWriter::set_i64(std::uint32_t col_id, std::int64_t v) -> bool {
  return set_word(col_id, PhysType::I64, std::bit_cast<std::uint64_t>(v));
}

auto RowWriter::set_f64(std::uint32_t col_id, double v) -> bool {
  // A REAL column stores its float rounding, as PostgreSQL stores a float4:
  // the one place a row's REAL value is rounded, defaults included.
  const auto* c = layout_->find(col_id);
  if (c != nullptr && c->sql.kind == SqlKind::Real) v = float_rounding(v);
  return set_word(col_id, PhysType::F64, std::bit_cast<std::uint64_t>(v));
}

auto RowWriter::set_bool(std::uint32_t col_id, bool v) -> bool {
  return set_word(col_id, PhysType::Bool, v ? 1 : 0);
}

auto RowWriter::set_bytes(std::uint32_t col_id, BytesView v) -> bool {
  auto i = index_of(col_id, PhysType::Bytes);
  if (!i) return false;
  present_[*i] = true;
  var_[*i].assign(v.begin(), v.end());
  return true;
}

auto RowWriter::set_null(std::uint32_t col_id) -> bool {
  auto i = index_of(col_id, std::nullopt);
  if (!i) return false;
  present_[*i] = false;
  fixed_[*i] = 0;
  var_[*i].clear();
  return true;
}

auto RowWriter::missing_required() const -> std::optional<std::uint32_t> {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    if (!present_[i] && !layout_->columns[i].nullable) return layout_->columns[i].col_id;
  }
  return std::nullopt;
}

auto RowWriter::finish(Bytes& out, std::size_t max_bytes) const -> bool {
  const auto& l = *layout_;
  auto var_bytes = std::size_t{0};
  for (const auto& v : var_) var_bytes += v.size();
  auto total = std::size_t{l.fixed_end} + 4 * std::size_t{l.nvar} + var_bytes;
  if (total > max_bytes || total > kMaxRowBytes) return false;

  // A layout that breaks the format's invariants is refused, never encoded
  // past the bitmap or the header's fields.
  if (!layout_valid(l)) return false;
  auto header_fields = std::array{narrow<std::uint16_t>(l.bitmap_bytes), narrow<std::uint16_t>(l.fixed_end),
                                  narrow<std::uint16_t>(l.nvar)};

  // Encoded in place at the end of out: no temporary row.
  auto at = out.size();
  out.resize(at + total, std::byte{0});
  store_le(out, at, l.generation, 2);
  for (std::size_t f = 0; f < header_fields.size(); ++f) store_le(out, at + 2 + 2 * f, header_fields[f], 2);
  store_le(out, at + 8, narrow<std::uint32_t>(total), 4);

  auto data = at + std::size_t{l.fixed_end} + 4 * std::size_t{l.nvar};
  for (std::size_t i = 0; i < l.columns.size(); ++i) {
    const auto& c = l.columns[i];
    if (present_[i]) out[at + kRowHeaderBytes + c.col_id / 8] |= static_cast<std::byte>(1u << (c.col_id % 8));
    if (is_fixed(c.type)) store_le(out, at + c.pos, fixed_[i], kFixedColumnBytes);
  }
  // Variable data in slot order; a slot no column holds (a dropped column) is empty.
  auto end = std::size_t{0};
  for (std::uint32_t s = 0; s < l.nvar; ++s) {
    for (std::size_t i = 0; i < l.columns.size(); ++i) {
      const auto& c = l.columns[i];
      if (is_fixed(c.type) || c.pos != s) continue;
      std::ranges::copy(var_[i], out.begin() + static_cast<std::ptrdiff_t>(data + end));
      end += var_[i].size();
      break;
    }
    store_le(out, at + l.fixed_end + 4 * std::size_t{s}, end, 4);
  }
  return true;
}

auto layout_valid(const RowLayout& l) -> bool {
  if (l.bitmap_bytes == 0 || l.bitmap_bytes % 8 != 0) return false;
  if (l.fixed_start != kRowHeaderBytes + l.bitmap_bytes || l.fixed_end < l.fixed_start) return false;
  if (l.fixed_end > kMaxVarTable || l.nvar > kMaxVarTable) return false;
  if ((l.fixed_end - l.fixed_start) % kFixedColumnBytes != 0) return false;
  for (const auto& c : l.columns) {
    if (c.col_id >= l.id_ceiling()) return false;
    // A type code no build knows (a damaged record) is out of the enum's range.
    auto code = static_cast<int>(c.type);
    if (code < static_cast<int>(PhysType::I64) || code > static_cast<int>(PhysType::Bytes)) return false;
    switch (c.type) {
      case PhysType::I64:
      case PhysType::F64:
      case PhysType::Bool:
        if (c.pos < l.fixed_start || c.pos + kFixedColumnBytes > l.fixed_end ||
            (c.pos - l.fixed_start) % kFixedColumnBytes != 0) {
          return false;
        }
        break;
      case PhysType::Bytes:
        if (c.pos >= l.nvar) return false;
        break;
    }
  }
  return true;
}

auto row_valid(BytesView row) -> bool {
  auto h = header(row);
  if (!h) return false;
  auto prev = std::uint32_t{0};
  for (std::uint32_t s = 0; s < h->nvar; ++s) {
    auto end = load_u32(row, h->var_table + 4 * std::size_t{s});
    if (end < prev || h->var_data() + end > h->row_bytes) return false;
    prev = end;
  }
  return true;
}

auto row_header_valid(BytesView row) -> bool { return header(row).has_value(); }

auto row_generation(BytesView row) -> std::uint16_t {
  return row.size() < 2 ? 0 : static_cast<std::uint16_t>(load_u16(row, 0));
}

auto row_present_bit(BytesView row, std::uint32_t col_id) -> bool {
  auto h = header(row);
  if (!h || col_id / 8 >= h->bitmap_bytes) return false;
  auto byte = kRowHeaderBytes + col_id / 8;
  return (std::to_integer<unsigned>(row[byte]) >> (col_id % 8) & 1u) != 0;
}

auto row_word(BytesView row, std::uint32_t off) -> std::optional<std::uint64_t> {
  auto h = header(row);
  if (!h || off < h->fixed_start() || (off - h->fixed_start()) % kFixedColumnBytes != 0) return std::nullopt;
  if (std::size_t{off} + kFixedColumnBytes > h->var_table) return 0;  // the row predates the column
  return load_le(row, off, kFixedColumnBytes);
}

auto row_i64(BytesView row, std::uint32_t off) -> std::optional<std::int64_t> {
  auto w = row_word(row, off);
  if (!w) return std::nullopt;
  return std::bit_cast<std::int64_t>(*w);
}

auto row_f64(BytesView row, std::uint32_t off) -> std::optional<double> {
  auto w = row_word(row, off);
  if (!w) return std::nullopt;
  return std::bit_cast<double>(*w);
}

auto row_bool(BytesView row, std::uint32_t off) -> std::optional<bool> {
  auto w = row_word(row, off);
  if (!w) return std::nullopt;
  return *w != 0;
}

auto row_bytes(BytesView row, std::uint32_t slot) -> std::optional<BytesView> {
  auto h = header(row);
  if (!h) return std::nullopt;
  if (slot >= h->nvar) return BytesView{};  // the row predates the column
  auto data = h->var_data();
  auto begin = slot == 0 ? 0u : load_u32(row, h->var_table + 4 * std::size_t{slot - 1});
  auto end = load_u32(row, h->var_table + 4 * std::size_t{slot});
  if (end < begin || data + end > h->row_bytes) return std::nullopt;
  return row.subspan(data + begin, end - begin);
}

auto row_holds(BytesView row, const ColumnSlot& col) -> bool {
  auto h = header(row);
  if (!h) return false;
  if (col.type == PhysType::Bytes) return col.pos < h->nvar;
  return col.pos >= h->fixed_start() && std::size_t{col.pos} + kFixedColumnBytes <= h->var_table;
}

auto row_is_null(BytesView row, const ColumnSlot& col) -> bool { return !row_present_bit(row, col.col_id); }

auto row_column_equal(BytesView a, BytesView b, const ColumnSlot& col) -> std::optional<bool> {
  auto null_a = row_is_null(a, col);
  auto null_b = row_is_null(b, col);
  if (null_a || null_b) return null_a == null_b;
  if (col.type == PhysType::Bytes) {
    auto x = row_bytes(a, col.pos);
    auto y = row_bytes(b, col.pos);
    if (!x || !y) return std::nullopt;
    return std::ranges::equal(*x, *y);
  }
  auto x = row_word(a, col.pos);
  auto y = row_word(b, col.pos);
  if (!x || !y) return std::nullopt;
  return *x == *y;
}

}  // namespace bytecask::sql
