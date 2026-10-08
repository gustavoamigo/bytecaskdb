// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Counted allocation-failure sweep (#364): for each operation, fail its 1st,
// 2nd, …, n-th allocation in turn (alloc_faults.h), until a run completes
// without reaching the fault. Each run is a forked child, so an allocation
// failure that ends in std::terminate is an outcome to record, not the end of
// the test binary. Every run is classified:
//
//   completed        the fault was not reached
//   returned         the operation returned, its transition whole
//   threw, unchanged it threw and published nothing
//   threw, applied   it threw after publishing its whole transition
//   degraded         the engine degraded; resume() recovered it
//   terminated       the process died (std::terminate)
//   hung             the run did not end: always a failure
//
// and anything else — a running engine whose state is not the baseline or the
// whole transition, a structural inconsistency, a resume() that fails once
// memory is back, a reopen that recovers something else — fails the test.
// A terminated run's directory is reopened as after a SIGKILL and must hold
// the baseline or the whole transition. Whether termination is acceptable is
// not settled (#364): it is counted and reported, and fails the test only
// with BYTECASK_ALLOC_SWEEP_STRICT=1.
//
// BYTECASK_ALLOC_SWEEP_TRACE=1 prints each run's outcome, and the stack of
// each run that terminated. BYTECASK_ALLOC_SWEEP_STRIDE=k fails every k-th
// allocation instead of every one; BYTECASK_ALLOC_SWEEP_N=n only the n-th,
// and BYTECASK_ALLOC_SWEEP_PASS=one|cascade runs only that pass.
// See docs/correctness_validation.md, "Counted allocation-failure sweep".

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alloc_faults.h"
#include "fault_injector.h"
#include <catch2/catch_test_macros.hpp>

#ifndef BYTECASK_NO_ALLOC_FAULTS
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

import bytecask;
import bytecask.blind_btree;

#include "proof/invariants.h"
#include "sweep_operations.h"

#ifndef BYTECASK_NO_ALLOC_FAULTS

