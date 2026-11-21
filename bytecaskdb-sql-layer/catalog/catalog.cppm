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
//   01 03 <name>    name entry: kind(1) id(4, big-endian); outside public,
//                   01 03 00 <schema:4> <name> (docs/catalog_design.md, *Schemas*)
//   01 0D <id:4>    schema record; 01 0E <name> its name entry
//
// A Catalog is immutable once built. LiveCatalog publishes a new one on each
// DDL; readers pin one with a snapshot that agrees with it.

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The engine through its public header, as the plugin and the bindings link
// it; the sql-layer imports no engine module.
#include "include/bytecask.hpp"
// The bc_constraint codes a ConstraintDetail carries.
#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

export module bytecask.sql.catalog;

export import bytecask.sql.key_codec;
export import bytecask.sql.sqlstate;
export import bytecask.sql.names;
export import bytecask.sql.row_codec;
import bytecask.sql.jsonb;

export namespace bytecask::sql {

// A BC_E_CONSTRAINT detail (bc_error_detail): the bc_constraint in the high
// byte, the object id in the low 24 bits. The one encoding of the detail and
// the one mapping from constraint codes to names and text, for the plan
// API's error and the embedded API's exception.
struct ConstraintDetail {
  static constexpr std::uint32_t kMaxObjectId = 0x00FF'FFFFu;

  std::uint32_t constraint;  // a bc_constraint
  std::uint32_t object_id;

  [[nodiscard]] static constexpr auto decode(std::uint32_t detail) -> ConstraintDetail {
    return {detail >> 24, detail & kMaxObjectId};
  }
  // Throws std::out_of_range for a detail that is not valid(): a kind
  // bc_plan.h does not define, or an id wider than its 24 bits.
  [[nodiscard]] auto encode() const -> std::uint32_t {
    if (!valid()) throw std::out_of_range("constraint kind unknown or object id past 2^24");
    return (constraint << 24) | object_id;
  }
  // "unique", "not_null", "check", "foreign_key", "range" or "check_option"; empty for a
  // code bc_plan.h does not define. The one list of known kinds.
  [[nodiscard]] auto name() const -> std::string_view;
  // A kind bc_plan.h defines, and an id that fits its 24 bits: what
  // encode() takes and bc_raise accepts.
  [[nodiscard]] auto valid() const -> bool { return !name().empty() && object_id <= kMaxObjectId; }
  [[nodiscard]] auto message() const -> std::string;
};

// A DDL whose existing data breaks a constraint: CREATE UNIQUE INDEX over
// rows that hold a duplicate (BC_C_UNIQUE with the table's id: the index's
// id was never published, and the next DDL takes it). The data's
// refusal, not the statement's (that is DdlRefused): SQL reports the
// constraint's SQLSTATE, 23505. Nothing was written.
class ConstraintViolation : public std::runtime_error {
public:
  ConstraintViolation(const std::string& what, ConstraintDetail detail) : std::runtime_error{what}, detail_{detail} {}
  ConstraintViolation(const ConstraintViolation&) = default;
  ConstraintViolation& operator=(const ConstraintViolation&) = default;
  ~ConstraintViolation() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto detail() const noexcept -> ConstraintDetail { return detail_; }

private:
  ConstraintDetail detail_;
};

// A DDL statement the catalog refuses before it writes anything: a name
// taken or missing, a column or key part that does not exist or repeats, a
// default not of its column's type. The refusals PostgreSQL makes of the
// statement itself; the engine's own refusals stay std::invalid_argument and
// std::length_error. Each carries the SQLSTATE PostgreSQL gives the same
// refusal; the API's sqlstate() reports it.
class DdlRefused : public std::invalid_argument {
public:
  explicit DdlRefused(const std::string& what, SqlState sqlstate = kSyntaxError)
      : std::invalid_argument{what}, sqlstate_{sqlstate} {}
  DdlRefused(const DdlRefused&) = default;
  DdlRefused& operator=(const DdlRefused&) = default;
  ~DdlRefused() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto sqlstate() const noexcept -> std::string_view { return sqlstate_.view(); }
  // PostgreSQL's DETAIL and HINT for the refusal, empty where it has none.
  // Shared, so copying the error cannot throw.
  [[nodiscard]] auto detail() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->first} : ""; }
  [[nodiscard]] auto hint() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->second} : ""; }
  [[nodiscard]] auto with_notes(std::string detail, std::string hint) && -> DdlRefused&& {
    notes_ = std::make_shared<const std::pair<std::string, std::string>>(std::move(detail), std::move(hint));
    return std::move(*this);
  }

private:
  SqlState sqlstate_;
  std::shared_ptr<const std::pair<std::string, std::string>> notes_;
};

// A DDL that found another DDL of the same catalog in progress, and so did
// nothing rather than wait for it: retryable, SQLSTATE 40001 at the API.
class DdlInProgress : public std::runtime_error {
public:
  DdlInProgress() : std::runtime_error{"could not serialize access: another DDL is in progress"} {}
  DdlInProgress(const DdlInProgress&) = default;
  DdlInProgress& operator=(const DdlInProgress&) = default;
  ~DdlInProgress() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// A backfill that passed the bytes it may gather (Catalog::next_version's
// backfill_limit): the engine's 1 GiB batch in autocommit, the transaction's
// bound in a transaction. Its table is the one being rewritten or indexed.
class BackfillTooLarge : public std::length_error {
public:
  BackfillTooLarge(const std::string& what, std::string table) : std::length_error{what}, table_{std::move(table)} {}
  BackfillTooLarge(const BackfillTooLarge&) = default;
  BackfillTooLarge& operator=(const BackfillTooLarge&) = default;
  ~BackfillTooLarge() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto table() const noexcept -> const std::string& { return table_; }

private:
  std::string table_;
};

// A commit whose batch failed and left the engine degraded, after the batch
// may have reached the data file: whether it took effect is unknown, never
// "nothing written". SQLSTATE 08007 at the API.
// It holds nothing allocated, and its message is a static string: neither
// making nor throwing it can fail for want of memory, so the outcome cannot
// be lost to a second failure.
class CommitOutcomeUnknown : public std::exception {
public:
  CommitOutcomeUnknown() = default;
  CommitOutcomeUnknown(const CommitOutcomeUnknown&) = default;
  CommitOutcomeUnknown& operator=(const CommitOutcomeUnknown&) = default;
  ~CommitOutcomeUnknown() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto what() const noexcept -> const char* override;
};

// Every commit of the layer, DDL, autocommit and transactions alike, goes
// through this one rule: the batch is applied, and a failure after which the
// engine is degraded, when it was not before, may have come after the batch
// reached the data file, so it is thrown as CommitOutcomeUnknown, which
// allocates nothing. Any other failure is rethrown as it is, having written
// nothing. Another writer's failure in between also reads as unknown: the
// safe direction. A resume() between the two checks, by an embedder holding
// the DB (this layer never calls it), reads an unknown outcome as an
// ordinary failure: the unsafe one. Exact reporting, which closes both,
// needs the engine to say whether this batch was appended (#531).
[[nodiscard]] auto apply_batch_or_unknown(DB& db, WriteOptions opts, WritePlan plan) -> std::optional<CommitResult>;

// A value of one of the four physical types, or SQL NULL (monostate). The
// runtime's bc_value is this type.
using Datum = std::variant<std::monostate, std::int64_t, double, bool, Bytes>;

// TIME is microseconds since midnight, in [0, kMicrosPerDay]: 24:00:00 is a
// TIME, as in PostgreSQL.
inline constexpr std::int64_t kMicrosPerDay = 86'400'000'000;
// TIME WITH TIME ZONE is a TIME and a UTC offset in seconds east of UTC,
// within PostgreSQL's bound of +-15:59:59. Its value is one i64,
//   (time - offset seconds) * kTimeTzZones + (kMaxTimeTzOffset - offset),
// the UTC time first, then the offset, highest first: so integer order is
// PostgreSQL's timetz_cmp (the UTC time, then its zone, seconds west), and
// two values are equal only when both their time and their offset are.
// The value is never more than 2^55 in magnitude.
inline constexpr std::int64_t kMaxTimeTzOffset = 15 * 3600 + 59 * 60 + 59;
inline constexpr std::int64_t kTimeTzZones = std::int64_t{1} << 17;  // > 2 * kMaxTimeTzOffset + 1
struct TimeTz {
  std::int64_t time;    // microseconds since midnight, 0 to kMicrosPerDay
  std::int64_t offset;  // seconds east of UTC, within kMaxTimeTzOffset
};
[[nodiscard]] constexpr auto timetz_pack(TimeTz t) -> std::int64_t {
  return (t.time - t.offset * 1'000'000) * kTimeTzZones + (kMaxTimeTzOffset - t.offset);
}
[[nodiscard]] constexpr auto timetz_unpack(std::int64_t v) -> TimeTz {
  auto zone = v % kTimeTzZones;
  if (zone < 0) zone += kTimeTzZones;
  auto utc = (v - zone) / kTimeTzZones;
  auto offset = kMaxTimeTzOffset - zone;
  return {utc + offset * 1'000'000, offset};
}
// Whether v is a value timetz_pack writes.
[[nodiscard]] constexpr auto timetz_valid(std::int64_t v) -> bool {
  constexpr auto kLimit = (kMicrosPerDay + kMaxTimeTzOffset * 1'000'000 + 1) * kTimeTzZones;
  if (v <= -kLimit || v >= kLimit) return false;
  auto t = timetz_unpack(v);
  return t.time >= 0 && t.time <= kMicrosPerDay && t.offset >= -kMaxTimeTzOffset && t.offset <= kMaxTimeTzOffset;
}
static_assert(timetz_unpack(timetz_pack({kMicrosPerDay, -kMaxTimeTzOffset})).offset == -kMaxTimeTzOffset);
static_assert(timetz_unpack(timetz_pack({0, kMaxTimeTzOffset})).time == 0);
static_assert(timetz_pack({12 * 3'600'000'000LL, 3600}) < timetz_pack({11 * 3'600'000'000LL, 0}));
// PostgreSQL's DATE and TIMESTAMP ranges, in this layer's epoch (1970-01-01):
// days in [kMinDate, kEndDate), from 4714-11-24 BC to 5874898-01-01; and
// microseconds from kMinTimestamp (4714-11-24 BC 00:00). PostgreSQL's
// TIMESTAMP ends after 294276 AD, past what an i64 from 1970 reaches, so
// every larger value is a TIMESTAMP.
inline constexpr std::int64_t kMinDate = -2'440'588;
inline constexpr std::int64_t kEndDate = 2'145'042'906;
inline constexpr std::int64_t kMinTimestamp = -210'866'803'200'000'000;
// 10^0 .. 10^kMaxDecimalPrecision: DECIMAL(p, s)'s bound on its scaled value.
inline constexpr auto kPowersOfTen = [] {
  std::array<std::int64_t, kMaxDecimalPrecision + 1> p{};
  p[0] = 1;
  for (std::size_t i = 1; i < p.size(); ++i) p[i] = p[i - 1] * 10;
  return p;
}();

// Whether the fields a kind does not use are zero and the ones it uses in range.
[[nodiscard]] auto sql_type_valid(SqlType t) -> bool;
// As SQL writes it: "integer", "varchar(16)", "numeric(12,2)".
[[nodiscard]] auto sql_type_name(SqlType t) -> std::string;
// Whether a datum is NULL or of the physical type.
[[nodiscard]] auto datum_fits(const Datum& d, PhysType t) -> bool;
// The code points of well-formed UTF-8 with no NUL, or nullopt: a
// truncated or overlong sequence, a surrogate, one past U+10FFFF, a stray
// continuation byte, or a NUL, which PostgreSQL's text refuses too. One pass:
// the check of TEXT and VARCHAR values, and VARCHAR(n)'s length.
[[nodiscard]] auto utf8_code_points(BytesView s) -> std::optional<std::size_t>;
// What a parameter compared with a column of type t is, in two parts from
// one place so they cannot drift apart:
// - range: t without its modifier (VARCHAR(n) as TEXT, DECIMAL(p, s) as an
//   unbounded integer), as PostgreSQL types a $n by its column. A key
//   value, a lookup's or a bound's, is checked against it before the plan
//   runs: the type's range, not the modifier's.
// - identity: what two comparisons of one $n must share. range, except that
//   a DECIMAL keeps its scale, which says what number one scaled integer
//   is; at scale 0 it is that integer, a BIGINT.
struct ComparedType {
  SqlType identity;
  SqlType range;
};
[[nodiscard]] auto compared_type(SqlType t) -> ComparedType;
// Why a value of the column's physical type is outside its declared type,
// in PostgreSQL's words, or nullopt: a SMALLINT or INTEGER out of range, a
// REAL that overflows or underflows float, a TEXT or VARCHAR that is not
// UTF-8 (22021), a VARCHAR(n) of more than n code points, a DECIMAL(p, s) of
// more than p digits, a DATE, TIMESTAMP or TIME outside PostgreSQL's range
// for it. The one check of a declared
// range, for a value written (BC_C_RANGE) and a DEFAULT.
[[nodiscard]] auto out_of_range(SqlType t, std::int64_t v) -> std::optional<std::string>;
[[nodiscard]] auto out_of_range(SqlType t, double v) -> std::optional<std::string>;
[[nodiscard]] auto out_of_range(SqlType t, BytesView v) -> std::optional<std::string>;
[[nodiscard]] inline auto out_of_range(SqlType, bool) -> std::optional<std::string> { return std::nullopt; }
[[nodiscard]] auto out_of_range(SqlType t, const Datum& d) -> std::optional<std::string>;
// The SQLSTATE PostgreSQL reports for an out_of_range message, found by how
// it starts (a message may go on: "… for parameter $2"): 22003, 22001,
// 22021 or 22008. Empty for a message out_of_range never writes.
[[nodiscard]] auto range_sqlstate(std::string_view message) noexcept -> std::string_view;
// Every message start range_sqlstate knows, with its SQLSTATE: a test makes
// out_of_range write each one.
struct RangeMessage {
  std::string_view start;
  std::string_view sqlstate;
};
[[nodiscard]] auto range_messages() noexcept -> std::span<const RangeMessage>;
// Sets one column of a row writer to a datum (NULL sets it null). False for
// a type mismatch.
[[nodiscard]] auto set_datum(RowWriter& w, std::uint32_t col_id, const Datum& d) -> bool;

// A DEFAULT of the clock (CURRENT_DATE, CURRENT_TIMESTAMP and now(),
// LOCALTIME, LOCALTIMESTAMP, CURRENT_TIME), its precision 0 to 6, or of a
// session function (CURRENT_USER, CURRENT_ROLE and USER; SESSION_USER;
// CURRENT_CATALOG; CURRENT_SCHEMA), a NAME, its precision unused. Each
// statement reads the clock and the session once; the binder gives a row
// that leaves the column out, or sets it to DEFAULT, that reading, so the
// column's stored default stays NULL.
struct ClockDefault {
  enum class Fn : std::uint8_t {
    CurrentDate = 1, CurrentTimestamp, LocalTime, LocalTimestamp,
    CurrentUser, SessionUser, CurrentCatalog, CurrentSchema,
    CurrentTime,
  };
  static constexpr std::uint8_t kLastFn = static_cast<std::uint8_t>(Fn::CurrentTime);
  Fn fn{Fn::CurrentDate};
  std::uint8_t precision{6};

  friend auto operator==(const ClockDefault&, const ClockDefault&) -> bool = default;
};

// Whether `fn` is a session function, a NAME, not the clock.
[[nodiscard]] constexpr auto is_session_fn(ClockDefault::Fn fn) -> bool {
  using F = ClockDefault::Fn;
  return fn == F::CurrentUser || fn == F::SessionUser || fn == F::CurrentCatalog || fn == F::CurrentSchema;
}

// Whether a column of `kind` takes `fn`'s value by PostgreSQL's assignment
// cast, and the layer computes it from the statement's reading: a date the
// date of a timestamp's; a time the time of any reading but CURRENT_DATE; a
// time with time zone CURRENT_TIME's, LOCALTIME's at the session's zone or
// CURRENT_TIMESTAMP's; a timestamp either timestamp. (A date to a timestamp
// is midnight, not the reading: not taken.) A session function's NAME goes
// to a string column of any kind.
[[nodiscard]] constexpr auto clock_assignable(ClockDefault::Fn fn, SqlKind kind) -> bool {
  using F = ClockDefault::Fn;
  if (is_session_fn(fn)) {
    return kind == SqlKind::Text || kind == SqlKind::Varchar || kind == SqlKind::Char || kind == SqlKind::Name;
  }
  switch (kind) {
    case SqlKind::Date: return fn == F::CurrentDate || fn == F::CurrentTimestamp || fn == F::LocalTimestamp;
    case SqlKind::Time: return fn != F::CurrentDate;
    case SqlKind::TimeTz: return fn == F::CurrentTime || fn == F::LocalTime || fn == F::CurrentTimestamp;
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz: return fn == F::CurrentTimestamp || fn == F::LocalTimestamp;
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Decimal:
    case SqlKind::Numeric:
    case SqlKind::Boolean:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Name:
    case SqlKind::Bytea:
    case SqlKind::Jsonb: return false;
  }
  return false;
}

struct Column {
  std::uint32_t id;  // permanent; never reused
  std::string name;
  SqlType sql;
  PhysType type;  // phys_type(sql)
  bool nullable;
  Datum default_value;  // monostate: no default (NULL)
  // The row id of a table declared without a primary key: its primary key,
  // a BIGINT the runtime fills (LiveCatalog::next_row_id). No name resolves
  // to it, and no statement names, sets or returns it.
  bool hidden{false};
  std::optional<ClockDefault> clock_default;  // default_value is then NULL
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

// A CHECK constraint: its name and the text of its expression, bound again
// at each INSERT and UPDATE of the table (the text is the definition, as a
// view's is).
struct CheckConstraint {
  std::string name;
  std::string expr;
};

struct Table {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t schema{kPublicSchema};  // its indexes' too
  std::uint32_t version{1};  // bumped by every DDL on this table
  std::uint32_t next_col_id{0};
  std::vector<Column> columns;
  std::vector<std::uint32_t> pk_col_ids;
  std::vector<Index> indexes;
  std::vector<CheckConstraint> checks;
  std::uint32_t layout_id{0};  // the current generation's object id
  RowLayout layout;            // the current generation
  // Earlier generations, oldest first: empty until REBUILD (#503).
  std::vector<std::pair<std::uint32_t, RowLayout>> retired_layouts;
  // A table a query reads more than once is read as one instance of it per
  // use: a copy under the name the query gives the use, with an id of its own
  // at or above kInstanceBase, and `source` the id of the table whose rows it
  // reads. 0 for a table of the catalog (docs/planner_design.md, *Joins of
  // several tables*).
  static constexpr std::uint32_t kInstanceBase = std::uint32_t{1} << 30;
  std::uint32_t source{0};
  // A derived relation, a grouped view a query reads as rows of its own: a
  // table the binder makes and owns, whose columns are its query's outputs,
  // with an id in [kDerivedBase, kDerivedEnd) and no storage, key or index.
  static constexpr std::uint32_t kDerivedBase = std::uint32_t{1} << 29;
  static constexpr std::uint32_t kDerivedEnd = kDerivedBase + (std::uint32_t{1} << 20);
  [[nodiscard]] static constexpr auto derived(std::uint32_t id) -> bool { return id >= kDerivedBase && id < kDerivedEnd; }

  // The table whose rows this one reads: its own, or its source's.
  [[nodiscard]] auto storage_id() const -> std::uint32_t;

