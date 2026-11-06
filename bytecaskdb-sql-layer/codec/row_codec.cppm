// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// row_codec.cppm — the row format of docs/types_design.md, *Row
// format*:
//
//   u16   layout_generation
//   u8    flags              ; bit 0: var_end entries are u32, else u16
//   u8    bitmap_bytes       ; size of the null bitmap
//   u16   var_table          ; offset of var_end: where the fixed region ends
//   u16   nvar               ; entries in var_end
//   u8[]  null_bitmap        ; bit set = present, indexed by permanent column id
//         fixed columns      ; each its declared width, at permanent offsets
//   u16[] or u32[] var_end   ; end offsets, relative to the start of var data
//   u8[]  var data
//
// The row's length is its record's, and its last var_end is that length
// less var data's offset. The bitmap says present: a column set
// null and a column the row predates both have the bit clear. The sizes in
// the header let a reader with an older layout's constants find every
// region. All integers are little-endian. One
// decoder, row_column, reads every column, bounds-checked against the row: a
// clear bit is absent (NULL), and a set bit for a column the row does not
// hold, or a header that does not parse, is malformed, never read as 0 or
// empty and never read past.

module;
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module bytecask.sql.row_codec;

export import bytecask.sql.key_codec;

export namespace bytecask::sql {

inline constexpr std::size_t kRowHeaderBytes = 8;
// The engine's hard ceiling on a value; its configured limit may be lower.
inline constexpr std::size_t kMaxRowBytes = (std::size_t{1} << 28) - 1;
// The bitmap is sized at create: the fewest bytes that hold every column id
// with kBitmapSpareBits to spare.
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
  // PostgreSQL's numeric of no declared precision, of any digits: AVG's
  // result. No column is declared one, so no record holds it; a result row
  // carries its value as its text (Rows), and its scale is the scale of
  // the integer sum it divides.
  Numeric,
  // TIMESTAMP WITH TIME ZONE: the same instant as Timestamp (microseconds
  // since 1970-01-01 00:00:00 UTC), told apart by this tag. Appended, so no
  // earlier value moves; a catalog that holds it is closed to an earlier build.
  TimestampTz,
  // CHAR(n): text of at most n code points, stored without its trailing
  // spaces (they are insignificant, as PostgreSQL's bpchar compares them)
  // and padded to n when a result is returned. Appended; a catalog that
  // holds it is closed to an earlier build.
  Char,
};

// DECIMAL(p, s) is a scaled i64: p of at most 18 digits fits.
inline constexpr std::uint8_t kMaxDecimalPrecision = 18;

struct SqlType {
  SqlKind kind;
  std::uint32_t length{0};    // VARCHAR(n), CHAR(n): n, in code points
  std::uint8_t precision{0};  // DECIMAL(p, s): p, 1..18
  std::uint8_t scale{0};      // DECIMAL(p, s): s, 0..p
  auto operator==(const SqlType&) const -> bool = default;
};

// The places after the point a type holds: a DECIMAL's scale, 0 for an integer.
[[nodiscard]] constexpr auto decimal_scale(SqlType t) -> std::uint8_t { return t.kind == SqlKind::Decimal ? t.scale : std::uint8_t{0}; }

[[nodiscard]] auto phys_type(SqlType t) -> PhysType;
// The bytes a column of this type takes in a row's fixed region: its
// declared width (1 for BOOLEAN, 2 for SMALLINT and DECIMAL(p <= 4), 4 for
// INTEGER, DATE, REAL and DECIMAL(p <= 9), 8 otherwise). 0 for a variable
// column. The row builder refuses a value outside the declared type, so a
// value always fits its width.
[[nodiscard]] auto fixed_width(SqlType t) -> std::uint32_t;
// The key part of a column of this type: its physical type and width, and
// whether it is a REAL. Integers are keyed at the declared width of SMALLINT
// (2), INTEGER and DATE (4), and at 8 bytes otherwise: a DECIMAL compared
// with a parameter is compared without its precision, so its key holds any
// i64.
[[nodiscard]] auto key_part_of(SqlType t, bool nullable, bool desc) -> KeyPart;
// The SQL type a column of only a physical type is declared as: BIGINT,
// DOUBLE PRECISION, BOOLEAN or BYTEA, none with a range narrower than its
// physical type. A result row's columns, and tests', state theirs with it.
[[nodiscard]] auto plain_sql_type(PhysType t) -> SqlType;

