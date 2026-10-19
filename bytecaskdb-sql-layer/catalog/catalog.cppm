// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.cppm — tables, their permanent column ids, SQL types, defaults,
// primary keys, secondary indexes and layout generations, persisted under
// namespace 0x01 (docs/catalog_design.md).
//
// Written from scratch: the plugin's catalog is GPL-2.0 and this directory is
// MIT.
//
// Key space:
//   01 01 01        next object id (tables, indexes, layouts), u64 big-endian
//   01 01 02        catalog version, bumped by every DDL, u64 big-endian
//   01 02 <id:4>    table record (indexes and layouts included), append-only
//   01 03 <name>    name entry: kind(1) id(4, big-endian)
//
// A Catalog is immutable once built. LiveCatalog publishes a new one on each
// DDL; readers pin one with a snapshot that agrees with it. Definitions are
// in catalog.cpp.

module;
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The engine through its public header, as the plugin and the bindings link
// it; the sql-layer imports no engine module.
#include "include/bytecask.hpp"

export module bytecask.sql.catalog;

export import bytecask.sql.key_codec;
export import bytecask.sql.row_codec;

export namespace bytecask::sql {

// A value of one of the four physical types, or SQL NULL (monostate). The
// runtime's bc_value is this type.
using Datum = std::variant<std::monostate, std::int64_t, double, bool, Bytes>;

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

// Whether the fields a kind does not use are zero and the ones it uses in range.
[[nodiscard]] auto sql_type_valid(SqlType t) -> bool;
[[nodiscard]] auto phys_type(SqlType t) -> PhysType;
// As SQL writes it: "integer", "varchar(16)", "numeric(12,2)".
[[nodiscard]] auto sql_type_name(SqlType t) -> std::string;
// Whether a datum is NULL or of the physical type.
[[nodiscard]] auto datum_fits(const Datum& d, PhysType t) -> bool;
// Sets one column of a row writer to a datum (NULL sets it null). False for
// a type mismatch.
[[nodiscard]] auto set_datum(RowWriter& w, std::uint32_t col_id, const Datum& d) -> bool;

struct Column {
  std::uint32_t id;  // permanent; never reused
  std::string name;
  SqlType sql;
  PhysType type;  // phys_type(sql)
  bool nullable;
  Datum default_value;  // monostate: no default (NULL)
};

struct IndexPart {
  std::uint32_t col_id;
  bool desc{false};
};

// A secondary index: its parts, then the primary key's. A unique index has
// the same entries; its check is on the prefix of its parts.
struct Index {
  std::uint32_t id;
  std::string name;
  bool unique{false};
  std::vector<IndexPart> parts;
};

struct Table {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t version{1};  // bumped by every DDL on this table
  std::uint32_t next_col_id{0};
  std::vector<Column> columns;
  std::vector<std::uint32_t> pk_col_ids;
  std::vector<Index> indexes;
  std::uint32_t layout_id{0};  // the current generation's object id
  RowLayout layout;            // the current generation
  // Earlier generations, oldest first: empty until REBUILD (#503).
  std::vector<std::pair<std::uint32_t, RowLayout>> retired_layouts;

  [[nodiscard]] auto column(std::string_view name) const -> const Column*;
  [[nodiscard]] auto column(std::uint32_t id) const -> const Column*;
  [[nodiscard]] auto index(std::string_view name) const -> const Index*;
};

struct ColumnSpec {
  std::string name;
  SqlType sql;
  bool nullable{true};
  Datum default_value{};
};

struct IndexColumnSpec {
  std::string name;
  bool desc{false};
};

// What a key builder needs for a table (its primary key) or an index.
struct KeyLayout {
  std::uint8_t ns;
  const Table* table;
  const Index* index;          // nullptr for a table's primary key
  std::vector<KeyPart> parts;  // every part: index columns, then primary key
  std::size_t index_parts;     // how many of them precede the primary key
};

// The stage-1 DDL statements, as values: what LiveCatalog prepares.
struct CreateTableDdl {
  std::string name;
  std::vector<ColumnSpec> columns;
  std::vector<std::string> pk;
};
struct CreateIndexDdl {
  std::string name;
  std::string table;
  std::vector<IndexColumnSpec> columns;
  bool unique{false};
};
struct DropTableDdl {
  std::string name;
  bool if_exists{false};
};
struct DropIndexDdl {
  std::string name;
  bool if_exists{false};
};
struct AddColumnDdl {
  std::string table;
  ColumnSpec column;
};
using Ddl = std::variant<CreateTableDdl, CreateIndexDdl, DropTableDdl, DropIndexDdl, AddColumnDdl>;

class Catalog {
public:
  Catalog() = default;
  // Moving keeps every pointer a KeyLayout holds (map nodes do not move); a
  // copy would not, so there is none.
  Catalog(Catalog&&) = default;
  Catalog& operator=(Catalog&&) = default;
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;
  ~Catalog() = default;