  // A column by name: never the hidden row id, which has none.
  [[nodiscard]] auto column(std::string_view name) const -> const Column*;
  [[nodiscard]] auto column(std::uint32_t id) const -> const Column*;
  [[nodiscard]] auto index(std::string_view name) const -> const Index*;
  // The hidden row id, the primary key of a table declared without one, or
  // nullptr.
  [[nodiscard]] auto row_id() const -> const Column*;
};

// A view: the text of the CREATE VIEW statement that defined it, parsed and
// bound again at each use, the names of its columns as they were when it
// was created, and the tables and views it names directly. A use whose
// columns are not these is refused: PostgreSQL keeps the columns a `SELECT *`
// view had, so a view the tables have outgrown must not give more.
// One unqualified relation name a view's query reads, and the schema it
// resolved to when the view was created: what the name means at every use
// of the view, never a search (docs/catalog_design.md, *Views resolve
// against what they were created against*).
struct ViewName {
  enum class Kind : std::uint8_t { Relation = 1 };
  Kind kind{Kind::Relation};
  std::string name;
  std::uint32_t schema{kPublicSchema};
  friend auto operator==(const ViewName&, const ViewName&) -> bool = default;
};

inline constexpr std::uint8_t kNoCheckOption = 0;
inline constexpr std::uint8_t kLocalCheckOption = 1;
inline constexpr std::uint8_t kCascadedCheckOption = 2;

struct View {
  std::uint32_t id{0};
  std::string name;
  std::string definition;
  std::vector<std::uint32_t> depends_on;  // table and view ids, each once
  std::vector<std::string> columns;
  std::uint32_t schema{kPublicSchema};
  // WITH CHECK OPTION: kNoCheckOption, kLocalCheckOption or kCascadedCheckOption.
  std::uint8_t check_option{0};
  // Each unqualified relation name of the query, once.
  std::vector<ViewName> names;
  // Written before schemas (record format 1): it stores no names, and each
  // name its query reads is public's, the only schema there was.
  bool public_names{false};
};

// A stored procedure (docs/procedures_design.md): the text of the CREATE
// PROCEDURE statement that defined it, and its signature. Its compiled code
// is never stored. Procedures have a namespace of their own: a procedure and
// a table may share a name.
struct ProcedureArg {
  enum class Mode : std::uint8_t { In = 1, InOut = 2, Out = 3 };
  Mode mode{Mode::In};
  std::string name;
  SqlType type;
  auto operator==(const ProcedureArg&) const -> bool = default;
  // An IN or INOUT argument: one a CALL passes a value to.
  [[nodiscard]] auto is_input() const -> bool { return mode != Mode::Out; }
};

struct Procedure {
  std::uint32_t id{0};
  std::string name;
  std::string definition;
  std::vector<ProcedureArg> args;
  std::uint32_t schema{kPublicSchema};
};

// A role (CREATE ROLE): a name in a namespace of its own, an id from the one
// counter. The bootstrap superuser every session is (#653's default
// resolver) is implicit: never written, no name, id kBootstrapRole.
struct Role {
  std::uint32_t id{0};
  std::string name;
};

// PostgreSQL's table privileges, its ACL letters arwdDxtm, as bits.
namespace privilege {
inline constexpr std::uint16_t kSelect = 1, kInsert = 2, kUpdate = 4, kDelete = 8, kTruncate = 16, kReferences = 32,
                               kTrigger = 64, kMaintain = 128;
inline constexpr std::uint16_t kAll = 255;  // a table's or view's: all of them but USAGE
inline constexpr std::uint16_t kUsage = 256;
inline constexpr std::uint16_t kCreate = 512;  // a schema's
// The privileges a column takes, a sequence takes, a domain or type takes.
inline constexpr std::uint16_t kColumns = kSelect | kInsert | kUpdate | kReferences;
inline constexpr std::uint16_t kSequence = kSelect | kUpdate | kUsage;
inline constexpr std::uint16_t kType = kUsage;
inline constexpr std::uint16_t kSchema = kUsage | kCreate;
}  // namespace privilege

// A type of the type namespace, which tables' and views' row types share
// too: a domain over a base type, or a shell type (CREATE TYPE name), which
// nothing can use, as in PostgreSQL. A domain is not yet a column's type.
struct TypeObject {
  enum class Kind : std::uint8_t { Shell = 1, Domain = 2 };
  std::uint32_t id{0};
  std::string name;
  Kind kind{Kind::Shell};
  SqlType base{};  // a domain's
  std::uint32_t schema{kPublicSchema};
};

// A sequence: a name of the relation namespace, and an id. Its options and
// its counter, nextval and the rest are not built.
struct Sequence {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t schema{kPublicSchema};
};

// A schema (docs/catalog_design.md, *Schemas*): a name in a namespace of its
// own, an id from the one counter, and its owner, a role or the bootstrap
// role. `public` is schema 0, implicit: it has no record.
struct Schema {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t owner{0};
};

// The grantee PUBLIC, and the bootstrap superuser: never an object id, which
// stays at or below 2^24 - 1.
inline constexpr std::uint32_t kPublic = 0;
inline constexpr std::uint32_t kBootstrapRole = 0x01000000;
// An ACL entry of the whole table or view, not of one of its columns.
inline constexpr std::uint32_t kWholeObject = 0xFFFFFFFF;

// One grantee's privileges on a table or view, or on one of its columns (a
// table's column id; a view's column position), and which of them it may
// grant on. `grantor` is kBootstrapRole: every session is the superuser, so
// a grant acts as the owner's. An object's entries are sorted by (column,
// grantee), one each, every one with a privilege.
struct AclEntry {
  std::uint32_t grantee{kPublic};
  std::uint32_t column{kWholeObject};
  std::uint16_t privileges{0};
  std::uint16_t grant_options{0};
  std::uint32_t grantor{kBootstrapRole};

  friend auto operator==(const AclEntry&, const AclEntry&) -> bool = default;
};

// Whether `written`, a type a DROP PROCEDURE's argument list names, is
// `declared`, an argument's type: the same kind, whatever the modifiers, as
// PostgreSQL ignores an argument's modifier. VARCHAR is TEXT, since the
// layer reads VARCHAR without a length as TEXT.
[[nodiscard]] auto same_argument_type(SqlType written, SqlType declared) -> bool;

struct ColumnSpec {
  std::string name;
  SqlType sql;
  bool nullable{true};
  Datum default_value{};
  std::optional<ClockDefault> clock_default;
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

// A UNIQUE constraint of CREATE TABLE: its name, "" for one the catalog
// chooses as PostgreSQL does, and its columns. It is a unique index.
struct UniqueSpec {
  std::string name;
  std::vector<std::string> columns;
};

// The stage-1 DDL statements, as values: what LiveCatalog prepares.
// A CREATE TABLE with no primary key gets a hidden row id as its key; its
// UNIQUE constraints are unique indexes created with it, in its batch.
// A CHECK of CREATE TABLE: its name, "" for one the catalog chooses as
// PostgreSQL does, its expression's text, and the column it is written on,
// "" for a table's. The binder has checked the expression.
struct CheckSpec {
  std::string name;
  std::string expr;
  std::string column;
};
// `if_not_exists`: a relation of the name makes it change nothing. With no
// columns it is only that: the binder found the name taken, and skipped the
// definition unread, as PostgreSQL skips it.
// `rows`: CREATE TABLE AS's rows, one value per column, each of its
// column's physical type and within its SQL type. Only for a table with no
// primary key: they are written in the table's batch under row ids 1..n,
// as a backfill, within its limit.
struct CreateTableDdl {
  QualifiedName name;
  std::vector<ColumnSpec> columns;
  std::vector<std::string> pk;
  std::vector<UniqueSpec> unique;
  std::vector<CheckSpec> checks;
  bool if_not_exists{false};
  std::vector<std::vector<Datum>> rows;
};
struct CreateIndexDdl {
  std::string name;
  QualifiedName table;  // `name` is in its schema
  std::vector<IndexColumnSpec> columns;
  bool unique{false};
};
struct DropTableDdl {
  QualifiedName name;
  bool if_exists{false};
};
struct DropIndexDdl {
  QualifiedName name;
  bool if_exists{false};
};
struct AddColumnDdl {
  QualifiedName table;
  ColumnSpec column;
};
struct CreateViewDdl {
  QualifiedName name;
  std::string definition;
  std::vector<std::uint32_t> depends_on;
  std::vector<std::string> columns;
  std::vector<ViewName> names;  // each unqualified relation name and where it resolved
  std::uint8_t check_option{kNoCheckOption};  // View::check_option
};
struct DropViewDdl {
  QualifiedName name;
  bool if_exists{false};
};
struct CreateProcedureDdl {
  QualifiedName name;
  std::string definition;
  bool replace{false};
  std::vector<ProcedureArg> args;
};
// `arg_types`: the DROP's argument list, nullopt when none was written.
struct DropProcedureDdl {
  QualifiedName name;
  std::optional<std::vector<SqlType>> arg_types;
  bool if_exists{false};
};
struct CreateRoleDdl {
  std::string name;
};
struct DropRoleDdl {
  std::vector<std::string> names;
  bool if_exists{false};
};
// GRANT or REVOKE, as written: the catalog resolves and checks every name,
// in PostgreSQL's order (the objects, the grantees and the grantor, the
// privileges, their columns).
struct CreateDomainDdl {
  QualifiedName name;
  SqlType base;
};
struct CreateTypeDdl {
  QualifiedName name;
};
struct CreateSequenceDdl {
  QualifiedName name;
  bool if_not_exists{false};
};
// DROP DOMAIN (`domain`) or DROP TYPE, which drops a domain too.
struct DropTypeDdl {
  QualifiedName name;
  bool domain{false};
  bool if_exists{false};
};
struct DropSequenceDdl {
  QualifiedName name;
  bool if_exists{false};
};
struct GrantDdl {
  enum class Target { Table, Sequence, Domain, Type };
  struct Privilege {
    std::string name;  // "select", …, or "all"
    std::vector<std::string> columns;
  };
  bool grant{true};
  bool grant_option{false};
  std::vector<Privilege> privileges;  // empty: ALL
  std::vector<QualifiedName> objects;
  std::vector<std::string> grantees;  // "" is PUBLIC
  std::optional<std::string> grantor;
  Target target{Target::Table};  // what ON names: a table or view, a sequence, a domain, a type
};
// CREATE SCHEMA: `owner` the AUTHORIZATION role named, nullopt for the
// bootstrap role. `if_not_exists`: a taken name changes nothing.
struct CreateSchemaDdl {
  std::string name;
  std::optional<std::string> owner;
  bool if_not_exists{false};
};
// DROP SCHEMA of each name: all or none. RESTRICT refuses a schema that
// holds an object; `cascade` drops what Catalog::schema_drop lists with it.
struct DropSchemaDdl {
  std::vector<std::string> names;
  bool if_exists{false};
  bool cascade{false};
};
// A DROP … IF EXISTS the binder found nothing to do for, its name in a
// schema that does not exist: it changes nothing, and `notice` is
// PostgreSQL's NOTICE for it (schema "s" does not exist, skipping).
struct SkippedDdl {
  std::string notice;
};
using Ddl = std::variant<CreateTableDdl, CreateIndexDdl, DropTableDdl, DropIndexDdl, AddColumnDdl, CreateViewDdl,
                         DropViewDdl, CreateProcedureDdl, DropProcedureDdl, CreateRoleDdl, DropRoleDdl, GrantDdl,
                         CreateDomainDdl, CreateTypeDdl, CreateSequenceDdl, DropTypeDdl, DropSequenceDdl,
                         CreateSchemaDdl, DropSchemaDdl, SkippedDdl>;

// An ordered scan of one key prefix: where a DDL's backfill reads a table's
// rows. SnapshotRows reads an engine snapshot, for autocommit DDL; a
// transaction's view of its snapshot and its own writes is another
// (docs/transaction_design.md, *Mechanism: the transaction's private catalog*).
class RowSource {
public:
  RowSource() = default;
  RowSource(const RowSource&) = delete;
  RowSource& operator=(const RowSource&) = delete;
  virtual ~RowSource();  // out of line: anchors the vtable (-Wweak-vtables)
  // visit(key, value) for each entry whose key starts with `prefix`, in key
  // order. Throws what visit throws, and as the reads it makes throw.
  virtual void scan(BytesView prefix, const std::function<void(BytesView, BytesView)>& visit) const = 0;
};

// The rows of an engine snapshot, which must outlive it.
class SnapshotRows final : public RowSource {
public:
  explicit SnapshotRows(const Snapshot& snap) : snap_{snap} {}
  ~SnapshotRows() override;
  void scan(BytesView prefix, const std::function<void(BytesView, BytesView)>& visit) const override;

private:
  const Snapshot& snap_;
};

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
  [[nodiscard]] auto table(const QualifiedName& name) const -> const Table*;
  [[nodiscard]] auto table(std::uint32_t id) const -> const Table*;
  [[nodiscard]] auto views() const -> const std::map<std::uint32_t, View>& { return views_; }
  [[nodiscard]] auto view(const QualifiedName& name) const -> const View*;
  [[nodiscard]] auto view(std::uint32_t id) const -> const View*;
  [[nodiscard]] auto procedures() const -> const std::map<std::uint32_t, Procedure>& { return procedures_; }
  [[nodiscard]] auto procedure(const QualifiedName& name) const -> const Procedure*;
  [[nodiscard]] auto types() const -> const std::map<std::uint32_t, TypeObject>& { return types_; }
  [[nodiscard]] auto type(const QualifiedName& name) const -> const TypeObject*;
  [[nodiscard]] auto sequences() const -> const std::map<std::uint32_t, Sequence>& { return sequences_; }
  [[nodiscard]] auto sequence(const QualifiedName& name) const -> const Sequence*;
  [[nodiscard]] auto roles() const -> const std::map<std::uint32_t, Role>& { return roles_; }
  // Every schema but public, which has no record.
  [[nodiscard]] auto schemas() const -> const std::map<std::uint32_t, Schema>& { return schemas_; }
  [[nodiscard]] auto schema(std::string_view name) const -> const Schema*;
  [[nodiscard]] auto schema(std::uint32_t id) const -> const Schema*;
  // A schema's name as PostgreSQL writes it: "public" for schema 0.
  [[nodiscard]] auto schema_name(std::uint32_t id) const -> std::string;
  // A name as PostgreSQL 17 writes a missing object's: "s.x", bare in
  // public, unquoted. (PostgreSQL also keeps a written "public.", which a
  // resolved name no longer carries.)
  [[nodiscard]] auto written(const QualifiedName& name) const -> std::string;
  [[nodiscard]] auto role(std::string_view name) const -> const Role*;
  // A table's or view's ACL, by its id: empty when nothing is granted on it.
  [[nodiscard]] auto acls() const -> const std::map<std::uint32_t, std::vector<AclEntry>>& { return acls_; }
  [[nodiscard]] auto acl(std::uint32_t object) const -> std::span<const AclEntry>;
  // An index by name, with its table.
  [[nodiscard]] auto index(const QualifiedName& name) const -> std::pair<const Table*, const Index*>;
  // A table or an index: both draw ids from one counter.
  [[nodiscard]] auto key_layout(std::uint32_t table_or_index) const -> const KeyLayout*;

  // Object ids stay at or below 2^24 - 1: bc_error_detail carries one in its
  // low 24 bits (and a plan's constants are an enum, whose values are ints).
  static constexpr std::uint32_t kMaxObjectId = ConstraintDetail::kMaxObjectId;
  // 7: the record of docs/catalog_design.md, whose layouts are for the row
  // format and keys of docs/types_design.md: fixed columns and integer key
  // parts at their declared widths, the 8-byte row header (#780). Formats
  // 1 to 6 laid rows and keys out in 8-byte words under a 12-byte header;
  // they are refused, so no row or key of theirs is decoded as this one.
  // 8: format 7, then the table's schema (docs/catalog_design.md, *Schema
  // catalog record and name entry keys*). A format-7 record still reads, as
  // a table of public; this build writes only format 8.
  static constexpr std::uint16_t kRecordFormat = 8;
  static constexpr std::uint16_t kPublicRecordFormat = 7;

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

  // The version after `ddl`, with its batch in `ops`; nullptr for a no-op.
  // Gathers at most `backfill_limit` bytes of backfill (std::length_error
  // past it). Reads the rows a backfill needs from `rows`, which must agree
  // with this version. Sets `backfill` if the batch rewrites or indexes rows.
  // Writes nothing; throws as LiveCatalog::prepare does.
  [[nodiscard]] auto next_version(const Ddl& ddl, const RowSource& rows, std::vector<DdlOp>& ops, bool& backfill,
                                  std::size_t backfill_limit) const -> std::shared_ptr<const Catalog>;

  // What a copy of this catalog holds, as a transaction charges it to its
  // bound (docs/transaction_design.md, *Mechanism: the transaction's private
  // catalog*): kEstimateBase, then per table, column, index and other object
  // its constant, plus the bytes of every name and definition. An estimate,
  // not a measure: the constants are generous.
  [[nodiscard]] auto estimated_bytes() const noexcept -> std::uint64_t;
  static constexpr std::uint64_t kEstimateBase = 1024;
  static constexpr std::uint64_t kEstimateTable = 1024;
  static constexpr std::uint64_t kEstimateColumn = 256;
  static constexpr std::uint64_t kEstimateIndex = 256;
  static constexpr std::uint64_t kEstimateObject = 256;  // a view, procedure, role, type, sequence or ACL

  // A view that depends on the table or view `id`, if any: what makes
  // DROP of it 2BP01.
  [[nodiscard]] auto find_view_dependent(std::uint32_t id) const -> const View*;

  // What DROP SCHEMA drops besides the schemas, as PostgreSQL 17 lists it:
  // each table, view, sequence, domain, shell type and procedure of a named
  // schema, in creation (id) order, each followed by the views elsewhere
  // that read it, and the views that read those. An index goes with its
  // table and is not listed. `depends_on` is what the object depends on, as
  // RESTRICT's DETAIL words it.
  struct SchemaDrop {
    struct Object {
      std::uint32_t id{0};
      std::string described;   // "table s.t"
      std::string depends_on;  // "schema s", "view s.v"
    };
    std::vector<std::uint32_t> schemas;  // each dropped schema, once; empty when IF EXISTS skipped them all
    std::vector<Object> objects;
    // PostgreSQL's DETAIL of the objects, one line each as `line` writes it,
    // past kMaxReported cut to "and N other objects (see server log for list)".
    [[nodiscard]] auto detail(const std::function<std::string(const Object&)>& line) const -> std::string;
    static constexpr std::size_t kMaxReported = 100;  // PostgreSQL's MAX_REPORTED_DEPS
  };
  // Throws DdlRefused as the DROP does for its names: 3F000 for a missing
  // schema (unless IF EXISTS), 2BP01 for pg_catalog, 0A000 for public and
  // information_schema.
  [[nodiscard]] auto schema_drop(const DropSchemaDdl& ddl) const -> SchemaDrop;

private:
  friend class LiveCatalog;

  // A copy of every table, with its own lookups.
  [[nodiscard]] auto clone() const -> Catalog;
  void put_table(Table t);
  void erase_table(std::uint32_t id);
  void rebuild_lookups();

  std::uint64_t next_id_{1};
  std::uint64_t version_{0};
  std::map<std::uint32_t, Table> tables_;
  std::map<std::uint32_t, View> views_;
  // The lookups by name are keyed by (schema, name): each object's record
  // names its schema, an index is in its table's.
  std::map<QualifiedName, std::uint32_t> names_;  // table, index, view or sequence name -> id
  std::map<std::uint32_t, Procedure> procedures_;
  std::map<QualifiedName, std::uint32_t> procedure_names_;  // a namespace of their own
  std::map<std::uint32_t, TypeObject> types_;
  std::map<QualifiedName, std::uint32_t> type_names_;  // the type namespace: domains and shell types
  std::map<std::uint32_t, Sequence> sequences_;                   // named in names_, the relation namespace
  std::map<std::uint32_t, Role> roles_;
  std::map<std::string, std::uint32_t, std::less<>> role_names_;  // a namespace of their own
  std::map<std::uint32_t, std::vector<AclEntry>> acls_;           // object id -> its ACL, never empty
  // Every schema but public. Each has an ACL record; one holding only the
  // owner's default (no entry) is not in acls_.
  std::map<std::uint32_t, Schema> schemas_;
  std::map<std::string, std::uint32_t, std::less<>> schema_names_;  // a namespace of their own
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
  // Throws DdlRefused for a statement the catalog refuses (a duplicate
  // name, an unknown column…), ConstraintViolation for a unique index over
  // duplicates, the engine's std::invalid_argument for a key over its
  // limit, and
  // std::length_error when ids or a layout run out or a backfill passes the
  // engine's batch limit. Writes nothing.
  [[nodiscard]] auto prepare(const Ddl& ddl) -> Prepared;
  // Commits and publishes. False, with nothing written, on a conflict: a
  // DDL or a write to a backfilled table committed since the prepare.
  // Throws DdlInProgress, having done nothing, if another DDL of this
  // catalog is in progress: a DDL never waits for another;
  // CommitOutcomeUnknown as apply_batch_or_unknown throws it (the next pin
  // catches up with whatever the data file holds).
  [[nodiscard]] auto commit(Prepared p) -> bool;
  // prepare and commit, retried on a conflict kDdlAttempts times in all.
  // Returns the version it published; throws as prepare does, DdlInProgress
  // as commit does, or std::runtime_error when every attempt conflicted.
  auto apply(const Ddl& ddl) -> std::shared_ptr<const Catalog>;
  // apply, and whether the DDL changed the catalog: false when IF EXISTS or
  // IF NOT EXISTS skipped it, and `catalog` is then the version it read.
  // `base` is the version the DDL read.
  struct Applied {
    std::shared_ptr<const Catalog> catalog;
    bool changed{false};
    std::shared_ptr<const Catalog> base;
  };
  auto run(const Ddl& ddl) -> Applied;

  static constexpr int kDdlAttempts = 3;

