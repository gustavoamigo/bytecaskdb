// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The `code` both Node backends put on every error they throw, so a caller
// can tell a degraded engine from a closed one, or from bad input, without
// matching message text. The native addon and the Embind layer each build a
// JS Error from the same ErrorInfo; see docs/native_node_binding_design.md,
// "Error translation".
//
// Include after the engine's types are in scope (`import bytecask;` or
// include/bytecask.hpp): it names bytecask::DbDegraded, DbFollowerMode and
// DbClosed.

#pragma once

#include <stdexcept>
#include <string>
#include <system_error>

namespace bytecask_node {

// Thrown by a binding for a handle that can no longer be used: a closed DB,
// snapshot, plan or file manifest, or a snapshot or plan already consumed.
// Reported as BC_CLOSED, like the engine's own DbClosed.
// Header-only, so it has no out-of-line virtual to anchor its vtable; each
// binding is one translation unit, which emits it once.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
class HandleClosed : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};
#pragma clang diagnostic pop

struct ErrorInfo {
  const char *code;
  int errno_value;  // the system error's errno for BC_IO, otherwise 0
};

// Most specific class first: DbDegraded and system_error are runtime_errors,
// DbClosed and invalid_argument logic_errors. A runtime_error that is none of
// them (corruption, a packed limit) is BC_RUNTIME; the engine has no finer
// class for corruption yet.
inline auto classify_error(const std::exception &e) -> ErrorInfo {
  if (dynamic_cast<const bytecask::DbDegraded *>(&e)) return {"BC_DEGRADED", 0};
  if (dynamic_cast<const bytecask::DbFollowerMode *>(&e))
    return {"BC_FOLLOWER_MODE", 0};
  if (dynamic_cast<const bytecask::DbClosed *>(&e) ||
      dynamic_cast<const HandleClosed *>(&e))
    return {"BC_CLOSED", 0};
  if (const auto *se = dynamic_cast<const std::system_error *>(&e))
    return {"BC_IO",
            se->code().category() == std::generic_category() ||
                    se->code().category() == std::system_category()
                ? se->code().value()
                : 0};
  if (dynamic_cast<const std::invalid_argument *>(&e) ||
      dynamic_cast<const std::length_error *>(&e) ||
      dynamic_cast<const std::out_of_range *>(&e))
    return {"BC_INVALID_ARGUMENT", 0};
  if (dynamic_cast<const std::logic_error *>(&e)) return {"BC_LOGIC", 0};
  return {"BC_RUNTIME", 0};
}

}  // namespace bytecask_node
