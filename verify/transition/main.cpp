// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — the transition-table checks of verify/transition/.
//
// Runs the two commit-path protocol models over every interleaving of N
// writers (plus the committer, in the write group's), checks their invariants
// after every transition, and then runs each mutant of #589 and requires the
// check to fail on it. The self-check is what ci.yml runs.
//
//   transition_check                    the self-check: the reference must be
//                                       clean at every size and every mutant
//                                       must be caught
//   transition_check --mutant=NAME      run one mutant and fail on it
//   transition_check --trace            print the counterexample of every run
//                                       that violates (a mutant runs with its
//                                       counterexample printed regardless)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "explorer.h"
#include "flush_model.h"
#include "writegroup_model.h"

namespace {

using transition::Explore;
using transition::Result;

// Writers per model run. The models are small; the state space grows with N
// and with the flush model's two outcomes per flush, and this range keeps the
// whole self-check under a second on a slow CI machine (a fraction of that
// here). Raise the tail of each list for a deeper local run; RunWGAt and
// RunFlushAt instantiate the sizes that are listed there.
constexpr int kWGSizes[] = {2, 3, 4, 5, 6};
constexpr int kFlushSizes[] = {2, 3, 4, 5};

struct Report {
  bool violated{false};
  double ms{0};
};

template <class Model>
auto RunModel(const char *model, const char *variant, int n, bool trace)
    -> Report {
  const auto t0 = std::chrono::steady_clock::now();
  typename Model::State initial{};
  const Result r = Explore<Model>(initial);
  const auto wall = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  std::printf("  %-10s %-24s N=%d  %-8s %8zu states %8zu done %6.1f ms\n",
              model, variant, n, r.clean ? "clean" : "VIOLATION", r.states,
              r.completions, wall);
  if (!r.clean && trace) {
    std::printf("      %s\n      trace:", r.failure.c_str());
    for (const auto &step : r.trace) std::printf(" %s", step.c_str());
    std::printf("\n");
  }
  if (r.truncated)
    std::printf("      (%s N=%d: exploration truncated at the depth limit)\n",
                variant, n);
  return {!r.clean, wall};
}

template <int N, transition::HandoverMutant M>
auto RunWGFor(const char *variant, int n, bool trace) -> Report {
  return RunModel<transition::WriteGroupModel<N, M>>("writegroup", variant, n,
                                                     trace);
}

template <transition::HandoverMutant M>
auto RunWGAt(const char *variant, int n, bool trace) -> Report {
  switch (n) {
    case 2:
      return RunWGFor<2, M>(variant, n, trace);
    case 3:
      return RunWGFor<3, M>(variant, n, trace);
    case 4:
      return RunWGFor<4, M>(variant, n, trace);
    case 5:
      return RunWGFor<5, M>(variant, n, trace);
    case 6:
      return RunWGFor<6, M>(variant, n, trace);
    default:
      std::printf("  unsupported writegroup N=%d\n", n);
      return {false, 0};
  }
}

template <int N, transition::FlushMutant M>
auto RunFlushFor(const char *variant, int n, bool trace) -> Report {
  return RunModel<transition::FlushModel<N, M>>("flush", variant, n, trace);
}

template <transition::FlushMutant M>
auto RunFlushAt(const char *variant, int n, bool trace) -> Report {
  switch (n) {
    case 2:
      return RunFlushFor<2, M>(variant, n, trace);
    case 3:
      return RunFlushFor<3, M>(variant, n, trace);
    case 4:
      return RunFlushFor<4, M>(variant, n, trace);
    case 5:
      return RunFlushFor<5, M>(variant, n, trace);
    default:
      std::printf("  unsupported flush N=%d\n", n);
      return {false, 0};
  }
}

// Which model instantiation a variant runs: the model, and the enumerator its
// template carries.
enum class Which {
  kWGNone,
  kWGSleepingReadEarly,
  kWGHandOverClearedAtWake,
  kWGWakeSkipped,
  kFlushNone,
  kFlushRoleKeptOnThrow,
};

struct Variant {
  const char *name;
  Which which;
  bool writegroup;
  bool mutant;
};

constexpr Variant kVariants[] = {
    {"reference", Which::kWGNone, true, false},
    {"sleeping-read-early", Which::kWGSleepingReadEarly, true, true},
    {"hand-over-cleared-at-wake", Which::kWGHandOverClearedAtWake, true, true},
    {"wake-skipped", Which::kWGWakeSkipped, true, true},
    {"flush-reference", Which::kFlushNone, false, false},
    {"flush-role-kept-on-throw", Which::kFlushRoleKeptOnThrow, false, true},
};

auto RunVariant(const Variant &v, int n, bool trace) -> Report {
  if (v.writegroup) {
    switch (v.which) {
      case Which::kWGNone:
        return RunWGAt<transition::HandoverMutant::kNone>(v.name, n, trace);
      case Which::kWGSleepingReadEarly:
        return RunWGAt<transition::HandoverMutant::kSleepingReadEarly>(
            v.name, n, trace);
      case Which::kWGHandOverClearedAtWake:
        return RunWGAt<transition::HandoverMutant::kHandOverClearedAtWake>(
            v.name, n, trace);
      case Which::kWGWakeSkipped:
        return RunWGAt<transition::HandoverMutant::kWakeSkipped>(v.name, n,
                                                                 trace);
      case Which::kFlushNone:
      case Which::kFlushRoleKeptOnThrow:
        break;
    }
  } else {
    switch (v.which) {
      case Which::kFlushNone:
        return RunFlushAt<transition::FlushMutant::kNone>(v.name, n, trace);
      case Which::kFlushRoleKeptOnThrow:
        return RunFlushAt<transition::FlushMutant::kRoleKeptOnThrow>(v.name,
                                                                     n,
                                                                     trace);
      case Which::kWGNone:
      case Which::kWGSleepingReadEarly:
      case Which::kWGHandOverClearedAtWake:
      case Which::kWGWakeSkipped:
        break;
    }
  }
  std::printf("  variant '%s' does not belong to that model\n", v.name);
  return {false, 0};
}

// Runs a variant over its sizes, stopping at the first one where the check
// fails. Returns the last report: `violated` says whether the check failed on
// it — the finding for a mutant, the defect for a reference.
auto RunOverSizes(const Variant &v, bool trace, double *total_ms)
    -> Report {
  Report last;
  const auto *begin = v.writegroup ? kWGSizes : kFlushSizes;
  const auto *end = begin + (v.writegroup ? std::size(kWGSizes)
                                          : std::size(kFlushSizes));
  for (const int *p = begin; p != end; ++p) {
    last = RunVariant(v, *p, trace);
    *total_ms += last.ms;
    if (last.violated) return last;
  }
  return last;
}

}  // namespace