  // The catalog version the engine holds now, read from its latest state:
  // what a commit that conflicted reads to tell a DDL published since from
  // a duplicate key. Throws as the engine's reads throw.
  [[nodiscard]] auto head_version() const -> std::uint64_t;
  // The bytes a backfill may gather: the engine's 1 GiB batch, unless a
  // test set less.
  [[nodiscard]] auto backfill_limit() const noexcept -> std::size_t { return backfill_limit_; }

  // The next row id of `t`, a table whose primary key is its hidden row id:
  // one past every id it has handed out, which starts one past the
  // greatest id the engine holds for the table (from 1 for none). Ids are
  // drawn, never returned: an insert that fails or rolls back leaves a gap,
  // and none is handed out twice while this catalog lives. The data file is
  // the only record, so after a reopen the ids of rows deleted from the end
  // of the table may be handed out again. A second LiveCatalog on the same
  // open DB (only tests make one) counts on its own; the engine's check of
  // every written key refuses an id both draw. Thread-safe. Throws
  // std::length_error past INT64_MAX, and as the engine's reads throw.
  [[nodiscard]] auto next_row_id(const Table& t) -> std::int64_t;
  // CREATE TABLE AS writes its n rows under row ids 1..n itself, before
  // its table is published (in a transaction's overlay, or in its batch):
  // for `ddl`, one, the next id `next`'s table hands out is past n. An id
  // counter outlives a rolled-back table of the same id, so it may be
  // behind. Does nothing for any other DDL. Thread-safe; throws as
  // next_row_id does.
  void reserve_row_ids(const Catalog& next, const Ddl& ddl);

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

  // One DDL at a time: apply holds it from prepare to publication. Only
  // ever try-locked: a DDL that finds it held throws DdlInProgress.
  std::mutex ddl_mu_;
  std::function<void()> commit_hook_;
  std::size_t backfill_limit_{kMaxBackfillBytes};
  // Odd from just before a DDL's batch reaches the engine until current_
  // is swapped (or the commit fails).
  mutable std::atomic<std::uint64_t> gen_{0};
  // Per table with a row id, the next id to hand out; a table enters at its
  // first insert. Held only to find and advance an entry, and to seed one.
  std::mutex row_ids_mu_;
  std::map<std::uint32_t, std::int64_t> row_ids_;
  // Under row_ids_mu_: `t`'s entry, seeded from the engine if it has none.
  auto seeded_row_id_locked(const Table& t) -> std::map<std::uint32_t, std::int64_t>::iterator;
};

// The key of the catalog version counter, which every DDL that changes the
// catalog writes, and which a transaction with DDL guards at its commit.
[[nodiscard]] auto catalog_version_key() -> Bytes;
// The catalog record's key and a table's row-key range: for tests and tools.
[[nodiscard]] auto table_record_key(std::uint32_t table_id) -> Bytes;
[[nodiscard]] auto view_record_key(std::uint32_t view_id) -> Bytes;
[[nodiscard]] auto procedure_record_key(std::uint32_t procedure_id) -> Bytes;
[[nodiscard]] auto procedure_name_key(const QualifiedName& name) -> Bytes;
[[nodiscard]] auto role_record_key(std::uint32_t role_id) -> Bytes;
[[nodiscard]] auto role_name_key(std::string_view name) -> Bytes;
[[nodiscard]] auto acl_record_key(std::uint32_t object_id) -> Bytes;
[[nodiscard]] auto type_record_key(std::uint32_t type_id) -> Bytes;
[[nodiscard]] auto type_name_key(const QualifiedName& name) -> Bytes;
[[nodiscard]] auto sequence_record_key(std::uint32_t sequence_id) -> Bytes;
// A relation's name entry; an index's is in its table's schema.
[[nodiscard]] auto name_entry_key(const QualifiedName& name) -> Bytes;
[[nodiscard]] auto schema_record_key(std::uint32_t schema_id) -> Bytes;
[[nodiscard]] auto schema_name_key(std::string_view name) -> Bytes;
// Every row key of a table starts with its row prefix: kNsRow, the table id.
[[nodiscard]] auto row_prefix(std::uint32_t table_id) -> Bytes;
[[nodiscard]] auto has_prefix(BytesView key, BytesView prefix) -> bool;

// The keys under `prefix` in the snapshot, in key order: visit(key, value)
// for each. The one prefix scan of a snapshot.
template <typename Visit>
void for_each_in_prefix(const Snapshot& snap, BytesView prefix, Visit&& visit) {
  for (const auto& e : snap.iter_from({}, prefix)) {
    if (!has_prefix(e.key, prefix)) break;
    visit(e.key, e.value);
  }
}
// The rows of a table in the snapshot, in primary key order: visit(key, row)
// for each.
template <typename Visit>
void for_each_row(const Snapshot& snap, std::uint32_t table_id, Visit&& visit) {
  for_each_in_prefix(snap, row_prefix(table_id), std::forward<Visit>(visit));
}
// Serializes a table as kRecordFormat; parses one, throwing
// std::runtime_error if it does not parse or breaks an invariant.
[[nodiscard]] auto serialize_table(const Table& t) -> Bytes;
[[nodiscard]] auto parse_table(BytesView record) -> Table;
[[nodiscard]] auto serialize_view(const View& v) -> Bytes;
[[nodiscard]] auto parse_view(BytesView record) -> View;
[[nodiscard]] auto serialize_procedure(const Procedure& p) -> Bytes;
[[nodiscard]] auto parse_procedure(BytesView record) -> Procedure;
[[nodiscard]] auto serialize_role(const Role& r) -> Bytes;
[[nodiscard]] auto parse_role(BytesView record) -> Role;
[[nodiscard]] auto serialize_type(const TypeObject& t) -> Bytes;
[[nodiscard]] auto parse_type(BytesView record) -> TypeObject;
[[nodiscard]] auto serialize_sequence(const Sequence& s) -> Bytes;
[[nodiscard]] auto parse_sequence(BytesView record) -> Sequence;
[[nodiscard]] auto serialize_schema(const Schema& s) -> Bytes;
[[nodiscard]] auto parse_schema(BytesView record) -> Schema;
// An ACL record, written in format 2: `is_default` holds no entry and means
// the owner's default privileges, a schema's alone.
[[nodiscard]] auto serialize_acl(std::uint32_t object, const std::vector<AclEntry>& acl, bool is_default = false) -> Bytes;
// The object id and its entries, checked sorted, one per (column, grantee),
// each with a privilege and no grant option it lacks; whether it is the
// default, and whether it was of format 2 (a schema's must be).
struct ParsedAcl {
  std::uint32_t object{0};
  std::vector<AclEntry> entries;
  bool is_default{false};
  bool schema_format{false};
};
[[nodiscard]] auto parse_acl(BytesView record) -> ParsedAcl;
// Whether a name may be an object's: not empty, and no NUL byte, so that no
// name entry of a schema's form is ever a public name's (*Schema catalog
// record and name entry keys*).
[[nodiscard]] constexpr auto valid_name(std::string_view name) -> bool {
  return !name.empty() && name.find('\0') == std::string_view::npos;
}

// Encodes the primary key of `t` from a row: kNsRow, the table id, the parts.
[[nodiscard]] auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool;
// Encodes the index entry of row: kNsIndex, the index id, the parts, the
// primary key. `prefix_len`, if given, receives the length of the index-part
// prefix, and `has_null` whether any index part was null.
[[nodiscard]] auto encode_index_key_from_row(const Table& t, const Index& ix, BytesView row, Bytes& out,
                                             std::size_t* prefix_len = nullptr, bool* has_null = nullptr) -> bool;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::byte kSubCounter{0x01};
constexpr std::byte kSubTable{0x02};
constexpr std::byte kSubName{0x03};
constexpr std::byte kSubView{0x04};
constexpr std::byte kSubProcedure{0x05};
constexpr std::byte kSubProcedureName{0x06};
constexpr std::byte kSubRole{0x07};
constexpr std::byte kSubRoleName{0x08};
constexpr std::byte kSubAcl{0x09};
constexpr std::byte kSubType{0x0A};
constexpr std::byte kSubTypeName{0x0B};
constexpr std::byte kSubSequence{0x0C};
constexpr std::byte kSubSchema{0x0D};
constexpr std::byte kSubSchemaName{0x0E};
// The sub-prefixes this build knows are kSubCounter to kSubLast, every one
// in between: a new kind takes the next byte and moves kSubLast.
constexpr std::byte kSubLast = kSubSchemaName;
// A name entry outside public: this byte, then the schema id, then the name.
// No name is empty or holds a NUL byte, so the two forms never collide.
constexpr std::byte kSchemaMarker{0x00};
constexpr std::byte kCounterNextId{0x01};
constexpr std::byte kCounterVersion{0x02};
constexpr std::byte kCounterLast = kCounterVersion;
constexpr std::uint8_t kKindTable = 1;
constexpr std::uint8_t kKindIndex = 2;
constexpr std::uint8_t kKindView = 3;
// Format 2 of a view, procedure, type, sequence and ACL record appends what
// schemas need (docs/catalog_design.md, *Schema catalog record and name
// entry keys*); format 1 still reads, as an object of public.
constexpr std::uint16_t kViewRecordFormat = 2;
constexpr std::uint8_t kKindProcedure = 4;
constexpr std::uint16_t kProcedureRecordFormat = 2;
constexpr std::uint8_t kKindRole = 5;
constexpr std::uint16_t kRoleRecordFormat = 1;
constexpr std::uint16_t kAclRecordFormat = 2;
constexpr std::uint8_t kKindSequence = 6;
constexpr std::uint8_t kKindType = 7;
constexpr std::uint16_t kTypeRecordFormat = 2;
constexpr std::uint16_t kSequenceRecordFormat = 2;
constexpr std::uint8_t kKindSchema = 8;
constexpr std::uint16_t kSchemaRecordFormat = 1;

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


auto index_prefix(std::uint32_t index_id) -> Bytes {
  Bytes p;
  key_put_prefix(p, kNsIndex, index_id);
  return p;
}

// Column c's key part (key_part_of): its type's width, nullable and
// direction as given.
auto key_part(const Column& c, bool nullable = false, bool desc = false) -> KeyPart {
  return key_part_of(c.sql, nullable, desc);
}

// Appends one column of `row` as a key part. False if the row is malformed
// or a non-nullable part is null; `was_null` receives whether it was null.
auto put_part_from_row(const Table& t, std::uint32_t col_id, KeyPart part, BytesView row, Bytes& out,
                       bool* was_null = nullptr) -> bool {
  const auto* slot = t.layout.find(col_id);
  if (slot == nullptr) return false;
  auto v = row_column(row, *slot);
  switch (v.state) {
    case ColumnState::Malformed: return false;
    case ColumnState::Absent:
      if (!part.nullable) return false;
      if (was_null != nullptr) *was_null = true;
      key_put_null(out, part);
      return true;
    case ColumnState::Value: break;
  }
  switch (slot->type) {
    // A row's column holds only what its width does, which is its key part's
    // or less: false here is a row that does not parse.
    case PhysType::I64: return key_put_i64(out, v.i64(), part);
    case PhysType::F64: key_put_f64(out, v.f64(), part); return true;
    case PhysType::Bool: key_put_bool(out, v.boolean(), part); return true;
    case PhysType::Bytes: key_put_bytes(out, v.bytes, part); return true;
    case PhysType::Num: throw std::logic_error("a key part that is a NUMERIC");
    case PhysType::Jsonb: throw std::logic_error("a key part that is a jsonb");
  }
  return false;
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

// What a backfill has gathered so far, refused once past the engine's 1 GiB
// batch limit: the engine would refuse the batch at commit, and gathering
// all of a large table first would run out of memory before it got there.
class BackfillBudget {
public:
  BackfillBudget(std::string table, std::size_t limit) : table_{std::move(table)}, limit_{limit} {}
  void charge(std::size_t bytes) {
    used_ += bytes;
    if (used_ > limit_) {
      throw BackfillTooLarge("backfill of table \"" + table_ +
                                 "\" does not fit the engine's 1 GiB batch; create indexes and columns with defaults "
                                 "before loading the table (#505)",
                             table_);
    }
  }

private:
  std::string table_;
  std::size_t limit_;
  std::size_t used_{0};
};

// The catalog version a snapshot holds: 0 before the first DDL.
auto snapshot_version(const Snapshot& snap) -> std::uint64_t {
  Bytes v;
  return snap.get({}, counter_key(kCounterVersion), v) ? be_value(v) : 0;
}

// The bytes of the first `limit` of s that end on a UTF-8 character
// (PostgreSQL's pg_mbcliplen).
auto clip_utf8(std::string_view s, std::size_t limit) -> std::size_t {
  while (limit > 0 && limit < s.size() && (static_cast<unsigned char>(s[limit]) & 0xC0) == 0x80) --limit;
  return limit;
}

// PostgreSQL's name for an object it names (makeObjectName): name1_name2_label
// within 63 bytes, the longer of name1 and name2 shortened first, each then
// clipped to a character.
auto object_name(std::string_view name1, std::string_view name2, std::string_view label) -> std::string {
  constexpr std::size_t kMaxName = 63;  // PostgreSQL's NAMEDATALEN - 1
  auto overhead = 1 + label.size() + 1;
  auto avail = kMaxName - overhead;
  auto n1 = name1.size();
  auto n2 = name2.size();
  while (n1 + n2 > avail) {
    if (n1 > n2) {
      --n1;
    } else {
      --n2;
    }
  }
  return std::string{name1.substr(0, clip_utf8(name1, n1))} + "_" + std::string{name2.substr(0, clip_utf8(name2, n2))} +
         "_" + std::string{label};
}

// The name PostgreSQL gives a UNIQUE constraint's index it names
// (ChooseIndexName): table_columns_key, the columns joined by _, and key1,
// key2… where the name is taken.
template <typename Taken>
auto unique_index_name(std::string_view table, const std::vector<std::string>& columns, const Taken& taken) -> std::string {
  std::string name2;
  for (const auto& c : columns) {
    if (!name2.empty()) name2 += '_';
    name2 += c;
    if (name2.size() >= 64) break;
  }
  for (std::uint32_t pass = 0;; ++pass) {
    auto name = object_name(table, name2, pass == 0 ? std::string{"key"} : "key" + std::to_string(pass));
    if (!taken(name)) return name;
  }
}

auto reserve_ids(std::uint64_t& next_id, std::uint32_t n) -> std::uint32_t {
  auto first = next_id;
  if (first + n - 1 > Catalog::kMaxObjectId) throw std::length_error("catalog object ids exhausted");
  next_id += n;
  return static_cast<std::uint32_t>(first);
}

// A jsonb column's default, as the catalog holds it in memory: a stored
// value (its format byte and node) that passes the full check. Any other
// column's: true.
auto jsonb_default_valid(SqlType t, const Datum& d) -> bool {
  const auto* b = std::get_if<Bytes>(&d);
  return t.kind != SqlKind::Jsonb || b == nullptr || jsonb::check_value(*b, {}).ok();
}

void check_column_spec(const ColumnSpec& c) {
  if (!sql_type_valid(c.sql)) throw DdlRefused("column " + c.name + " has an invalid type");
  if (!datum_fits(c.default_value, phys_type(c.sql)) || !jsonb_default_valid(c.sql, c.default_value)) {
    throw DdlRefused("default of column " + c.name + " is not of its type", kDatatypeMismatch);
  }
  if (c.clock_default && (!clock_assignable(c.clock_default->fn, c.sql.kind) || c.clock_default->precision > 6 ||
                          !std::holds_alternative<std::monostate>(c.default_value))) {
    throw DdlRefused("default of column " + c.name + " is not of its type", kDatatypeMismatch);
  }
}

}  // namespace

DdlRefused::~DdlRefused() = default;

RowSource::~RowSource() = default;
SnapshotRows::~SnapshotRows() = default;

void SnapshotRows::scan(BytesView prefix, const std::function<void(BytesView, BytesView)>& visit) const {
  for_each_in_prefix(snap_, prefix, visit);
}
DdlInProgress::~DdlInProgress() = default;
BackfillTooLarge::~BackfillTooLarge() = default;
CommitOutcomeUnknown::~CommitOutcomeUnknown() = default;
auto CommitOutcomeUnknown::what() const noexcept -> const char* {
  return "the commit's outcome is unknown: it failed after its batch may have reached the data file";
}

auto apply_batch_or_unknown(DB& db, WriteOptions opts, WritePlan plan) -> std::optional<CommitResult> {
  auto was_degraded = db.is_degraded();
  try {
    return db.apply_batch(opts, std::move(plan));
  } catch (...) {
    if (was_degraded || !db.is_degraded()) throw;
    throw CommitOutcomeUnknown{};
  }
}
ConstraintViolation::~ConstraintViolation() = default;

auto ConstraintDetail::name() const -> std::string_view {
  switch (constraint) {
    case BC_C_UNIQUE: return "unique";
    case BC_C_NOT_NULL: return "not_null";
    case BC_C_CHECK: return "check";
    case BC_C_FOREIGN_KEY: return "foreign_key";
    case BC_C_RANGE: return "range";
    case BC_C_CHECK_OPTION: return "check_option";
    default: return {};
  }
}

auto ConstraintDetail::message() const -> std::string {
  auto object = std::to_string(object_id);
  switch (constraint) {
    case BC_C_UNIQUE: return "duplicate key value violates unique constraint of object " + object;
    case BC_C_NOT_NULL: return "null value violates a not-null constraint of table " + object;
    case BC_C_CHECK: return "check constraint " + object + " violated";
    case BC_C_FOREIGN_KEY: return "foreign key constraint " + object + " violated";
    case BC_C_RANGE: return "value out of range for a column of table " + object;
    case BC_C_CHECK_OPTION: return "new row violates check option for view " + object;
    default: return "constraint violated on object " + object;
  }
}


auto has_prefix(BytesView key, BytesView prefix) -> bool {
  return key.size() >= prefix.size() && std::ranges::equal(key.first(prefix.size()), prefix);
}

auto row_prefix(std::uint32_t table_id) -> Bytes {
  Bytes p;
  key_put_prefix(p, kNsRow, table_id);
  return p;
}

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
    case SqlKind::Varchar:
    case SqlKind::Char: return t.length > 0 && t.precision == 0 && t.scale == 0;
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
    case SqlKind::Name:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz: return t.length == 0 && t.precision == 0 && t.scale == 0;
    // A column's has no modifier; AVG's result, which carries its sum's
    // scale, is never a column's type.
    case SqlKind::Numeric: return t.length == 0 && t.precision == 0 && t.scale == 0;
    case SqlKind::Jsonb: return t.length == 0 && t.precision == 0 && t.scale == 0;
  }
  return false;  // a kind no build knows
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
    case SqlKind::Char: return "char(" + std::to_string(t.length) + ")";
    case SqlKind::Name: return "name";
    case SqlKind::Decimal:
      if (t.precision == 0) return "numeric";  // no declared precision: a SUM's result
      return "numeric(" + std::to_string(t.precision) + "," + std::to_string(t.scale) + ")";
    case SqlKind::Timestamp: return "timestamp";
    case SqlKind::TimestampTz: return "timestamptz";
    case SqlKind::Date: return "date";
    case SqlKind::Time: return "time";
    case SqlKind::TimeTz: return "timetz";
    case SqlKind::Numeric: return "numeric";
    case SqlKind::Jsonb: return "jsonb";
  }
  return "?";
}

auto utf8_code_points(BytesView s) -> std::optional<std::size_t> {
  auto n = std::size_t{0};
  for (std::size_t i = 0; i < s.size(); ++n) {
    auto b0 = std::to_integer<std::uint32_t>(s[i]);
    // The sequence length and the bounds of the second byte that keep it
    // shortest and inside U+0000..U+10FFFF, surrogates excluded (RFC 3629).
    auto len = std::size_t{0};
    auto lo = 0x80u;
    auto hi = 0xBFu;
    if (b0 == 0x00) return std::nullopt;
    if (b0 < 0x80) {
      len = 1;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
      len = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      len = 3;
      if (b0 == 0xE0) lo = 0xA0;
      if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      len = 4;
      if (b0 == 0xF0) lo = 0x90;
      if (b0 == 0xF4) hi = 0x8F;
    } else {
      return std::nullopt;  // a continuation byte, C0, C1, or F5..FF
    }
    if (len > s.size() - i) return std::nullopt;
    for (std::size_t k = 1; k < len; ++k) {
      auto b = std::to_integer<std::uint32_t>(s[i + k]);
      if (b < (k == 1 ? lo : 0x80u) || b > (k == 1 ? hi : 0xBFu)) return std::nullopt;
    }
    i += len;
  }
  return n;
}

