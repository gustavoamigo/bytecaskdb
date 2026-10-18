// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "include/bytecask.hpp"

TEST_CASE("sql-layer links the engine", "[sql][smoke]") {
  auto dir = std::filesystem::temp_directory_path() / "sql_layer_smoke";
  std::filesystem::remove_all(dir);
  {
    auto db = bytecask::DB::open(dir);
    auto k = std::as_bytes(std::span{"k", 1});
    db.put({}, k, k);
    bytecask::Bytes out;
    CHECK(db.get({}, k, out));
    db.close();
  }
  std::filesystem::remove_all(dir);
}
