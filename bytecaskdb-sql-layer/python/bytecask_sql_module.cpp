// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_sql — the SQL layer's embedded API for Python, via nanobind: what
// the proof tests (tests/proof/, #515) drive. Five calls: open, execute,
// close, the catalog, and a table's rows (test only: it links
// sql_layer_testing); and begin(), a transaction with execute, commit and
// rollback.
//
// Values cross as Python values by the column's SQL type: integers as int,
// DOUBLE PRECISION and REAL as float, BOOLEAN as bool, BYTEA as bytes, TEXT
// and VARCHAR as str, DECIMAL as decimal.Decimal; the date and time types
// as their stored integer. Parameters are a list or a tuple, and go by
// Python type: None, bool, int (within BIGINT), float, str (as UTF-8, so
// not one with a lone surrogate) and bytes. Any other params object, a str,
// a buffer (bytes, bytearray, memoryview), a dict or a set among them, is
// refused: its items are not the placeholders' values in order.
//
// Errors: bytecask_sql.ExecError for a plan that ran and failed, with its
// code (a bc_error_code), conflict (true when every retry conflicted: code
// is then BC_CONFLICT, the same number as BC_E_IO), and constraint and
// object_id: for BC_E_CONSTRAINT with a known kind, the kind's name
// ("unique", "not_null", ...) and the table's or index's id; otherwise both
// None. bytecask_sql.SqlError for a statement the layer refuses before it
// runs (parse and bind errors, a DDL the catalog refuses, parameters of the
// wrong number or type, a params object that is not a list or tuple);
// bytecask_sql.Closed, also a ValueError, for any call after close();
// bytecask_sql.TransactionAborted (SQLSTATE 25P02) for a statement on a
// transaction a statement failed in; bytecask_sql.TransactionResolutionUnknown
// (08007) for a commit whose outcome is unknown; bytecask_sql.FeatureNotSupported
// (0A000), a SqlError, for a DDL statement given to a transaction and for a
// statement the planner can't plan ("no plan: ..."). A transaction whose commit
// conflicts raises ExecError with conflict set (40001). All
// derive from bytecask_sql.Error. Anything else, an engine refusal at run
// time included, is nanobind's translation of the C++ exception.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unique_ptr.h>

#include "include/bytecask.hpp"


import bytecask.sql.api;

namespace nb = nanobind;
using namespace nb::literals;
namespace sql = bytecask::sql;

