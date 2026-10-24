// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — transition-table model of the fdatasync role on the commit
// path: `DB::flush_in_flight_`, `durable_cv_`, `flush_error_` and the
// published state as `DB::commit_wait`, `DB::flush_once`, `DB::flush_threw`
// and `DB::finish_flush` use them (bytecaskdb/db_commit.cpp:397-558).
//
// A writer commits, then waits in commit_wait for the flush that covers its
// target: durable_seq for a sync write, the published state for an unsynced
// one. It either takes the flush role itself or parks on durable_cv_ until
// the role is released. Each row below is one step of one writer; every
// interleaving of N writers is enumerated, and the invariant is checked after
// every step (#589):
//
//   A parked writer whose target is uncovered implies a flush is in flight,
//   an error is recorded, or the engine is degraded.
//
// "A flush is in flight" is not the flag but the promise the flag stands for:
// a thread is holding the role, between taking it and releasing it. The two
// are checked together, so a role left held with nobody holding it — the #364
// shape — fails the check instead of claiming cover it cannot deliver.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "explorer.h"

namespace transition {

enum class FlushMutant {
  // The protocol as it is on main.
  kNone,
  // #589's fourth mutant, the #364 shape (the same break as
  // tests/durability_mutations/flush_role_kept_on_throw.patch): a
  // flush_pending that throws escapes DB::flush_once without DB::flush_threw,
  // so DB::finish_flush never runs — the flush role is kept, nothing is
  // recorded, and every writer that parks afterwards is never woken.
  // Guarded-by: "the flush-role invariant, at N=2"; in the engine the same
  // break is guarded by "refuse writes: a flush that throws releases the
  // flush role" (which hangs).
  kRoleKeptOnThrow,
};

// The mutant is part of the model, not of a state: one model instantiation
// per variant, and every state it explores is a state of that variant's
// protocol.
template <int N, FlushMutant Mutant = FlushMutant::kNone>
struct FlushModel {
  static constexpr std::size_t kN = static_cast<std::size_t>(N);

  enum class Fp : std::uint8_t {
    Idle,      // has not committed
    Check,     // in commit_wait's loop, about to look at the state
    Parked,    // on durable_cv_, waiting for a flush that covers it
    Flushing,  // holds the role: DB::flush_once runs on this thread
    RoleEnd,   // the flush ran; DB::finish_flush is next
    Finished,  // returned to its caller
  };

  struct State {
    std::array<Fp, kN> fp{};
    std::array<std::uint8_t, kN> target{};  // the writer's commit sequence
    // Sequences: what has been appended, what the published state covers,
    // what fdatasync confirmed, and the highest one that asked for a sync.
    std::uint8_t seq{0};
    std::uint8_t published{0};
    std::uint8_t durable{0};
    std::uint8_t sync_requested{0};
    bool degraded{false};     // the published state, or write_fault_
    bool flush_error{false};  // flush_error_
    bool role{false};         // flush_in_flight_
  };

  struct Step {
    std::int8_t actor;  // a writer index
    bool fail{false};   // read only for the flush step
  };

  // The last writer commits without asking for a sync: the two coverage rules
  // are both in play at every size.
  static constexpr auto sync(std::size_t i) -> bool { return i != kN - 1; }

  static auto Enabled(const State &s) -> std::vector<Step> {
    std::vector<Step> out;
    for (std::size_t i = 0; i < kN; ++i) {
      const auto actor = static_cast<std::int8_t>(i);
      switch (s.fp[i]) {
        case Fp::Idle:
        case Fp::Check:
          out.push_back({actor, false});
          break;
        case Fp::Flushing:
          // The flush may land or throw where the counted allocation-failure
          // sweep puts std::bad_alloc (#364). Both outcomes are explored.
          out.push_back({actor, false});
          out.push_back({actor, true});
          break;
        case Fp::RoleEnd:
          // DB::flush_once ends in DB::finish_flush whatever happened.
          out.push_back({actor, false});
          break;
        case Fp::Parked:
        case Fp::Finished:
          break;
      }
    }
    return out;
  }

  static auto Label(const State &s, const Step &step) -> std::string {
    const auto i = static_cast<std::size_t>(step.actor);
    const char *name = "";
    switch (s.fp[i]) {
      case Fp::Idle:
        name = "commit";
        break;
      case Fp::Check:
        name = "check";
        break;
      case Fp::Flushing:
        name = step.fail ? "flush(throws)" : "flush";
        break;
      case Fp::RoleEnd:
        name = "finish_flush";
        break;
      case Fp::Parked:
      case Fp::Finished:
        name = "?";
        break;
    }
    return "w" + std::to_string(step.actor) + "." + name;
  }