  // Reads every record under namespace 0x01 from the snapshot. Throws
  // std::runtime_error for a record or name entry that does not parse.
  [[nodiscard]] static auto load(const Snapshot& snap) -> Catalog;

  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  [[nodiscard]] auto next_object_id() const noexcept -> std::uint64_t { return next_id_; }
  [[nodiscard]] auto tables() const -> const std::map<std::uint32_t, Table>& { return tables_; }
  [[nodiscard]] auto table(std::string_view name) const -> const Table*;
  [[nodiscard]] auto table(std::uint32_t id) const -> const Table*;
  // An index by name, with its table.
  [[nodiscard]] auto index(std::string_view name) const -> std::pair<const Table*, const Index*>;
  // A table or an index: both draw ids from one counter.
  [[nodiscard]] auto key_layout(std::uint32_t table_or_index) const -> const KeyLayout*;

  // Object ids stay at or below 2^24 - 1: bc_error_detail carries one in its
  // low 24 bits (and a plan's constants are an enum, whose values are ints).
  static constexpr std::uint32_t kMaxObjectId = 0x00FF'FFFFu;
  // 3: #489's record (SQL types, defaults, unique and DESC parts, layout
  // entries). 2 was #490's, 1 the prototype's; both are refused.
  static constexpr std::uint16_t kRecordFormat = 3;

  // The constants a plan on this table needs, named from ids alone so that
  // no table, index or column name has to be a C identifier and no two can
  // collide: TBL_<table>, IDX_<index>, COL_<table>_<column> (the column id),
  // and PART_<table or index>_<column> (the column's position in that key).
  // `object` is what the name stands for, for the plan's header comment.
  struct PlanConstant {
    std::string name;
    std::uint32_t value;
    std::string object;
  };
  [[nodiscard]] static auto constants(const Table& t) -> std::vector<PlanConstant>;

  // One operation of a DDL's batch, collected while the snapshot is read and
  // handed to a WritePlan on that snapshot afterwards.
  struct DdlOp {
    enum class Kind { Put, Del, DelRange, EnsureAbsent, EnsureUnchanged, EnsureRangeUnchanged };
    Kind kind;
    Bytes a, b;
  };

private:
  friend class LiveCatalog;

  // The version after `ddl`, with its batch in `ops`; nullptr for a no-op.
  // Gathers at most `backfill_limit` bytes of backfill (std::length_error
  // past it). Reads the rows a backfill needs from `snap`, which must agree with this
  // version. Sets `backfill` if the batch rewrites or indexes rows. Throws as
  // LiveCatalog::prepare does.
  [[nodiscard]] auto next_version(const Ddl& ddl, const Snapshot& snap, std::vector<DdlOp>& ops,
                                  bool& backfill, std::size_t backfill_limit) const -> std::shared_ptr<const Catalog>;

  // A copy of every table, with its own lookups.
  [[nodiscard]] auto clone() const -> Catalog;
  void put_table(Table t);
  void erase_table(std::uint32_t id);
  void rebuild_lookups();

  std::uint64_t next_id_{1};
  std::uint64_t version_{0};
  std::map<std::uint32_t, Table> tables_;
  std::map<std::string, std::uint32_t, std::less<>> names_;  // table or index name -> id
  std::map<std::uint32_t, KeyLayout> key_layouts_;
};

// The live catalog: the current version, published by each DDL, and the
// DDL statements themselves. Thread-safe. Holds the DB by reference; it must
// outlive this.
class LiveCatalog {
public:
  // Loads the catalog; throws as Catalog::load does.
  explicit LiveCatalog(DB& db);
  LiveCatalog(const LiveCatalog&) = delete;
  LiveCatalog& operator=(const LiveCatalog&) = delete;
  ~LiveCatalog() = default;