namespace {

using bytecask::DB;
using bytecask::Options;
using bytecask::testing::consistency_errors;
using bytecask::testing::key_values;
using bytecask::testing::ScopedAllocFaults;
using bytecask::testing::SuspendSyscallFaults;
using bytecask::testing::to_bytes;
using namespace bytecask::testing::sweep_ops;  // NOLINT(google-build-using-namespace)

// No operation here allocates more than this; a sweep that gets there is not
// converging.
constexpr long kMaxAllocations = 200'000;

// A run that has not ended by then is stuck.
constexpr int kRunSeconds = 20;

struct Pass {
  const char *name;
  bool cascade;
};

constexpr Pass kPasses[] = {
    {"one", false},
    {"every one from N on", true},
};

auto env_flag(const char *name) -> bool {
  const char *v = std::getenv(name);
  return v != nullptr && *v != '\0' && *v != '0';
}

// BYTECASK_ALLOC_SWEEP_N=n runs that one N in each pass, to investigate it.
auto only_n() -> long {
  const char *v = std::getenv("BYTECASK_ALLOC_SWEEP_N");
  return v != nullptr ? std::strtol(v, nullptr, 10) : 0;
}

auto stride() -> long {
  const char *v = std::getenv("BYTECASK_ALLOC_SWEEP_STRIDE");
  const long k = v != nullptr ? std::strtol(v, nullptr, 10) : 1;
  return k > 0 ? k : 1;
}

// A run's outcome, as the child reports it on its pipe.
struct Outcome {
  std::string kind;    // see the file comment; "error" for a failed check
  std::string detail;  // what failed, for "error"
  bool fired{false};
};

auto encode(const Outcome &o) -> std::string {
  return std::format("{}\t{}\t{}\n", o.kind, o.fired ? 1 : 0, o.detail);
}

auto decode(std::string_view line) -> std::optional<Outcome> {
  const auto a = line.find('\t');
  if (a == std::string_view::npos) return std::nullopt;
  const auto b = line.find('\t', a + 1);
  if (b == std::string_view::npos) return std::nullopt;
  auto detail = line.substr(b + 1);
  while (!detail.empty() && detail.back() == '\n') detail.remove_suffix(1);
  return Outcome{.kind = std::string{line.substr(0, a)},
                 .detail = std::string{detail},
                 .fired = line.substr(a + 1, b - a - 1) == "1"};
}

// Thrown in the child by a check that fails; caught at the top of the child.
struct CheckFailed {
  std::string what;
};

void expect(bool ok, std::string_view what) {
  if (!ok) throw CheckFailed{std::string{what}};
}

void expect_consistent(const DB &db, std::string_view when) {
  const auto errors = consistency_errors(db);
  if (!errors.empty())
    throw CheckFailed{std::format("{}: {}", when, errors.front())};
}

// The first key two states disagree on, for a failure message.
auto first_difference(const KeyValues &got, const KeyValues &want) -> std::string {
  for (const auto &[k, v] : want) {
    const auto it = got.find(k);
    if (it == got.end()) return std::format("'{}' missing", k);
    if (it->second != v) return std::format("'{}' has another value", k);
  }
  for (const auto &[k, v] : got) {
    if (!want.contains(k)) return std::format("'{}' should not be there", k);
  }
  return "no difference";
}

auto one_of(const KeyValues &got, std::initializer_list<const KeyValues *> states)
    -> bool {
  return std::ranges::any_of(states,
                             [&](const KeyValues *s) { return got == *s; });
}

// Opens two copies of `dir`, serial and parallel recovery, and holds both to
// `expected`.
void expect_reopens(const std::filesystem::path &dir, const KeyValues &expected,
                    const Options &opts, std::string_view when) {
  const auto twin = dir.parent_path() / (dir.filename().string() + "_parallel");
  std::filesystem::remove_all(twin);
  std::filesystem::copy(dir, twin);
  for (const auto &[d, threads] :
       {std::pair{dir, 1U}, std::pair{twin, 4U}}) {
    auto o = opts;
    o.recovery_threads = threads;
    auto db = DB::open(d, o);
    const auto got = key_values(db);
    expect(got == expected,
           std::format("{}: recovery with {} thread(s) recovered another state: {}",
                       when, threads, first_difference(got, expected)));
    expect_consistent(db, std::format("{}, recovered", when));
  }
}

// In the child: prints the stack std::terminate was called from, which still
// holds the frame the allocation failed in.
[[noreturn]] void print_stack_and_abort() {
  std::array<void *, 64> frames{};
  const auto n = ::backtrace(frames.data(), static_cast<int>(frames.size()));
  ::backtrace_symbols_fd(frames.data(), n, STDERR_FILENO);
  std::abort();
}

// What an operation does to the state, computed once in the parent so that a
// run whose child died can still be judged.
struct Expected {
  KeyValues baseline;
  KeyValues after;
  std::vector<KeyValues> partial;
};

auto expected_of(const Operation &op) -> Expected {
  TempDir td;
  auto db = open_db(td.path / "db", op.opts);
  op.setup(*db);
  Expected x;
  x.baseline = key_values(*db);
  x.after = op.transition(x.baseline);
  if (op.partial) x.partial = op.partial(x.baseline);
  return x;
}

// One run of `op` with allocation n failing, in the child. Never throws.
auto run_in_child(const Operation &op, const Pass &pass, long n,
                  const std::filesystem::path &dir) -> Outcome {
  Outcome out;
  try {
    auto db = open_db(dir, op.opts);
    op.setup(*db);
    wait_hints_idle(*db);
    const auto baseline = key_values(*db);
    const auto after = op.transition(baseline);
    const auto partial =
        op.partial ? op.partial(baseline) : std::vector<KeyValues>{};

    bool threw = false;
    bool did_work = false;
    {
      ScopedAllocFaults faults{n, pass.cascade};
      try {
        did_work = op.run(*db);
      } catch (...) {
        threw = true;
      }
      wait_hints_idle(*db);
      out.fired = faults.report().fired;
    }

    const auto live = key_values(*db);
    const bool degraded = db->is_degraded();
    if (!out.fired) {
      expect(!threw, "the fault was not reached, yet the operation threw");
      expect(did_work, "the fault was not reached, yet the operation did nothing");
      expect(!degraded, "the fault was not reached, yet the engine degraded");
      expect(live == after, "the fault was not reached, yet the state is not the transition");
      out.kind = "completed";
    } else if (degraded) {
      expect(one_of(live, {&baseline, &after}),
             "degraded with a state that is neither the baseline nor the transition");
      try {
        db->resume();
      } catch (const std::exception &e) {
        throw CheckFailed{std::format("resume() failed with memory back: {}", e.what())};
      }
      expect(!db->is_degraded(), "resume() returned, the engine still degraded");
      const auto resumed = key_values(*db);
      const bool kept_partial =
          live == baseline &&
          (resumed == baseline || std::ranges::find(partial, resumed) != partial.end());
      expect(kept_partial || resumed == after,
             "resume() neither kept nor completed the transition");
      out.kind = "degraded";
    } else if (threw) {
      expect(one_of(live, {&baseline, &after}),
             "threw, leaving a state that is neither the baseline nor the transition");
      out.kind = live == baseline ? "threw, unchanged" : "threw, applied";
    } else {
      expect(live == after, "returned, but the state is not the transition");
      out.kind = "returned";
    }
    expect_consistent(*db, "after the fault");

    if (op.retry) {
      op.retry(*db);
      expect(key_values(*db) == after, "the operation, run again, did not complete");
    }
    if (op.takes_writes) {
      expect(db->put({.sync = true}, to_bytes("~probe"), to_bytes("p")).durable,
             "a write after the fault was not durable");
    }
    const auto final_state = key_values(*db);
    db->close();
    db.reset();
    expect_reopens(dir, final_state, op.opts, "the clean reopen");
  } catch (const CheckFailed &f) {
    out.kind = "error";
    out.detail = f.what;
  } catch (const std::exception &e) {
    out.kind = "error";
    out.detail = std::format("a check threw: {}", e.what());
  } catch (...) {
    out.kind = "error";
    out.detail = "a check threw something that is not a std::exception";
  }
  return out;
}

// A child that died: the directory is what a SIGKILL leaves, the page cache
// intact. It must open to the baseline or the whole transition (or, for an
// operation of several atomic units, a partial state).
auto judge_terminated(const Operation &op, const Expected &x,
                      const std::filesystem::path &dir) -> std::string {
  try {
    for (const unsigned threads : {1U, 4U}) {
      const auto copy =
          dir.parent_path() / std::format("{}_dead_{}", dir.filename().string(), threads);
      std::filesystem::copy(dir, copy);
      auto o = op.opts;
      o.recovery_threads = threads;
      auto db = DB::open(copy, o);
      const auto got = key_values(db);
      if (got != x.baseline && got != x.after &&
          std::ranges::find(x.partial, got) == x.partial.end())
        return std::format("after the process died, recovery with {} thread(s) "
                           "found neither the baseline nor the transition",
                           threads);
      const auto errors = consistency_errors(db);
      if (!errors.empty())
        return std::format("after the process died: {}", errors.front());
    }
  } catch (const std::exception &e) {
    return std::format("after the process died, the reopen threw: {}", e.what());
  }
  return {};
}

struct Tally {
  std::map<std::string, long> counts;
  std::vector<std::string> failures;
  std::vector<std::string> terminations;
};

// Forks a child for run n and classifies it. Returns whether the fault fired.
auto run_case(const Operation &op, const Expected &x, const Pass &pass, long n,
              Tally &tally) -> bool {
  TempDir td;
  const auto dir = td.path / "db";
  std::array<int, 2> fds{};
  REQUIRE(::pipe(fds.data()) == 0);
  std::fflush(nullptr);
  const auto pid = ::fork();
  REQUIRE(pid != -1);
  if (pid == 0) {
    ::close(fds[0]);
    // Catch2's handler would report the abort as this test's failure and
    // print a summary from the child; the parent judges it instead.
    std::signal(SIGABRT, SIG_DFL);
    if (env_flag("BYTECASK_ALLOC_SWEEP_TRACE")) {
      std::set_terminate(print_stack_and_abort);
    } else if (const auto null = ::open("/dev/null", O_WRONLY); null != -1) {
      // The engine logs what it does about each failure (a hint the worker
      // could not write, …): thousands of lines a sweep, none a finding.
      ::dup2(null, STDERR_FILENO);
    }
    const auto line = encode(run_in_child(op, pass, n, dir));
    (void)::write(fds[1], line.data(), line.size());
    ::_exit(0);
  }
  ::close(fds[1]);
  // The child writes one line as it ends. One that has written nothing by
  // the deadline is stuck: a waiter whose wake-up the failed allocation lost.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{kRunSeconds};
  std::string got;
  bool hung = false;
  std::array<char, 512> buf{};
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      hung = true;
      break;
    }
    pollfd pfd{.fd = fds[0], .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&pfd, 1, static_cast<int>(left.count()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    const auto r = ::read(fds[0], buf.data(), buf.size());
    if (r > 0) {
      got.append(buf.data(), static_cast<std::size_t>(r));
    } else if (r == 0 || errno != EINTR) {
      break;
    }
  }
  ::close(fds[0]);
  if (hung) ::kill(pid, SIGKILL);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);

