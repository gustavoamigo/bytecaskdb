// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The prototype's schema on a fresh database, and the runtime handles a plan
// needs:
//   CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT);
//   CREATE INDEX t_by_name ON t (name);

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/runtime.hpp"
#include "test_util.hpp"

namespace sql_test {

struct Fixture {
  TempDir dir{"plan"};
  std::unique_ptr<bytecask::DB> db;  // DB is not movable: open() is elided into new
  bytecask::sql::Catalog cat;
  bc_db bdb{};
  bc_catalog bcat{};
  bc_ctx* cx{nullptr};

  Fixture() { open(true); }
  ~Fixture() { close(); }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void open(bool create) {
    db.reset(new bytecask::DB(bytecask::DB::open(dir.path())));
    cat = bytecask::sql::Catalog::load(*db);
    if (create) {
      using bytecask::sql::PhysType;
      (void)cat.create_table(*db, "t",
                             {{"id", PhysType::I64, false}, {"name", PhysType::Bytes, true}, {"qty", PhysType::I64, true}},
                             {"id"});
      (void)cat.create_index(*db, "t_by_name", "t", {"name"});
    }
    bdb = {db.get()};
    bcat = {&cat};
    cx = bc_ctx_new(&bdb, &bcat, 1u << 20);
  }

  void close() {
    bc_ctx_free(cx);
    cx = nullptr;
    if (db) db->close();
    db.reset();
  }

  void reopen() {
    close();
    open(false);
  }

  // Result rows of the last execution, decoded by their result layout.
  auto results(std::uint32_t layout) -> std::vector<std::vector<bc_value>> {
    std::vector<std::vector<bc_value>> out;
    const auto* types = cat.result_types(layout);
    const auto* rl = cat.row_layout(layout);
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx, &row, &len)) {
      auto view = std::as_bytes(std::span{row, len});
      std::vector<bc_value> r;
      for (std::uint32_t i = 0; i < types->size(); ++i) {
        const auto& slot = rl->columns[i];
        if (bytecask::sql::row_is_null(view, slot)) {
          r.emplace_back(std::monostate{});
        } else if (slot.type == bytecask::sql::PhysType::I64) {
          r.emplace_back(*bytecask::sql::row_i64(view, slot.pos));
        } else {
          auto b = *bytecask::sql::row_bytes(view, slot.pos);
          r.emplace_back(bytecask::sql::Bytes(b.begin(), b.end()));
        }
      }
      out.push_back(std::move(r));
    }
    return out;
  }
};

inline auto params(std::vector<bc_value> v) -> bc_params { return bc_params{std::move(v)}; }

}  // namespace sql_test