struct ColumnSlot {
  // The SQL type is required, and the physical type follows from it: a
  // layout cannot hold a column without the declared type its values are
  // checked against.
  ColumnSlot(std::uint32_t id, SqlType declared, bool null_ok, std::uint32_t at)
      : col_id{id}, type{phys_type(declared)}, nullable{null_ok}, pos{at}, width{fixed_width(declared)}, sql{declared} {}

  std::uint32_t col_id;  // permanent; the bitmap bit
  PhysType type;
  bool nullable;
  std::uint32_t pos;    // fixed: byte offset in the row; variable: slot
  std::uint32_t width;  // fixed: its bytes (fixed_width); variable: 0
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

// Assigns positions in column order: fixed columns packed at their widths
// after the bitmap, variable columns the next slot. Throws std::length_error
// if the bitmap would outgrow the header's 8-bit size or the fixed region
// its 16-bit var_table.
[[nodiscard]] auto make_layout(std::span<const ColumnDef> cols, std::uint16_t generation = 1)
    -> RowLayout;
// Appends columns to a layout, keeping every existing position (ADD COLUMN).
// False, and nothing appended, if a column id has no bit in the bitmap or
// the fixed region would outgrow the header's 16-bit var_table: the layout
// needs a REBUILD.
[[nodiscard]] auto append_columns(RowLayout& layout, std::span<const ColumnDef> cols) -> bool;
// Whether a layout keeps the format's invariants: a bitmap of 1 to 255
// bytes, every column id below its ceiling, fixed columns inside the fixed
// region and none overlapping another, slots below nvar and none used twice,
// the header's 16-bit fields, a known type. What a layout read from the
// catalog is checked against.
[[nodiscard]] auto layout_valid(const RowLayout& layout) -> bool;
// The layout of a plan's result rows: column i has id i and is nullable.
[[nodiscard]] auto make_result_layout(std::span<const PhysType> types) -> RowLayout;

// Encodes rows of one layout. Every column starts null.
class RowWriter {
public:
  explicit RowWriter(const RowLayout& layout);

  // Every column of old that this layout knows. False if one is malformed
  // (row_column): the writer is then partly copied, to be dropped or copied
  // into again.
  [[nodiscard]] auto copy_from(BytesView old) -> bool;
  // false if col_id is not in the layout or has another type; set_i64 also
  // if v does not fit the column's width (the caller checks the declared
  // range first, so a value is never truncated).
  [[nodiscard]] auto set_i64(std::uint32_t col_id, std::int64_t v) -> bool;
  [[nodiscard]] auto set_f64(std::uint32_t col_id, double v) -> bool;
  [[nodiscard]] auto set_bool(std::uint32_t col_id, bool v) -> bool;
  [[nodiscard]] auto set_bytes(std::uint32_t col_id, BytesView v) -> bool;
  [[nodiscard]] auto set_null(std::uint32_t col_id) -> bool;
  // The id of a non-nullable column that is null, if there is one.
  [[nodiscard]] auto missing_required() const -> std::optional<std::uint32_t>;
  // Appends the row to out; false, and nothing appended, if it exceeds
  // max_bytes or a column lies outside the layout's regions.
  [[nodiscard]] auto finish(Bytes& out, std::size_t max_bytes = kMaxRowBytes) const -> bool;

private:
  auto set_word(std::uint32_t col_id, PhysType type, std::uint64_t word) -> bool;
  // Sets the fixed column at layout index i to word: the store set_word and
  // set_f64 share once each has found the column.
  void put_word(std::size_t i, std::uint64_t word);
  // The column's index in the layout, if it has one of `type` (any type when nullopt).
  [[nodiscard]] auto index_of(std::uint32_t col_id, std::optional<PhysType> type) const -> std::optional<std::size_t>;