  const auto label = std::format("{} [{}] N = {}", op.name, pass.name, n);
  Outcome out;
  if (hung) {
    // A hang is never an acceptable outcome, whatever termination is.
    out = {.kind = "hung", .detail = {}, .fired = true};
    tally.failures.push_back(std::format(
        "{}: still running after {} s, killed", label, kRunSeconds));
  } else if (WIFSIGNALED(status)) {
    out = {.kind = "terminated", .detail = {}, .fired = true};
    tally.terminations.push_back(label);
    if (const auto err = judge_terminated(op, x, dir); !err.empty())
      tally.failures.push_back(std::format("{}: {}", label, err));
  } else if (auto decoded = decode(got); decoded && WIFEXITED(status) &&
                                         WEXITSTATUS(status) == 0) {
    out = *decoded;
    if (out.kind == "error")
      tally.failures.push_back(std::format("{}: {}", label, out.detail));
  } else {
    out = {.kind = "error", .detail = {}, .fired = true};
    tally.failures.push_back(
        std::format("{}: the child exited with status {} and reported '{}'",
                    label, status, got));
  }
  ++tally.counts[out.kind];
  if (env_flag("BYTECASK_ALLOC_SWEEP_TRACE"))
    std::fprintf(stderr, "alloc sweep %s: %s\n", label.c_str(), out.kind.c_str());
  return out.fired;
}