namespace {

// The module's exception classes, created at import and owned by the module
// object; plain handles, so nothing runs at process exit.
struct Exceptions {
  nb::handle error, sql_error, exec_error, closed, aborted, unknown, not_supported;
};
Exceptions exceptions;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once at import
// decimal.Decimal, looked up once at import, not per value; held for the
// process, as the exception classes are.
nb::handle decimal_type;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once at import

// An exception of class `type` with e's message and, as `sqlstate`, the
// SQLSTATE the API reports for it (sql::sqlstate).
auto make_exception(nb::handle type, const std::exception& e) -> nb::object {
  nb::object exc = nb::borrow(type)(nb::str(e.what()));
  auto state = sql::sqlstate(e);
  exc.attr("sqlstate") = nb::str(state.data(), state.size());
  return exc;
}

void raise_as(nb::handle type, const std::exception& e) {
  PyErr_SetObject(type.ptr(), make_exception(type, e).ptr());
}

void raise_exec_error(const sql::ExecError& e) {
  nb::object exc = make_exception(exceptions.exec_error, e);
  exc.attr("code") = e.code();
  exc.attr("conflict") = e.conflict();
  exc.attr("constraint") = nb::none();
  exc.attr("object_id") = nb::none();
  // Both or neither: an object id without a known constraint kind is
  // nothing a caller can read.
  if (auto c = e.constraint(); c && c->valid()) {
    auto name = c->name();
    exc.attr("constraint") = nb::str(name.data(), name.size());
    exc.attr("object_id") = c->object_id;
  }
  PyErr_SetObject(exceptions.exec_error.ptr(), exc.ptr());
}

auto to_bytes(std::string_view s) -> sql::Bytes {
  auto b = std::as_bytes(std::span{s.data(), s.size()});
  return {b.begin(), b.end()};
}

// Parameter `i` (0-based) as a datum. One the layer has no type for, an int
// outside BIGINT among them, is a ParamError: SqlError, as a parameter of
// the wrong type is.
auto to_datum(nb::handle v, std::size_t i) -> sql::Value {
  auto param = "parameter $" + std::to_string(i + 1);
  if (v.is_none()) return std::monostate{};
  if (nb::isinstance<nb::bool_>(v)) return nb::cast<bool>(v);
  if (nb::isinstance<nb::int_>(v)) {
    std::int64_t i64 = 0;
    if (!nb::try_cast(v, i64)) throw sql::ParamError(param + " is an integer outside BIGINT's range");
    return i64;
  }
  if (nb::isinstance<nb::float_>(v)) return nb::cast<double>(v);
  if (nb::isinstance<nb::str>(v)) {
    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(v.ptr(), &size);
    if (utf8 == nullptr) {  // a lone surrogate has no UTF-8
      PyErr_Clear();
      throw sql::ParamError(param + " is a str that is not valid Unicode");
    }
    return to_bytes({utf8, static_cast<std::size_t>(size)});
  }
  if (nb::isinstance<nb::bytes>(v)) {
    auto b = nb::cast<nb::bytes>(v);
    return to_bytes({b.c_str(), b.size()});
  }
  throw sql::ParamError(param + " is not None, bool, int, float, str or bytes");
}

auto bytes_view(const sql::Bytes& b) -> std::string_view {
  return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes to chars for Python
}

// A stored value as Python sees it, by the column's SQL type.
auto to_python(const sql::Value& v, sql::SqlType t) -> nb::object {
  if (std::holds_alternative<std::monostate>(v)) return nb::none();
  if (const auto* b = std::get_if<sql::Bytes>(&v)) {
    // A NUMERIC: its text (sql::Rows).
    if (t.kind == sql::SqlKind::Numeric) return nb::borrow(decimal_type)(nb::str(bytes_view(*b).data(), b->size()));
    if (t.kind == sql::SqlKind::Text || t.kind == sql::SqlKind::Varchar) {
      auto s = bytes_view(*b);
      auto* o = PyUnicode_DecodeUTF8(s.data(), std::ssize(s), "strict");
      if (o == nullptr) throw nb::python_error();  // stored bytes that are not UTF-8: UnicodeDecodeError
      return nb::steal(o);
    }
    return nb::bytes(bytes_view(*b).data(), b->size());
  }
  if (const auto* i = std::get_if<std::int64_t>(&v)) {
    if (t.kind == sql::SqlKind::Decimal) {
      // Built from its digits, so no decimal context can round it or raise.
      auto magnitude = *i < 0 ? 0 - static_cast<std::uint64_t>(*i) : static_cast<std::uint64_t>(*i);
      nb::list digits;
      for (auto c : std::to_string(magnitude)) digits.append(c - '0');
      return nb::borrow(decimal_type)(
          nb::make_tuple(*i < 0 ? 1 : 0, nb::tuple(digits), -static_cast<int>(t.scale)));
    }
    return nb::int_(*i);
  }
  if (const auto* d = std::get_if<double>(&v)) return nb::float_(*d);
  return nb::bool_(std::get<bool>(v));
}

auto to_python(const sql::Rows& r) -> nb::dict {
  nb::list rows;
  for (const auto& row : r.rows) {
    nb::list out;
    for (std::size_t i = 0; i < row.size(); ++i) out.append(to_python(row[i], r.types.at(i)));
    rows.append(nb::tuple(out));
  }
  nb::list columns;
  for (const auto& c : r.columns) columns.append(c);
  nb::dict d;
  d["columns"] = columns;
  d["rows"] = rows;
  d["affected"] = r.affected;
  return d;
}

auto catalog(const sql::Database& db) -> nb::list {
  nb::list tables;
  auto cat = db.catalog();  // held: tables() is a view into it
  for (const auto& [id, t] : cat->tables()) {
    nb::list columns;
    for (const auto& c : t.columns) {
      nb::dict col;
      col["id"] = c.id;
      col["name"] = c.name;
      col["type"] = sql::sql_type_name(c.sql);
      col["nullable"] = c.nullable;
      col["default"] = to_python(c.default_value, c.sql);
      columns.append(col);
    }
    nb::list pk;
    for (auto col_id : t.pk_col_ids) pk.append(t.column(col_id)->name);
    nb::dict table;
    table["id"] = id;
    table["name"] = t.name;
    table["columns"] = columns;
    table["primary_key"] = pk;
    tables.append(table);
  }
  return tables;
}

auto to_values(const nb::object& params) -> std::vector<sql::Value> {
  // Only a list or a tuple: a str, a buffer, a UserString or an
  // array.array is a sequence of characters or ints, never parameters, and
  // a dict or set has no placeholder order.
  if (!nb::isinstance<nb::list>(params) && !nb::isinstance<nb::tuple>(params)) {
    throw sql::ParamError("params is a list or tuple of parameters");
  }
  std::vector<sql::Value> values;
  for (auto p : params) values.push_back(to_datum(p, values.size()));
  return values;
}

auto execute(sql::Database& db, std::string_view text, const nb::object& params) -> nb::dict {
  // A closed database is Closed whatever the parameters are.
  if (!db.is_open()) throw bytecask::DbClosed();
  return to_python(db.execute(text, to_values(params)));
}

// A statement of a transaction. A params object that is not parameters is
// refused before the statement reaches the transaction, as a client library
// refuses it before PostgreSQL sees it, and does not abort it.
auto txn_execute(sql::Transaction& t, std::string_view text, const nb::object& params) -> nb::dict {
  return to_python(t.execute(text, to_values(params)));
}

}  // namespace