  const RowLayout* layout_;
  std::vector<bool> present_;
  // The column's value as ColumnRead::word holds it, by column index:
  // finish narrows it to the column's width.
  std::vector<std::uint64_t> fixed_;
  std::vector<Bytes> var_;
};

// Raw accessors on an encoded row.
[[nodiscard]] auto row_valid(BytesView row) -> bool;
// The header alone is consistent with the row's length: what every accessor
// checks before it reads. Constant time, unlike row_valid.
[[nodiscard]] auto row_header_valid(BytesView row) -> bool;
[[nodiscard]] auto row_generation(BytesView row) -> std::uint16_t;
// What a stored row holds for one column of its layout.
enum class ColumnState : std::uint8_t {
  Absent,     // its present bit is clear or past the bitmap: NULL, or a column the row predates
  Value,      // present, and inside the row
  Malformed,  // the header does not parse, or the bit is set and the column lies outside the
              // row's regions or its bytes run past the row
};
struct ColumnRead {
  ColumnState state{ColumnState::Malformed};
  // A fixed column's value widened to 64 bits, when Value: an integer
  // sign-extended, a REAL's float as a double's bits, a bool 0 or 1.
  std::uint64_t word{0};
  BytesView bytes;        // a variable column's bytes, when Value: a view into the row

  [[nodiscard]] auto i64() const -> std::int64_t { return std::bit_cast<std::int64_t>(word); }
  [[nodiscard]] auto f64() const -> double { return std::bit_cast<double>(word); }
  [[nodiscard]] auto boolean() const -> bool { return word != 0; }  // any non-zero word is true
};
// The one reader of a stored row's columns, named by id (the bitmap bit),
// physical type, position and width in the row's layout generation. Every reader
// of a column goes through it, so a present column the row does not hold is
// Malformed everywhere, never read as 0, empty or NULL. The bytes are a view
// into `row`: valid while the row is.
[[nodiscard]] auto row_column(BytesView row, std::uint32_t col_id, PhysType type, std::uint32_t pos,
                              std::uint32_t width) -> ColumnRead;
[[nodiscard]] inline auto row_column(BytesView row, const ColumnSlot& col) -> ColumnRead {
  return row_column(row, col.col_id, col.type, col.pos, col.width);
}
// Whether a column holds the same encoding in two rows of one layout: null
// in both, or the same word or bytes. Equal values with different encodings
// (-0.0 and 0.0) compare unequal. nullopt for a malformed row.
[[nodiscard]] auto row_column_equal(BytesView a, BytesView b, const ColumnSlot& col) -> std::optional<bool>;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kMaxBitmapBytes = 0xFF;
constexpr std::uint32_t kMaxVarTable = 0xFFFF;
constexpr std::uint8_t kWideVarEnd = 0x01;  // flags bit 0: var_end entries are u32

// A little-endian unsigned of W bytes: one fixed-size copy the compiler
// turns into a single load, where a loop over a runtime width is not.
template <typename U>
auto load_as(BytesView b, std::size_t at) -> std::uint64_t {
  U v;
  std::memcpy(&v, b.data() + at, sizeof v);
  if constexpr (std::endian::native == std::endian::big) v = std::byteswap(v);
  return v;
}

auto load_le(BytesView b, std::size_t at, std::size_t width) -> std::uint64_t {
  switch (width) {
    case 1: return std::to_integer<std::uint64_t>(b[at]);
    case 2: return load_as<std::uint16_t>(b, at);
    case 4: return load_as<std::uint32_t>(b, at);
    case 8: return load_as<std::uint64_t>(b, at);
  }
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < width; ++i) v |= std::to_integer<std::uint64_t>(b[at + i]) << (8 * i);
  return v;
}

auto load_u16(BytesView b, std::size_t at) -> std::uint32_t { return static_cast<std::uint32_t>(load_le(b, at, 2)); }

template <typename U>
void store_as(Bytes& out, std::size_t at, std::uint64_t v) {
  auto u = static_cast<U>(v);
  if constexpr (std::endian::native == std::endian::big) u = std::byteswap(u);
  std::memcpy(out.data() + at, &u, sizeof u);
}

