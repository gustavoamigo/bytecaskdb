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
// reported, never read past.

module;
#include <algorithm>
#include <bit>
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

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kMinBitmapBits = 64;

auto load_u16(BytesView b, std::size_t at) -> std::uint32_t {
  return std::to_integer<std::uint32_t>(b[at]) |
         (std::to_integer<std::uint32_t>(b[at + 1]) << 8);
}

auto load_u32(BytesView b, std::size_t at) -> std::uint32_t {
  return load_u16(b, at) | (load_u16(b, at + 2) << 16);
}

void store_le(Bytes& out, std::size_t at, std::uint64_t v, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }
}

auto is_fixed(PhysType t) -> bool { return t != PhysType::Bytes; }

struct Header {
  std::uint32_t row_bytes;
  std::uint32_t var_table;
  std::uint32_t nvar;
};

// The header, if it is consistent with the row's length.
auto header(BytesView row) -> std::optional<Header> {
  if (row.size() < kRowHeaderBytes) return std::nullopt;
  Header h{load_u16(row, 2), load_u16(row, 4), load_u16(row, 6)};
  if (h.row_bytes != row.size() || h.var_table < kRowHeaderBytes ||
      std::size_t{h.var_table} + 4 * std::size_t{h.nvar} > h.row_bytes) {
    return std::nullopt;
  }
  return h;
}

}  // namespace

auto RowLayout::find(std::uint32_t col_id) const -> const ColumnSlot* {
  auto it = std::ranges::find(columns, col_id, &ColumnSlot::col_id);
  return it == columns.end() ? nullptr : &*it;
}

auto make_layout(std::span<const ColumnDef> cols, std::uint16_t generation) -> RowLayout {
  RowLayout l;
  l.generation = generation;
  auto max_id = std::uint32_t{0};
  for (const auto& c : cols) max_id = std::max(max_id, c.col_id + 1);
  // A multiple of 64 bits keeps the fixed region 8-byte aligned.
  l.bitmap_bytes = std::max(kMinBitmapBits, (max_id + 63u) & ~63u) / 8;
  l.fixed_start = static_cast<std::uint32_t>(kRowHeaderBytes) + l.bitmap_bytes;
  l.fixed_end = l.fixed_start;
  (void)append_columns(l, cols);
  return l;
}

auto append_columns(RowLayout& layout, std::span<const ColumnDef> cols) -> bool {
  for (const auto& c : cols) {
    if (c.col_id >= layout.bitmap_bytes * 8) return false;
    ColumnSlot s{c.col_id, c.type, c.nullable, 0};
    if (is_fixed(c.type)) {
      s.pos = layout.fixed_end;
      layout.fixed_end += 8;
    } else {
      s.pos = layout.nvar++;
    }
    layout.columns.push_back(s);
  }
  return true;
}

RowWriter::RowWriter(const RowLayout& layout)
    : layout_{&layout},
      null_(layout.columns.size(), true),
      fixed_(layout.columns.size(), 0),
      var_(layout.columns.size()) {}

void RowWriter::reset() {
  std::ranges::fill(null_, true);
  std::ranges::fill(fixed_, 0);
  for (auto& v : var_) v.clear();
}

void RowWriter::copy_from(BytesView old) {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    const auto& c = layout_->columns[i];
    if (row_is_null(old, c)) {
      null_[i] = true;
      continue;
    }
    null_[i] = false;
    if (is_fixed(c.type)) {
      fixed_[i] = row_i64(old, c.pos).value_or(0);
    } else {
      auto v = row_bytes(old, c.pos).value_or(BytesView{});
      var_[i].assign(v.begin(), v.end());
    }
  }
}

auto RowWriter::set_i64(std::uint32_t col_id, std::int64_t v) -> bool {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    if (layout_->columns[i].col_id != col_id) continue;
    if (layout_->columns[i].type != PhysType::I64) return false;
    null_[i] = false;
    fixed_[i] = v;
    return true;
  }
  return false;
}

auto RowWriter::set_bytes(std::uint32_t col_id, BytesView v) -> bool {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    if (layout_->columns[i].col_id != col_id) continue;
    if (layout_->columns[i].type != PhysType::Bytes) return false;
    null_[i] = false;
    var_[i].assign(v.begin(), v.end());
    return true;
  }
  return false;
}

auto RowWriter::set_null(std::uint32_t col_id) -> bool {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    if (layout_->columns[i].col_id != col_id) continue;
    null_[i] = true;
    fixed_[i] = 0;
    var_[i].clear();
    return true;
  }
  return false;
}