void alloc_sweep(const Operation &op) {
  const auto x = expected_of(op);
  const auto step = stride();
  Tally tally;
  const char *pass_filter = std::getenv("BYTECASK_ALLOC_SWEEP_PASS");
  for (const auto &pass : kPasses) {
    if (pass_filter != nullptr &&
        std::string_view{pass_filter} != (pass.cascade ? "cascade" : "one"))
      continue;
    if (const auto only = only_n(); only > 0) {
      (void)run_case(op, x, pass, only, tally);
      continue;
    }
    long n = 1;
    while (run_case(op, x, pass, n, tally)) {
      n += step;
      REQUIRE(n <= kMaxAllocations);
    }
    std::string summary;
    for (const auto &[kind, count] : tally.counts)
      summary += std::format("{}{} {}", summary.empty() ? "" : ", ", count, kind);
    std::fprintf(stderr, "alloc sweep %s [%s]: %ld runs: %s\n", op.name.c_str(),
                 pass.name, (n - 1) / step + 1, summary.c_str());
    tally.counts.clear();
  }
  for (const auto &f : tally.failures) FAIL_CHECK(f);
  if (!tally.terminations.empty()) {
    const auto msg = std::format(
        "{}: {} run(s) terminated the process, the first at {}", op.name,
        tally.terminations.size(), tally.terminations.front());
    if (env_flag("BYTECASK_ALLOC_SWEEP_STRICT")) {
      FAIL_CHECK(msg);
    } else {
      WARN(msg);
    }
  }
}

}  // namespace