  static void Apply(State &s, const Step &step) {
    const auto i = static_cast<std::size_t>(step.actor);
    switch (s.fp[i]) {
      case Fp::Idle:
        // A commit appends at the next sequence (execute_slots, under
        // write_mu_) and remembers what its caller wants covered.
        s.target[i] = ++s.seq;
        if (sync(i) && s.target[i] > s.sync_requested)
          s.sync_requested = s.target[i];
        s.fp[i] = Fp::Check;
        break;
      case Fp::Check:
        // commit_wait (db_commit.cpp:516): covered, or the error, or the
        // degraded engine, or take the role, or park. Checking the predicate
        // and parking in one step is what durable_cv_.wait(lk, pred) does.
        if (Covered(s, i)) {
          s.fp[i] = Fp::Finished;
        } else if (s.flush_error || s.degraded) {
          s.fp[i] = Fp::Finished;
        } else if (!s.role) {
          s.role = true;
          s.fp[i] = Fp::Flushing;
        } else {
          s.fp[i] = Fp::Parked;
        }
        break;
      case Fp::Flushing:
        if (step.fail) {
          ApplyThrow(s, i);
        } else {
          // flush_pending (db_commit.cpp:359-396): capture the head, fdatasync
          // if the head owes one, publish what was captured.
          const auto captured = s.seq;
          s.published = captured;
          if (s.sync_requested > s.durable) s.durable = captured;
          s.fp[i] = Fp::RoleEnd;
        }
        break;
      case Fp::RoleEnd:
        // finish_flush (db_commit.cpp:415): release the role under
        // durable_mu_ and notify durable_cv_.
        s.role = false;
        for (std::size_t j = 0; j < kN; ++j) {
          if (s.fp[j] == Fp::Parked) s.fp[j] = Fp::Check;
        }
        s.fp[i] = Fp::Finished;
        break;
      case Fp::Parked:
      case Fp::Finished:
        break;
    }
  }

  static auto Check(const State &s, std::string *why) -> bool {
    bool role_held = false;
    for (std::size_t j = 0; j < kN; ++j) {
      role_held = role_held || s.fp[j] == Fp::Flushing ||
                  s.fp[j] == Fp::RoleEnd;
    }
    if (s.role != role_held) {
      *why = s.role
          ? "invariant: the flush role is kept — flush_in_flight_ is set "
            "with no thread holding it, so no flush can ever cover a parked "
            "writer again"
          : "invariant: a thread is running a flush with flush_in_flight_ "
            "clear";
      return false;
    }
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.fp[i] != Fp::Parked) continue;
      if (Covered(s, i) || role_held || s.flush_error || s.degraded) continue;
      *why = "invariant: a parked writer's target is uncovered, with no "
             "flush in flight, no error recorded and the engine not "
             "degraded";
      return false;
    }
    return true;
  }

  static auto Done(const State &s) -> bool {
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.fp[i] != Fp::Finished) return false;
    }
    return true;
  }

  static auto KeyOf(const State &s) -> Key {
    KeyBuilder k;
    for (std::size_t i = 0; i < kN; ++i) k.put(static_cast<std::uint64_t>(s.fp[i]), 3);
    for (std::size_t i = 0; i < kN; ++i) k.put(s.target[i], 4);
    k.put(s.seq, 4);
    k.put(s.published, 4);
    k.put(s.durable, 4);
    k.put(s.sync_requested, 4);
    k.put(s.degraded ? 1 : 0, 1);
    k.put(s.flush_error ? 1 : 0, 1);
    k.put(s.role ? 1 : 0, 1);
    return k.done();
  }

 private:
  // commit_wait's coverage rule (db_commit.cpp:521-524).
  static auto Covered(const State &s, std::size_t i) -> bool {
    return sync(i) ? s.durable >= s.target[i] : s.published >= s.target[i];
  }

  // A flush that throws. On main DB::flush_once catches it, DB::flush_threw
  // records the error and refuses writes, and DB::finish_flush still releases
  // the role and notifies (db_commit.cpp:421-475). The mutant is the #364
  // shape: the throw escapes before anything is recorded, and the role is
  // never released — the same break as
  // tests/durability_mutations/flush_role_kept_on_throw.patch.
  static void ApplyThrow(State &s, std::size_t i) {
    if constexpr (Mutant == FlushMutant::kRoleKeptOnThrow) {
      s.fp[i] = Fp::Finished;  // the role is kept; nothing is recorded
    } else {
      s.degraded = true;
      s.flush_error = true;
      s.fp[i] = Fp::RoleEnd;
    }
  }
};

}  // namespace transition