int main(int argc, char **argv) {
  bool trace = false;
  const char *mutant = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--trace") == 0) trace = true;
    if (std::strncmp(argv[i], "--mutant=", 9) == 0) mutant = argv[i] + 9;
    if (std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "usage: transition_check [--mutant=NAME] [--trace]\n"
          "mutants: sleeping-read-early, hand-over-cleared-at-wake,\n"
          "         wake-skipped, flush-role-kept-on-throw\n");
      return 0;
    }
  }

  std::printf(
      "transition-table checks of the commit path (verify/transition/)\n");

  if (mutant != nullptr) {
    // One mutant on its own. The check is required to fail on it, so a run
    // that finds nothing is the failure of the check: exit 1 here means "the
    // check fails on this mutant", as asked for in #589.
    for (const auto &v : kVariants) {
      if (!v.mutant || std::strcmp(v.name, mutant) != 0) continue;
      double ignored = 0;
      const Report r = RunOverSizes(v, true, &ignored);
      std::printf("%s: the check %s\n", v.name,
                  r.violated ? "fails on it" : "does not fail on it");
      return r.violated ? 1 : 0;
    }
    std::printf("unknown mutant '%s'\n", mutant);
    return 2;
  }

  double total_ms = 0;
  bool ok = true;
  std::printf("\nthe protocol as it is on main:\n");
  for (const auto &v : kVariants) {
    if (v.mutant) continue;
    // The reference runs every size; RunOverSizes stops early only on a
    // violation, which for a reference is the finding itself.
    const Report r = RunOverSizes(v, trace, &total_ms);
    ok = ok && !r.violated;
  }

  std::printf("\nmutants, each required to fail the check:\n");
  for (const auto &v : kVariants) {
    if (!v.mutant) continue;
    // The counterexample is the evidence a mutant is caught: print it.
    const Report r = RunOverSizes(v, true, &total_ms);
    std::printf("  -> %s: %s\n", v.name, r.violated ? "caught" : "NOT CAUGHT");
    ok = ok && r.violated;
  }

  std::printf("\n%.0f ms total\n", total_ms);
  std::printf("%s\n", ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
