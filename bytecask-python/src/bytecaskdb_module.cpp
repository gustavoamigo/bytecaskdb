// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Python bindings for ByteCaskDB via nanobind.
// Wraps the C++23 module interface directly.
//
// This file is a regular translation unit (not a module unit) that imports
// the bytecask module. NB_MODULE must live outside any module partition
// because it declares extern "C" symbols in the global module.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

import bytecask;

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// ---------------------------------------------------------------------------
// Byte conversion helpers
// ---------------------------------------------------------------------------

auto to_view(nb::bytes b) -> bytecask::BytesView {
  return std::as_bytes(
      std::span{b.c_str(), static_cast<std::size_t>(b.size())});
}

auto to_pybytes(const bytecask::Bytes &b) -> nb::bytes {
  return nb::bytes(reinterpret_cast<const char *>(b.data()), b.size());
}

auto key_to_pybytes(const bytecask::Key &k) -> nb::bytes {
  auto data = reinterpret_cast<const char *>(&*k.begin());
  return nb::bytes(data, k.size());
}

// ---------------------------------------------------------------------------
// PyDB — wraps non-moveable DB via guaranteed copy elision.
// ---------------------------------------------------------------------------

struct PyDB {
  bytecask::DB db;

  PyDB(std::filesystem::path dir, bytecask::Options opts)
      : db{bytecask::DB::open(std::move(dir), std::move(opts))} {}
};

// ---------------------------------------------------------------------------
// PySnapshot — wraps move-only Snapshot with use-after-consume guard.
//
// The C++ Snapshot is move-only: WritePlan(Snapshot) consumes it.  Without
// this wrapper, the Python object would silently become a dangling shell
// after construction of a WritePlan, causing a segfault on the next call.
// With the wrapper, every method calls check() and raises RuntimeError
// instead of touching moved-from memory.
// ---------------------------------------------------------------------------

struct PySnapshot {
  std::optional<bytecask::Snapshot> snap;
  std::unique_ptr<nb::ft_mutex> mu{std::make_unique<nb::ft_mutex>()};

  explicit PySnapshot(bytecask::Snapshot s) : snap{std::move(s)} {}

  void check() const {
    if (!snap) {
      throw std::runtime_error("Snapshot already consumed by WritePlan");
    }
  }

  auto take() -> bytecask::Snapshot {
    nb::ft_lock_guard lock{*mu};
    check();
    auto s = std::move(*snap);
    snap.reset();
    return s;
  }
};

// ---------------------------------------------------------------------------
// PyWritePlan — wraps move-only WritePlan; single-use, consumed by
// apply_batch.
// ---------------------------------------------------------------------------

struct PyWritePlan {
  std::optional<bytecask::WritePlan> plan;
  std::unique_ptr<nb::ft_mutex> mu{std::make_unique<nb::ft_mutex>()};

  PyWritePlan() : plan{bytecask::WritePlan{}} {}

  explicit PyWritePlan(bytecask::Snapshot snap)
      : plan{bytecask::WritePlan{std::move(snap)}} {}

  void put(nb::bytes key, nb::bytes value) {
    check();
    plan->put(to_view(key), to_view(value));
  }

  void del(nb::bytes key) {
    check();
    plan->del(to_view(key));
  }

  void del_range(nb::bytes from, nb::bytes to) {
    check();
    plan->del_range(to_view(from), to_view(to));
  }

  void ensure_present(nb::bytes key) {
    check();
    plan->ensure_present(to_view(key));
  }

  void ensure_absent(nb::bytes key) {
    check();
    plan->ensure_absent(to_view(key));
  }

  void ensure_unchanged(nb::bytes key) {
    check();
    plan->ensure_unchanged(to_view(key));
  }

  void ensure_range_unchanged(nb::bytes from, nb::bytes to) {
    check();
    plan->ensure_range_unchanged(to_view(from), to_view(to));
  }

  [[nodiscard]] auto has_snapshot() const -> bool {
    check();
    return plan->has_snapshot();
  }

