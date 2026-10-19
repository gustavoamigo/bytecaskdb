// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_sql — the SQL layer's embedded API for Python, via nanobind: what
// the proof tests (tests/proof/, #515) drive. Five calls: open, execute,
// close, the catalog, and a table's rows.
//
// Values cross as Python values by the column's SQL type: integers as int,
// DOUBLE PRECISION and REAL as float, BOOLEAN as bool, BYTEA as bytes, TEXT
// and VARCHAR as str, DECIMAL as decimal.Decimal; the date and time types
// as their stored integer. Parameters go by Python type: None, bool, int,
// float, str (as UTF-8) and bytes.
//
// Errors: bytecask_sql.ExecError for a plan that ran and failed, with its
// code (a bc_error_code, or BC_CONFLICT when every retry conflicted) and, for
// BC_E_CONSTRAINT, the constraint kind and the object id; bytecask_sql.SqlError for
// a statement the layer refuses before it runs (parse, bind, a DDL the
// catalog refuses, parameters of the wrong number or type). Both derive
// from bytecask_sql.Error.

#include <cstdint>
#include <exception>
#include <filesystem>
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

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.api;

namespace nb = nanobind;
using namespace nb::literals;
namespace sql = bytecask::sql;

namespace {

// The module's exception classes, created at import and owned by the module
// object; plain handles, so nothing runs at process exit.
struct Exceptions {
  nb::handle error, sql_error, exec_error;
};
Exceptions exceptions;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once at import

auto constraint_name(std::uint32_t kind) -> nb::object {
  switch (kind) {
    case BC_C_UNIQUE: return nb::str("unique");
    case BC_C_NOT_NULL: return nb::str("not_null");
    case BC_C_CHECK: return nb::str("check");
    case BC_C_FOREIGN_KEY: return nb::str("foreign_key");
    case BC_C_RANGE: return nb::str("range");
    default: return nb::none();
  }
}

void raise_exec_error(const sql::ExecError& e) {
  nb::object exc = nb::borrow(exceptions.exec_error)(nb::str(e.what()));
  exc.attr("code") = e.code();
  exc.attr("constraint") = nb::none();
  exc.attr("object_id") = nb::none();
  if (e.code() == BC_E_CONSTRAINT) {
    exc.attr("constraint") = constraint_name(e.detail() >> 24);
    exc.attr("object_id") = e.detail() & 0x00FF'FFFFu;
  }
  PyErr_SetObject(exceptions.exec_error.ptr(), exc.ptr());
}

auto to_bytes(std::string_view s) -> sql::Bytes {
  auto b = std::as_bytes(std::span{s.data(), s.size()});
  return {b.begin(), b.end()};
}

auto to_datum(nb::handle v) -> sql::Value {
  if (v.is_none()) return std::monostate{};
  if (nb::isinstance<nb::bool_>(v)) return nb::cast<bool>(v);
  if (nb::isinstance<nb::int_>(v)) return nb::cast<std::int64_t>(v);
  if (nb::isinstance<nb::float_>(v)) return nb::cast<double>(v);
  if (nb::isinstance<nb::str>(v)) return to_bytes(nb::cast<std::string_view>(v));
  if (nb::isinstance<nb::bytes>(v)) {
    auto b = nb::cast<nb::bytes>(v);
    return to_bytes({b.c_str(), b.size()});
  }
  throw nb::type_error("a parameter is None, bool, int, float, str or bytes");
}

auto bytes_view(const sql::Bytes& b) -> std::string_view {
  return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes to chars for Python
}

// A stored value as Python sees it, by the column's SQL type.
auto to_python(const sql::Value& v, sql::SqlType t) -> nb::object {
  if (std::holds_alternative<std::monostate>(v)) return nb::none();
  if (const auto* b = std::get_if<sql::Bytes>(&v)) {
    if (t.kind == sql::SqlKind::Text || t.kind == sql::SqlKind::Varchar) {
      auto s = bytes_view(*b);
      return nb::steal(PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "strict"));
    }
    return nb::bytes(bytes_view(*b).data(), b->size());
  }
  if (const auto* i = std::get_if<std::int64_t>(&v)) {
    if (t.kind == sql::SqlKind::Decimal) {
      return nb::module_::import_("decimal").attr("Decimal")(*i).attr("scaleb")(-static_cast<int>(t.scale));
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
  for (const auto& [id, t] : db.catalog()->tables()) {
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

auto execute(sql::Database& db, std::string_view text, const nb::sequence& params) -> nb::dict {
  std::vector<sql::Value> values;
  for (auto p : params) values.push_back(to_datum(p));
  return to_python(db.execute(text, values));
}

}  // namespace

NB_MODULE(bytecask_sql, m) {
  m.doc() = "The ByteCaskDB SQL layer's embedded API, for the proof tests.";

  exceptions.error = PyErr_NewException("bytecask_sql.Error", PyExc_Exception, nullptr);
  exceptions.sql_error = PyErr_NewException("bytecask_sql.SqlError", exceptions.error.ptr(), nullptr);
  exceptions.exec_error = PyErr_NewException("bytecask_sql.ExecError", exceptions.error.ptr(), nullptr);
  m.attr("Error") = exceptions.error;
  m.attr("SqlError") = exceptions.sql_error;
  m.attr("ExecError") = exceptions.exec_error;

  nb::register_exception_translator([](const std::exception_ptr& p, void*) {
    try {
      std::rethrow_exception(p);
    } catch (const sql::ExecError& e) {
      raise_exec_error(e);
    } catch (const sql::ast::SqlError& e) {
      PyErr_SetString(exceptions.sql_error.ptr(), e.what());
    } catch (const std::invalid_argument& e) {
      // A statement the catalog refuses, or parameters of the wrong number or type.
      PyErr_SetString(exceptions.sql_error.ptr(), e.what());
    } catch (const std::length_error& e) {
      // Ids, a layout or a backfill past their limits.
      PyErr_SetString(exceptions.sql_error.ptr(), e.what());
    }
  });

  nb::class_<sql::Database>(m, "Database")
      .def("execute", &execute, "sql"_a, "params"_a = nb::tuple(),
           "Runs one statement: a dict of columns, rows (tuples) and affected.")
      .def("close", &sql::Database::close, "Closes the engine; no later call is allowed.")
      .def("catalog", &catalog, "Every table: its id, name, columns (id, name, type, nullable, default) and primary key.")
      .def(
          "table_rows",
          [](sql::Database& db, std::string_view table) { return to_python(db.table_rows(table)); },
          "table"_a,
          "Test only: every row of a table, in primary-key order, read from the engine and decoded "
          "through the catalog.");

  m.def(
      "open", [](const std::filesystem::path& dir) { return sql::Database::open(dir); }, "dir"_a,
      "Opens (or creates) a database directory.");
}
