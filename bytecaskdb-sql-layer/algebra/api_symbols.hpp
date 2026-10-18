// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#pragma once

#include <span>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h.
[[nodiscard]] auto plan_api_symbols() -> std::span<const ApiSymbol>;