  auto take() -> bytecask::WritePlan {
    nb::ft_lock_guard lock{*mu};
    check();
    auto p = std::move(*plan);
    plan.reset();
    return p;
  }

private:
  void check() const {
    if (!plan) {
      throw std::runtime_error(
          "WritePlan already consumed by apply_batch");
    }
  }
};

// ---------------------------------------------------------------------------
// Iterator wrappers — implement Python __iter__ / __next__ protocol.
// ---------------------------------------------------------------------------

struct PyEntryIterator {
  bytecask::EntryIterator cur;
  bool exhausted;

  explicit PyEntryIterator(bytecask::EntryIterator c)
      : cur{std::move(c)},
        exhausted{cur == std::default_sentinel} {}

  auto next() -> nb::tuple {
    if (exhausted) {
      throw nb::stop_iteration();
    }
    const auto &[key, value] = *cur;
    auto py_key = key_to_pybytes(key);
    auto py_val = to_pybytes(value);
    ++cur;
    exhausted = (cur == std::default_sentinel);
    return nb::make_tuple(py_key, py_val);
  }
};

struct PyKeyIterator {
  bytecask::KeyIterator cur;
  bool exhausted;

  explicit PyKeyIterator(bytecask::KeyIterator c)
      : cur{std::move(c)},
        exhausted{cur == std::default_sentinel} {}

  auto next() -> nb::bytes {
    if (exhausted) {
      throw nb::stop_iteration();
    }
    const auto &key = *cur;
    auto py_key = key_to_pybytes(key);
    ++cur;
    exhausted = (cur == std::default_sentinel);
    return py_key;
  }
};

struct PyReverseEntryIterator {
  bytecask::ReverseEntryIterator cur;
  bytecask::ReverseEntryIterator end;

  PyReverseEntryIterator(bytecask::ReverseEntryIterator c,
                         bytecask::ReverseEntryIterator e)
      : cur{std::move(c)}, end{std::move(e)} {}

  auto next() -> nb::tuple {
    if (cur == end) {
      throw nb::stop_iteration();
    }
    const auto &[key, value] = *cur;
    auto py_key = key_to_pybytes(key);
    auto py_val = to_pybytes(value);
    ++cur;
    return nb::make_tuple(py_key, py_val);
  }
};

struct PyReverseKeyIterator {
  bytecask::ReverseKeyIterator cur;
  bytecask::ReverseKeyIterator end;

  PyReverseKeyIterator(bytecask::ReverseKeyIterator c,
                       bytecask::ReverseKeyIterator e)
      : cur{std::move(c)}, end{std::move(e)} {}

  auto next() -> nb::bytes {
    if (cur == end) {
      throw nb::stop_iteration();
    }
    const auto &key = *cur;
    auto py_key = key_to_pybytes(key);
    ++cur;
    return py_key;
  }
};

} // namespace

