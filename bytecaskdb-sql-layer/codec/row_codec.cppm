// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// row_codec.cppm — the row format of docs/plan_api_design.md, *Row format*:
//
//   u16   layout_generation
//   u16   row_bytes          ; the whole row
//   u16   var_table          ; prototype: offset of var_end, where the fixed region ends
//   u16   nvar               ; prototype: entries in var_end
//   u8[]  null_bitmap        ; bit set = null, indexed by permanent column id
//         fixed columns      ; at permanent offsets, little-endian, 8-byte aligned
//   u32[] var_end            ; end offsets, relative to the start of var data
//   u8[]  var data
//
// var_table and nvar are not in the design. Without them a reader cannot find
// var_end once a fixed column has been appended to the layout, so the
// short-row rule (a column the row predates reads as null) cannot hold for
// variable columns; see the write-up on #497. The bitmap's size is fixed when
// the layout is made, for the same reason.
//
// All integers are little-endian. Every accessor is bounds-checked against the
// row: a column past the end of a shorter row is null, a malformed row is
// reported, never read past. Definitions are in row_codec.cpp.

module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

export module bytecask.sql.row_codec;

export import bytecask.sql.key_codec;

export namespace bytecask::sql {

inline constexpr std::size_t kRowHeaderBytes = 8;
inline constexpr std::size_t kMaxRowBytes = 0xFFFF;

struct ColumnSlot {
  std::uint32_t col_id;  // permanent; the null bitmap bit
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
};

struct ColumnDef {
  std::uint32_t col_id;
  PhysType type;
  bool nullable;
};

// Assigns positions in column order: fixed columns 8 bytes each after the
// bitmap, variable columns the next slot. The bitmap holds at least 64 bits so
// columns can be appended without moving the fixed region.
[[nodiscard]] auto make_layout(std::span<const ColumnDef> cols, std::uint16_t generation = 1)
    -> RowLayout;
// Appends columns to a layout, keeping every existing position (ADD COLUMN).
// Returns false if a column id no longer fits in the bitmap.
auto append_columns(RowLayout& layout, std::span<const ColumnDef> cols) -> bool;

// Encodes rows of one layout. Every column starts null.
class RowWriter {
public:
  explicit RowWriter(const RowLayout& layout);

  void reset();
  void copy_from(BytesView old);  // every column of old that this layout knows
  // false if col_id is not in the layout or has another type.
  auto set_i64(std::uint32_t col_id, std::int64_t v) -> bool;
  auto set_bytes(std::uint32_t col_id, BytesView v) -> bool;
  auto set_null(std::uint32_t col_id) -> bool;
  [[nodiscard]] auto missing_required() const -> bool;  // a non-nullable column is null
  // Appends the row to out; false if it exceeds kMaxRowBytes.
  auto finish(Bytes& out) const -> bool;

private:
  const RowLayout* layout_;
  std::vector<bool> null_;
  std::vector<std::int64_t> fixed_;
  std::vector<Bytes> var_;
};

// Raw accessors on an encoded row. nullopt means the row is malformed.
[[nodiscard]] auto row_valid(BytesView row) -> bool;
[[nodiscard]] auto row_generation(BytesView row) -> std::uint16_t;
// The bitmap bit only. A malformed row, or a bit past the bitmap, is null.
[[nodiscard]] auto row_null_bit(BytesView row, std::uint32_t col_id) -> bool;
// Fixed column at `off`; 0 if past the row's fixed region.
[[nodiscard]] auto row_i64(BytesView row, std::uint32_t off) -> std::optional<std::int64_t>;
// Variable slot; empty if past the row's var_end table.
[[nodiscard]] auto row_bytes(BytesView row, std::uint32_t slot) -> std::optional<BytesView>;
// Whether a column of `layout` is null in `row`: its bit, or past the row.
[[nodiscard]] auto row_is_null(BytesView row, const ColumnSlot& col) -> bool;

}  // namespace bytecask::sql