auto RowWriter::missing_required() const -> bool {
  for (std::size_t i = 0; i < layout_->columns.size(); ++i) {
    if (null_[i] && !layout_->columns[i].nullable) return true;
  }
  return false;
}

auto RowWriter::finish(Bytes& out) const -> bool {
  const auto& l = *layout_;
  auto var_bytes = std::size_t{0};
  for (const auto& v : var_) var_bytes += v.size();
  auto total = std::size_t{l.fixed_end} + 4 * std::size_t{l.nvar} + var_bytes;
  if (total > kMaxRowBytes) return false;

  Bytes row(total, std::byte{0});
  store_le(row, 0, l.generation, 2);
  store_le(row, 2, total, 2);
  store_le(row, 4, l.fixed_end, 2);
  store_le(row, 6, l.nvar, 2);

  std::vector<std::uint32_t> var_len(l.nvar, 0);
  std::vector<std::size_t> var_src(l.nvar, 0);
  for (std::size_t i = 0; i < l.columns.size(); ++i) {
    const auto& c = l.columns[i];
    if (null_[i]) {
      auto byte = kRowHeaderBytes + c.col_id / 8;
      row[byte] |= static_cast<std::byte>(1u << (c.col_id % 8));
    }
    if (is_fixed(c.type)) {
      store_le(row, c.pos, std::bit_cast<std::uint64_t>(fixed_[i]), 8);
    } else {
      var_len[c.pos] = static_cast<std::uint32_t>(var_[i].size());
      var_src[c.pos] = i;
    }
  }
  auto data = std::size_t{l.fixed_end} + 4 * std::size_t{l.nvar};
  auto end = std::uint32_t{0};
  for (std::uint32_t s = 0; s < l.nvar; ++s) {
    const auto& v = var_[var_src[s]];
    std::ranges::copy(v, row.begin() + static_cast<std::ptrdiff_t>(data + end));
    end += var_len[s];
    store_le(row, l.fixed_end + 4 * std::size_t{s}, end, 4);
  }
  out.insert(out.end(), row.begin(), row.end());
  return true;
}

auto row_valid(BytesView row) -> bool {
  auto h = header(row);
  if (!h) return false;
  auto data = std::size_t{h->var_table} + 4 * std::size_t{h->nvar};
  auto prev = std::uint32_t{0};
  for (std::uint32_t s = 0; s < h->nvar; ++s) {
    auto end = load_u32(row, h->var_table + 4 * std::size_t{s});
    if (end < prev || data + end > h->row_bytes) return false;
    prev = end;
  }
  return true;
}

auto row_generation(BytesView row) -> std::uint16_t {
  return row.size() < 2 ? 0 : static_cast<std::uint16_t>(load_u16(row, 0));
}

auto row_null_bit(BytesView row, std::uint32_t col_id) -> bool {
  auto h = header(row);
  if (!h) return true;
  auto byte = kRowHeaderBytes + col_id / 8;
  if (byte >= h->var_table) return true;
  return (std::to_integer<unsigned>(row[byte]) >> (col_id % 8) & 1u) != 0;
}

auto row_i64(BytesView row, std::uint32_t off) -> std::optional<std::int64_t> {
  auto h = header(row);
  if (!h || off < kRowHeaderBytes) return std::nullopt;
  if (std::size_t{off} + 8 > h->var_table) return 0;  // the row predates the column
  auto lo = std::uint64_t{load_u32(row, off)};
  auto hi = std::uint64_t{load_u32(row, off + 4)};
  return std::bit_cast<std::int64_t>(lo | (hi << 32));
}

auto row_bytes(BytesView row, std::uint32_t slot) -> std::optional<BytesView> {
  auto h = header(row);
  if (!h) return std::nullopt;
  if (slot >= h->nvar) return BytesView{};  // the row predates the column
  auto data = std::size_t{h->var_table} + 4 * std::size_t{h->nvar};
  auto begin = slot == 0 ? 0u : load_u32(row, h->var_table + 4 * std::size_t{slot - 1});
  auto end = load_u32(row, h->var_table + 4 * std::size_t{slot});
  if (end < begin || data + end > h->row_bytes) return std::nullopt;
  return row.subspan(data + begin, end - begin);
}

auto row_is_null(BytesView row, const ColumnSlot& col) -> bool {
  auto h = header(row);
  if (!h) return true;
  if (is_fixed(col.type) ? std::size_t{col.pos} + 8 > h->var_table : col.pos >= h->nvar) {
    return true;
  }
  return row_null_bit(row, col.col_id);
}

}  // namespace bytecask::sql
