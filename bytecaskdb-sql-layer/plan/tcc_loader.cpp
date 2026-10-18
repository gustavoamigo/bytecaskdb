// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
// prototype: see tcc_loader.hpp.

#include "bytecaskdb-sql-layer/plan/tcc_loader.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include "bytecaskdb-sql-layer/algebra/api_symbols.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <libtcc.h>
#pragma clang diagnostic pop

namespace bytecask::sql {

namespace {

void collect_error(void* opaque, const char* msg) {
  auto& out = *static_cast<std::string*>(opaque);
  out += msg;
  out += '\n';
}

// A void* from tcc_get_symbol as the plan's function type; POSIX guarantees
// the conversion, as for dlsym.
auto as_plan_fn(void* p) -> bc_plan_fn { return reinterpret_cast<bc_plan_fn>(p); }

}  // namespace

TccPlan::~TccPlan() {
  if (state_ != nullptr) tcc_delete(state_);
}

auto TccPlan::compile(std::string_view source) -> std::unique_ptr<TccPlan> {
  auto start = std::chrono::steady_clock::now();
  std::unique_ptr<TccPlan> plan{new TccPlan};
  std::string errors;
  plan->state_ = tcc_new();
  if (plan->state_ == nullptr) throw std::runtime_error("tcc_new failed");
  auto* s = plan->state_;
  tcc_set_error_func(s, &errors, collect_error);
  // TinyCC's own headers (stdbool.h, stddef.h) and the plan API's header.
  // prototype: both are paths in the build tree, fixed at compile time.
  tcc_set_lib_path(s, SQL_LAYER_TCC_LIB_PATH);
  tcc_add_include_path(s, SQL_LAYER_ALGEBRA_DIR);
  tcc_set_options(s, "-nostdlib");
  tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
  for (const auto& sym : plan_api_symbols()) tcc_add_symbol(s, sym.name, sym.address);
  std::string text{source};
  if (tcc_compile_string(s, text.c_str()) != 0) throw std::runtime_error("plan does not compile:\n" + errors);
  if (tcc_relocate(s) < 0) throw std::runtime_error("plan does not link:\n" + errors);
  plan->fn_ = as_plan_fn(tcc_get_symbol(s, "plan"));
  if (plan->fn_ == nullptr) throw std::runtime_error("plan defines no plan()");
  plan->compile_us_ =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  return plan;
}

}  // namespace bytecask::sql