auto compared_type(SqlType t) -> ComparedType {
  auto same = [](SqlType u) { return ComparedType{u, u}; };
  switch (t.kind) {
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Name: return same({SqlKind::Text});
    case SqlKind::Decimal:
      if (t.scale == 0) return same({SqlKind::BigInt});
      return {{SqlKind::Decimal, 0, 0, t.scale}, {SqlKind::BigInt}};
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz:
    case SqlKind::Numeric:
    case SqlKind::Jsonb: return same(t);
  }
  return same(t);
}

auto out_of_range(SqlType t, std::int64_t v) -> std::optional<std::string> {
  switch (t.kind) {
    case SqlKind::SmallInt:
      if (!std::in_range<std::int16_t>(v)) return "smallint out of range";
      break;
    case SqlKind::Integer:
      if (!std::in_range<std::int32_t>(v)) return "integer out of range";
      break;
    case SqlKind::Decimal: {
      // p digits: |v| < 10^p, on the scaled value.
      auto limit = kPowersOfTen.at(t.precision);
      if (v <= -limit || v >= limit) return "numeric field overflow";
      break;
    }
    case SqlKind::Time:
      if (v < 0 || v > kMicrosPerDay) return "time out of range";
      break;
    case SqlKind::TimeTz:
      if (!timetz_valid(v)) return "time out of range";
      break;
    case SqlKind::Date:
      if (v < kMinDate || v >= kEndDate) return "date out of range";
      break;
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
      if (v < kMinTimestamp) return "timestamp out of range";
      break;
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Name:
    case SqlKind::Numeric:
    case SqlKind::Jsonb: break;
  }
  return std::nullopt;
}

auto out_of_range(SqlType t, double v) -> std::optional<std::string> {
  if (t.kind != SqlKind::Real) return std::nullopt;
  // As PostgreSQL's float8 to float4: a finite value whose float rounding is
  // infinite overflows, a nonzero one whose rounding is zero underflows.
  // float_rounding holds the thresholds, and never converts out of range.
  auto r = float_rounding(v);
  if (std::isinf(r) && std::isfinite(v)) return "value out of range: overflow";
  if (r == 0.0 && v != 0.0) return "value out of range: underflow";
  return std::nullopt;
}

auto out_of_range(SqlType t, BytesView v) -> std::optional<std::string> {
  // A NUMERIC's text is read as UTF-8 first, as PostgreSQL checks a
  // parameter's encoding before numeric_in reads it.
  // A NAME's longer value is cut, not refused (name_prefix).
  if (t.kind != SqlKind::Text && t.kind != SqlKind::Varchar && t.kind != SqlKind::Char && t.kind != SqlKind::Name &&
      t.kind != SqlKind::Numeric) {
    return std::nullopt;
  }
  auto n = utf8_code_points(v);
  if (!n) return "invalid byte sequence for encoding \"UTF8\"";
  if (t.kind == SqlKind::Varchar && *n > t.length) {
    return "value too long for type character varying(" + std::to_string(t.length) + ")";
  }
  if (t.kind == SqlKind::Char && *n > t.length) {
    // Spaces past n are cut, not refused: only the other characters count.
    while (!v.empty() && v.back() == std::byte{' '}) v = v.first(v.size() - 1);
    if (utf8_code_points(v) > t.length) return "value too long for type character(" + std::to_string(t.length) + ")";
  }
  return std::nullopt;
}

namespace {

constexpr RangeMessage kRangeMessages[] = {
    {"smallint out of range", "22003"},
    {"bigint out of range", "22003"},
    {"integer out of range", "22003"},
    {"value out of range: overflow", "22003"},   // REAL
    {"value out of range: underflow", "22003"},  // REAL
    {"numeric field overflow", "22003"},         // DECIMAL(p, s)
    {"value overflows numeric format", "22003"},  // NUMERIC: numeric_in
    {"invalid input syntax for type numeric", "22P02"},  // NUMERIC: numeric_in
    {"value too long for type character varying", "22001"},
    {"value too long for type character(", "22001"},
    {"invalid byte sequence for encoding", "22021"},  // TEXT, VARCHAR
    {"date out of range", "22008"},
    {"timestamp out of range", "22008"},
    {"time out of range", "22008"},
};

}  // namespace

auto range_sqlstate(std::string_view message) noexcept -> std::string_view {
  for (const auto& m : kRangeMessages) {
    if (message.starts_with(m.start)) return m.sqlstate;
  }
  return {};
}

auto range_messages() noexcept -> std::span<const RangeMessage> { return kRangeMessages; }

auto out_of_range(SqlType t, const Datum& d) -> std::optional<std::string> {
  return std::visit(
      [&](const auto& v) -> std::optional<std::string> {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, std::monostate>) {
          return std::nullopt;
        } else if constexpr (std::is_same_v<V, Bytes>) {
          return out_of_range(t, BytesView{v});
        } else {
          return out_of_range(t, v);
        }
      },
      d);
}

auto datum_fits(const Datum& d, PhysType t) -> bool {
  switch (t) {
    case PhysType::I64: return !std::holds_alternative<double>(d) && !std::holds_alternative<bool>(d) &&
                               !std::holds_alternative<Bytes>(d);
    case PhysType::F64: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<double>(d);
    case PhysType::Bool: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<bool>(d);
    case PhysType::Bytes:
    case PhysType::Num:
    case PhysType::Jsonb: return std::holds_alternative<std::monostate>(d) || std::holds_alternative<Bytes>(d);
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

auto view_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubView};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto procedure_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubProcedure};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

namespace {

// A name entry's key under `sub`: the name's bytes in public, else the
// marker and the schema id before them.
auto qualified_entry_key(std::byte sub, const QualifiedName& name) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, sub};
  if (name.schema != kPublicSchema) {
    k.push_back(kSchemaMarker);
    auto idb = be_bytes(name.schema, 4);
    k.insert(k.end(), idb.begin(), idb.end());
  }
  for (auto c : name.name) k.push_back(static_cast<std::byte>(c));
  return k;
}

}  // namespace

auto procedure_name_key(const QualifiedName& name) -> Bytes { return qualified_entry_key(kSubProcedureName, name); }

auto schema_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubSchema};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto schema_name_key(std::string_view name) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, kSubSchemaName};
  for (auto c : name) k.push_back(static_cast<std::byte>(c));
  return k;
}

auto role_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubRole};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto role_name_key(std::string_view name) -> Bytes {
  Bytes k{std::byte{kNsCatalog}, kSubRoleName};
  for (auto c : name) k.push_back(static_cast<std::byte>(c));
  return k;
}

auto type_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubType};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto type_name_key(const QualifiedName& name) -> Bytes { return qualified_entry_key(kSubTypeName, name); }

auto sequence_record_key(std::uint32_t id) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubSequence};
  auto idb = be_bytes(id, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto acl_record_key(std::uint32_t object) -> Bytes {
  auto k = Bytes{std::byte{kNsCatalog}, kSubAcl};
  auto idb = be_bytes(object, 4);
  k.insert(k.end(), idb.begin(), idb.end());
  return k;
}

auto same_argument_type(SqlType written, SqlType declared) -> bool {
  auto text = [](SqlKind k) { return k == SqlKind::Varchar ? SqlKind::Text : k; };
  return text(written.kind) == text(declared.kind);
}

auto name_entry_key(const QualifiedName& name) -> Bytes { return qualified_entry_key(kSubName, name); }

namespace {

// A CHAR(n) is stored without its trailing spaces, which a key built from a
// parameter would keep: it is not a key or index column yet. A NUMERIC of no
// precision has no order-preserving key form yet.
void refuse_key_type(const Column& c) {
  if (c.sql.kind == SqlKind::Char) {
    throw DdlRefused("CHAR(n) column \"" + c.name + "\" as a primary key, unique or index column is not supported yet; use VARCHAR(n)",
                     kFeatureNotSupported);
  }
  if (c.sql.kind == SqlKind::Numeric) {
    throw DdlRefused("NUMERIC column \"" + c.name + "\" without a precision as a primary key, unique or index column is not "
                     "supported yet; declare NUMERIC(p, s)",
                     kFeatureNotSupported);
  }
  // jsonb's key form is stage 2's sort key, and its key columns the next
  // phase's (docs/jsonb_design.md, *jsonb as a key, unique or index column*).
  if (c.sql.kind == SqlKind::Jsonb) {
    throw DdlRefused("jsonb column \"" + c.name + "\" as a primary key, unique or index column is not supported yet",
                     kFeatureNotSupported);
  }
}

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
    l.columns.emplace_back(col_id, c->sql, c->nullable, pos);
  }
  e.expect_end("layout entry of table " + t.name);
  if (!layout_valid(l)) corrupt("table " + t.name + " has a malformed row layout");
  return {id, std::move(l)};
}

}  // namespace

