// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// tcc_loader.cppm — compiles a plan file in process with TinyCC and links it
// against the plan API by name. The plan sees only the functions in
// plan_api_symbols(): no libc, no other symbol of the process. libtcc.h is
// seen by tcc_loader.cpp, which holds the definitions, and nowhere else.
//
// prototype: the run-time tier of #493, without the checker that must run
// before it.

module;
#include <memory>
#include <string_view>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

export module bytecask.sql.tcc_loader;

// libtcc.h's opaque compiler state, which TccPlan holds. Declared in the
// global module, as the header declares it ([basic.link]/10), so
// tcc_loader.cpp's include of the header completes the same entity.
extern "C" {
struct TCCState;
}

export namespace bytecask::sql {

class TccPlan {
public:
  // Throws std::runtime_error with TinyCC's diagnostics if the file does not
  // compile or link.
  [[nodiscard]] static auto compile(std::string_view source) -> std::unique_ptr<TccPlan>;

  ~TccPlan();
  TccPlan(const TccPlan&) = delete;
  TccPlan& operator=(const TccPlan&) = delete;

  // Valid while this object lives: the code is in memory it owns.
  [[nodiscard]] auto fn() const noexcept -> bc_plan_fn { return fn_; }
  [[nodiscard]] auto compile_us() const noexcept -> double { return compile_us_; }

private:
  TccPlan() = default;
  TCCState* state_{nullptr};
  bc_plan_fn fn_{nullptr};
  double compile_us_{0};
};

}  // namespace bytecask::sql