NB_MODULE(_bytecaskdb, m) {
  // -------------------------------------------------------------------------
  // Exceptions
  // -------------------------------------------------------------------------

  nb::exception<bytecask::DbDegraded>(m, "DbDegraded", PyExc_RuntimeError);

  nb::register_exception_translator(
      [](const std::exception_ptr &p, void *) {
        try {
          std::rethrow_exception(p);
        } catch (const bytecask::DbDegraded &) {
          // Registered above — let nanobind handle it.
          throw;
        } catch (const std::system_error &e) {
          PyErr_SetString(PyExc_OSError, e.what());
        } catch (const std::logic_error &e) {
          PyErr_SetString(PyExc_ValueError, e.what());
        }
      });

  // -------------------------------------------------------------------------
  // Options
  // -------------------------------------------------------------------------

  nb::class_<bytecask::Options>(m, "Options",
      "Configuration for DB.open().")
      .def(nb::init<>())
      .def_rw("max_file_bytes", &bytecask::Options::max_file_bytes,
              "Active file rotation threshold in bytes (default 64 MiB).")
      .def_rw("recovery_threads", &bytecask::Options::recovery_threads,
              "Number of threads for parallel hint-file replay (default 4).")
      .def_rw("fail_recovery_on_crc_errors",
              &bytecask::Options::fail_recovery_on_crc_errors,
              "If True (default), CRC errors during recovery raise.");

  nb::class_<bytecask::WriteOptions>(m, "WriteOptions",
      "Per-write options for put, del_, apply_batch, etc.")
      .def(nb::init<>())
      .def_rw("sync", &bytecask::WriteOptions::sync,
              "If True (default), call fdatasync after write.")
      .def_rw("solo", &bytecask::WriteOptions::solo,
              "If True, bypass group commit (for benchmarking).");

  nb::class_<bytecask::ReadOptions>(m, "ReadOptions",
      "Per-read options for get, iter_from, etc.")
      .def(nb::init<>())
      .def_rw("verify_checksums", &bytecask::ReadOptions::verify_checksums,
              "If True, CRC-verify each value read from disk (default False).");

  nb::class_<bytecask::VacuumOptions>(m, "VacuumOptions",
      "Options for DB.vacuum().")
      .def(nb::init<>())
      .def_rw("fragmentation_threshold",
              &bytecask::VacuumOptions::fragmentation_threshold,
              "Minimum fragmentation ratio for a file to be eligible.");

  // -------------------------------------------------------------------------
  // Iterators
  // -------------------------------------------------------------------------

  nb::class_<PyEntryIterator>(m, "EntryIterator")
      .def("__iter__", [](PyEntryIterator &self) -> PyEntryIterator & {
        return self;
      })
      .def("__next__", &PyEntryIterator::next, nb::lock_self());

  nb::class_<PyKeyIterator>(m, "KeyIterator")
      .def("__iter__", [](PyKeyIterator &self) -> PyKeyIterator & {
        return self;
      })
      .def("__next__", &PyKeyIterator::next, nb::lock_self());

  nb::class_<PyReverseEntryIterator>(m, "ReverseEntryIterator")
      .def("__iter__",
           [](PyReverseEntryIterator &self) -> PyReverseEntryIterator & {
             return self;
           })
      .def("__next__", &PyReverseEntryIterator::next, nb::lock_self());

  nb::class_<PyReverseKeyIterator>(m, "ReverseKeyIterator")
      .def("__iter__",
           [](PyReverseKeyIterator &self) -> PyReverseKeyIterator & {
             return self;
           })
      .def("__next__", &PyReverseKeyIterator::next, nb::lock_self());

  // -------------------------------------------------------------------------
  // Snapshot
  // -------------------------------------------------------------------------

  nb::class_<PySnapshot>(m, "Snapshot",
      "Frozen, read-only view of the database at a point in time.\n\n"
      "Raises RuntimeError if used after being consumed by WritePlan.")
      .def(
          "get",
          [](PySnapshot &self, nb::bytes key,
             std::optional<bytecask::ReadOptions> opts) -> nb::object {
            self.check();
            bytecask::Bytes out;
            auto k = to_view(key);
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            nb::gil_scoped_release release;
            bool found = self.snap->get(ropts, k, out);
            nb::gil_scoped_acquire acquire;
            if (!found) return nb::none();
            return to_pybytes(out);
          },
          "Return the value for key, or None if not found.",
          "key"_a, "opts"_a = nb::none())
      .def(
          "contains_key",
          [](PySnapshot &self, nb::bytes key,
             std::optional<bytecask::ReadOptions> opts) -> bool {
            self.check();
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            return self.snap->contains_key(ropts, to_view(key));
          },
          "Return True if key exists. No disk I/O.",
          "key"_a, "opts"_a = nb::none())
      .def(
          "iter_from",
          [](PySnapshot &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyEntryIterator {
            self.check();
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.snap->iter_from(ropts, to_view(from_key));
            return PyEntryIterator{std::move(range.begin())};
          },
          "Iterate (key, value) pairs in ascending order from from_key.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "keys_from",
          [](PySnapshot &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyKeyIterator {
            self.check();
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.snap->keys_from(ropts, to_view(from_key));
            return PyKeyIterator{std::move(range.begin())};
          },
          "Iterate keys in ascending order. No disk I/O.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "riter_from",
          [](PySnapshot &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyReverseEntryIterator {
            self.check();
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.snap->riter_from(ropts, to_view(from_key));
            return PyReverseEntryIterator{std::move(range.begin()),
                                          std::move(range.end())};
          },
          "Iterate (key, value) pairs in descending order from from_key.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "rkeys_from",
          [](PySnapshot &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyReverseKeyIterator {
            self.check();
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.snap->rkeys_from(ropts, to_view(from_key));
            return PyReverseKeyIterator{std::move(range.begin()),
                                        std::move(range.end())};
          },
          "Iterate keys in descending order. No disk I/O.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      // Context manager support.
      .def("__enter__",
           [](nb::object self) -> nb::object {
             return self;
           })
      .def("__exit__",
           [](PySnapshot &, nb::args) {
             // Snapshot released when Python GC collects it.
             // __exit__ is a no-op — the destructor handles cleanup.
           });

  // -------------------------------------------------------------------------
  // WritePlan
  // -------------------------------------------------------------------------

  nb::class_<PyWritePlan>(m, "WritePlan",
      "Atomic write plan for DB.apply_batch().\n\n"
      "Construct without arguments for a simple unconditional batch.\n"
      "Construct with a Snapshot to enable ensure_unchanged guards and\n"
      "automatic write-write conflict detection.")
      .def(nb::init<>())
      .def("__init__",
           [](PyWritePlan *self, PySnapshot *snap) {
             new (self) PyWritePlan{snap->take()};
           },
           "snapshot"_a)
      .def("put", &PyWritePlan::put, "Stage a key-value write.",
           "key"_a, "value"_a, nb::lock_self())
      .def("del_", &PyWritePlan::del, "Stage a key deletion.",
           "key"_a, nb::lock_self())
      .def("del_range", &PyWritePlan::del_range,
           "Stage a range deletion: all keys in [from_key, to_key).",
           "from_key"_a, "to_key"_a, nb::lock_self())
      .def("ensure_present", &PyWritePlan::ensure_present,
           "Guard: key must exist at commit time.",
           "key"_a, nb::lock_self())
      .def("ensure_absent", &PyWritePlan::ensure_absent,
           "Guard: key must be absent at commit time.",
           "key"_a, nb::lock_self())
      .def("ensure_unchanged", &PyWritePlan::ensure_unchanged,
           "Guard: key must not have changed since the snapshot.",
           "key"_a, nb::lock_self())
      .def("ensure_range_unchanged", &PyWritePlan::ensure_range_unchanged,
           "Guard: no key in [from_key, to_key) changed since the snapshot.",
           "from_key"_a, "to_key"_a, nb::lock_self())
      .def_prop_ro("has_snapshot", &PyWritePlan::has_snapshot,
                   "True if this plan was constructed with a snapshot.");

  // -------------------------------------------------------------------------
  // DB
  // -------------------------------------------------------------------------

  nb::class_<PyDB>(m, "DB",
      "ByteCaskDB database handle.\n\n"
      "Open or create a database with DB.open(path).")
      .def_static(
          "open",
          [](std::filesystem::path path_arg,
             std::optional<bytecask::Options> opts) {
            return new PyDB(std::move(path_arg),
                            opts.value_or(bytecask::Options{}));
          },
          "Open or create a database at path.",
          "path"_a, "opts"_a = nb::none(), nb::rv_policy::take_ownership)
      .def(
          "get",
          [](PyDB &self, nb::bytes key,
             std::optional<bytecask::ReadOptions> opts) -> nb::object {
            bytecask::Bytes out;
            auto k = to_view(key);
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            nb::gil_scoped_release release;
            bool found = self.db.get(ropts, k, out);
            nb::gil_scoped_acquire acquire;
            if (!found) return nb::none();
            return to_pybytes(out);
          },
          "Return the value for key, or None if not found.",
          "key"_a, "opts"_a = nb::none())
      .def(
          "put",
          [](PyDB &self, nb::bytes key, nb::bytes value,
             std::optional<bytecask::WriteOptions> opts) {
            auto k = to_view(key);
            auto v = to_view(value);
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            self.db.put(wopts, k, v);
          },
          "Write key -> value. Overwrites any existing value.",
          "key"_a, "value"_a, "opts"_a = nb::none())
      .def(
          "del_",
          [](PyDB &self, nb::bytes key,
             std::optional<bytecask::WriteOptions> opts) -> bool {
            auto k = to_view(key);
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            return self.db.del(wopts, k);
          },
          "Delete key. Return True if it existed.",
          "key"_a, "opts"_a = nb::none())
      .def(
          "del_range",
          [](PyDB &self, nb::bytes from_key, nb::bytes to_key,
             std::optional<bytecask::WriteOptions> opts) {
            auto f = to_view(from_key);
            auto t = to_view(to_key);
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            self.db.del_range(wopts, f, t);
          },
          "Delete all keys in [from_key, to_key) with a single disk append.",
          "from_key"_a, "to_key"_a, "opts"_a = nb::none())
      .def(
          "contains_key",
          [](PyDB &self, nb::bytes key,
             std::optional<bytecask::ReadOptions> opts) -> bool {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            return self.db.contains_key(ropts, to_view(key));
          },
          "Return True if key exists. No disk I/O.",
          "key"_a, "opts"_a = nb::none())
      .def(
          "apply_batch",
          [](PyDB &self, PyWritePlan &plan,
             std::optional<bytecask::WriteOptions> opts) -> bool {
            auto p = plan.take();
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            return self.db.apply_batch(wopts, std::move(p));
          },
          "Apply plan atomically. Return True if committed, False on conflict.",
          "plan"_a, "opts"_a = nb::none())
      .def(
          "snapshot",
          [](PyDB &self) { return PySnapshot{self.db.snapshot()}; },
          "Return a frozen, read-only view of the database at this instant.")
      .def(
          "iter_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyEntryIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.iter_from(ropts, to_view(from_key));
            return PyEntryIterator{std::move(range.begin())};
          },
          "Iterate (key, value) pairs in ascending order from from_key.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "keys_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyKeyIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.keys_from(ropts, to_view(from_key));
            return PyKeyIterator{std::move(range.begin())};
          },
          "Iterate keys in ascending order. No disk I/O.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "riter_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts)
              -> PyReverseEntryIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.riter_from(ropts, to_view(from_key));
            return PyReverseEntryIterator{std::move(range.begin()),
                                          std::move(range.end())};
          },
          "Iterate (key, value) pairs in descending order from from_key.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "rkeys_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts)
              -> PyReverseKeyIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.rkeys_from(ropts, to_view(from_key));
            return PyReverseKeyIterator{std::move(range.begin()),
                                        std::move(range.end())};
          },
          "Iterate keys in descending order. No disk I/O.",
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none(),
          nb::keep_alive<0, 1>())
      .def(
          "vacuum",
          [](PyDB &self, std::optional<bytecask::VacuumOptions> opts)
              -> bool {
            auto vopts = opts.value_or(bytecask::VacuumOptions{});
            nb::gil_scoped_release release;
            return self.db.vacuum(vopts);
          },
          "Run one vacuum pass. Return True if a file was vacuumed.",
          "opts"_a = nb::none())
      .def_prop_ro(
          "is_degraded",
          [](PyDB &self) { return self.db.is_degraded(); },
          "True if the engine is in a degraded state.")
      .def_prop_ro(
          "degraded_reason",
          [](PyDB &self) -> std::string {
            return std::string{self.db.degraded_reason()};
          },
          "Diagnostic string describing why the engine degraded, or empty.")
      .def("resume",
           [](PyDB &self) {
             nb::gil_scoped_release release;
             self.db.resume();
           },
           "Attempt recovery from a degraded state.");
}