// The low `width` bytes of v, little-endian: a fixed-size store for the
// widths a row holds, as load_le reads them.
void store_le(Bytes& out, std::size_t at, std::uint64_t v, std::size_t width) {
  switch (width) {
    case 1: out[at] = static_cast<std::byte>(v & 0xFF); return;
    case 2: store_as<std::uint16_t>(out, at, v); return;
    case 4: store_as<std::uint32_t>(out, at, v); return;
    case 8: store_as<std::uint64_t>(out, at, v); return;
  }
  for (std::size_t i = 0; i < width; ++i) {
    out[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }
}

struct Header {
  std::uint32_t bitmap_bytes;
  std::uint32_t var_table;
  std::uint32_t nvar;
  std::uint32_t end_bytes;  // 2 or 4: one var_end entry's
  std::size_t row_bytes;
  [[nodiscard]] auto fixed_start() const -> std::uint32_t {
    return static_cast<std::uint32_t>(kRowHeaderBytes) + bitmap_bytes;
  }
  [[nodiscard]] auto var_data() const -> std::size_t {
    return std::size_t{var_table} + std::size_t{end_bytes} * nvar;
  }
  [[nodiscard]] auto var_end(BytesView row, std::uint32_t slot) const -> std::size_t {
    return load_le(row, var_table + std::size_t{end_bytes} * slot, end_bytes);
  }
};

// The header, if it is consistent with the row's length and its regions nest.
auto header(BytesView row) -> std::optional<Header> {
  if (row.size() < kRowHeaderBytes) return std::nullopt;
  auto flags = std::to_integer<std::uint8_t>(row[2]);
  if ((flags & ~kWideVarEnd) != 0) return std::nullopt;  // a bit no build sets
  Header h{std::to_integer<std::uint32_t>(row[3]), load_u16(row, 4), load_u16(row, 6),
           (flags & kWideVarEnd) != 0 ? 4u : 2u, row.size()};
  if (h.bitmap_bytes == 0 || h.var_table < h.fixed_start() || h.var_data() > h.row_bytes) return std::nullopt;
  // The row ends where its variable data does: a row cut short, or one with
  // bytes past its end, is malformed, as a stored length would show.
  auto data_end = h.nvar == 0 ? std::size_t{0} : h.var_end(row, h.nvar - 1);
  if (h.var_data() + data_end != h.row_bytes) return std::nullopt;
  return h;
}

auto bitmap_bytes_for(std::uint32_t id_ceiling) -> std::uint32_t { return (id_ceiling + kBitmapSpareBits + 7) / 8; }

// Whether a fixed column's width is one its physical type is stored in.
auto width_valid(PhysType type, std::uint32_t width) -> bool {
  switch (type) {
    case PhysType::I64: return width == 2 || width == 4 || width == 8;
    case PhysType::F64: return width == 4 || width == 8;
    case PhysType::Bool: return width == 1;
    case PhysType::Bytes: return width == 0;
  }
  return false;
}

// A fixed column's stored bytes as ColumnRead::word holds them.
auto widen(PhysType type, std::uint64_t raw, std::uint32_t width) -> std::uint64_t {
  switch (type) {
    case PhysType::I64: {
      // Sign-extend from the width: flipping the top bit and subtracting
      // it, modulo 2^64, copies it into every bit above.
      auto sign = std::uint64_t{1} << (8 * width - 1);
      return (raw ^ sign) - sign;
    }
    case PhysType::F64:
      if (width == 4) {
        return std::bit_cast<std::uint64_t>(
            static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))));
      }
      return raw;
    case PhysType::Bool: return raw != 0 ? 1 : 0;  // any non-zero byte is true
    case PhysType::Bytes: return 0;
  }
  return 0;
}

// A word as a fixed column of `width` stores it: its low bytes, a REAL as
// its float's bits. Exact for every value set_* accepts.
auto narrow_word(PhysType type, std::uint64_t word, std::uint32_t width) -> std::uint64_t {
  if (type == PhysType::F64 && width == 4) {
    return std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<double>(word)));
  }
  return word;  // store_le writes the low `width` bytes
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
  if (l.bitmap_bytes > kMaxBitmapBytes) throw std::length_error("column ids too large for one row layout");
  l.fixed_start = static_cast<std::uint32_t>(kRowHeaderBytes) + l.bitmap_bytes;
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
      fixed_end += fixed_width(c.sql);
      if (fixed_end > kMaxVarTable) return false;
    } else {
      if (nvar + 1 > kMaxVarTable) return false;
      ++nvar;
    }
  }
  for (const auto& c : cols) {
    ColumnSlot s{c.col_id, c.sql, c.nullable, 0};
    if (is_fixed(c.type)) {
      s.pos = layout.fixed_end;
      layout.fixed_end += s.width;
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
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time: return PhysType::I64;
    case SqlKind::Real:
    case SqlKind::Double: return PhysType::F64;
    case SqlKind::Boolean: return PhysType::Bool;
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Numeric: return PhysType::Bytes;
  }
  return PhysType::Bytes;
}