namespace {

// A column's default as its record holds it: a jsonb's is its canonical text,
// so the record never holds the binary layout (docs/jsonb_design.md, *One
// form for a jsonb value at each boundary*).
auto stored_default(const Column& c) -> Datum {
  const auto* b = std::get_if<Bytes>(&c.default_value);
  if (c.sql.kind != SqlKind::Jsonb || b == nullptr) return c.default_value;
  std::string text;
  if (auto r = jsonb::value_text(*b, text); !r.ok()) {
    throw std::logic_error("default of jsonb column " + c.name + " does not decode: " + r.corrupt);
  }
  auto t = std::as_bytes(std::span{text.data(), text.size()});
  return Bytes(t.begin(), t.end());
}

// A jsonb default read from its record: its canonical text, parsed into the
// value the rows use. Text that does not parse, or is not canonical, is a
// damaged catalog.
auto loaded_jsonb_default(const Datum& d, const std::string& column) -> Datum {
  const auto* b = std::get_if<Bytes>(&d);
  if (b == nullptr) return d;
  std::string_view text{reinterpret_cast<const char*>(b->data()), b->size()};
  Bytes value;
  std::string again;
  if (!jsonb::parse_value(text, value).ok() || !jsonb::value_text(value, again).ok() || again != text) {
    corrupt("default of jsonb column " + column + " is not a jsonb value's canonical text");
  }
  return value;
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
      write_datum(e, stored_default(c));
      e.u8(c.hidden ? 1 : 0);
      if (c.clock_default) {
        e.u8(static_cast<std::uint8_t>(c.clock_default->fn));
        e.u8(c.clock_default->precision);
      }
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
  w.u16(t.checks.size());
  for (const auto& c : t.checks) {
    w.entry([&](RecordWriter& e) {
      e.str(c.name);
      e.blob(std::as_bytes(std::span{c.expr}));
    });
  }
  w.u32(t.schema);
  return w.bytes();
}

namespace {

// A record's format: `format` or, read as an object of public, 1. Any other
// is a later build's.
auto record_format(RecordReader& r, std::uint16_t format, std::string_view what) -> std::uint32_t {
  auto f = r.u16();
  if (f != format && f != 1) {
    throw std::runtime_error("catalog " + std::string{what} + " record format " + std::to_string(f) + " is not " +
                             std::to_string(format) + ": the database was written by a later SQL-layer build");
  }
  return f;
}

}  // namespace

// A view record: u16 format, u32 id, str name, blob definition,
// u16 ndeps { u32 id }, u16 ncols { str name }; format 2 then u32 schema,
// u8 check_option, u16 nnames { u8 kind, str name, u32 schema }.
// Append-only, as a table's.
auto serialize_view(const View& v) -> Bytes {
  RecordWriter w;
  w.u16(kViewRecordFormat);
  w.u32(v.id);
  w.str(v.name);
  w.blob(std::as_bytes(std::span{v.definition}));
  w.u16(v.depends_on.size());
  for (auto id : v.depends_on) w.u32(id);
  w.u16(v.columns.size());
  for (const auto& c : v.columns) w.str(c);
  w.u32(v.schema);
  w.u8(v.check_option);
  w.u16(v.names.size());
  for (const auto& n : v.names) {
    w.u8(static_cast<std::uint8_t>(n.kind));
    w.str(n.name);
    w.u32(n.schema);
  }
  return w.bytes();
}

auto parse_view(BytesView in) -> View {
  RecordReader r{in};
  auto format = record_format(r, kViewRecordFormat, "view");
  View v;
  v.id = r.u32();
  v.name = r.str();
  v.definition = as_string(r.blob());
  auto ndeps = r.u16();
  for (std::uint32_t i = 0; i < ndeps; ++i) v.depends_on.push_back(r.u32());
  auto ncols = r.u16();
  for (std::uint32_t i = 0; i < ncols; ++i) v.columns.push_back(r.str());
  v.public_names = format != kViewRecordFormat;
  if (format == kViewRecordFormat) {
    v.schema = r.u32();
    v.check_option = r.u8();
    auto nnames = r.u16();
    for (std::uint32_t i = 0; i < nnames; ++i) {
      ViewName n;
      auto kind = r.u8();
      if (kind != static_cast<std::uint8_t>(ViewName::Kind::Relation)) corrupt("view " + v.name + " stores a name of an unknown kind");
      n.name = r.str();
      n.schema = r.u32();
      if (!valid_name(n.name)) corrupt("view " + v.name + " stores an empty name, or one holding a NUL byte");
      if (std::ranges::contains(v.names, n.name, &ViewName::name)) corrupt("view " + v.name + " stores a name twice");
      v.names.push_back(std::move(n));
    }
    if (v.check_option > kCascadedCheckOption) corrupt("view " + v.name + " has a check option this build does not know");
  }
  r.expect_end("record of view " + v.name);
  if (!valid_name(v.name)) corrupt("a view's name is empty or holds a NUL byte");
  if (v.definition.empty() || v.columns.empty()) corrupt("view " + v.name + " has no definition or no columns");
  return v;
}

// A procedure record: u16 format, u32 id, str name, blob definition,
// u16 nargs { u8 mode, str name, u8 kind, u32 length, u8 precision, u8 scale };
// format 2 then u32 schema.
auto serialize_procedure(const Procedure& p) -> Bytes {
  RecordWriter w;
  w.u16(kProcedureRecordFormat);
  w.u32(p.id);
  w.str(p.name);
  w.blob(std::as_bytes(std::span{p.definition}));
  w.u16(p.args.size());
  for (const auto& a : p.args) {
    w.u8(static_cast<std::uint8_t>(a.mode));
    w.str(a.name);
    w.u8(static_cast<std::uint8_t>(a.type.kind));
    w.u32(a.type.length);
    w.u8(a.type.precision);
    w.u8(a.type.scale);
  }
  w.u32(p.schema);
  return w.bytes();
}

// A role record: u16 format, u32 id, str name.
auto serialize_role(const Role& role) -> Bytes {
  RecordWriter w;
  w.u16(kRoleRecordFormat);
  w.u32(role.id);
  w.str(role.name);
  return w.bytes();
}

auto parse_role(BytesView in) -> Role {
  RecordReader r{in};
  auto format = r.u16();
  if (format != kRoleRecordFormat) {
    throw std::runtime_error("catalog role record format " + std::to_string(format) + " is not " +
                             std::to_string(kRoleRecordFormat) + ": the database was written by a later SQL-layer build");
  }
  Role role;
  role.id = r.u32();
  role.name = r.str();
  r.expect_end("record of role " + role.name);
  if (!valid_name(role.name)) corrupt("a role's name is empty or holds a NUL byte");
  return role;
}

// A type record: u16 format, u32 id, str name, u8 kind (1 shell, 2 domain),
// and a domain's base type: u8 kind, u32 length, u8 precision, u8 scale;
// format 2 then u32 schema.
auto serialize_type(const TypeObject& t) -> Bytes {
  RecordWriter w;
  w.u16(kTypeRecordFormat);
  w.u32(t.id);
  w.str(t.name);
  w.u8(static_cast<std::uint8_t>(t.kind));
  if (t.kind == TypeObject::Kind::Domain) {
    w.u8(static_cast<std::uint8_t>(t.base.kind));
    w.u32(t.base.length);
    w.u8(t.base.precision);
    w.u8(t.base.scale);
  }
  w.u32(t.schema);
  return w.bytes();
}

auto parse_type(BytesView in) -> TypeObject {
  RecordReader r{in};
  auto format = record_format(r, kTypeRecordFormat, "type");
  TypeObject t;
  t.id = r.u32();
  t.name = r.str();
  auto kind = r.u8();
  if (kind != 1 && kind != 2) corrupt("type " + t.name + " has an unknown kind");
  t.kind = static_cast<TypeObject::Kind>(kind);
  if (t.kind == TypeObject::Kind::Domain) {
    t.base.kind = static_cast<SqlKind>(r.u8());
    t.base.length = r.u32();
    t.base.precision = r.u8();
    t.base.scale = r.u8();
    if (!sql_type_valid(t.base)) corrupt("domain " + t.name + " has an invalid type");
  }
  if (format == kTypeRecordFormat) t.schema = r.u32();
  r.expect_end("record of type " + t.name);
  if (!valid_name(t.name)) corrupt("a type's name is empty or holds a NUL byte");
  return t;
}

// A sequence record: u16 format, u32 id, str name; format 2 then u32 schema.
auto serialize_sequence(const Sequence& s) -> Bytes {
  RecordWriter w;
  w.u16(kSequenceRecordFormat);
  w.u32(s.id);
  w.str(s.name);
  w.u32(s.schema);
  return w.bytes();
}

auto parse_sequence(BytesView in) -> Sequence {
  RecordReader r{in};
  auto format = record_format(r, kSequenceRecordFormat, "sequence");
  Sequence s;
  s.id = r.u32();
  s.name = r.str();
  if (format == kSequenceRecordFormat) s.schema = r.u32();
  r.expect_end("record of sequence " + s.name);
  if (!valid_name(s.name)) corrupt("a sequence's name is empty or holds a NUL byte");
  return s;
}

// A schema record: u16 format, u32 id, str name, u32 owner.
auto serialize_schema(const Schema& s) -> Bytes {
  RecordWriter w;
  w.u16(kSchemaRecordFormat);
  w.u32(s.id);
  w.str(s.name);
  w.u32(s.owner);
  return w.bytes();
}

auto parse_schema(BytesView in) -> Schema {
  RecordReader r{in};
  auto format = r.u16();
  if (format != kSchemaRecordFormat) {
    throw std::runtime_error("catalog schema record format " + std::to_string(format) + " is not " +
                             std::to_string(kSchemaRecordFormat) + ": the database was written by a later SQL-layer build");
  }
  Schema s;
  s.id = r.u32();
  s.name = r.str();
  s.owner = r.u32();
  r.expect_end("record of schema " + s.name);
  if (!valid_name(s.name)) corrupt("a schema's name is empty or holds a NUL byte");
  return s;
}

// An ACL record: u16 format, u32 object id, u8 is_default (format 2), u32 n
// { u32 grantee, u32 column, u16 privileges, u16 grant options, u32 grantor }.
// `is_default`: the owner's default privileges, no entry: only a schema's
// record is ever so (docs/catalog_design.md, *Schema owner and privileges*).
auto serialize_acl(std::uint32_t object, const std::vector<AclEntry>& acl, bool is_default) -> Bytes {
  if (is_default && !acl.empty()) throw std::logic_error("a default ACL with entries");
  RecordWriter w;
  w.u16(kAclRecordFormat);
  w.u32(object);
  w.u8(is_default ? 1 : 0);
  w.u32(static_cast<std::uint32_t>(acl.size()));
  for (const auto& e : acl) {
    w.u32(e.grantee);
    w.u32(e.column);
    w.u16(e.privileges);
    w.u16(e.grant_options);
    w.u32(e.grantor);
  }
  return w.bytes();
}

auto parse_acl(BytesView in) -> ParsedAcl {
  RecordReader r{in};
  auto format = record_format(r, kAclRecordFormat, "privileges");
  auto object = r.u32();
  auto is_default = false;
  if (format == kAclRecordFormat) {
    auto d = r.u8();
    if (d > 1) corrupt("privileges of object " + std::to_string(object) + " hold an invalid default flag");
    is_default = d == 1;
  }
  auto n = r.u32();
  std::vector<AclEntry> acl;
  for (std::uint32_t i = 0; i < n; ++i) {
    AclEntry e;
    e.grantee = r.u32();
    e.column = r.u32();
    e.privileges = r.u16();
    e.grant_options = r.u16();
    e.grantor = r.u32();
    // Which privileges the object takes is checked at load, by its kind.
    if (e.privileges == 0 || (e.privileges & ~(privilege::kAll | privilege::kUsage | privilege::kCreate)) != 0 ||
        (e.grant_options & ~e.privileges) != 0) {
      corrupt("privileges of object " + std::to_string(object) + " hold an invalid entry");
    }
    if (!acl.empty() && std::tuple{acl.back().column, acl.back().grantee} >= std::tuple{e.column, e.grantee}) {
      corrupt("privileges of object " + std::to_string(object) + " are not sorted, or repeat an entry");
    }
    acl.push_back(e);
  }
  r.expect_end("privileges record of object " + std::to_string(object));
  // A format-1 record is never empty; only a schema's of format 2 may be
  // (checked at load, by its object's kind).
  if (acl.empty() && format != kAclRecordFormat) corrupt("privileges record of object " + std::to_string(object) + " is empty");
  if (is_default && !acl.empty()) corrupt("privileges of object " + std::to_string(object) + " are the default and hold entries");
  return {object, std::move(acl), is_default, format == kAclRecordFormat};
}

auto parse_procedure(BytesView in) -> Procedure {
  RecordReader r{in};
  auto format = record_format(r, kProcedureRecordFormat, "procedure");
  Procedure p;
  p.id = r.u32();
  p.name = r.str();
  p.definition = as_string(r.blob());
  auto nargs = r.u16();
  std::set<std::string, std::less<>> names;
  for (std::uint32_t i = 0; i < nargs; ++i) {
    ProcedureArg a;
    auto mode = r.u8();
    if (mode < 1 || mode > 3) corrupt("argument of procedure " + p.name + " has an unknown mode");
    a.mode = static_cast<ProcedureArg::Mode>(mode);
    a.name = r.str();
    a.type.kind = static_cast<SqlKind>(r.u8());
    a.type.length = r.u32();
    a.type.precision = r.u8();
    a.type.scale = r.u8();
    if (!sql_type_valid(a.type)) corrupt("argument " + a.name + " of procedure " + p.name + " has an invalid type");
    if (a.name.empty() || !names.insert(a.name).second) {
      corrupt("procedure " + p.name + " has an unnamed or repeated argument");
    }
    p.args.push_back(std::move(a));
  }
  if (format == kProcedureRecordFormat) p.schema = r.u32();
  r.expect_end("record of procedure " + p.name);
  if (!valid_name(p.name) || p.definition.empty()) corrupt("a procedure has no name or no definition, or a NUL in its name");
  return p;
}

auto parse_table(BytesView in) -> Table {
  RecordReader r{in};
  auto format = r.u16();
  if (format != Catalog::kRecordFormat && format != Catalog::kPublicRecordFormat) {
    throw std::runtime_error(
        "catalog record format " + std::to_string(format) + " is not " + std::to_string(Catalog::kRecordFormat) +
        (format < Catalog::kPublicRecordFormat
             ? ": the database was written by an earlier SQL-layer build, whose row and key formats this build "
               "does not read"
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
    auto hidden = e.u8();
    // A clock default's function and precision trail the entry, written only for one.
    if (!e.at_end()) {
      auto fn = e.u8();
      auto precision = e.u8();
      if (fn < 1 || fn > ClockDefault::kLastFn || precision > 6 || !std::holds_alternative<std::monostate>(c.default_value)) {
        corrupt("column " + c.name + " of table " + t.name + " has a bad clock default");
      }
      c.clock_default = ClockDefault{static_cast<ClockDefault::Fn>(fn), precision};
    }
    e.expect_end("column entry of table " + t.name);
    if (!sql_type_valid(c.sql)) corrupt("column " + c.name + " of table " + t.name + " has an invalid type");
    if (nullable > 1) corrupt("column " + c.name + " of table " + t.name + " has a bad nullable flag");
    if (hidden > 1) corrupt("column " + c.name + " of table " + t.name + " has a bad hidden flag");
    c.type = phys_type(c.sql);
    c.nullable = nullable == 1;
    c.hidden = hidden == 1;
    if (!datum_fits(c.default_value, c.type)) corrupt("default of column " + c.name + " is not of its type");
    if (c.sql.kind == SqlKind::Jsonb) c.default_value = loaded_jsonb_default(c.default_value, c.name);
    if (c.clock_default && !clock_assignable(c.clock_default->fn, c.sql.kind)) {
      corrupt("clock default of column " + c.name + " is not of its type");
    }
    if (c.id >= t.next_col_id || t.column(c.id) != nullptr || (!c.hidden && t.column(c.name) != nullptr)) {
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
  auto nchecks = r.u16();
  for (std::uint32_t i = 0; i < nchecks; ++i) {
    RecordReader e{r.blob()};
    CheckConstraint c;
    c.name = e.str();
    auto text = e.blob();
    c.expr.assign(reinterpret_cast<const char*>(text.data()), text.size());
    e.expect_end("CHECK entry of table " + t.name);
    if (c.name.empty() || c.expr.empty() || std::ranges::contains(t.checks, c.name, &CheckConstraint::name)) {
      corrupt("table " + t.name + " has a malformed CHECK");
    }
    t.checks.push_back(std::move(c));
  }
  if (format == Catalog::kRecordFormat) t.schema = r.u32();
  r.expect_end("record of table " + t.name);
  if (!valid_name(t.name) || !std::ranges::all_of(t.indexes, [](const Index& ix) { return valid_name(ix.name); })) {
    corrupt("a table's or index's name is empty or holds a NUL byte");
  }

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
  // A hidden column is the row id: the whole primary key, a BIGINT with no
  // name and no default that no index names, and the table's only one.
  auto row_id_ok = [&](const Column& c) {
    auto indexed = std::ranges::any_of(
        t.indexes, [&](const Index& ix) { return std::ranges::contains(ix.parts, c.id, &IndexPart::col_id); });
    return t.pk_col_ids == std::vector{c.id} && c.sql.kind == SqlKind::BigInt && c.name.empty() &&
           std::holds_alternative<std::monostate>(c.default_value) && !indexed;
  };
  auto hidden = std::ranges::count_if(t.columns, &Column::hidden);
  if (hidden > 1 || (hidden == 1 && !row_id_ok(*std::ranges::find_if(t.columns, &Column::hidden)))) {
    corrupt("table " + t.name + " has a malformed row id");
  }
  if (t.layout.columns.size() != t.columns.size()) corrupt("current layout of table " + t.name + " misses a column");
  return t;
}

// ------------------------------------------------------------------ lookups

auto Table::storage_id() const -> std::uint32_t { return source != 0 ? source : id; }

auto Table::column(std::string_view n) const -> const Column* {
  auto it = std::ranges::find_if(columns, [&](const Column& c) { return !c.hidden && c.name == n; });
  return it == columns.end() ? nullptr : &*it;
}

auto Table::column(std::uint32_t cid) const -> const Column* {
  auto it = std::ranges::find(columns, cid, &Column::id);
  return it == columns.end() ? nullptr : &*it;
}

auto Table::index(std::string_view n) const -> const Index* {
  auto it = std::ranges::find(indexes, n, &Index::name);
  return it == indexes.end() ? nullptr : &*it;
}

auto Table::row_id() const -> const Column* {
  if (pk_col_ids.size() != 1) return nullptr;
  const auto* c = column(pk_col_ids[0]);
  return c != nullptr && c->hidden ? c : nullptr;
}

namespace {

// A key of [from, to) refuses the catalog, named by its first bytes. Two
// record reads per range at most (Snapshot::count_keys), whatever the
// database holds.
void refuse_keys_in(const Snapshot& snap, BytesView from, BytesView to, std::size_t shown_bytes, std::string_view what) {
  if (snap.count_keys(from, to, 1) == 0) return;
  auto keys = snap.keys_from({}, from);
  auto first = Bytes{*keys.begin()};
  auto shown = std::string{};
  for (auto b : BytesView{first}.first(std::min(first.size(), shown_bytes))) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    auto v = std::to_integer<std::size_t>(b);
    shown += shown.empty() ? "" : " ";
    shown += kHex[v >> 4];
    shown += kHex[v & 0xF];
  }
  corrupt("key " + shown + " is " + std::string{what} + " this build does not know: the database was written by a later SQL-layer build");
}

void refuse_unknown_keys(const Snapshot& snap) {
  auto ns = std::byte{kNsCatalog};
  auto next = [](std::byte b) { return static_cast<std::byte>(std::to_integer<std::uint8_t>(b) + 1); };
  // The namespace's own key and sub-prefix 00.
  refuse_keys_in(snap, Bytes{ns}, Bytes{ns, kSubCounter}, 2, "under a catalog sub-prefix");
  // Under the counters' sub-prefix, the counters' keys alone.
  refuse_keys_in(snap, Bytes{ns, kSubCounter}, counter_key(kCounterNextId), 4, "a catalog counter");
  for (auto c = kCounterNextId; c <= kCounterLast; c = next(c)) {
    auto past = c == kCounterLast ? Bytes{ns, next(kSubCounter)} : counter_key(next(c));
    refuse_keys_in(snap, Bytes{ns, kSubCounter, c, std::byte{0}}, past, 4, "a catalog counter");
  }
  static_assert(std::to_integer<std::uint8_t>(kSubLast) < 0xFF);
  refuse_keys_in(snap, Bytes{ns, next(kSubLast)}, Bytes{next(ns)}, 2, "under a catalog sub-prefix");
}

}  // namespace

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
  // No key outside the known set: the counters, read by key, and each
  // sub-prefix after them. A record kind or counter of a later build is
  // refused, never ignored.
  refuse_unknown_keys(snap);
  // Each record kind under its sub-prefix, each under its own id's key.
  auto records = [&](std::byte sub, auto parse, auto& into, std::string_view what) {
    auto prefix = Bytes{std::byte{kNsCatalog}, sub};
    for (const auto& e : snap.iter_from({}, prefix)) {
      if (!has_prefix(e.key, prefix)) break;
      auto parsed = parse(e.value);
      if (e.key.size() != prefix.size() + 4 || be_value(e.key.subspan(prefix.size())) != parsed.id) {
        corrupt("record of " + std::string{what} + " " + parsed.name + " is under another id's key");
      }
      auto id = parsed.id;
      into.emplace(id, std::move(parsed));
    }
  };
  records(kSubTable, parse_table, cat.tables_, "table");
  records(kSubView, parse_view, cat.views_, "view");
  records(kSubProcedure, parse_procedure, cat.procedures_, "procedure");
  records(kSubRole, parse_role, cat.roles_, "role");
  records(kSubType, parse_type, cat.types_, "type");
  records(kSubSequence, parse_sequence, cat.sequences_, "sequence");
  records(kSubSchema, parse_schema, cat.schemas_, "schema");
  // The ACL records; a schema's default one holds no entry and is not in
  // acls_, which holds only entries.
  std::set<std::uint32_t> acl_records;
  auto acls = Bytes{std::byte{kNsCatalog}, kSubAcl};
  for (const auto& e : snap.iter_from({}, acls)) {
    if (!has_prefix(e.key, acls)) break;
    auto parsed = parse_acl(e.value);
    if (e.key.size() != acls.size() + 4 || be_value(e.key.subspan(acls.size())) != parsed.object) {
      corrupt("privileges of object " + std::to_string(parsed.object) + " are under another id's key");
    }
    auto object = parsed.object;
    acl_records.insert(object);
    if (cat.schemas_.contains(object) && !parsed.schema_format) {
      corrupt("privileges of schema " + cat.schemas_.at(object).name + " are of an earlier format");
    }
    if (parsed.entries.empty()) {
      if (!cat.schemas_.contains(object)) corrupt("privileges record of object " + std::to_string(object) + " is empty");
      continue;
    }
    cat.acls_.emplace(object, std::move(parsed.entries));
  }
  // Schemas: each named once, never a name PostgreSQL reserves, owned by a
  // role, with its ACL record.
  for (const auto& [id, sc] : cat.schemas_) {
    if (sc.name == "public" || sc.name == "information_schema" || sc.name.starts_with("pg_")) {
      corrupt("schema " + sc.name + " has a reserved name");
    }
    if (sc.owner != kBootstrapRole && !cat.roles_.contains(sc.owner)) corrupt("schema " + sc.name + " is owned by no role");
    if (!acl_records.contains(id)) corrupt("schema " + sc.name + " has no privileges record");
  }
  auto schema_ok = [&](std::uint32_t schema) { return schema == kPublicSchema || cat.schemas_.contains(schema); };
  auto in_schema = [&](std::uint32_t schema, const std::string& what) {
    if (!schema_ok(schema)) corrupt(what + " is in a schema that does not exist");
  };
  for (const auto& [id, t] : cat.tables_) in_schema(t.schema, "table " + t.name);
  for (const auto& [id, w] : cat.views_) in_schema(w.schema, "view " + w.name);
  for (const auto& [id, p] : cat.procedures_) in_schema(p.schema, "procedure " + p.name);
  for (const auto& [id, t] : cat.types_) in_schema(t.schema, "type " + t.name);
  for (const auto& [id, q] : cat.sequences_) in_schema(q.schema, "sequence " + q.name);
  cat.rebuild_lookups();

  // Every id below the counter and used once; every name entry names its
  // object, and every object has its entry.
  std::set<std::uint32_t> ids;
  auto use = [&](std::uint32_t id) {
    if (id == 0 || id >= cat.next_id_ || !ids.insert(id).second) corrupt("object id " + std::to_string(id) + " reused");
  };
  for (const auto& [id, sc] : cat.schemas_) use(id);
  std::size_t objects = 0;
  for (const auto& [id, t] : cat.tables_) {
    use(id);
    use(t.layout_id);
    for (const auto& [lid, l] : t.retired_layouts) use(lid);
    for (const auto& ix : t.indexes) use(ix.id);
    objects += 1 + t.indexes.size();
  }
  for (const auto& [id, def] : cat.views_) {
    use(id);
    for (auto dep : def.depends_on) {
      if (!cat.tables_.contains(dep) && !cat.views_.contains(dep)) corrupt("view " + def.name + " depends on a missing object");
    }
    // Each stored name resolves, in the schema it names, to an object the
    // view depends on: the object it was created against.
    for (const auto& n : def.names) {
      auto at = QualifiedName{n.schema, n.name};
      const auto* t = cat.table(at);
      const auto* w = cat.view(at);
      auto id_of = t != nullptr ? t->id : w != nullptr ? w->id : 0;
      if (!schema_ok(n.schema) || id_of == 0 || !std::ranges::contains(def.depends_on, id_of)) {
        corrupt("view " + def.name + " stores a name, " + n.name + ", of no relation it depends on");
      }
    }
  }
  objects += cat.views_.size();
  for (const auto& [id, s] : cat.sequences_) use(id);
  objects += cat.sequences_.size();
  // A name entry's key: the name in public, or the marker, a schema's id
  // (never public's, and of a schema that exists) and the name.
  auto entry_name = [&](BytesView suffix, std::string_view what) -> QualifiedName {
    QualifiedName out{kPublicSchema, {}};
    if (!suffix.empty() && suffix[0] == kSchemaMarker) {
      if (suffix.size() < 6) corrupt(std::string{what} + " name entry of a schema is too short");
      out.schema = static_cast<std::uint32_t>(be_value(suffix.subspan(1, 4)));
      if (out.schema == kPublicSchema || !cat.schemas_.contains(out.schema)) {
        corrupt(std::string{what} + " name entry is in a schema that does not exist");
      }
      suffix = suffix.subspan(5);
    }
    out.name = as_string(suffix);
    if (!valid_name(out.name)) corrupt(std::string{what} + " name entry is empty or holds a NUL byte");
    return out;
  };
  auto entry_id = [](BytesView value, std::uint8_t kind) -> std::optional<std::uint32_t> {
    if (value.size() != 5 || std::to_integer<std::uint8_t>(value[0]) != kind) return std::nullopt;
    return static_cast<std::uint32_t>(be_value(value.subspan(1)));
  };
  std::size_t entries = 0;
  auto names = Bytes{std::byte{kNsCatalog}, kSubName};
  for (const auto& e : snap.iter_from({}, names)) {
    if (!has_prefix(e.key, names)) break;
    auto name = entry_name(e.key.subspan(names.size()), "relation");
    auto it = cat.names_.find(name);
    auto kind = it == cat.names_.end()                     ? 0
                : cat.tables_.contains(it->second)    ? kKindTable
                : cat.views_.contains(it->second)     ? kKindView
                : cat.sequences_.contains(it->second) ? kKindSequence
                                                      : kKindIndex;
    if (it == cat.names_.end() || entry_id(e.value, static_cast<std::uint8_t>(kind)) != it->second) {
      corrupt("name entry " + name.name + " does not match the records");
    }
    ++entries;
  }
  if (entries != objects || cat.names_.size() != objects) corrupt("a table, index, view or sequence has no name entry");
  // The type namespace: each domain or shell type has its entry, and none
  // shares a name with a row type of a table or view of its schema.
  for (const auto& [id, t] : cat.types_) {
    use(id);
    auto row_type = QualifiedName{t.schema, t.name};
    if (cat.table(row_type) != nullptr || cat.view(row_type) != nullptr) corrupt("type " + t.name + " shares a relation's row type name");
  }
  // The entries of a namespace with an entry per object: each the one of
  // its object, and none missing.
  auto namespace_entries = [&](std::byte sub, std::uint8_t kind, const auto& lookup, std::size_t count, std::string_view what) {
    std::size_t n = 0;
    auto prefix = Bytes{std::byte{kNsCatalog}, sub};
    for (const auto& e : snap.iter_from({}, prefix)) {
      if (!has_prefix(e.key, prefix)) break;
      auto name = entry_name(e.key.subspan(prefix.size()), what);
      auto it = lookup.find(name);
      if (it == lookup.end() || entry_id(e.value, kind) != it->second) {
        corrupt(std::string{what} + " name entry " + name.name + " does not match the records");
      }
      ++n;
    }
    if (n != count) corrupt("a " + std::string{what} + " has no name entry");
  };
  namespace_entries(kSubTypeName, kKindType, cat.type_names_, cat.types_.size(), "type");
  for (const auto& [id, p] : cat.procedures_) use(id);
  namespace_entries(kSubProcedureName, kKindProcedure, cat.procedure_names_, cat.procedures_.size(), "procedure");
  for (const auto& [id, r] : cat.roles_) use(id);
  std::size_t role_entries = 0;
  auto role_names = Bytes{std::byte{kNsCatalog}, kSubRoleName};
  for (const auto& e : snap.iter_from({}, role_names)) {
    if (!has_prefix(e.key, role_names)) break;
    auto name = as_string(e.key.subspan(role_names.size()));
    auto it = cat.role_names_.find(name);
    if (it == cat.role_names_.end() || entry_id(e.value, kKindRole) != it->second) {
      corrupt("role name entry " + name + " does not match the records");
    }
    ++role_entries;
  }
  if (role_entries != cat.roles_.size()) corrupt("a role has no name entry");
  std::size_t schema_entries = 0;
  auto schema_names = Bytes{std::byte{kNsCatalog}, kSubSchemaName};
  for (const auto& e : snap.iter_from({}, schema_names)) {
    if (!has_prefix(e.key, schema_names)) break;
    auto name = as_string(e.key.subspan(schema_names.size()));
    auto it = cat.schema_names_.find(name);
    if (it == cat.schema_names_.end() || entry_id(e.value, kKindSchema) != it->second) {
      corrupt("schema name entry " + name + " does not match the records");
    }
    ++schema_entries;
  }
  if (schema_entries != cat.schemas_.size()) corrupt("a schema has no name entry");
  // Each ACL is of a table, view, sequence, type or schema, its privileges
  // that kind's, its columns a table's or view's, its grantees PUBLIC or a
  // role, its grantor the bootstrap superuser or a role.
  for (const auto& [object, acl] : cat.acls_) {
    const auto* t = cat.table(object);
    const auto* v = cat.view(object);
    auto relation = t != nullptr || v != nullptr;
    auto allowed = relation                            ? privilege::kAll
                   : cat.sequences_.contains(object) ? privilege::kSequence
                   : cat.schemas_.contains(object)   ? privilege::kSchema
                                                     : privilege::kType;
    if (!relation && !cat.sequences_.contains(object) && !cat.types_.contains(object) && !cat.schemas_.contains(object)) {
      corrupt("privileges of object " + std::to_string(object) + ", no table, view, sequence, type or schema");
    }
    for (const auto& e : acl) {
      auto bits_ok = (e.privileges & ~(e.column == kWholeObject ? allowed : privilege::kColumns)) == 0;
      auto column_ok = bits_ok && (e.column == kWholeObject ||
                                   (relation && (t != nullptr ? t->column(e.column) != nullptr && !t->column(e.column)->hidden
                                                              : e.column < v->columns.size())));
      auto grantee_ok = e.grantee == kPublic || cat.roles_.contains(e.grantee);
      auto grantor_ok = e.grantor == kBootstrapRole || cat.roles_.contains(e.grantor);
      if (!column_ok || !grantee_ok || !grantor_ok) {
        corrupt("privileges of object " + std::to_string(object) + " name a missing column or role");
      }
    }
  }
  return cat;
}

auto Catalog::clone() const -> Catalog {
  Catalog c;
  c.next_id_ = next_id_;
  c.version_ = version_;
  c.tables_ = tables_;
  c.views_ = views_;
  c.procedures_ = procedures_;
  c.roles_ = roles_;
  c.acls_ = acls_;
  c.types_ = types_;
  c.sequences_ = sequences_;
  c.schemas_ = schemas_;
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
  procedure_names_.clear();
  for (const auto& [id, p] : procedures_) {
    if (!procedure_names_.emplace(QualifiedName{p.schema, p.name}, id).second) corrupt("procedure name " + p.name + " is used twice");
  }
  schema_names_.clear();
  for (const auto& [id, sc] : schemas_) {
    if (!schema_names_.emplace(sc.name, id).second) corrupt("schema name " + sc.name + " is used twice");
  }
  role_names_.clear();
  for (const auto& [id, r] : roles_) {
    if (!role_names_.emplace(r.name, id).second) corrupt("role name " + r.name + " is used twice");
  }
  type_names_.clear();
  for (const auto& [id, t] : types_) {
    if (!type_names_.emplace(QualifiedName{t.schema, t.name}, id).second) corrupt("type name " + t.name + " is used twice");
  }
  key_layouts_.clear();
  // Each record names its schema; an index's name is in its table's.
  auto name_taken = [&](std::uint32_t schema, const std::string& n, std::uint32_t id) {
    if (!names_.emplace(QualifiedName{schema, n}, id).second) corrupt("name " + n + " is used twice");
  };
  for (const auto& [id, v] : views_) name_taken(v.schema, v.name, id);
  for (const auto& [id, s] : sequences_) name_taken(s.schema, s.name, id);
  for (const auto& [id, t] : tables_) {
    name_taken(t.schema, t.name, id);
    std::vector<KeyPart> pk_parts;
    for (auto cid : t.pk_col_ids) pk_parts.push_back(key_part(*t.column(cid)));
    key_layouts_[id] = KeyLayout{kNsRow, &t, nullptr, pk_parts, 0};
    for (const auto& ix : t.indexes) {
      name_taken(t.schema, ix.name, ix.id);
      KeyLayout kl{kNsIndex, &t, &ix, {}, ix.parts.size()};
      for (const auto& p : ix.parts) {
        const auto* c = t.column(p.col_id);
        kl.parts.push_back(key_part(*c, c->nullable, p.desc));
      }
      kl.parts.insert(kl.parts.end(), pk_parts.begin(), pk_parts.end());
      key_layouts_[ix.id] = std::move(kl);
    }
  }
}

auto Catalog::estimated_bytes() const noexcept -> std::uint64_t {
  auto out = kEstimateBase;
  for (const auto& [id, t] : tables_) {
    out += kEstimateTable + t.name.size();
    for (const auto& c : t.columns) out += kEstimateColumn + c.name.size();
    for (const auto& ix : t.indexes) out += kEstimateIndex + ix.name.size();
    for (const auto& ch : t.checks) out += kEstimateObject + ch.name.size() + ch.expr.size();
  }
  for (const auto& [id, v] : views_) out += kEstimateObject + v.name.size() + v.definition.size();
  for (const auto& [id, p] : procedures_) out += kEstimateObject + p.name.size() + p.definition.size();
  for (const auto& [id, r] : roles_) out += kEstimateObject + r.name.size();
  for (const auto& [id, t] : types_) out += kEstimateObject + t.name.size();
  for (const auto& [id, s] : sequences_) out += kEstimateObject + s.name.size();
  for (const auto& [id, acl] : acls_) out += kEstimateObject + acl.size() * sizeof(AclEntry);
  for (const auto& [id, sc] : schemas_) out += kEstimateObject + sc.name.size();
  return out;
}

auto Catalog::table(const QualifiedName& name) const -> const Table* {
  auto it = names_.find(name);
  if (it == names_.end()) return nullptr;
  return table(it->second);
}

auto Catalog::table(std::uint32_t id) const -> const Table* {
  auto it = tables_.find(id);
  return it == tables_.end() ? nullptr : &it->second;
}

auto Catalog::view(const QualifiedName& name) const -> const View* {
  auto it = names_.find(name);
  return it == names_.end() ? nullptr : view(it->second);
}

auto Catalog::view(std::uint32_t id) const -> const View* {
  auto it = views_.find(id);
  return it == views_.end() ? nullptr : &it->second;
}

auto Catalog::procedure(const QualifiedName& name) const -> const Procedure* {
  auto it = procedure_names_.find(name);
  return it == procedure_names_.end() ? nullptr : &procedures_.at(it->second);
}

auto Catalog::type(const QualifiedName& name) const -> const TypeObject* {
  auto it = type_names_.find(name);
  return it == type_names_.end() ? nullptr : &types_.at(it->second);
}

auto Catalog::sequence(const QualifiedName& name) const -> const Sequence* {
  auto it = names_.find(name);
  if (it == names_.end()) return nullptr;
  auto s = sequences_.find(it->second);
  return s == sequences_.end() ? nullptr : &s->second;
}

auto Catalog::role(std::string_view name) const -> const Role* {
  auto it = role_names_.find(name);
  return it == role_names_.end() ? nullptr : &roles_.at(it->second);
}

auto Catalog::schema(std::string_view name) const -> const Schema* {
  auto it = schema_names_.find(name);
  return it == schema_names_.end() ? nullptr : &schemas_.at(it->second);
}

auto Catalog::schema(std::uint32_t id) const -> const Schema* {
  auto it = schemas_.find(id);
  return it == schemas_.end() ? nullptr : &it->second;
}

auto Catalog::schema_name(std::uint32_t id) const -> std::string {
  if (id == kPublicSchema) return "public";
  const auto* sc = schema(id);
  return sc != nullptr ? sc->name : "schema of id " + std::to_string(id);
}

auto Catalog::written(const QualifiedName& name) const -> std::string {
  return name.schema == kPublicSchema ? name.name : schema_name(name.schema) + "." + name.name;
}

auto Catalog::acl(std::uint32_t object) const -> std::span<const AclEntry> {
  auto it = acls_.find(object);
  return it == acls_.end() ? std::span<const AclEntry>{} : std::span<const AclEntry>{it->second};
}

auto Catalog::find_view_dependent(std::uint32_t id) const -> const View* {
  for (const auto& [vid, v] : views_) {
    if (std::ranges::find(v.depends_on, id) != v.depends_on.end()) return &v;
  }
  return nullptr;
}

auto Catalog::index(const QualifiedName& name) const -> std::pair<const Table*, const Index*> {
  auto it = names_.find(name);
  if (it == names_.end()) return {nullptr, nullptr};
  const auto* kl = key_layout(it->second);
  if (kl == nullptr || kl->index == nullptr) return {nullptr, nullptr};
  return {kl->table, kl->index};
}

auto Catalog::key_layout(std::uint32_t id) const -> const KeyLayout* {
  auto it = key_layouts_.find(id);
  return it == key_layouts_.end() ? nullptr : &it->second;
}

auto Catalog::constants(const Table& t) -> std::vector<PlanConstant> {
  std::vector<PlanConstant> out;
  auto id = [](std::uint32_t v) { return std::to_string(v); };
  auto named = [](const Column& c) { return c.hidden ? std::string{"(row id)"} : c.name; };
  out.push_back({"TBL_" + id(t.id), t.storage_id(), t.name});
  for (const auto& ix : t.indexes) out.push_back({"IDX_" + id(ix.id), ix.id, ix.name});
  for (const auto& c : t.columns) out.push_back({"COL_" + id(t.id) + "_" + id(c.id), c.id, t.name + "." + named(c)});
  // Key parts by position: a table's primary key, then each index's parts
  // and the primary key after them. A column an index names and its primary
  // key suffix repeats keeps its first position.
  auto parts = [&](std::uint32_t key_id, const std::string& key_name, const std::vector<std::uint32_t>& cols) {
    std::vector<std::uint32_t> seen;
    for (std::size_t i = 0; i < cols.size(); ++i) {
      if (std::ranges::find(seen, cols[i]) != seen.end()) continue;
      seen.push_back(cols[i]);
      out.push_back({"PART_" + id(key_id) + "_" + id(cols[i]), static_cast<std::uint32_t>(i),
                     key_name + "." + named(*t.column(cols[i]))});
    }
  };
  parts(t.id, t.name, t.pk_col_ids);
  for (const auto& ix : t.indexes) {
    std::vector<std::uint32_t> cols;
    for (const auto& p : ix.parts) cols.push_back(p.col_id);
    cols.insert(cols.end(), t.pk_col_ids.begin(), t.pk_col_ids.end());
    parts(ix.id, ix.name, cols);
  }
  return out;
}

// --------------------------------------------------------------------- DDL

namespace {

// A name a DDL writes: refused unless valid_name holds (42602). Reachable
// only through the Catalog API: the parser's names are never empty and
// libpg_query reads no NUL byte.
void check_name(std::string_view name) {
  if (!valid_name(name)) throw DdlRefused("invalid name: a name is not empty and holds no NUL byte", kInvalidName);
}

// A name as PostgreSQL's quote_identifier writes it in a message: bare when
// it is lower-case letters, digits and underscores not led by a digit,
// quoted otherwise. (Keywords, which it quotes too, are written bare.)
auto quoted_ident(std::string_view name) -> std::string {
  auto plain = !name.empty() && !(name[0] >= '0' && name[0] <= '9') && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
  if (plain) return std::string{name};
  std::string out{"\""};
  for (auto c : name) {
    out += c;
    if (c == '"') out += c;
  }
  return out + "\"";
}

// PostgreSQL's name for an argument type in a function's signature, as its
// DROP SCHEMA DETAIL writes it: format_type without the modifiers.
auto signature_type(SqlType t) -> std::string_view {
  switch (t.kind) {
    case SqlKind::SmallInt: return "smallint";
    case SqlKind::Integer: return "integer";
    case SqlKind::BigInt: return "bigint";
    case SqlKind::Real: return "real";
    case SqlKind::Double: return "double precision";
    case SqlKind::Boolean: return "boolean";
    case SqlKind::Decimal:
    case SqlKind::Numeric: return "numeric";
    case SqlKind::Jsonb: return "jsonb";
    case SqlKind::Text: return "text";
    case SqlKind::Varchar: return "character varying";
    case SqlKind::Char: return "character";
    case SqlKind::Name: return "name";
    case SqlKind::Bytea: return "bytea";
    case SqlKind::Date: return "date";
    case SqlKind::Time: return "time without time zone";
    case SqlKind::TimeTz: return "time with time zone";
    case SqlKind::Timestamp: return "timestamp without time zone";
    case SqlKind::TimestampTz: return "timestamp with time zone";
  }
  return "unknown";
}

}  // namespace

auto Catalog::SchemaDrop::detail(const std::function<std::string(const Object&)>& line) const -> std::string {
  std::string out;
  for (std::size_t i = 0; i < objects.size() && i < kMaxReported; ++i) out += (i == 0 ? "" : "\n") + line(objects[i]);
  if (objects.size() > kMaxReported) {
    auto more = objects.size() - kMaxReported;  // PostgreSQL's ngettext: "object" for one
    out += "\nand " + std::to_string(more) + (more == 1 ? " other object" : " other objects") + " (see server log for list)";
  }
  return out;
}

auto Catalog::schema_drop(const DropSchemaDdl& ddl) const -> SchemaDrop {
  SchemaDrop out;
  // Every name first: a missing one drops nothing (3F000), or is skipped.
  for (const auto& name : ddl.names) {
    if (name == "public" || system_schema(name) != 0) continue;
    const auto* sc = schema(name);
    if (sc == nullptr) {
      if (ddl.if_exists) continue;
      throw DdlRefused("schema \"" + name + "\" does not exist", kInvalidSchemaName);
    }
    if (!std::ranges::contains(out.schemas, sc->id)) out.schemas.push_back(sc->id);
  }
  for (const auto& name : ddl.names) {
    if (name == "pg_catalog" || name == "pg_toast") {
      throw DdlRefused("cannot drop schema " + name + " because it is required by the database system",
                       kDependentObjectsStillExist);
    }
    // public is implicit and information_schema holds nothing here
    // (docs/catalog_design.md, *Differences from PostgreSQL 17*).
    if (name == "public" || name == "information_schema") {
      throw DdlRefused("DROP SCHEMA " + name + " is not supported", kFeatureNotSupported);
    }
  }
  auto qualified = [&](std::uint32_t schema, const std::string& name) {
    return (schema == kPublicSchema ? std::string{} : quoted_ident(schema_name(schema)) + ".") + quoted_ident(name);
  };
  auto described = [&](std::uint32_t id) -> std::string {
    if (const auto* t = table(id)) return "table " + qualified(t->schema, t->name);
    if (const auto* w = view(id)) return "view " + qualified(w->schema, w->name);
    if (const auto it = sequences_.find(id); it != sequences_.end()) return "sequence " + qualified(it->second.schema, it->second.name);
    if (const auto it = types_.find(id); it != types_.end()) return "type " + qualified(it->second.schema, it->second.name);
    const auto& p = procedures_.at(id);
    std::string args;
    for (const auto& a : p.args) args += (args.empty() ? "" : ",") + std::string{signature_type(a.type)};
    return "function " + qualified(p.schema, p.name) + "(" + args + ")";
  };
  std::map<std::uint32_t, std::uint32_t> held;  // object id -> its schema, of the dropped schemas
  auto hold = [&](std::uint32_t id, std::uint32_t schema) {
    if (std::ranges::contains(out.schemas, schema)) held.emplace(id, schema);
  };
  for (const auto& [id, t] : tables_) hold(id, t.schema);
  for (const auto& [id, w] : views_) hold(id, w.schema);
  for (const auto& [id, q] : sequences_) hold(id, q.schema);
  for (const auto& [id, t] : types_) hold(id, t.schema);
  for (const auto& [id, p] : procedures_) hold(id, p.schema);
  std::set<std::uint32_t> listed;
  std::function<void(std::uint32_t)> readers = [&](std::uint32_t id) {
    for (const auto& [vid, w] : views_) {
      if (held.contains(vid) || listed.contains(vid) || !std::ranges::contains(w.depends_on, id)) continue;
      listed.insert(vid);
      out.objects.push_back({vid, described(vid), described(id)});
      readers(vid);
    }
  };
  for (const auto& [id, schema] : held) {
    listed.insert(id);
    out.objects.push_back({id, described(id), "schema " + schema_name(schema)});  // bare, as PostgreSQL 17
    readers(id);
  }
  return out;
}

auto Catalog::next_version(const Ddl& ddl, const RowSource& rows, std::vector<DdlOp>& ops, bool& backfill,
                           std::size_t backfill_limit) const
    -> std::shared_ptr<const Catalog> {
  if (std::holds_alternative<SkippedDdl>(ddl)) return nullptr;
  auto next = std::make_shared<Catalog>(clone());
  next->version_ = version_ + 1;
  auto finish = [&] {
    guard_catalog(ops);
    put_counters(ops, next->next_id_, next->version_);
    next->rebuild_lookups();
    return std::shared_ptr<const Catalog>{std::move(next)};
  };
  // Every name the DDL writes is valid, and every schema it names exists:
  // one the binder resolved may have been dropped since (3F000).
  auto check_qualified = [&](const QualifiedName& n) {
    check_name(n.name);
    if (n.schema != kPublicSchema && !schemas_.contains(n.schema)) {
      throw DdlRefused("schema \"" + schema_name(n.schema) + "\" does not exist", kInvalidSchemaName);
    }
  };
  std::visit(
      [&](const auto& d) {
        using D = std::remove_cvref_t<decltype(d)>;
        // A name a DDL creates; a name it drops, alters or grants on is
        // looked up, and one in a schema that does not exist is not found.
        if constexpr (std::is_same_v<D, CreateTableDdl> || std::is_same_v<D, CreateViewDdl> ||
                      std::is_same_v<D, CreateProcedureDdl> || std::is_same_v<D, CreateDomainDdl> ||
                      std::is_same_v<D, CreateTypeDdl> || std::is_same_v<D, CreateSequenceDdl>) {
          check_qualified(d.name);
        }
        if constexpr (std::is_same_v<D, CreateIndexDdl> || std::is_same_v<D, CreateRoleDdl> ||
                      std::is_same_v<D, CreateSchemaDdl>) {
          check_name(d.name);
        }
      },
      ddl);
  auto name_free = [&](const QualifiedName& name) {
    if (names_.contains(name)) throw DdlRefused("relation \"" + name.name + "\" already exists", kDuplicateTable);
  };
  // A table's or view's row type, a domain and a shell type share one namespace.
  auto type_free = [&](const QualifiedName& name) {
    if (type_names_.contains(name)) throw DdlRefused("type \"" + name.name + "\" already exists", kDuplicateObject);
  };
  auto find_table = [&](const QualifiedName& name) -> const Table& {
    const auto* t = table(name);
    if (t == nullptr && view(name) != nullptr) throw DdlRefused("\"" + name.name + "\" is not a table", kWrongObjectType);
    if (t == nullptr) throw DdlRefused("relation \"" + name.name + "\" does not exist", kUndefinedTable);
    return *t;
  };
  // Each drop's batch and its change to `next`, shared by DROP of one object
  // and DROP SCHEMA … CASCADE: the records, the name entries, the rows and
  // index entries, and the ACL record.
  auto drop_acl = [&](std::uint32_t id) {
    if (acls_.contains(id)) put_op(ops, DdlOp::Kind::Del, acl_record_key(id));
    next->acls_.erase(id);
  };
  auto drop_table = [&](const Table& t) {
    put_op(ops, DdlOp::Kind::Del, table_record_key(t.id));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(QualifiedName{t.schema, t.name}));
    auto row_keys = row_prefix(t.id);
    put_op(ops, DdlOp::Kind::DelRange, row_keys, key_successor(row_keys));
    for (const auto& ix : t.indexes) {
      put_op(ops, DdlOp::Kind::Del, name_entry_key(QualifiedName{t.schema, ix.name}));
      auto p = index_prefix(ix.id);
      put_op(ops, DdlOp::Kind::DelRange, p, key_successor(p));
    }
    drop_acl(t.id);
    next->erase_table(t.id);
  };
  auto drop_view = [&](const View& v) {
    put_op(ops, DdlOp::Kind::Del, view_record_key(v.id));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(QualifiedName{v.schema, v.name}));
    drop_acl(v.id);
    next->views_.erase(v.id);
  };
  auto drop_type = [&](const TypeObject& t) {
    put_op(ops, DdlOp::Kind::Del, type_record_key(t.id));
    put_op(ops, DdlOp::Kind::Del, type_name_key(QualifiedName{t.schema, t.name}));
    drop_acl(t.id);
    next->types_.erase(t.id);
  };
  auto drop_sequence = [&](const Sequence& q) {
    put_op(ops, DdlOp::Kind::Del, sequence_record_key(q.id));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(QualifiedName{q.schema, q.name}));
    drop_acl(q.id);
    next->sequences_.erase(q.id);
  };
  auto drop_procedure = [&](const Procedure& p) {
    put_op(ops, DdlOp::Kind::Del, procedure_record_key(p.id));
    put_op(ops, DdlOp::Kind::Del, procedure_name_key(QualifiedName{p.schema, p.name}));
    drop_acl(p.id);
    next->procedures_.erase(p.id);
  };
  backfill = false;

