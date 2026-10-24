// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — transition-table model of the write group's slot hand-over:
// `WriteGroup::submit`, `WriteGroup::run_batch` and `WriteGroup::committer_loop`
// in bytecaskdb/concurrency.cppm (the class at line 112). The protocol crosses
// three flags and a queue: `running_`, `hand_over_`, `committer_sleeping_` and
// `queue_` — plus `committer_cv_`, whose notify is the one step that reaches
// another thread.
//
// Each row of the table below is one step of one actor, and one critical
// section of the code: a step runs only when its actor reaches it, and the
// explorer enumerates every interleaving of N writers and the committer. The
// invariant is checked after every step (#589):
//
//   A non-empty queue implies a batch is running or the committer has been
//   woken to run one.
//
// Not the engine: no mutex object, no executor, no EngineSlot. What is
// modelled is the protocol the flags implement; what is folded away is named
// in the "Out of scope" list of verify/transition/README.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "explorer.h"

namespace transition {

// The committer thread; writers are 0..N-1. One submit per writer, which is
// the interleaving that matters: the hand-over is per batch, and a writer
// that submits twice only adds a second lap of the same protocol.
inline constexpr int kCommitter = -1;

// Mutants of the model, one per deviation named in #589. Each breaks one line
// of the code the way #589 describes it, and each names the check that fails
// on it with a `Guarded-by:` header of the discipline in
// docs/correctness_validation.md (*Durability sites*): the checker runs every
// mutant on every run (ci.yml) and fails unless each is caught.
enum class HandoverMutant {
  // The protocol as it is on main.
  kNone,
  // #589's first mutant: `committer_sleeping_` read before the queue lock is
  // released, so a wake can be sent to a thread that has not slept yet and,
  // here, withheld from one that has. The load is hoisted to `submit`, before
  // the critical section reads the flag as it is.
  // Guarded-by: "the hand-over invariant, at N=2"
  kSleepingReadEarly,
  // #589's second mutant: `hand_over_` cleared after the wake instead of at
  // handover (concurrency.cppm's `hand_over_.store(false)` moved from the
  // committer's takeover to its return from wait).
  // Guarded-by: "the hand-over invariant, at N=2"
  kHandOverClearedAtWake,
  // #589's third mutant: the wake skipped when `more` is true
  // (concurrency.cppm's `if (wake) committer_cv_.notify_one()`).
  // Guarded-by: "the hand-over invariant, at N=2"
  kWakeSkipped,
};

// The mutant is part of the model, not of a state: one model instantiation
// per variant, and every state it explores is a state of that variant's
// protocol.
template <int N, HandoverMutant Mutant = HandoverMutant::kNone>
struct WriteGroupModel {
  static constexpr std::size_t kN = static_cast<std::size_t>(N);

  enum class Wp : std::uint8_t {
    Idle,      // has not submitted
    Run,       // holds a batch and is about to run it
    End,       // the batch ran; the hand-over decision is next
    Notify,    // owes committer_cv_.notify_one()
    Waiting,   // queued behind a running batch, waiting for the release
    Finished,  // returned to its caller
  };

  enum class Slot : std::uint8_t {
    None,      // the writer has not submitted
    Queued,    // in queue_, waiting to be taken
    InBatch,   // taken by the batch some actor holds
    Executed,  // the executor ran it; its owner may return
  };

  enum class Cp : std::uint8_t {
    Gate,    // reads hand_over_ and either takes the queue or sleeps
    Parked,  // inside committer_cv_.wait, or on its way back out
    Run,     // holds a batch and is about to run it
    End,     // the batch ran; checks the queue for another one
  };

  struct State {
    std::array<Wp, kN> wp{};
    std::array<Slot, kN> slot{};
    // The flag as an unsynchronized read saw it at each writer's submit.
    // Only kSleepingReadEarly consults it.
    std::array<bool, kN> sampled{};
    Cp committer{Cp::Gate};
    std::uint8_t batch_owner{0};  // 0 none, 1 the committer, i + 2 writer i
    bool running{false};          // running_
    bool hand_over{false};        // hand_over_
    bool sleeping{false};         // committer_sleeping_
    bool parked{false};           // inside committer_cv_.wait
    bool wake_pending{false};     // a notify_one that reached the sleeper
    bool wake_owed{false};        // a decided notify_one not yet issued
  };

