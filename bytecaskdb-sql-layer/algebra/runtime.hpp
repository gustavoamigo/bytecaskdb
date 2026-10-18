// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.hpp — the C++ side of the plan API: the types bc_plan.h leaves
// opaque, built by the server (or a test) before it creates a context.

#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "bytecaskdb-sql-layer/catalog/catalog.hpp"

// The open engine a context reads through and commits to. Not owned.
struct bc_db {
  bytecask::DB* db;
};

// The live catalog. Not owned; must outlive every context built on it.
struct bc_catalog {
  const bytecask::sql::Catalog* cat;
};

// One bound parameter: SQL NULL, an integer, a double or bytes.
using bc_value = std::variant<std::monostate, std::int64_t, double, bytecask::sql::Bytes>;

struct bc_params {
  std::vector<bc_value> values;
};

// prototype: the affected-row count bc_emit_count recorded, or -1. The plan
// API has no runtime call that reads it back; see the write-up on #497.
[[nodiscard]] auto bc_ctx_affected(const bc_ctx* cx) -> std::int64_t;