  if (const auto* ct = std::get_if<CreateTableDdl>(&ddl)) {
    auto name = ct->name.name;
    // IF NOT EXISTS: a name taken now changes nothing. One the binder found
    // taken, and a DDL since has freed, is a conflict with that DDL.
    if (ct->if_not_exists && names_.contains(ct->name)) return nullptr;
    if (ct->if_not_exists && ct->columns.empty()) throw DdlInProgress{};
    // PostgreSQL's order: the statement's own columns and key parts are
    // refused before a name already taken (42701, 42703 over 42P07); a table
    // of no columns and a column of an unsupported type or default, which
    // PostgreSQL runs, after it.
    std::vector<std::string> names;
    for (const auto& spec : ct->columns) {
      auto cname = std::string{spec.name};
      if (std::ranges::find(names, cname) != names.end()) {
        throw DdlRefused("column \"" + cname + "\" specified more than once", kDuplicateColumn);
      }
      names.push_back(std::move(cname));
    }
    for (const auto& p : ct->pk) {
      if (std::ranges::find(names, p) == names.end()) {
        throw DdlRefused("primary key column \"" + p + "\" does not exist", kUndefinedColumn);
      }
      if (std::ranges::count(ct->pk, p) > 1) {
        throw DdlRefused("column \"" + p + "\" appears twice in the primary key", kDuplicateColumn);
      }
    }
    // The UNIQUE constraints, as PostgreSQL takes them: each column present
    // and named once. One on the primary key's columns, in order, is the
    // key; one on an earlier one's is that index, which takes its name if it
    // had none.
    std::vector<UniqueSpec> unique;
    for (const auto& u : ct->unique) {
      for (const auto& c : u.columns) {
        if (std::ranges::find(names, c) == names.end()) {
          throw DdlRefused("column \"" + c + "\" named in key does not exist", kUndefinedColumn);
        }
        if (std::ranges::count(u.columns, c) > 1) {
          throw DdlRefused("column \"" + c + "\" appears twice in unique constraint", kDuplicateColumn);
        }
      }
      if (u.columns == ct->pk) continue;
      if (auto prior = std::ranges::find(unique, u.columns, &UniqueSpec::columns); prior != unique.end()) {
        if (prior->name.empty()) prior->name = u.name;
        continue;
      }
      unique.push_back(u);
    }
    name_free(ct->name);
    type_free(ct->name);
    if (ct->columns.empty()) throw DdlRefused("a table needs columns", kFeatureNotSupported);
    Table t;
    t.name = name;
    std::vector<ColumnDef> defs;
    for (const auto& spec : ct->columns) {
      check_column_spec(spec);
      auto cname = std::string{spec.name};
      auto in_pk = std::ranges::any_of(ct->pk, [&](const std::string& p) { return std::string{p} == cname; });
      Column c{t.next_col_id++, cname, spec.sql, phys_type(spec.sql), spec.nullable && !in_pk, spec.default_value};
      c.clock_default = spec.clock_default;
      defs.emplace_back(c.id, c.sql, c.nullable);
      t.columns.push_back(std::move(c));
    }
    for (const auto& p : ct->pk) {
      refuse_key_type(*t.column(p));
      t.pk_col_ids.push_back(t.column(p)->id);
    }
    if (ct->pk.empty()) {
      // No primary key: a hidden row id is the key, after the columns.
      Column c{t.next_col_id++, "", {SqlKind::BigInt}, PhysType::I64, false, {}, true};
      defs.emplace_back(c.id, c.sql, c.nullable);
      t.pk_col_ids.push_back(c.id);
      t.columns.push_back(std::move(c));
    }
    t.layout = make_layout(defs);
    t.id = reserve_ids(next->next_id_, 2);
    t.layout_id = t.id + 1;
    t.schema = ct->name.schema;
    put_op(ops, DdlOp::Kind::EnsureAbsent, name_entry_key(ct->name));
    put_op(ops, DdlOp::Kind::Put, name_entry_key(ct->name), name_value(kKindTable, t.id));
    // Each a unique index, named in order: as written, or as PostgreSQL
    // chooses, past every relation name and the ones before it.
    auto taken = [&](const std::string& n) {
      return names_.contains(QualifiedName{ct->name.schema, n}) || n == t.name ||
             std::ranges::contains(t.indexes, n, &Index::name);
    };
    for (const auto& u : unique) {
      Index ix;
      ix.name = u.name.empty() ? unique_index_name(t.name, u.columns, taken) : u.name;
      if (taken(ix.name)) throw DdlRefused("relation \"" + ix.name + "\" already exists", kDuplicateTable);
      ix.unique = true;
      for (const auto& c : u.columns) {
        refuse_key_type(*t.column(c));
        ix.parts.push_back({t.column(c)->id, false});
      }
      ix.id = reserve_ids(next->next_id_, 1);
      auto key = name_entry_key(QualifiedName{t.schema, ix.name});
      put_op(ops, DdlOp::Kind::EnsureAbsent, key);
      put_op(ops, DdlOp::Kind::Put, key, name_value(kKindIndex, ix.id));
      t.indexes.push_back(std::move(ix));
    }
    // The CHECKs, named as written or as PostgreSQL's ChooseConstraintName
    // does: table_column_check for one on a column, table_check for the
    // table's, then 1, 2… where a CHECK of the table has the name.
    for (const auto& c : ct->checks) {
      if (c.name.empty()) continue;
      if (std::ranges::contains(t.checks, c.name, &CheckConstraint::name)) {
        throw DdlRefused("constraint \"" + c.name + "\" for relation \"" + t.name + "\" already exists", kDuplicateObject);
      }
      t.checks.push_back({c.name, c.expr});
    }
    for (const auto& c : ct->checks) {
      if (!c.name.empty()) continue;
      auto base = t.name + (c.column.empty() ? "" : "_" + c.column) + "_check";
      auto chosen = base;
      for (auto n = 1; std::ranges::contains(t.checks, chosen, &CheckConstraint::name); ++n) chosen = base + std::to_string(n);
      t.checks.push_back({chosen, c.expr});
    }
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));
    if (!ct->rows.empty()) {
      // CREATE TABLE AS: each row under the next row id, in the same batch.
      if (!ct->pk.empty() || !t.indexes.empty()) throw std::logic_error("CREATE TABLE AS rows for a table with keys");
      backfill = true;
      BackfillBudget budget{t.name, backfill_limit};
      RowWriter w{t.layout};  // every column is set for each row: no reset between them
      const auto& hidden = t.columns.back();
      std::int64_t id = 0;
      for (const auto& values : ct->rows) {
        if (values.size() + 1 != t.columns.size()) throw std::logic_error("CREATE TABLE AS row of the wrong width");
        for (std::size_t i = 0; i < values.size(); ++i) {
          const auto& c = t.columns[i];
          if (!datum_fits(values[i], c.type) || out_of_range(c.sql, values[i])) {
            throw std::logic_error("CREATE TABLE AS value outside column " + c.name + "'s type");
          }
          if (!set_datum(w, c.id, values[i])) throw std::logic_error("CREATE TABLE AS value of the wrong type");
        }
        if (!w.set_i64(hidden.id, ++id)) throw std::logic_error("CREATE TABLE AS row id");
        Bytes row;
        if (!w.finish(row)) throw std::length_error("a row of table " + t.name + " is over the value limit");
        Bytes key;
        if (!encode_pk_from_row(t, row, key)) throw std::logic_error("CREATE TABLE AS row key");
        budget.charge(key.size() + row.size());
        put_op(ops, DdlOp::Kind::Put, std::move(key), std::move(row));
      }
    }
    next->put_table(std::move(t));
    return finish();
  }

  if (const auto* ci = std::get_if<CreateIndexDdl>(&ddl)) {
    auto name = std::string{ci->name};
    name_free(QualifiedName{ci->table.schema, name});  // an index is in its table's schema
    auto t = find_table(ci->table);
    if (ci->columns.empty()) throw DdlRefused("an index needs columns");
    Index ix;
    ix.name = name;
    ix.unique = ci->unique;
    for (const auto& c : ci->columns) {
      const auto* col = t.column(c.name);
      if (col == nullptr) throw DdlRefused("column \"" + std::string{c.name} + "\" does not exist", kUndefinedColumn);
      if (std::ranges::find(ix.parts, col->id, &IndexPart::col_id) != ix.parts.end()) {
        throw DdlRefused("column \"" + col->name + "\" appears twice in the index", kDuplicateColumn);
      }
      refuse_key_type(*col);
      ix.parts.push_back({col->id, c.desc});
    }
    ix.id = reserve_ids(next->next_id_, 1);
    t.indexes.push_back(ix);
    ++t.version;
    auto key = name_entry_key(QualifiedName{t.schema, name});
    put_op(ops, DdlOp::Kind::EnsureAbsent, key);
    put_op(ops, DdlOp::Kind::Put, key, name_value(kKindIndex, ix.id));
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));

    // The entries for the rows already in the table, in the same batch.
    backfill = true;
    std::set<Bytes> prefixes;  // a unique index's non-null prefixes seen so far
    BackfillBudget budget{t.name, backfill_limit};
    rows.scan(row_prefix(t.id), [&](BytesView, BytesView row) {
      Bytes k;
      std::size_t prefix_len = 0;
      bool has_null = false;
      if (!encode_index_key_from_row(t, ix, row, k, &prefix_len, &has_null)) {
        throw std::runtime_error("malformed row in table " + t.name);
      }
      if (ix.unique && !has_null && !prefixes.emplace(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(prefix_len)).second) {
        throw ConstraintViolation("could not create unique index \"" + name + "\": table \"" + t.name +
                                      "\" holds a duplicate key",
                                  ConstraintDetail{BC_C_UNIQUE, t.id});
      }
      budget.charge(k.size() + prefix_len);
      put_op(ops, DdlOp::Kind::Put, std::move(k));
    });
    guard_rows(ops, t.id);
    next->put_table(std::move(t));
    return finish();
  }

  if (const auto* dt = std::get_if<DropTableDdl>(&ddl)) {
    const auto* found = table(dt->name);
    if (found == nullptr) {
      if (view(dt->name) != nullptr) throw DdlRefused("\"" + dt->name.name + "\" is not a table", kWrongObjectType);
      if (index(dt->name).first != nullptr) throw DdlRefused("\"" + dt->name.name + "\" is an index", kWrongObjectType);
      if (sequence(dt->name) != nullptr) throw DdlRefused("\"" + dt->name.name + "\" is not a table", kWrongObjectType);
      if (dt->if_exists) return nullptr;
      throw DdlRefused("table \"" + dt->name.name + "\" does not exist", kUndefinedTable);
    }
    const auto& t = *found;
    if (const auto* dependent = find_view_dependent(t.id)) {
      throw DdlRefused("cannot drop table " + t.name + " because other objects depend on it: view " + dependent->name +
                           " depends on table " + t.name,
                       kDependentObjectsStillExist);
    }
    drop_table(t);
    return finish();
  }

  if (const auto* di = std::get_if<DropIndexDdl>(&ddl)) {
    auto [owner, found] = index(di->name);
    if (found == nullptr) {
      if (table(di->name) != nullptr || view(di->name) != nullptr || sequence(di->name) != nullptr) {
        throw DdlRefused("\"" + di->name.name + "\" is not an index", kWrongObjectType);
      }
      if (di->if_exists) return nullptr;
      throw DdlRefused("index \"" + di->name.name + "\" does not exist", kUndefinedObject);
    }
    auto t = *owner;
    auto id = found->id;
    std::erase_if(t.indexes, [&](const Index& ix) { return ix.id == id; });
    ++t.version;
    put_op(ops, DdlOp::Kind::Put, table_record_key(t.id), serialize_table(t));
    put_op(ops, DdlOp::Kind::Del, name_entry_key(QualifiedName{t.schema, di->name.name}));
    auto p = index_prefix(id);
    put_op(ops, DdlOp::Kind::DelRange, p, key_successor(p));
    next->put_table(std::move(t));
    return finish();
  }

  if (const auto* cv = std::get_if<CreateViewDdl>(&ddl)) {
    name_free(cv->name);
    type_free(cv->name);
    if (cv->check_option > kCascadedCheckOption) throw std::logic_error("view " + cv->name.name + " of an unknown check option");
    View v{0, cv->name.name, cv->definition, cv->depends_on, cv->columns, cv->name.schema, cv->check_option, cv->names, false};
    std::ranges::sort(v.depends_on);
    v.depends_on.erase(std::unique(v.depends_on.begin(), v.depends_on.end()), v.depends_on.end());
    for (auto dep : v.depends_on) {
      if (table(dep) == nullptr && view(dep) == nullptr) {
        throw DdlRefused("a relation view \"" + v.name + "\" reads does not exist", kUndefinedTable);
      }
    }
    // Each stored name is the binder's resolution of one the query reads: of
    // an object it depends on, named once.
    for (const auto& n : v.names) {
      const auto* t = table(QualifiedName{n.schema, n.name});
      const auto* w = view(QualifiedName{n.schema, n.name});
      auto id = t != nullptr ? t->id : w != nullptr ? w->id : 0;
      if (id == 0 || !std::ranges::contains(v.depends_on, id) ||
          std::ranges::count(v.names, n.name, &ViewName::name) != 1) {
        throw std::logic_error("view " + v.name + " stores a name, " + n.name + ", of no relation it depends on");
      }
    }
    v.id = reserve_ids(next->next_id_, 1);
    put_op(ops, DdlOp::Kind::EnsureAbsent, name_entry_key(cv->name));
    put_op(ops, DdlOp::Kind::Put, name_entry_key(cv->name), name_value(kKindView, v.id));
    put_op(ops, DdlOp::Kind::Put, view_record_key(v.id), serialize_view(v));
    next->views_.emplace(v.id, std::move(v));
    return finish();
  }

  if (const auto* dv = std::get_if<DropViewDdl>(&ddl)) {
    const auto* found = view(dv->name);
    if (found == nullptr) {
      if (table(dv->name) != nullptr || index(dv->name).first != nullptr || sequence(dv->name) != nullptr) {
        throw DdlRefused("\"" + dv->name.name + "\" is not a view", kWrongObjectType);
      }
      if (dv->if_exists) return nullptr;
      throw DdlRefused("view \"" + dv->name.name + "\" does not exist", kUndefinedTable);
    }
    if (const auto* dependent = find_view_dependent(found->id)) {
      throw DdlRefused("cannot drop view " + found->name + " because other objects depend on it: view " +
                           dependent->name + " depends on view " + found->name,
                       kDependentObjectsStillExist);
    }
    drop_view(*found);
    return finish();
  }

  if (const auto* cp = std::get_if<CreateProcedureDdl>(&ddl)) {
    std::set<std::string, std::less<>> arg_names;
    for (const auto& a : cp->args) {
      if (!arg_names.insert(a.name).second) {
        throw DdlRefused("parameter name \"" + a.name + "\" used more than once", kInvalidFunctionDefinition);
      }
      if (!sql_type_valid(a.type)) throw DdlRefused("argument " + a.name + " has an invalid type", kFeatureNotSupported);
    }
    Procedure p{0, cp->name.name, cp->definition, cp->args, cp->name.schema};
    if (const auto* existing = procedure(cp->name)) {
      if (!cp->replace) throw DdlRefused("procedure \"" + p.name + "\" already exists", kDuplicateFunction);
      // PostgreSQL would overload, or refuse a renamed argument: one
      // procedure per name keeps neither (docs/procedures_design.md).
      if (existing->args != cp->args) {
        throw DdlRefused("CREATE OR REPLACE PROCEDURE cannot change the arguments of procedure \"" + p.name +
                             "\": drop it first",
                         kFeatureNotSupported);
      }
      p.id = existing->id;
    } else {
      p.id = reserve_ids(next->next_id_, 1);
      put_op(ops, DdlOp::Kind::EnsureAbsent, procedure_name_key(cp->name));
      put_op(ops, DdlOp::Kind::Put, procedure_name_key(cp->name), name_value(kKindProcedure, p.id));
    }
    put_op(ops, DdlOp::Kind::Put, procedure_record_key(p.id), serialize_procedure(p));
    next->procedures_.insert_or_assign(p.id, std::move(p));
    return finish();
  }

  if (const auto* dp = std::get_if<DropProcedureDdl>(&ddl)) {
    const auto* found = procedure(dp->name);
    // A written list names the input arguments, or all of them.
    auto matches = [&](bool inputs_only) {
      std::vector<SqlType> declared;
      for (const auto& a : found->args) {
        if (!inputs_only || a.is_input()) declared.push_back(a.type);
      }
      return std::ranges::equal(*dp->arg_types, declared, same_argument_type);
    };
    if (found != nullptr && dp->arg_types && !matches(true) && !matches(false)) found = nullptr;
    if (found == nullptr) {
      if (dp->if_exists) return nullptr;
      // PostgreSQL 17's words: the signature as written, or, with no
      // argument list, the name alone.
      if (!dp->arg_types) {
        throw DdlRefused("could not find a procedure named \"" + written(dp->name) + "\"", kUndefinedFunction);
      }
      std::string args;
      for (const auto& t : *dp->arg_types) args += (args.empty() ? "" : ", ") + std::string{signature_type(t)};
      throw DdlRefused("procedure " + written(dp->name) + "(" + args + ") does not exist", kUndefinedFunction);
    }
    drop_procedure(*found);
    return finish();
  }

  if (const auto* cr = std::get_if<CreateRoleDdl>(&ddl)) {
    // PostgreSQL reserves PUBLIC, NONE and the pg_ prefix.
    if (cr->name == "public" || cr->name == "none" || cr->name.starts_with("pg_")) {
      throw DdlRefused("role name \"" + cr->name + "\" is reserved", kReservedName);
    }
    if (role(cr->name) != nullptr) throw DdlRefused("role \"" + cr->name + "\" already exists", kDuplicateObject);
    Role r{reserve_ids(next->next_id_, 1), cr->name};
    put_op(ops, DdlOp::Kind::EnsureAbsent, role_name_key(r.name));
    put_op(ops, DdlOp::Kind::Put, role_name_key(r.name), name_value(kKindRole, r.id));
    put_op(ops, DdlOp::Kind::Put, role_record_key(r.id), serialize_role(r));
    next->roles_.emplace(r.id, std::move(r));
    return finish();
  }

  if (const auto* dr = std::get_if<DropRoleDdl>(&ddl)) {
    auto dropped = false;
    for (const auto& name : dr->names) {
      const auto* r = next->role(name);
      if (r == nullptr) {
        // A name dropped earlier in the list is no longer there either.
        if (dr->if_exists || role(name) != nullptr) continue;
        throw DdlRefused("role \"" + name + "\" does not exist", kUndefinedObject);
      }
      // A line for each schema it owns, in id order, as PostgreSQL 17 lists them.
      std::string owned;
      for (const auto& [id, sc] : next->schemas_) {
        if (sc.owner == r->id) owned += (owned.empty() ? "" : "\n") + ("owner of schema " + sc.name);
      }
      if (!owned.empty()) {
        throw DdlRefused("role \"" + name + "\" cannot be dropped because some objects depend on it",
                         kDependentObjectsStillExist)
            .with_notes(std::move(owned), "");
      }
      for (const auto& [object, acl] : next->acls_) {
        if (std::ranges::any_of(acl, [&](const AclEntry& e) { return e.grantee == r->id || e.grantor == r->id; })) {
          throw DdlRefused("role \"" + name + "\" cannot be dropped because some objects depend on it",
                           kDependentObjectsStillExist);
        }
      }
      put_op(ops, DdlOp::Kind::Del, role_record_key(r->id));
      put_op(ops, DdlOp::Kind::Del, role_name_key(r->name));
      next->roles_.erase(r->id);
      next->rebuild_lookups();
      dropped = true;
    }
    if (!dropped) return nullptr;
    return finish();
  }

  if (const auto* cd = std::get_if<CreateDomainDdl>(&ddl)) {
    // A shell type of the name is filled in, as PostgreSQL's TypeCreate
    // does: the domain takes its id, and so keeps what was granted on it.
    const auto* shell = type(cd->name);
    if ((shell != nullptr && shell->kind != TypeObject::Kind::Shell) || table(cd->name) != nullptr || view(cd->name) != nullptr) {
      throw DdlRefused("type \"" + cd->name.name + "\" already exists", kDuplicateObject);
    }
    if (!sql_type_valid(cd->base)) throw DdlRefused("domain " + cd->name.name + " has an invalid type", kFeatureNotSupported);
    TypeObject t{shell != nullptr ? shell->id : reserve_ids(next->next_id_, 1), cd->name.name, TypeObject::Kind::Domain,
                 cd->base, cd->name.schema};
    if (shell == nullptr) {
      put_op(ops, DdlOp::Kind::EnsureAbsent, type_name_key(cd->name));
      put_op(ops, DdlOp::Kind::Put, type_name_key(cd->name), name_value(kKindType, t.id));
    }
    put_op(ops, DdlOp::Kind::Put, type_record_key(t.id), serialize_type(t));
    next->types_.insert_or_assign(t.id, std::move(t));
    return finish();
  }

  if (const auto* cty = std::get_if<CreateTypeDdl>(&ddl)) {
    if (type(cty->name) != nullptr || table(cty->name) != nullptr || view(cty->name) != nullptr) {
      throw DdlRefused("type \"" + cty->name.name + "\" already exists", kDuplicateObject);
    }
    TypeObject t{reserve_ids(next->next_id_, 1), cty->name.name, TypeObject::Kind::Shell, {}, cty->name.schema};
    put_op(ops, DdlOp::Kind::EnsureAbsent, type_name_key(cty->name));
    put_op(ops, DdlOp::Kind::Put, type_name_key(cty->name), name_value(kKindType, t.id));
    put_op(ops, DdlOp::Kind::Put, type_record_key(t.id), serialize_type(t));
    next->types_.emplace(t.id, std::move(t));
    return finish();
  }

  if (const auto* cs = std::get_if<CreateSequenceDdl>(&ddl)) {
    if (names_.contains(cs->name)) {
      if (cs->if_not_exists) return nullptr;
      throw DdlRefused("relation \"" + cs->name.name + "\" already exists", kDuplicateTable);
    }
    Sequence s{reserve_ids(next->next_id_, 1), cs->name.name, cs->name.schema};
    put_op(ops, DdlOp::Kind::EnsureAbsent, name_entry_key(cs->name));
    put_op(ops, DdlOp::Kind::Put, name_entry_key(cs->name), name_value(kKindSequence, s.id));
    put_op(ops, DdlOp::Kind::Put, sequence_record_key(s.id), serialize_sequence(s));
    next->sequences_.emplace(s.id, std::move(s));
    return finish();
  }

  if (const auto* dty = std::get_if<DropTypeDdl>(&ddl)) {
    const auto* found = type(dty->name);
    if (found == nullptr) {
      if (table(dty->name) != nullptr || view(dty->name) != nullptr) {
        if (dty->domain) throw DdlRefused("\"" + dty->name.name + "\" is not a domain", kWrongObjectType);
        throw DdlRefused("cannot drop type " + dty->name.name + " because table or view " + dty->name.name + " requires it",
                         kDependentObjectsStillExist);
      }
      if (dty->if_exists) return nullptr;
      throw DdlRefused("type \"" + written(dty->name) + "\" does not exist", kUndefinedObject);
    }
    if (dty->domain && found->kind != TypeObject::Kind::Domain) {
      throw DdlRefused("\"" + dty->name.name + "\" is not a domain", kWrongObjectType);
    }
    drop_type(*found);
    return finish();
  }

  if (const auto* ds = std::get_if<DropSequenceDdl>(&ddl)) {
    const auto* found = sequence(ds->name);
    if (found == nullptr) {
      if (names_.contains(ds->name)) throw DdlRefused("\"" + ds->name.name + "\" is not a sequence", kWrongObjectType);
      if (ds->if_exists) return nullptr;
      throw DdlRefused("sequence \"" + ds->name.name + "\" does not exist", kUndefinedTable);
    }
    drop_sequence(*found);
    return finish();
  }

  if (const auto* csc = std::get_if<CreateSchemaDdl>(&ddl)) {
    // PostgreSQL 17's order: the role, the reserved prefix, then the name.
    auto owner = kBootstrapRole;
    if (csc->owner) {
      const auto* r = role(*csc->owner);
      if (r == nullptr) throw DdlRefused("role \"" + *csc->owner + "\" does not exist", kUndefinedObject);
      owner = r->id;
    }
    if (csc->name.starts_with("pg_")) {
      throw DdlRefused("unacceptable schema name \"" + csc->name + "\"", kReservedName)
          .with_notes("The prefix \"pg_\" is reserved for system schemas.", "");
    }
    // public and information_schema exist in PostgreSQL, with no record here.
    if (csc->name == "public" || csc->name == "information_schema" || schema(csc->name) != nullptr) {
      if (csc->if_not_exists) return nullptr;
      throw DdlRefused("schema \"" + csc->name + "\" already exists", kDuplicateSchema);
    }
    Schema sc{reserve_ids(next->next_id_, 1), csc->name, owner};
    put_op(ops, DdlOp::Kind::EnsureAbsent, schema_name_key(sc.name));
    put_op(ops, DdlOp::Kind::Put, schema_name_key(sc.name), name_value(kKindSchema, sc.id));
    put_op(ops, DdlOp::Kind::Put, schema_record_key(sc.id), serialize_schema(sc));
    // Its ACL record, the owner's default, exists as long as it does: a build
    // from before schemas refuses its format (*How an older build reacts*).
    put_op(ops, DdlOp::Kind::Put, acl_record_key(sc.id), serialize_acl(sc.id, {}, true));
    next->schemas_.emplace(sc.id, std::move(sc));
    return finish();
  }

  if (const auto* dsc = std::get_if<DropSchemaDdl>(&ddl)) {
    const auto drop = schema_drop(*dsc);
    if (!dsc->cascade && !drop.objects.empty()) {
      auto message = dsc->names.size() == 1
                         ? "cannot drop schema " + dsc->names.front() + " because other objects depend on it"
                         : std::string{"cannot drop desired object(s) because other objects depend on them"};
      throw DdlRefused(message, kDependentObjectsStillExist)
          .with_notes(drop.detail([](const SchemaDrop::Object& o) { return o.described + " depends on " + o.depends_on; }),
                      "Use DROP ... CASCADE to drop the dependent objects too.");
    }
    if (drop.schemas.empty()) return nullptr;
    // CASCADE: each object as its own DROP writes it, in one batch with the
    // schemas. A table takes its indexes; the closure holds every view that
    // reads a dropped relation, so no view is left reading one.
    for (const auto& o : drop.objects) {
      if (const auto* t = table(o.id)) {
        drop_table(*t);
      } else if (const auto* w = view(o.id)) {
        drop_view(*w);
      } else if (const auto q = sequences_.find(o.id); q != sequences_.end()) {
        drop_sequence(q->second);
      } else if (const auto ty = types_.find(o.id); ty != types_.end()) {
        drop_type(ty->second);
      } else {
        drop_procedure(procedures_.at(o.id));
      }
    }
    for (auto id : drop.schemas) {
      put_op(ops, DdlOp::Kind::Del, schema_record_key(id));
      put_op(ops, DdlOp::Kind::Del, schema_name_key(schemas_.at(id).name));
      put_op(ops, DdlOp::Kind::Del, acl_record_key(id));
      next->schemas_.erase(id);
      next->acls_.erase(id);
    }
    return finish();
  }

  if (const auto* g = std::get_if<GrantDdl>(&ddl)) {
    using Target = GrantDdl::Target;
    // The objects, of the kind ON names, in PostgreSQL's words when one is not.
    std::vector<std::uint32_t> objects;
    for (const auto& object : g->objects) {
      const auto& name = object.name;
      switch (g->target) {
        case Target::Table:
          if (const auto* t = table(object)) {
            objects.push_back(t->id);
          } else if (const auto* v = view(object)) {
            objects.push_back(v->id);
          } else if (index(object).first != nullptr) {
            throw DdlRefused("\"" + name + "\" is an index", kWrongObjectType);
          } else if (sequence(object) != nullptr) {
            throw DdlRefused("GRANT ON TABLE of sequence \"" + name + "\" is not supported: grant ON SEQUENCE",
                             kFeatureNotSupported);
          } else {
            throw DdlRefused("relation \"" + written(object) + "\" does not exist", kUndefinedTable);
          }
          break;
        case Target::Sequence:
          if (const auto* s = sequence(object)) {
            objects.push_back(s->id);
          } else if (names_.contains(object)) {
            throw DdlRefused("\"" + name + "\" is not a sequence", kWrongObjectType);
          } else {
            throw DdlRefused("relation \"" + written(object) + "\" does not exist", kUndefinedTable);
          }
          break;
        case Target::Domain:
        case Target::Type:
          if (const auto* ty = type(object); ty != nullptr && ty->kind == TypeObject::Kind::Shell) {
            throw DdlRefused("type \"" + name + "\" is only a shell", kUndefinedObject);  // nothing may use one
          } else if (ty != nullptr) {
            objects.push_back(ty->id);
          } else if (g->target == Target::Domain && (table(object) != nullptr || view(object) != nullptr)) {
            throw DdlRefused("\"" + name + "\" is not a domain", kWrongObjectType);
          } else if (table(object) != nullptr || view(object) != nullptr) {
            throw DdlRefused("GRANT ON TYPE of a table's or view's row type is not supported", kFeatureNotSupported);
          } else {
            throw DdlRefused("type \"" + written(object) + "\" does not exist", kUndefinedObject);
          }
          break;
      }
    }
    auto role_id = [&](const std::string& name) {
      const auto* r = role(name);
      if (r == nullptr) throw DdlRefused("role \"" + name + "\" does not exist", kUndefinedObject);
      return r->id;
    };
    std::vector<std::uint32_t> grantees;
    for (const auto& name : g->grantees) {
      if (name.empty() && g->grant && g->grant_option) {
        throw DdlRefused("grant options can only be granted to roles", kInvalidGrantOperation);
      }
      grantees.push_back(name.empty() ? kPublic : role_id(name));
    }
    // Every session is the bootstrap superuser: a grantor named is another role.
    if (g->grantor) {
      (void)role_id(*g->grantor);
      throw DdlRefused("grantor must be current user", kFeatureNotSupported);
    }
    // The privileges: on the whole object, and per column name, each one the
    // object's kind takes (0LP01 otherwise, as an unrecognized one is 42601).
    auto [kind_name, allowed] = [&]() -> std::pair<std::string, std::uint16_t> {
      switch (g->target) {
        case Target::Table: return {"table", privilege::kAll};
        case Target::Sequence: return {"sequence", privilege::kSequence};
        case Target::Domain: return {"domain", privilege::kType};
        case Target::Type: return {"type", privilege::kType};
      }
      return {"table", privilege::kAll};
    }();
    auto bit = [&](const std::string& name, bool column) -> std::uint16_t {
      using namespace privilege;
      static constexpr std::array<std::pair<std::string_view, std::uint16_t>, 9> kNames{
          {{"select", kSelect}, {"insert", kInsert}, {"update", kUpdate}, {"delete", kDelete}, {"truncate", kTruncate},
           {"references", kReferences}, {"trigger", kTrigger}, {"maintain", kMaintain}, {"usage", kUsage}}};
      if (column && g->target != Target::Table) {
        throw DdlRefused("column privileges are only valid for relations", kInvalidGrantOperation);
      }
      if (name == "all") return column ? kColumns : allowed;
      auto upper = [&] {
        std::string u;
        for (auto c : name) u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        return u;
      };
      for (const auto& [n, b] : kNames) {
        if (n != name) continue;
        if ((b & (column ? kColumns : allowed)) == 0) {
          throw DdlRefused("invalid privilege type " + upper() + " for " + (column ? "column" : kind_name),
                           kInvalidGrantOperation);
        }
        return b;
      }
      static constexpr std::array<std::string_view, 7> kOtherObjects{"execute", "create", "connect", "temporary",
                                                                     "temp", "set", "alter system"};
      if (std::ranges::find(kOtherObjects, name) != kOtherObjects.end()) {
        throw DdlRefused("invalid privilege type " + upper() + " for " + (column ? "column" : kind_name),
                         kInvalidGrantOperation);
      }
      throw DdlRefused("unrecognized privilege type \"" + name + "\"", kSyntaxError);
    };
    std::uint16_t whole = 0;
    std::vector<std::pair<std::string, std::uint16_t>> by_column;
    if (g->privileges.empty()) whole = allowed;
    for (const auto& p : g->privileges) {
      if (p.columns.empty()) {
        whole |= bit(p.name, false);
      } else {
        auto b = bit(p.name, true);
        for (const auto& c : p.columns) by_column.emplace_back(c, b);
      }
    }
    for (auto object : objects) {
      const auto* t = table(object);
      const auto* v = view(object);
      // A column's id: a table's column id; a view's position.
      auto column_id = [&](const std::string& name) -> std::uint32_t {
        if (t != nullptr) {
          if (const auto* c = t->column(name); c != nullptr && !c->hidden) return c->id;
        } else if (auto it = std::ranges::find(v->columns, name); it != v->columns.end()) {
          return static_cast<std::uint32_t>(it - v->columns.begin());
        }
        throw DdlRefused("column \"" + name + "\" of relation \"" + (t != nullptr ? t->name : v->name) + "\" does not exist",
                         kUndefinedColumn);
      };
      std::vector<std::pair<std::uint32_t, std::uint16_t>> columns;
      for (const auto& [name, b] : by_column) columns.emplace_back(column_id(name), b);
      auto current = next->acl(object);
      std::vector<AclEntry> acl(current.begin(), current.end());
      auto entry = [&](std::uint32_t column, std::uint32_t grantee) -> AclEntry& {
        auto key = std::tuple{column, grantee};
        auto it = std::ranges::find_if(acl, [&](const AclEntry& e) { return std::tuple{e.column, e.grantee} >= key; });
        if (it == acl.end() || std::tuple{it->column, it->grantee} != key) it = acl.insert(it, AclEntry{grantee, column, 0, 0, kBootstrapRole});
        return *it;
      };
      // REVOKE takes the privilege, or only its grant option; a table-level
      // REVOKE takes it from every column too, as PostgreSQL's does.
      auto take = [&](AclEntry& e, std::uint16_t b) {
        e.grant_options &= static_cast<std::uint16_t>(~b);
        if (!g->grant_option) e.privileges &= static_cast<std::uint16_t>(~b);
      };
      for (auto grantee : grantees) {
        if (g->grant) {
          auto give = [&](AclEntry& e, std::uint16_t b) {
            e.privileges |= b;
            if (g->grant_option) e.grant_options |= b;
          };
          if (whole != 0) give(entry(kWholeObject, grantee), whole);
          for (const auto& [c, b] : columns) give(entry(c, grantee), b);
        } else {
          for (auto& e : acl) {
            if (e.grantee == grantee) take(e, whole & (e.column == kWholeObject ? allowed : privilege::kColumns));
          }
          for (const auto& [c, b] : columns) {
            for (auto& e : acl) {
              if (e.grantee == grantee && e.column == c) take(e, b);
            }
          }
        }
      }
      std::erase_if(acl, [](const AclEntry& e) { return e.privileges == 0; });
      if (acl.empty()) {
        if (next->acls_.erase(object) > 0) put_op(ops, DdlOp::Kind::Del, acl_record_key(object));
      } else {
        put_op(ops, DdlOp::Kind::Put, acl_record_key(object), serialize_acl(object, acl));
        next->acls_.insert_or_assign(object, std::move(acl));
      }
    }
    return finish();
  }

  const auto& ac = std::get<AddColumnDdl>(ddl);
  auto t = find_table(ac.table);
  check_column_spec(ac.column);
  auto cname = std::string{ac.column.name};
  if (t.column(cname) != nullptr) {
    throw DdlRefused("column \"" + cname + "\" of relation \"" + t.name + "\" already exists", kDuplicateColumn);
  }
  auto has_default = !std::holds_alternative<std::monostate>(ac.column.default_value);
  if (!ac.column.nullable && !has_default) {
    throw DdlRefused("column \"" + cname + "\" is NOT NULL and has no default for the existing rows", kNotNullViolation);
  }
  Column c{t.next_col_id, cname, ac.column.sql, phys_type(ac.column.sql), ac.column.nullable, ac.column.default_value};
  auto old_layout = t.layout;
  if (!append_columns(t.layout, std::vector<ColumnDef>{{c.id, c.sql, c.nullable}})) {
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
    BackfillBudget budget{t.name, backfill_limit};
    RowWriter w{t.layout};
    rows.scan(row_prefix(t.id), [&](BytesView key, BytesView row) {
      if (!row_valid(row) || row_generation(row) != old_layout.generation) {
        throw std::runtime_error("malformed row in table " + t.name);
      }
      // copy_from sets every column of the layout, so the writer needs no reset.
      if (!w.copy_from(row)) throw std::runtime_error("malformed row in table " + t.name);
      Bytes out;
      if (!set_datum(w, c.id, c.default_value) || !w.finish(out)) {
        throw std::length_error("a row of table " + t.name + " is over the value limit with the new column");
      }
      budget.charge(key.size() + out.size());
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
  current_.store(std::make_shared<const Catalog>(Catalog::load(snap)));
}

auto LiveCatalog::current() const -> std::shared_ptr<const Catalog> { return current_.load(); }

auto LiveCatalog::head_version() const -> std::uint64_t { return snapshot_version(db_.snapshot()); }

auto catalog_version_key() -> Bytes { return counter_key(kCounterVersion); }

auto LiveCatalog::next_row_id(const Table& t) -> std::int64_t {
  constexpr auto kLast = std::numeric_limits<std::int64_t>::max();  // never handed out: next would overflow
  auto used_up = [&] { return std::length_error("table " + t.name + " has used every row id"); };
  std::lock_guard<std::mutex> lock{row_ids_mu_};
  auto it = seeded_row_id_locked(t);
  if (it->second == kLast) throw used_up();
  return it->second++;
}

void LiveCatalog::reserve_row_ids(const Catalog& next, const Ddl& ddl) {
  const auto* ct = std::get_if<CreateTableDdl>(&ddl);
  if (ct == nullptr || ct->rows.empty()) return;
  const auto* t = next.table(ct->name);
  if (t == nullptr) throw std::logic_error("CREATE TABLE AS of no table");
  std::lock_guard<std::mutex> lock{row_ids_mu_};
  auto it = seeded_row_id_locked(*t);
  it->second = std::max<std::int64_t>(it->second, std::ssize(ct->rows) + 1);
}

auto LiveCatalog::seeded_row_id_locked(const Table& t) -> std::map<std::uint32_t, std::int64_t>::iterator {
  constexpr auto kLast = std::numeric_limits<std::int64_t>::max();
  auto used_up = [&] { return std::length_error("table " + t.name + " has used every row id"); };
  auto it = row_ids_.find(t.id);
  if (it == row_ids_.end()) {
    // Seeded from the engine's latest state, not a statement's snapshot: no
    // id of this catalog's is out yet, so every id in use is committed.
    std::int64_t next = 1;
    auto prefix = row_prefix(t.id);
    auto last = db_.rkeys_from({}, key_successor(prefix));
    if (auto k = last.begin(); k != last.end() && has_prefix(*k, prefix)) {
      KeyReader r{*k, prefix.size()};
      auto id = r.read_i64(KeyPart{PhysType::I64});
      if (!id || !r.at_end()) throw std::runtime_error("malformed row key in table " + t.name);
      if (*id == kLast) throw used_up();
      next = std::max<std::int64_t>(*id + 1, 1);
    }
    it = row_ids_.emplace(t.id, next).first;
  }
  return it;
}

auto LiveCatalog::pin() const -> Pinned {
  for (;;) {
    auto g = gen_.load();
    auto cat = current();
    auto snap = db_.snapshot();
    // One read: the catalog version the snapshot holds. The version was
    // loaded before the snapshot was taken, so the snapshot is never behind.
    auto version = snapshot_version(snap);
    if (version == cat->version()) return Pinned{std::move(cat), std::move(snap)};
    if (g % 2 == 1 || gen_.load() != g) {
      // This catalog's own DDL is between the engine's publication and the
      // swap of current_ (or has just finished): wait for the swap.
      if (g % 2 == 1) gen_.wait(g);
      continue;
    }
    // Another catalog on this database moved the version (only tests open
    // two): catch up from the snapshot rather than pair it with a stale
    // version. A racing publisher's version wins the swap; this one is
    // consistent with this snapshot either way.
    auto fresh = std::make_shared<const Catalog>(Catalog::load(snap));
    (void)current_.compare_exchange_strong(cat, fresh);
    return Pinned{std::move(fresh), std::move(snap)};
  }
}

auto LiveCatalog::prepare(const Ddl& ddl) -> Prepared {
  // pin() pairs the snapshot with the catalog version it holds, reloading
  // when another catalog on the database moved it: a DDL never builds on a
  // stale version, which would hand out its ids again.
  auto [base, snap] = pin();
  std::vector<Catalog::DdlOp> ops;
  Prepared p{base, nullptr, nullptr, false};
  p.next = base->next_version(ddl, SnapshotRows{snap}, ops, p.backfill, backfill_limit_);
  if (!p.next) return p;
  reserve_row_ids(*p.next, ddl);
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
  std::unique_lock<std::mutex> lock{ddl_mu_, std::try_to_lock};
  if (!lock.owns_lock()) throw DdlInProgress();
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
    auto r = apply_batch_or_unknown(db_, {.sync = true}, std::move(*p.plan));
    if (r) current_.store(std::move(p.next));
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

auto LiveCatalog::apply(const Ddl& ddl) -> std::shared_ptr<const Catalog> { return run(ddl).catalog; }

auto LiveCatalog::run(const Ddl& ddl) -> Applied {
  // One DDL at a time from prepare to publication, so a retry is only ever
  // for a write that raced a backfill, never for another DDL of this
  // catalog. A second DDL does not wait for the first: it fails at once.
  // Readers do not take this lock.
  std::unique_lock<std::mutex> lock{ddl_mu_, std::try_to_lock};
  if (!lock.owns_lock()) throw DdlInProgress();
  for (int attempt = 0; attempt < kDdlAttempts; ++attempt) {
    auto p = prepare(ddl);
    if (!p.next) return {p.base, false, p.base};
    auto next = p.next;  // what this DDL publishes; current() may be a later DDL's once the lock is released
    auto base = p.base;
    if (commit_locked(std::move(p))) return {std::move(next), true, std::move(base)};
  }
  throw std::runtime_error("DDL conflicted with concurrent writes on every attempt");
}

// ------------------------------------------------------------------- keys

auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool {
  key_put_prefix(out, kNsRow, t.id);
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, key_part(*t.column(id)), row, out)) return false;
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
    if (!put_part_from_row(t, p.col_id, key_part(*c, c->nullable, p.desc), row, out, &was_null)) return false;
    any_null = any_null || was_null;
  }
  if (prefix_len != nullptr) *prefix_len = out.size();
  if (has_null != nullptr) *has_null = any_null;
  for (auto id : t.pk_col_ids) {
    if (!put_part_from_row(t, id, key_part(*t.column(id)), row, out)) return false;
  }
  return true;
}

}  // namespace bytecask::sql