  struct Step {
    std::int8_t actor;  // kCommitter or a writer index
  };

  static auto Enabled(const State &s) -> std::vector<Step> {
    std::vector<Step> out;
    for (std::size_t i = 0; i < kN; ++i) {
      switch (s.wp[i]) {
        case Wp::Idle:
        case Wp::Run:
        case Wp::End:
        case Wp::Notify:
          out.push_back({static_cast<std::int8_t>(i)});
          break;
        case Wp::Waiting:
        case Wp::Finished:
          break;
      }
    }
    switch (s.committer) {
      case Cp::Gate:
      case Cp::Run:
      case Cp::End:
        out.push_back({kCommitter});
        break;
      case Cp::Parked:
        // The sleeper acts only on a notify_one that reached it. Spurious
        // wakeups are not modelled: a spurious wake would let a lost notify
        // be rescued by luck, and the check is about the protocol itself.
        if (s.wake_pending) out.push_back({kCommitter});
        break;
    }
    return out;
  }

  static auto Label(const State &s, const Step &step) -> std::string {
    if (step.actor == kCommitter) {
      switch (s.committer) {
        case Cp::Gate:
          return "c.gate";
        case Cp::Parked:
          return "c.wake";
        case Cp::Run:
          return "c.run";
        case Cp::End:
          return "c.end";
      }
    }
    const auto i = static_cast<std::size_t>(step.actor);
    const auto name = [&] {
      switch (s.wp[i]) {
        case Wp::Idle:
          return "submit";
        case Wp::Run:
          return "run";
        case Wp::End:
          return "end";
        case Wp::Notify:
          return "notify";
        case Wp::Waiting:
        case Wp::Finished:
          return "?";
      }
      return "?";
    };
    return "w" + std::to_string(step.actor) + "." + name();
  }

  static void Apply(State &s, const Step &step) {
    if (step.actor == kCommitter) return ApplyCommitter(s);
    const auto i = static_cast<std::size_t>(step.actor);
    switch (s.wp[i]) {
      case Wp::Idle:
        // submit (concurrency.cppm:157): the slot enters queue_; a writer that
        // finds no batch running runs one itself, taking the queue it has just
        // joined. The load of committer_sleeping_ that the wake decision
        // depends on is read here under the mutants that move it.
        s.sampled[i] = s.sleeping;
        s.slot[i] = Slot::Queued;
        if (s.running) {
          s.wp[i] = Wp::Waiting;
          break;
        }
        s.running = true;
        TakeQueue(s, i + 2);
        s.wp[i] = Wp::Run;
        break;
      case Wp::Run:
        // run_batch (concurrency.cppm:232): the executor runs the batch and
        // each slot is released to its owner (release + the owner's
        // wait_released are folded into this step; between them the owner
        // does nothing the invariant can see).
        RunBatch(s);
        s.wp[i] = Wp::End;
        break;
      case Wp::End:
        // The section that ends the batch (concurrency.cppm:187-197): `more`,
        // then either hand_over_ or running_ = false, then the wake decision.
        s.batch_owner = 0;
        if (Queued(s)) {
          s.hand_over = true;
          s.wake_owed = WakeDecided(s, i);
          s.wp[i] = s.wake_owed ? Wp::Notify : Wp::Finished;
        } else {
          s.running = false;
          s.wp[i] = Wp::Finished;
        }
        break;
      case Wp::Notify:
        // committer_cv_.notify_one() (concurrency.cppm:197), after the unlock:
        // it reaches the sleeper only if the sleeper is already in wait. A
        // notify to a thread that has not slept yet is dropped.
        if (s.parked) {
          s.parked = false;
          s.wake_pending = true;
        }
        s.wake_owed = false;
        s.wp[i] = Wp::Finished;
        break;
      case Wp::Waiting:
      case Wp::Finished:
        break;
    }
  }

