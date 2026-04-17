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
// PyBatch — wraps move-only Batch; single-use, consumed by apply_batch.
// ---------------------------------------------------------------------------

struct PyBatch {
  std::optional<bytecask::Batch> batch{bytecask::Batch{}};

  void put(nb::bytes key, nb::bytes value) {
    check();
    batch->put(to_view(key), to_view(value));
  }

  void del(nb::bytes key) {
    check();
    batch->del(to_view(key));
  }

  void del_range(nb::bytes from, nb::bytes to) {
    check();
    batch->del_range(to_view(from), to_view(to));
  }

  auto take() -> bytecask::Batch {
    check();
    auto b = std::move(*batch);
    batch.reset();
    return b;
  }

private:
  void check() const {
    if (!batch) {
      throw std::runtime_error("Batch already consumed by apply_batch");
    }
  }
};

// ---------------------------------------------------------------------------
// PyWritePlan — wraps move-only WritePlan; single-use, consumed by
// apply_batch_if.
// ---------------------------------------------------------------------------

struct PyWritePlan {
  std::optional<bytecask::WritePlan> plan;

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
    check();
    auto p = std::move(*plan);
    plan.reset();
    return p;
  }

private:
  void check() const {
    if (!plan) {
      throw std::runtime_error(
          "WritePlan already consumed by apply_batch_if");
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

  nb::class_<bytecask::Options>(m, "Options")
      .def(nb::init<>())
      .def_rw("max_file_bytes", &bytecask::Options::max_file_bytes)
      .def_rw("recovery_threads", &bytecask::Options::recovery_threads)
      .def_rw("fail_recovery_on_crc_errors",
              &bytecask::Options::fail_recovery_on_crc_errors);

  nb::class_<bytecask::WriteOptions>(m, "WriteOptions")
      .def(nb::init<>())
      .def_rw("sync", &bytecask::WriteOptions::sync)
      .def_rw("solo", &bytecask::WriteOptions::solo);

  nb::class_<bytecask::ReadOptions>(m, "ReadOptions")
      .def(nb::init<>())
      .def_rw("verify_checksums", &bytecask::ReadOptions::verify_checksums);

  nb::class_<bytecask::VacuumOptions>(m, "VacuumOptions")
      .def(nb::init<>())
      .def_rw("fragmentation_threshold",
              &bytecask::VacuumOptions::fragmentation_threshold)
      .def_rw("absorb_threshold",
              &bytecask::VacuumOptions::absorb_threshold);

  // -------------------------------------------------------------------------
  // Iterators
  // -------------------------------------------------------------------------

  nb::class_<PyEntryIterator>(m, "EntryIterator")
      .def("__iter__", [](PyEntryIterator &self) -> PyEntryIterator & {
        return self;
      })
      .def("__next__", &PyEntryIterator::next);

  nb::class_<PyKeyIterator>(m, "KeyIterator")
      .def("__iter__", [](PyKeyIterator &self) -> PyKeyIterator & {
        return self;
      })
      .def("__next__", &PyKeyIterator::next);

  nb::class_<PyReverseEntryIterator>(m, "ReverseEntryIterator")
      .def("__iter__",
           [](PyReverseEntryIterator &self) -> PyReverseEntryIterator & {
             return self;
           })
      .def("__next__", &PyReverseEntryIterator::next);

  nb::class_<PyReverseKeyIterator>(m, "ReverseKeyIterator")
      .def("__iter__",
           [](PyReverseKeyIterator &self) -> PyReverseKeyIterator & {
             return self;
           })
      .def("__next__", &PyReverseKeyIterator::next);

  // -------------------------------------------------------------------------
  // Snapshot
  // -------------------------------------------------------------------------

  nb::class_<bytecask::Snapshot>(m, "Snapshot")
      .def(
          "get",
          [](bytecask::Snapshot &self, nb::bytes key) -> nb::object {
            bytecask::Bytes out;
            auto k = to_view(key);
            nb::gil_scoped_release release;
            bool found = self.get(k, out);
            nb::gil_scoped_acquire acquire;
            if (!found) return nb::none();
            return to_pybytes(out);
          },
          "key"_a)
      .def(
          "contains_key",
          [](bytecask::Snapshot &self, nb::bytes key) -> bool {
            return self.contains_key(to_view(key));
          },
          "key"_a)
      .def(
          "iter_from",
          [](bytecask::Snapshot &self,
             nb::bytes from_key) -> PyEntryIterator {
            auto range = self.iter_from(to_view(from_key));
            return PyEntryIterator{std::move(range.begin())};
          },
          "from_key"_a = nb::bytes("", 0))
      .def(
          "keys_from",
          [](bytecask::Snapshot &self,
             nb::bytes from_key) -> PyKeyIterator {
            auto range = self.keys_from(to_view(from_key));
            return PyKeyIterator{std::move(range.begin())};
          },
          "from_key"_a = nb::bytes("", 0))
      .def(
          "riter_from",
          [](bytecask::Snapshot &self,
             nb::bytes from_key) -> PyReverseEntryIterator {
            auto range = self.riter_from(to_view(from_key));
            return PyReverseEntryIterator{std::move(range.begin()),
                                          std::move(range.end())};
          },
          "from_key"_a = nb::bytes("", 0))
      .def(
          "rkeys_from",
          [](bytecask::Snapshot &self,
             nb::bytes from_key) -> PyReverseKeyIterator {
            auto range = self.rkeys_from(to_view(from_key));
            return PyReverseKeyIterator{std::move(range.begin()),
                                        std::move(range.end())};
          },
          "from_key"_a = nb::bytes("", 0))
      // Context manager support.
      .def("__enter__",
           [](nb::object self) -> nb::object {
             return self;
           })
      .def("__exit__",
           [](bytecask::Snapshot &, nb::args) {
             // Snapshot released when Python GC collects it.
             // __exit__ is a no-op — the destructor handles cleanup.
           });

  // -------------------------------------------------------------------------
  // Batch
  // -------------------------------------------------------------------------

  nb::class_<PyBatch>(m, "Batch")
      .def(nb::init<>())
      .def("put", &PyBatch::put, "key"_a, "value"_a)
      .def("del_", &PyBatch::del, "key"_a)
      .def("del_range", &PyBatch::del_range, "from_key"_a, "to_key"_a);

  // -------------------------------------------------------------------------
  // WritePlan
  // -------------------------------------------------------------------------

  nb::class_<PyWritePlan>(m, "WritePlan")
      .def(nb::init<>())
      .def("__init__",
           [](PyWritePlan *self, bytecask::Snapshot *snap) {
             new (self) PyWritePlan{std::move(*snap)};
           },
           "snapshot"_a)
      .def("put", &PyWritePlan::put, "key"_a, "value"_a)
      .def("del_", &PyWritePlan::del, "key"_a)
      .def("del_range", &PyWritePlan::del_range, "from_key"_a, "to_key"_a)
      .def("ensure_present", &PyWritePlan::ensure_present, "key"_a)
      .def("ensure_absent", &PyWritePlan::ensure_absent, "key"_a)
      .def("ensure_unchanged", &PyWritePlan::ensure_unchanged, "key"_a)
      .def("ensure_range_unchanged", &PyWritePlan::ensure_range_unchanged,
           "from_key"_a, "to_key"_a)
      .def_prop_ro("has_snapshot", &PyWritePlan::has_snapshot);

  // -------------------------------------------------------------------------
  // DB
  // -------------------------------------------------------------------------

  nb::class_<PyDB>(m, "DB")
      .def_static(
          "open",
          [](std::filesystem::path path_arg,
             std::optional<bytecask::Options> opts) {
            return new PyDB(std::move(path_arg),
                            opts.value_or(bytecask::Options{}));
          },
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
          "from_key"_a, "to_key"_a, "opts"_a = nb::none())
      .def(
          "contains_key",
          [](PyDB &self, nb::bytes key) -> bool {
            return self.db.contains_key(to_view(key));
          },
          "key"_a)
      .def(
          "apply_batch",
          [](PyDB &self, PyBatch &batch,
             std::optional<bytecask::WriteOptions> opts) {
            auto b = batch.take();
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            self.db.apply_batch(wopts, std::move(b));
          },
          "batch"_a, "opts"_a = nb::none())
      .def(
          "apply_batch_if",
          [](PyDB &self, PyWritePlan &plan,
             std::optional<bytecask::WriteOptions> opts) -> bool {
            auto p = plan.take();
            auto wopts = opts.value_or(bytecask::WriteOptions{});
            nb::gil_scoped_release release;
            return self.db.apply_batch_if(wopts, std::move(p));
          },
          "plan"_a, "opts"_a = nb::none())
      .def(
          "snapshot",
          [](PyDB &self) { return self.db.snapshot(); })
      .def(
          "iter_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyEntryIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.iter_from(ropts, to_view(from_key));
            return PyEntryIterator{std::move(range.begin())};
          },
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none())
      .def(
          "keys_from",
          [](PyDB &self, nb::bytes from_key,
             std::optional<bytecask::ReadOptions> opts) -> PyKeyIterator {
            auto ropts = opts.value_or(bytecask::ReadOptions{});
            auto range = self.db.keys_from(ropts, to_view(from_key));
            return PyKeyIterator{std::move(range.begin())};
          },
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none())
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
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none())
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
          "from_key"_a = nb::bytes("", 0), "opts"_a = nb::none())
      .def(
          "vacuum",
          [](PyDB &self, std::optional<bytecask::VacuumOptions> opts)
              -> bool {
            auto vopts = opts.value_or(bytecask::VacuumOptions{});
            nb::gil_scoped_release release;
            return self.db.vacuum(vopts);
          },
          "opts"_a = nb::none())
      .def_prop_ro(
          "is_degraded",
          [](PyDB &self) { return self.db.is_degraded(); })
      .def_prop_ro(
          "degraded_reason",
          [](PyDB &self) -> std::string {
            return std::string{self.db.degraded_reason()};
          })
      .def("resume",
           [](PyDB &self) {
             nb::gil_scoped_release release;
             self.db.resume();
           });
}