// The counter must see the engine's allocations: a put allocates, and failing
// its first allocation makes it fail.
TEST_CASE("alloc sweep: the counter sees the engine's allocations",
          "[alloc_sweep]") {
  TempDir td;
  auto db = open_db(td.path / "db", {});
  {
    ScopedAllocFaults count{0, false};
    db->put({.sync = true}, to_bytes("k"), to_bytes("v"));
    CHECK(count.report().allocations > 0);
  }
  {
    ScopedAllocFaults fail{1, true};
    CHECK_THROWS(db->put({.sync = true}, to_bytes("k"), to_bytes("w")));
    CHECK(fail.report().fired);
  }
  // Test code under SuspendSyscallFaults is not counted.
  {
    ScopedAllocFaults count{0, false};
    {
      const SuspendSyscallFaults exempt;
      std::vector<int> v(64);
      (void)v;
    }
    CHECK(count.report().allocations == 0);
  }
}

TEST_CASE("alloc sweep: put", "[alloc_sweep]") { alloc_sweep(put_operation()); }

TEST_CASE("alloc sweep: del", "[alloc_sweep]") { alloc_sweep(del_operation()); }

TEST_CASE("alloc sweep: del_range", "[alloc_sweep]") {
  alloc_sweep(del_range_operation());
}

TEST_CASE("alloc sweep: apply_batch", "[alloc_sweep]") {
  alloc_sweep(apply_batch_operation());
}

TEST_CASE("alloc sweep: write across a rotation", "[alloc_sweep]") {
  alloc_sweep(rotation_operation());
}

TEST_CASE("alloc sweep: vacuum", "[alloc_sweep]") {
  alloc_sweep(vacuum_operation());
}

TEST_CASE("alloc sweep: ingest", "[alloc_sweep]") {
  alloc_sweep(ingest_operation());
}

TEST_CASE("alloc sweep: resume after a failed sync", "[alloc_sweep]") {
  alloc_sweep(resume_unsynced_operation());
}

TEST_CASE("alloc sweep: resume after a torn append", "[alloc_sweep]") {
  alloc_sweep(resume_torn_operation());
}


// ---- The key directory under allocation failure -------------------------