  // The hand-over invariant (#589): "a non-empty queue implies a batch is
  // running or the committer has been woken to run one". `running` is checked
  // too, since a queued slot with the flag clear could not be handed to
  // anybody.
  static auto Check(const State &s, std::string *why) -> bool {
    const bool queued = Queued(s);
    if (queued && !s.running) {
      *why = "invariant: slots are queued while running_ is clear";
      return false;
    }
    if (!queued) return true;
    const bool batch_running = s.batch_owner != 0;
    const bool committer_tasked =
        s.hand_over && (!s.parked || s.wake_pending || s.wake_owed);
    if (batch_running || committer_tasked) return true;
    *why = "invariant: the queue is non-empty, no batch is running, and the "
           "committer has not been woken to run one";
    return false;
  }

  static auto Done(const State &s) -> bool {
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.wp[i] != Wp::Finished) return false;
    }
    return true;
  }

  static auto KeyOf(const State &s) -> Key {
    KeyBuilder k;
    for (std::size_t i = 0; i < kN; ++i) k.put(static_cast<std::uint64_t>(s.wp[i]), 3);
    for (std::size_t i = 0; i < kN; ++i)
      k.put(static_cast<std::uint64_t>(s.slot[i]), 2);
    for (std::size_t i = 0; i < kN; ++i) k.put(s.sampled[i] ? 1 : 0, 1);
    k.put(static_cast<std::uint64_t>(s.committer), 2);
    k.put(s.batch_owner, 4);
    k.put(s.running ? 1 : 0, 1);
    k.put(s.hand_over ? 1 : 0, 1);
    k.put(s.sleeping ? 1 : 0, 1);
    k.put(s.parked ? 1 : 0, 1);
    k.put(s.wake_pending ? 1 : 0, 1);
    k.put(s.wake_owed ? 1 : 0, 1);
    return k.done();
  }

 private:
  static auto Queued(const State &s) -> bool {
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.slot[i] == Slot::Queued) return true;
    }
    return false;
  }

  static void TakeQueue(State &s, std::size_t owner) {
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.slot[i] == Slot::Queued) s.slot[i] = Slot::InBatch;
    }
    s.batch_owner = static_cast<std::uint8_t>(owner);
  }

  static void RunBatch(State &s) {
    for (std::size_t i = 0; i < kN; ++i) {
      if (s.slot[i] != Slot::InBatch) continue;
      s.slot[i] = Slot::Executed;
      if (s.wp[i] == Wp::Waiting) s.wp[i] = Wp::Finished;
    }
  }

  // `wake = more && committer_sleeping_` (concurrency.cppm:195). The mutant
  // reads the load before the queue lock, where it can be stale.
  static auto WakeDecided(const State &s, std::size_t i) -> bool {
    if constexpr (Mutant == HandoverMutant::kWakeSkipped) {
      return false;
    } else if constexpr (Mutant == HandoverMutant::kSleepingReadEarly) {
      return s.sampled[i];
    } else {
      return s.sleeping;
    }
  }

  static void ApplyCommitter(State &s) {
    switch (s.committer) {
      case Cp::Gate:
        // The gate is committer_loop's one hold of queue_mu_ around its
        // decision (concurrency.cppm:310-319): read hand_over_ and either
        // take the queue or — with the check and the entry into wait in the
        // same hold — sleep. Spinning before the sleep is a timing property,
        // not protocol: it changes which of these two the committer picks.
        if (s.hand_over) {
          if constexpr (Mutant != HandoverMutant::kHandOverClearedAtWake)
            s.hand_over = false;  // cleared at handover
          TakeQueue(s, 1);
          s.committer = Cp::Run;
        } else {
          s.sleeping = true;
          s.parked = true;
          s.committer = Cp::Parked;
        }
        break;
      case Cp::Parked:
        // wait() returns with queue_mu_ held (concurrency.cppm:313).
        s.wake_pending = false;
        s.sleeping = false;
        if constexpr (Mutant == HandoverMutant::kHandOverClearedAtWake)
          s.hand_over = false;  // cleared after the wake, not at handover
        s.committer = Cp::Gate;
        break;
      case Cp::Run:
        RunBatch(s);
        s.committer = Cp::End;
        break;
      case Cp::End:
        // The drain loop (concurrency.cppm:322-330): the queue is emptied
        // batch after batch; running_ is cleared only when it is empty.
        if (Queued(s)) {
          TakeQueue(s, 1);
          s.committer = Cp::Run;
        } else {
          s.running = false;
          s.committer = Cp::Gate;
        }
        break;
    }
  }
};

}  // namespace transition