auto fixed_width(SqlType t) -> std::uint32_t {
  switch (t.kind) {
    case SqlKind::Boolean: return 1;
    case SqlKind::SmallInt: return 2;
    case SqlKind::Integer:
    case SqlKind::Date:
    case SqlKind::Real: return 4;
    case SqlKind::Decimal: return t.precision <= 4 ? 2 : t.precision <= 9 ? 4 : 8;
    case SqlKind::BigInt:
    case SqlKind::Double:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Time: return 8;
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Numeric: return 0;
  }
  return 0;
}

auto key_part_of(SqlType t, bool nullable, bool desc) -> KeyPart {
  auto part = KeyPart{phys_type(t), nullable, desc, t.kind == SqlKind::Real};
  if (t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::Date) {
    part.width = static_cast<std::uint8_t>(fixed_width(t));
  }
  return part;
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


auto RowWriter::copy_from(BytesView old) -> bool {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    const auto& c = layout_->columns[i];
    auto v = row_column(old, c);
    switch (v.state) {
      case ColumnState::Malformed: return false;
      case ColumnState::Absent:
        present_[i] = false;  // finish writes nothing for it
        var_[i].clear();      // nor keeps its bytes, in a writer that is reused
        break;
      case ColumnState::Value:
        present_[i] = true;
        if (is_fixed(c.type)) {
          fixed_[i] = v.word;
        } else {
          var_[i].assign(v.bytes.begin(), v.bytes.end());
        }
        break;
    }
  }
  return true;
}

auto RowWriter::index_of(std::uint32_t col_id, std::optional<PhysType> type) const -> std::optional<std::size_t> {
  const auto* c = layout_->find(col_id);
  if (c == nullptr || (type && c->type != *type)) return std::nullopt;
  return static_cast<std::size_t>(c - layout_->columns.data());
}

void RowWriter::put_word(std::size_t i, std::uint64_t word) {
  present_[i] = true;
  fixed_[i] = word;
}

auto RowWriter::set_word(std::uint32_t col_id, PhysType type, std::uint64_t word) -> bool {
  auto i = index_of(col_id, type);
  if (!i) return false;
  put_word(*i, word);
  return true;
}

auto RowWriter::set_i64(std::uint32_t col_id, std::int64_t v) -> bool {
  auto i = index_of(col_id, PhysType::I64);
  if (!i) return false;
  // Never truncated: a value wider than the column's width is refused.
  if (!fits_width(v, layout_->columns[*i].width)) return false;
  put_word(*i, std::bit_cast<std::uint64_t>(v));
  return true;
}

auto RowWriter::set_f64(std::uint32_t col_id, double v) -> bool {
  // A REAL column stores its float rounding, as PostgreSQL stores a float4.
  // Every value written to a REAL column passes here. A DEFAULT, parsed as
  // a float by the binder, is already its own rounding and passes unchanged.
  auto i = index_of(col_id, PhysType::F64);
  if (!i) return false;
  if (layout_->columns[*i].sql.kind == SqlKind::Real) v = float_rounding(v);
  put_word(*i, std::bit_cast<std::uint64_t>(v));
  return true;
}

auto RowWriter::set_bool(std::uint32_t col_id, bool v) -> bool {
  return set_word(col_id, PhysType::Bool, v ? 1 : 0);
}

auto RowWriter::set_bytes(std::uint32_t col_id, BytesView v) -> bool {
  auto i = index_of(col_id, PhysType::Bytes);
  if (!i) return false;
  present_[*i] = true;
  // A CHAR(n) keeps no trailing spaces: every value stored passes here.
  if (layout_->columns[*i].sql.kind == SqlKind::Char) {
    while (!v.empty() && v.back() == std::byte{' '}) v = v.first(v.size() - 1);
  }
  var_[*i].assign(v.begin(), v.end());
  return true;
}

