// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// row_codec.cppm — the row format of docs/sql_layer_types_design.md, *Row
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
// past. Definitions are in row_codec.cpp.

module;
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

struct ColumnSlot {
  std::uint32_t col_id;  // permanent; the bitmap bit
  PhysType type;
  bool nullable;
  std::uint32_t pos;  // fixed: byte offset in the row; variable: slot
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
  std::uint32_t col_id;
  PhysType type;
  bool nullable;
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
  [[nodiscard]] auto missing_required() const -> bool;  // a non-nullable column is null
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
// Whether a column of a layout is null in `row`: its present bit is clear.
[[nodiscard]] auto row_is_null(BytesView row, const ColumnSlot& col) -> bool;

}  // namespace bytecask::sql