namespace {

struct KeyStore {
  std::vector<std::string> keys;
  auto ref(const std::string &k) -> bytecask::BlindRef {
    keys.push_back(k);
    return {1, static_cast<std::uint32_t>(keys.size() - 1)};
  }
  auto key_at(bytecask::BlindRef r) -> std::span<const std::byte> {
    return to_bytes(keys.at(r.offset));
  }
};

using Tree = bytecask::PersistentBlindBTree<bytecask::kBlindLeafBytes>;

auto deep_tree(KeyStore &res) -> Tree {
  Tree t;
  auto tr = t.transient();
  for (int i = 0; i < 20'000; ++i) {
    const auto k = std::format("user::{}::x", i * 7919 % 20'000);
    tr.set(to_bytes(k), res.ref(k), res);
  }
  return std::move(tr).persistent();
}

}  // namespace

// Runs fn in a forked child with stderr discarded; true if the child exited
// normally, false if it was ended by a signal (std::terminate aborts).
auto exits_cleanly_in_child(const std::function<void()> &fn) -> bool {
  std::fflush(nullptr);
  const auto pid = ::fork();
  REQUIRE(pid != -1);
  if (pid == 0) {
    std::signal(SIGABRT, SIG_DFL);
    if (const auto null = ::open("/dev/null", O_WRONLY); null != -1)
      ::dup2(null, STDERR_FILENO);
    fn();
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A transient dropped without publishing frees the nodes it created, in a
// noexcept destructor, typically because an allocation just failed: freeing
// must succeed when no allocation can, or the process ends there. (It may
// still try one: the node pool grows its free list, and frees the node
// instead when it cannot.)
TEST_CASE("alloc sweep: a transient tree is freed when nothing can be "
          "allocated",
          "[alloc_sweep]") {
  KeyStore res;
  const auto t = deep_tree(res);
  CHECK(exits_cleanly_in_child([&] {
    std::optional<ScopedAllocFaults> every_allocation_fails;
    {
      auto tr = t.transient();
      for (int i = 0; i < 2'000; ++i) {
        const auto k = std::format("user::{}::y", i * 31);
        tr.set(to_bytes(k), res.ref(k), res);
      }
      every_allocation_fails.emplace(1, true);
    }  // tr frees its nodes here
  }));
}

// A version whose publication fails leaves its base as it was, so the base
// can still be derived from: a half-published version would make every later
// write fail with "a version that already has a successor".
TEST_CASE("alloc sweep: a failed publication leaves the base derivable",
          "[alloc_sweep]") {
  KeyStore res;
  const auto t = deep_tree(res);
  for (long n = 1;; ++n) {
    REQUIRE(n <= kMaxAllocations);
    auto tr = t.transient();
    tr.set(to_bytes("new"), res.ref("new"), res);
    bool fired = false;
    {
      ScopedAllocFaults faults{n, false};
      try {
        const auto u = std::move(tr).persistent();
      } catch (const std::bad_alloc &) {
      }
      fired = faults.report().fired;
    }
    INFO("allocation " << n << " of the publication failed");
    auto again = t.transient();
    again.set(to_bytes("other"), res.ref("other"), res);
    CHECK_NOTHROW((void)std::move(again).persistent());
    if (!fired) break;
  }
}

#else  // BYTECASK_NO_ALLOC_FAULTS

TEST_CASE("alloc sweep: not available in this build", "[alloc_sweep]") {
  SKIP("operator new is not replaced under ThreadSanitizer, MemorySanitizer "
       "or WASM (alloc_faults.h)");
}

#endif  // BYTECASK_NO_ALLOC_FAULTS


// ---- Refusing writes after a failure past the append ---------------------
//
// Deterministic versions of what the sweep found, driven by test hooks, so
// they run on every build (sanitizers and WASM included) and in-process,
// where coverage sees them.

namespace {

using bytecask::DB;
using bytecask::DbDegraded;
using bytecask::Options;
using bytecask::testing::to_bytes;
using namespace bytecask::testing::sweep_ops;  // NOLINT(google-build-using-namespace)

auto value_of(const DB &db, std::string_view key) -> std::string {
  bytecask::Bytes out;
  if (!db.get({}, to_bytes(key), out)) return "<absent>";
  return bytecask::testing::to_string(out);
}

// The engine refuses every kind of write, and resume() brings it back with
// what reached the file; a reopen agrees.
void expect_refused_then_resumed(std::unique_ptr<DB> &db,
                                 const std::filesystem::path &dir,
                                 const Options &opts, std::string_view key,
                                 std::string_view resumed_value) {
  CHECK(db->is_degraded());
  CHECK(db->stats().at("bytecask.degraded") == 1);
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("late"), to_bytes("x")),
                  DbDegraded);
  CHECK_THROWS_AS((void)db->vacuum({.fragmentation_threshold = 0.0}),
                  DbDegraded);
  CHECK_THROWS_AS((void)db->create_manifest(), DbDegraded);
  REQUIRE_NOTHROW(db->resume());
  CHECK_FALSE(db->is_degraded());
  CHECK(value_of(*db, key) == resumed_value);
  bytecask::testing::assert_consistent(*db);
  CHECK(db->put({.sync = true}, to_bytes("late"), to_bytes("x")).durable);
  db->close();
  db.reset();
  auto reopened = DB::open(dir, opts);
  CHECK(value_of(reopened, key) == resumed_value);
  CHECK(value_of(reopened, "late") == "x");
  bytecask::testing::assert_consistent(reopened);
}

}  // namespace

TEST_CASE("refuse writes: a throw after the append refuses writes until "
          "resume()",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = open_db(dir, {});
  seed(*db);
  db->test_after_append_ = [] { throw std::bad_alloc{}; };
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("k1"), to_bytes("over")),
                  std::bad_alloc);
  db->test_after_append_ = nullptr;
  // Nothing was published, but the entry is in the file.
  CHECK(value_of(*db, "k1") == "v1");
  CHECK(db->degraded_reason().find("could not be published") !=
        std::string::npos);
  expect_refused_then_resumed(db, dir, {}, "k1", "over");
}