auto RowWriter::set_null(std::uint32_t col_id) -> bool {
  auto i = index_of(col_id, std::nullopt);
  if (!i) return false;
  present_[*i] = false;  // finish writes nothing for it
  var_[*i].clear();      // nor keeps its bytes, in a writer that is reused
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
  // The header's fields, and every column inside the region it is written
  // to: a layout that breaks them is refused, never encoded past them. The
  // catalog checks the rest of layout_valid when it loads a layout.
  if (l.bitmap_bytes == 0 || l.bitmap_bytes > kMaxBitmapBytes || l.fixed_start != kRowHeaderBytes + l.bitmap_bytes ||
      l.fixed_end < l.fixed_start || l.fixed_end > kMaxVarTable || l.nvar > kMaxVarTable) {
    return false;
  }
  // Only present columns are written: a column the writer holds a value for
  // but marks absent (a DEFAULT bc_row_begin set, then bc_row_set_null)
  // leaves its bytes zero and its slot empty.
  auto var_bytes = std::size_t{0};
  for (std::size_t i = 0; i < l.columns.size(); ++i) {
    const auto& c = l.columns[i];
    if (c.col_id >= l.id_ceiling()) return false;
    if (is_fixed(c.type)) {
      if (c.pos < l.fixed_start || std::size_t{c.pos} + c.width > l.fixed_end || !width_valid(c.type, c.width)) {
        return false;
      }
    } else if (c.pos >= l.nvar) {
      return false;
    } else if (present_[i]) {
      var_bytes += var_[i].size();
    }
  }
  // Two-byte var_end entries while every offset fits them, four past that.
  auto end_bytes = var_bytes <= 0xFFFF ? std::size_t{2} : std::size_t{4};
  auto total = std::size_t{l.fixed_end} + end_bytes * l.nvar + var_bytes;
  if (total > max_bytes || total > kMaxRowBytes) return false;

  // Encoded in place at the end of out: no temporary row.
  auto at = out.size();
  out.resize(at + total, std::byte{0});
  store_le(out, at, l.generation, 2);
  store_le(out, at + 2, end_bytes == 4 ? kWideVarEnd : 0, 1);
  store_le(out, at + 3, l.bitmap_bytes, 1);
  store_le(out, at + 4, l.fixed_end, 2);
  store_le(out, at + 6, l.nvar, 2);

  auto data = at + std::size_t{l.fixed_end} + end_bytes * l.nvar;
  for (std::size_t i = 0; i < l.columns.size(); ++i) {
    const auto& c = l.columns[i];
    if (!present_[i]) continue;
    out[at + kRowHeaderBytes + c.col_id / 8] |= static_cast<std::byte>(1u << (c.col_id % 8));
    if (is_fixed(c.type)) store_le(out, at + c.pos, narrow_word(c.type, fixed_[i], c.width), c.width);
  }
  // Variable data in slot order; a slot no column holds (a dropped column) is empty.
  auto end = std::size_t{0};
  for (std::uint32_t s = 0; s < l.nvar; ++s) {
    for (std::size_t i = 0; i < l.columns.size(); ++i) {
      const auto& c = l.columns[i];
      if (is_fixed(c.type) || c.pos != s) continue;
      if (present_[i]) {
        std::ranges::copy(var_[i], out.begin() + static_cast<std::ptrdiff_t>(data + end));
        end += var_[i].size();
      }
      break;
    }
    store_le(out, at + l.fixed_end + end_bytes * s, end, end_bytes);
  }
  return true;
}

auto layout_valid(const RowLayout& l) -> bool {
  if (l.bitmap_bytes == 0 || l.bitmap_bytes > kMaxBitmapBytes) return false;
  if (l.fixed_start != kRowHeaderBytes + l.bitmap_bytes || l.fixed_end < l.fixed_start) return false;
  if (l.fixed_end > kMaxVarTable || l.nvar > kMaxVarTable) return false;
  // Each byte of the fixed region, and each slot, belongs to one column at most.
  std::vector<bool> fixed_used(l.fixed_end - l.fixed_start, false);
  std::vector<bool> slot_used(l.nvar, false);
  for (const auto& c : l.columns) {
    if (c.col_id >= l.id_ceiling()) return false;
    // A type code no build knows (a damaged record) is out of the enum's range.
    auto code = static_cast<int>(c.type);
    if (code < static_cast<int>(PhysType::I64) || code > static_cast<int>(PhysType::Bytes)) return false;
    if (!width_valid(c.type, c.width)) return false;
    switch (c.type) {
      case PhysType::I64:
      case PhysType::F64:
      case PhysType::Bool:
        if (c.pos < l.fixed_start || std::size_t{c.pos} + c.width > l.fixed_end) return false;
        for (auto b = c.pos - l.fixed_start; b < c.pos - l.fixed_start + c.width; ++b) {
          if (fixed_used[b]) return false;
          fixed_used[b] = true;
        }
        break;
      case PhysType::Bytes:
        if (c.pos >= l.nvar || slot_used[c.pos]) return false;
        slot_used[c.pos] = true;
        break;
    }
  }
  return true;
}