  [[nodiscard]] auto current() const -> std::shared_ptr<const Catalog>;

  // A catalog version and an engine snapshot that agree: no DDL committed
  // between them. While a DDL's commit is in flight it reads the snapshot's
  // catalog version, and waits only if the engine has published the DDL and
  // current() not yet.
  struct Pinned {
    std::shared_ptr<const Catalog> cat;
    Snapshot snap;
  };
  [[nodiscard]] auto pin() const -> Pinned;

  // A DDL built on a snapshot, not yet committed. A no-op (a drop IF EXISTS
  // of nothing) has no plan.
  struct Prepared {
    std::shared_ptr<const Catalog> base;
    std::shared_ptr<const Catalog> next;
    std::unique_ptr<WritePlan> plan;
    bool backfill{false};
  };
  // Throws std::invalid_argument for a statement the catalog refuses (a
  // duplicate name, an unknown column, a unique index over duplicates…) and
  // std::length_error when ids or a layout run out or a backfill passes the
  // engine's batch limit. Writes nothing.
  [[nodiscard]] auto prepare(const Ddl& ddl) -> Prepared;
  // Commits and publishes. False, with nothing written, on a conflict: a
  // DDL or a write to a backfilled table committed since the prepare.
  [[nodiscard]] auto commit(Prepared p) -> bool;
  // prepare and commit, retried on a conflict kDdlAttempts times in all.
  // Returns the version it published; throws as prepare does, or
  // std::runtime_error when every attempt conflicted.
  auto apply(const Ddl& ddl) -> std::shared_ptr<const Catalog>;

  static constexpr int kDdlAttempts = 3;

  // Test seam: called during every commit after the generation counter
  // turns odd and before the batch reaches the engine, under the DDL lock.
  // Lets a test hold a DDL mid-commit. Not thread-safe to set while DDL runs.
  void set_commit_hook_for_testing(std::function<void()> hook) { commit_hook_ = std::move(hook); }
  // Test seam: the bytes a backfill may gather (the engine's 1 GiB batch
  // limit otherwise), so a test reaches the refusal with a small table.
  void set_backfill_limit_for_testing(std::size_t bytes) { backfill_limit_ = bytes; }
  static constexpr std::size_t kMaxBackfillBytes = std::size_t{1} << 30;

private:
  DB& db_;
  // Readers load it without a lock; a DDL stores the next version.
  mutable std::atomic<std::shared_ptr<const Catalog>> current_;  // pin() may catch up with another catalog
  [[nodiscard]] auto commit_locked(Prepared p) -> bool;

  std::mutex ddl_mu_;  // one DDL at a time: apply holds it from prepare to publication
  std::function<void()> commit_hook_;
  std::size_t backfill_limit_{kMaxBackfillBytes};
  // Odd from just before a DDL's batch reaches the engine until current_
  // is swapped (or the commit fails).
  mutable std::atomic<std::uint64_t> gen_{0};
};

// The catalog record's key and a table's row-key range: for tests and tools.
[[nodiscard]] auto table_record_key(std::uint32_t table_id) -> Bytes;
[[nodiscard]] auto name_entry_key(std::string_view name) -> Bytes;
// Serializes a table as kRecordFormat; parses one, throwing
// std::runtime_error if it does not parse or breaks an invariant.
[[nodiscard]] auto serialize_table(const Table& t) -> Bytes;
[[nodiscard]] auto parse_table(BytesView record) -> Table;

// Encodes the primary key of `t` from a row: kNsRow, the table id, the parts.
[[nodiscard]] auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool;
// Encodes the index entry of row: kNsIndex, the index id, the parts, the
// primary key. `prefix_len`, if given, receives the length of the index-part
// prefix, and `has_null` whether any index part was null.
[[nodiscard]] auto encode_index_key_from_row(const Table& t, const Index& ix, BytesView row, Bytes& out,
                                             std::size_t* prefix_len = nullptr, bool* has_null = nullptr) -> bool;

}  // namespace bytecask::sql