NB_MODULE(bytecask_sql, m) {
  m.doc() = "The ByteCaskDB SQL layer's embedded API, for the proof tests.";

  exceptions.error = PyErr_NewException("bytecask_sql.Error", PyExc_Exception, nullptr);
  exceptions.sql_error = PyErr_NewException("bytecask_sql.SqlError", exceptions.error.ptr(), nullptr);
  exceptions.exec_error = PyErr_NewException("bytecask_sql.ExecError", exceptions.error.ptr(), nullptr);
  auto closed_bases = nb::make_tuple(exceptions.error, nb::handle(PyExc_ValueError));
  exceptions.closed = PyErr_NewException("bytecask_sql.Closed", closed_bases.ptr(), nullptr);
  m.attr("Error") = exceptions.error;
  m.attr("SqlError") = exceptions.sql_error;
  m.attr("ExecError") = exceptions.exec_error;
  m.attr("Closed") = exceptions.closed;
  exceptions.aborted = PyErr_NewException("bytecask_sql.TransactionAborted", exceptions.error.ptr(), nullptr);
  m.attr("TransactionAborted") = exceptions.aborted;
  exceptions.unknown =
      PyErr_NewException("bytecask_sql.TransactionResolutionUnknown", exceptions.error.ptr(), nullptr);
  m.attr("TransactionResolutionUnknown") = exceptions.unknown;
  exceptions.not_supported =
      PyErr_NewException("bytecask_sql.FeatureNotSupported", exceptions.sql_error.ptr(), nullptr);
  m.attr("FeatureNotSupported") = exceptions.not_supported;
  decimal_type = nb::object(nb::module_::import_("decimal").attr("Decimal")).release();

  nb::register_exception_translator([](const std::exception_ptr& p, void*) {
    try {
      std::rethrow_exception(p);
    } catch (const sql::ExecError& e) {
      raise_exec_error(e);
    } catch (const sql::ast::SqlError& e) {
      raise_as(exceptions.sql_error, e);
    } catch (const sql::DdlRefused& e) {
      raise_as(exceptions.sql_error, e);
    } catch (const sql::ParamError& e) {
      raise_as(exceptions.sql_error, e);
    } catch (const bytecask::DbClosed& e) {
      raise_as(exceptions.closed, e);
    } catch (const sql::TransactionAborted& e) {
      raise_as(exceptions.aborted, e);
    } catch (const sql::FeatureNotSupported& e) {
      raise_as(exceptions.not_supported, e);
    } catch (const sql::TransactionResolutionUnknown& e) {
      raise_as(exceptions.unknown, e);
    }
  });

  m.def(
      "_range_messages",
      [] {
        nb::list out;
        for (const auto& r : sql::range_messages()) {
          out.append(
              nb::make_tuple(nb::str(r.start.data(), r.start.size()), nb::str(r.sqlstate.data(), r.sqlstate.size())));
        }
        return out;
      },
      "Each range message start the layer writes, with its SQLSTATE: what sqlstate maps.");

  nb::class_<sql::Transaction>(m, "Transaction")
      .def("execute", &txn_execute, "sql"_a, "params"_a = nb::tuple(),
           "Runs one statement in the transaction; a failure aborts it.")
      .def(
          "commit",
          [](sql::Transaction& t) {
            return t.commit() == sql::CommitOutcome::Committed ? "COMMIT" : "ROLLBACK";
          },
          "Writes every statement's writes as one batch: 'COMMIT'. After a failed statement writes nothing "
          "and returns 'ROLLBACK', PostgreSQL's command tag. Raises on a conflict.")
      .def("rollback", &sql::Transaction::rollback, "Ends the transaction; nothing it wrote is kept.")
      .def_prop_ro("is_open", &sql::Transaction::is_open)
      .def_prop_ro("is_aborted", &sql::Transaction::is_aborted);

  nb::class_<sql::Database>(m, "Database")
      .def(
          "begin",
          [](sql::Database& db, std::uint64_t max_bytes) { return db.begin({.max_bytes = max_bytes}); },
          "max_bytes"_a = sql::TransactionOptions{}.max_bytes, nb::keep_alive<0, 1>(),
          "Opens a transaction; its snapshot is taken at its first statement.")
      .def("execute", &execute, "sql"_a, "params"_a = nb::tuple(),
           "Runs one statement: a dict of columns, rows (tuples) and affected.")
      .def("close", &sql::Database::close, "Closes the engine; a later call raises Closed, a second close() nothing.")
      .def("catalog", &catalog, "Every table: its id, name, columns (id, name, type, nullable, default) and primary key.")
      .def(
          "table_rows",
          [](const sql::Database& db, std::string_view table) { return to_python(db.table_rows(table)); },
          "table"_a,
          "Test only: every row of a table, in primary-key order, read from the engine and decoded "
          "through the catalog.")
      .def(
          "_engine_rows",
          [](sql::Database& db, std::string_view table) {
            auto cat = db.catalog();  // held: t points into it
            const auto* t = cat->table(table);
            if (t == nullptr) throw std::invalid_argument("relation \"" + std::string{table} + "\" does not exist");
            auto snap = db.engine_for_testing().snapshot();
            nb::list rows;
            sql::for_each_row(snap, t->id, [&](sql::BytesView key, sql::BytesView row) {
              rows.append(nb::make_tuple(nb::bytes(key.data(), key.size()), nb::bytes(row.data(), row.size())));
            });
            return rows;
          },
          "table"_a, "Test only: a table's stored (key, row) pairs, as the engine holds them.")
      .def(
          "_engine_put",
          [](sql::Database& db, const nb::bytes& key, const nb::bytes& value) {
            auto view = [](const nb::bytes& b) { return std::as_bytes(std::span{b.c_str(), b.size()}); };
            (void)db.engine_for_testing().put({}, view(key), view(value));
          },
          "key"_a, "value"_a, "Test only: writes a key and value straight to the engine, past the layer's checks.")
      .def(
          "_alternatives",
          [](sql::Database& db, std::string_view text, const nb::object& params) {
            nb::list out;
            for (const auto& r : db.alternatives_for_testing(text, to_values(params))) out.append(to_python(r));
            return out;
          },
          "sql"_a, "params"_a = nb::tuple(),
          "Test only: runs every plan the search costs for a SELECT, autocommit, and returns each one's result, "
          "the cheapest plan's first.")
      .def("_conflict_on_every_commit", &sql::Database::conflict_on_every_commit_for_testing, "on"_a,
           "Test only: while on, every commit conflicts, so execute raises ExecError with conflict set.");

  m.def(
      "open", [](const std::filesystem::path& dir) { return sql::Database::open(dir); }, "dir"_a,
      "Opens (or creates) a database directory.");
}