auto row_valid(BytesView row) -> bool {
  auto h = header(row);
  if (!h) return false;
  auto prev = std::size_t{0};
  for (std::uint32_t s = 0; s < h->nvar; ++s) {
    auto end = h->var_end(row, s);
    if (end < prev || h->var_data() + end > h->row_bytes) return false;
    prev = end;
  }
  return true;
}

auto row_header_valid(BytesView row) -> bool { return header(row).has_value(); }

auto row_generation(BytesView row) -> std::uint16_t {
  return row.size() < 2 ? 0 : static_cast<std::uint16_t>(load_u16(row, 0));
}

namespace {
// row_column's two checks before it reads a value. The header, if the
// row's regions hold a present column; otherwise why not, Absent or
// Malformed.
auto locate(BytesView row, std::uint32_t col_id, PhysType type, std::uint32_t pos, std::uint32_t width)
    -> std::expected<Header, ColumnState> {
  auto h = header(row);
  if (!h) return std::unexpected(ColumnState::Malformed);
  if (col_id / 8 >= h->bitmap_bytes) return std::unexpected(ColumnState::Absent);  // the row predates the column
  auto bit = std::to_integer<unsigned>(row[kRowHeaderBytes + col_id / 8]) >> (col_id % 8) & 1u;
  if (bit == 0) return std::unexpected(ColumnState::Absent);
  // Present: the row must hold it. Outside its regions (a row that predates
  // the column, with the bit set) it is damaged.
  auto held = type == PhysType::Bytes
                  ? pos < h->nvar
                  : width_valid(type, width) && pos >= h->fixed_start() && std::size_t{pos} + width <= h->var_table;
  if (!held) return std::unexpected(ColumnState::Malformed);
  return *h;
}

// A held variable column's bytes, as offsets into the row: its two var_end
// entries, in order and inside the row; nullopt otherwise (Malformed).
auto var_bounds(BytesView row, const Header& h, std::uint32_t pos)
    -> std::optional<std::pair<std::size_t, std::size_t>> {
  auto data = h.var_data();
  auto begin = pos == 0 ? std::size_t{0} : h.var_end(row, pos - 1);
  auto end = h.var_end(row, pos);
  if (end < begin || data + end > h.row_bytes) return std::nullopt;
  return std::pair{data + begin, data + end};
}
}  // namespace

auto row_column(BytesView row, std::uint32_t col_id, PhysType type, std::uint32_t pos, std::uint32_t width)
    -> ColumnRead {
  auto h = locate(row, col_id, type, pos, width);
  if (!h) return {.state = h.error(), .word = 0, .bytes = {}};
  if (type != PhysType::Bytes) {
    return {.state = ColumnState::Value, .word = widen(type, load_le(row, pos, width), width), .bytes = {}};
  }
  auto b = var_bounds(row, *h, pos);
  if (!b) return {};
  return {.state = ColumnState::Value, .word = 0, .bytes = row.subspan(b->first, b->second - b->first)};
}

auto row_column_equal(BytesView a, BytesView b, const ColumnSlot& col) -> std::optional<bool> {
  auto x = row_column(a, col);
  auto y = row_column(b, col);
  if (x.state == ColumnState::Malformed || y.state == ColumnState::Malformed) return std::nullopt;
  if (x.state != y.state) return false;
  if (x.state == ColumnState::Absent) return true;
  if (col.type == PhysType::Bytes) return std::ranges::equal(x.bytes, y.bytes);
  return x.word == y.word;
}

}  // namespace bytecask::sql