// Publishing the degraded state allocates, and the failure being handled is
// often that nothing could be: the flag alone must refuse.
TEST_CASE("refuse writes: the refusal holds when the degraded state cannot be "
          "published",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = open_db(dir, {});
  seed(*db);
  db->test_after_append_ = [] { throw std::bad_alloc{}; };
  db->test_before_refusal_publish_ = [] { throw std::bad_alloc{}; };
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("k1"), to_bytes("over")),
                  std::bad_alloc);
  db->test_after_append_ = nullptr;
  db->test_before_refusal_publish_ = nullptr;
  CHECK_FALSE(db->engine_state()->degraded);
  CHECK(db->degraded_reason().find("could not be published") !=
        std::string::npos);
  expect_refused_then_resumed(db, dir, {}, "k1", "over");
}

// A flush that throws before it publishes must give the flush role back, or
// every later writer waits for it forever.
TEST_CASE("refuse writes: a flush that throws releases the flush role",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = open_db(dir, {});
  seed(*db);
  db->test_before_publish_ = [] { throw std::bad_alloc{}; };
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("k1"), to_bytes("over")),
                  std::bad_alloc);
  db->test_before_publish_ = nullptr;
  expect_refused_then_resumed(db, dir, {}, "k1", "over");
}

// The same through quiesce(), which a write barrier takes: here
// create_manifest, run while a write is in the head and not yet published.
TEST_CASE("refuse writes: a barrier's flush that throws releases the flush "
          "role",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = open_db(dir, {});
  seed(*db);
  bool manifest_threw = false;
  bool once = true;
  db->test_before_commit_wait_ = [&] {
    if (!once) return;
    once = false;
    db->test_before_publish_ = [] { throw std::bad_alloc{}; };
    try {
      (void)db->create_manifest();
    } catch (const std::bad_alloc &) {
      manifest_threw = true;
    }
    db->test_before_publish_ = nullptr;
  };
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("k1"), to_bytes("over")),
                  std::bad_alloc);
  db->test_before_commit_wait_ = nullptr;
  CHECK(manifest_threw);
  expect_refused_then_resumed(db, dir, {}, "k1", "over");
}

// A rotation that fails once the new file exists, while it installs the
// files in the transient, leaves the transient half rotated: it must not be
// published, as the I/O failure's handler publishes it.
TEST_CASE("refuse writes: a rotation that fails in memory publishes nothing",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  const Options opts{.max_file_bytes = 512};
  auto db = open_db(dir, opts);
  seed(*db);
  db->test_in_finish_rotation_ = [] { throw std::bad_alloc{}; };
  const std::string big(600, 'x');
  CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("big"), to_bytes(big)),
                  std::bad_alloc);
  db->test_in_finish_rotation_ = nullptr;
  bytecask::testing::assert_consistent(*db);
  CHECK(value_of(*db, "big") == "<absent>");
  expect_refused_then_resumed(db, dir, opts, "big", big);
}
