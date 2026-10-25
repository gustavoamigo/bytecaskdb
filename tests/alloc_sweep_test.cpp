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
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
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
import bytecask.btree;

#include "proof/invariants.h"
#include "sweep_operations.h"

#ifndef BYTECASK_NO_ALLOC_FAULTS

namespace {

using bytecask::DB;
using bytecask::Options;
using bytecask::testing::consistency_errors;
using bytecask::testing::key_values;
using bytecask::testing::ScopedAllocFaults;
using bytecask::testing::ExpectedLeaks;
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

// How a sweep child ends when std::terminate is called, so the parent tells
// it from an abort: whether the engine may end the process for want of
// memory is not settled, but an assertion or a panic that aborts is a bug.
constexpr int kTerminatedExitCode = 71;

void print_stack() {
  std::array<void *, 64> frames{};
  const auto n = ::backtrace(frames.data(), static_cast<int>(frames.size()));
  ::backtrace_symbols_fd(frames.data(), n, STDERR_FILENO);
}

// The signals a child can die of. Catch2's handlers for them, installed
// around each test case and inherited through fork, report the death as the
// running test's failure and write the reporter's output from the child too:
// with --out, a second report appended to the parent's file. A child goes
// back to the handlers the process started with instead: a sanitizer's,
// which still reports the error, or the default action.
constexpr std::array kFatalSignals{SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};

// Taken before main, so before Catch2 installs its own.
struct StartupHandlers {
  std::array<struct sigaction, kFatalSignals.size()> act{};
  StartupHandlers() {
    for (std::size_t i = 0; i < kFatalSignals.size(); ++i)
      ::sigaction(kFatalSignals[i], nullptr, &act[i]);
  }
  void restore(int sig) const {
    for (std::size_t i = 0; i < kFatalSignals.size(); ++i)
      if (kFatalSignals[i] == sig)
        ::sigaction(sig, &act[i], nullptr);
  }
};
const StartupHandlers g_startup_handlers;

// With BYTECASK_ALLOC_SWEEP_TRACE: a crash prints its stack first.
void print_stack_and_reraise(int sig) {
  print_stack();
  g_startup_handlers.restore(sig);
  std::raise(sig);
}

void startup_fatal_signals_in_child(bool trace) {
  for (const auto sig : kFatalSignals) {
    if (trace && sig != SIGABRT)
      std::signal(sig, print_stack_and_reraise);
    else
      g_startup_handlers.restore(sig);
  }
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
    // A warm pool hands out nodes without operator new, so which node
    // allocations the sweep counts would depend on the tests that ran
    // before (#583).
    bytecask::btree_detail::node_pool_trim();
    bool io_failed = false;
    {
      const auto io = op.io_fault ? op.io_fault() : nullptr;
      ScopedAllocFaults faults{n, pass.cascade};
      try {
        did_work = op.run(*db);
      } catch (...) {
        threw = true;
      }
      wait_hints_idle(*db);
      out.fired = faults.report().fired;
      io_failed = io && io->inj.fired;
    }

    const auto live = key_values(*db);
    const bool degraded = db->is_degraded();
    if (io_failed)
      expect(degraded, "an I/O failure was reported, yet the engine accepts writes");
    if (!out.fired && !io_failed) {
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

// Forks a child that runs `child` on a fresh directory and classifies it;
// `judge_dead` judges the directory a child that died left. Returns whether
// the fault fired.
auto run_forked(
    const std::string &label,
    const std::function<Outcome(const std::filesystem::path &)> &child,
    const std::function<std::string(const std::filesystem::path &)> &judge_dead,
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
    const auto trace = env_flag("BYTECASK_ALLOC_SWEEP_TRACE");
    startup_fatal_signals_in_child(trace);
    if (trace) {
      // The stack std::terminate was called from still holds the frame the
      // allocation failed in.
      std::set_terminate([] {
        print_stack();
        std::_Exit(kTerminatedExitCode);
      });
    } else {
      std::set_terminate([] { std::_Exit(kTerminatedExitCode); });
    }
    if (!trace) {
      if (const auto null = ::open("/dev/null", O_WRONLY); null != -1) {
      // The engine logs what it does about each failure (a hint the worker
      // could not write, …): thousands of lines a sweep, none a finding.
        ::dup2(null, STDERR_FILENO);
      }
    }
    const auto line = encode(child(dir));
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

  Outcome out;
  if (hung) {
    // A hang is never an acceptable outcome, whatever termination is.
    out = {.kind = "hung", .detail = {}, .fired = true};
    tally.failures.push_back(std::format(
        "{}: still running after {} s, killed", label, kRunSeconds));
  } else if (WIFEXITED(status) && WEXITSTATUS(status) == kTerminatedExitCode) {
    out = {.kind = "terminated", .detail = {}, .fired = true};
    tally.terminations.push_back(label);
    if (const auto err = judge_dead(dir); !err.empty())
      tally.failures.push_back(std::format("{}: {}", label, err));
  } else if (WIFSIGNALED(status) ||
             (WIFEXITED(status) &&
              WEXITSTATUS(status) == bytecask::btree_detail::kFreedNodeExitCode)) {
    // An abort that is not std::terminate (an assertion, a panic), a memory
    // error, or a key directory node used after it was freed.
    out = {.kind = "crashed", .detail = {}, .fired = true};
    tally.failures.push_back(
        WIFSIGNALED(status)
            ? std::format("{}: the child was killed by {}", label,
                          ::strsignal(WTERMSIG(status)))
            : std::format("{}: the child used a key directory node after it "
                          "was freed",
                          label));
    if (const auto err = judge_dead(dir); !err.empty())
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

// Run n of `op`'s sweep.
auto run_case(const Operation &op, const Expected &x, const Pass &pass, long n,
              Tally &tally) -> bool {
  return run_forked(
      std::format("{} [{}] N = {}", op.name, pass.name, n),
      [&](const std::filesystem::path &dir) {
        return run_in_child(op, pass, n, dir);
      },
      [&](const std::filesystem::path &dir) {
        return judge_terminated(op, x, dir);
      },
      tally);
}

// Runs `run_n(pass, n, tally)` for n = 1, 2, … in each pass until a run
// the fault never reached, and reports what the runs found.
// `strict`: a termination fails the test, as with
// BYTECASK_ALLOC_SWEEP_STRICT, for an operation whose contract says it ends
// nothing.
void sweep_passes(
    const std::string &name,
    const std::function<bool(const Pass &, long, Tally &)> &run_n,
    bool strict = false) {
  const auto step = stride();
  Tally tally;
  const char *pass_filter = std::getenv("BYTECASK_ALLOC_SWEEP_PASS");
  for (const auto &pass : kPasses) {
    if (pass_filter != nullptr &&
        std::string_view{pass_filter} != (pass.cascade ? "cascade" : "one"))
      continue;
    if (const auto only = only_n(); only > 0) {
      (void)run_n(pass, only, tally);
      continue;
    }
    long n = 1;
    while (run_n(pass, n, tally)) {
      n += step;
      REQUIRE(n <= kMaxAllocations);
    }
    std::string summary;
    for (const auto &[kind, count] : tally.counts)
      summary += std::format("{}{} {}", summary.empty() ? "" : ", ", count, kind);
    std::fprintf(stderr, "alloc sweep %s [%s]: %ld runs: %s\n", name.c_str(),
                 pass.name, (n - 1) / step + 1, summary.c_str());
    tally.counts.clear();
  }
  for (const auto &f : tally.failures) FAIL_CHECK(f);
  if (!tally.terminations.empty()) {
    const auto msg = std::format(
        "{}: {} run(s) terminated the process, the first at {}", name,
        tally.terminations.size(), tally.terminations.front());
    if (strict || env_flag("BYTECASK_ALLOC_SWEEP_STRICT")) {
      FAIL_CHECK(msg);
    } else {
      WARN(msg);
    }
  }
}

void alloc_sweep(const Operation &op) {
  const auto x = expected_of(op);
  sweep_passes(op.name, [&](const Pass &pass, long n, Tally &tally) {
    return run_case(op, x, pass, n, tally);
  });
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

TEST_CASE("alloc sweep: put under a snapshot", "[alloc_sweep]") {
  alloc_sweep(snapshot_put_operation());
}

TEST_CASE("alloc sweep: del_range", "[alloc_sweep]") {
  alloc_sweep(del_range_operation());
}

TEST_CASE("alloc sweep: apply_batch", "[alloc_sweep]") {
  alloc_sweep(apply_batch_operation());
}

TEST_CASE("alloc sweep: apply_batch that splits what it changed",
          "[alloc_sweep]") {
  alloc_sweep(splitting_batch_operation());
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

TEST_CASE("alloc sweep: ingest whose append fails", "[alloc_sweep]") {
  alloc_sweep(ingest_failed_append_operation());
}

TEST_CASE("alloc sweep: set_mode(Leader) whose marker append fails",
          "[alloc_sweep]") {
  alloc_sweep(promotion_failed_append_operation());
}

TEST_CASE("alloc sweep: set_mode(Follower) whose fdatasync fails",
          "[alloc_sweep]") {
  alloc_sweep(step_down_failed_sync_operation());
}

TEST_CASE("alloc sweep: create_manifest whose fdatasync fails",
          "[alloc_sweep]") {
  alloc_sweep(manifest_failed_sync_operation());
}

TEST_CASE("alloc sweep: resume after a failed sync", "[alloc_sweep]") {
  alloc_sweep(resume_unsynced_operation());
}

TEST_CASE("alloc sweep: resume after a torn append", "[alloc_sweep]") {
  alloc_sweep(resume_torn_operation());
}

// Recovery under allocation failure. No other sweep opens a DB with a fault
// armed, so nothing reached the parallel merge or the join of its ranges,
// where a failed allocation once freed leaves twice (#588). The database
// has several data files with their hints and enough keys that each range
// parallel recovery merges spans several key directory leaves; the joins of
// larger trees are swept in process above. Each run opens a copy with
// allocation n failing, under serial and under parallel recovery: the open
// throws std::bad_alloc, or it opens to every key, and either way the
// directory then reopens, under serial and parallel recovery, to every key.
// The contract says an open that cannot allocate ends nothing else, so a
// termination fails the test here (CONTRACT.md, `open`).
TEST_CASE("alloc sweep: open", "[alloc_sweep]") {
  TempDir td;
  const auto src = td.path / "src";
  Options opts;
  opts.max_file_bytes = 48 * 1024;
  KeyValues expected;
  {
    auto db = open_db(src, opts);
    for (int i = 0; i < 1'500; ++i)
      db->put({.sync = false}, to_bytes(std::format("user:{:05}", i * 7 % 1'500)),
              to_bytes(std::format("v{}", i)));
    for (int i = 0; i < 1'500; i += 10)
      (void)db->del({.sync = false}, to_bytes(std::format("user:{:05}", i)));
    expected = key_values(*db);
    db->close();
  }
  REQUIRE(std::ranges::count_if(
              std::filesystem::directory_iterator{src}, [](const auto &e) {
                return e.path().extension() == ".hint";
              }) >= 2);
  for (const unsigned threads : {1U, 4U}) {
    auto o = opts;
    o.recovery_threads = threads;
    const auto name = std::format("open with {} recovery thread(s)", threads);
    sweep_passes(name, [&](const Pass &pass, long n, Tally &tally) {
      return run_forked(
          std::format("{} [{}] N = {}", name, pass.name, n),
          [&](const std::filesystem::path &dir) {
            Outcome out;
            try {
              std::filesystem::copy(src, dir);
              std::unique_ptr<DB> db;
              std::exception_ptr thrown;  // looked at once memory is back
              {
                ScopedAllocFaults faults{n, pass.cascade};
                try {
                  db = open_db(dir, o);
                } catch (...) {
                  thrown = std::current_exception();
                }
                out.fired = faults.report().fired;
              }
              if (thrown) {
                try {
                  std::rethrow_exception(thrown);
                } catch (const std::bad_alloc &) {
                } catch (const std::exception &e) {
                  throw CheckFailed{std::format(
                      "open threw something other than std::bad_alloc: {}",
                      e.what())};
                } catch (...) {
                  throw CheckFailed{"open threw something that is not a "
                                    "std::exception"};
                }
              }
              if (db) {
                expect(key_values(*db) == expected,
                       "opened to another state");
                expect_consistent(*db, "opened under the fault");
                db->close();
                db.reset();
                out.kind = "opened";
              } else {
                expect(out.fired, "the fault was not reached, yet open threw");
                out.kind = "threw";
              }
              expect_reopens(dir, expected, o, "the reopen");
            } catch (const CheckFailed &f) {
              out = {.kind = "error", .detail = f.what, .fired = true};
            } catch (const std::exception &e) {
              out = {.kind = "error",
                     .detail = std::format("a check threw: {}", e.what()),
                     .fired = true};
            }
            return out;
          },
          [&](const std::filesystem::path &dir) -> std::string {
            try {
              expect_reopens(dir, expected, o, "after the process died");
            } catch (const CheckFailed &f) {
              return f.what;
            } catch (const std::exception &e) {
              return std::format("after the process died, the reopen threw: {}",
                                 e.what());
            }
            return {};
          },
          tally);
    }, /*strict=*/true);
  }
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

// Runs fn in a forked child with stderr discarded; returns its exit status,
// or 128 plus the signal that ended it (std::terminate aborts).
auto child_exit_code(const std::function<void()> &fn) -> int {
  std::fflush(nullptr);
  const auto pid = ::fork();
  REQUIRE(pid != -1);
  if (pid == 0) {
    startup_fatal_signals_in_child(false);
    if (const auto null = ::open("/dev/null", O_WRONLY); null != -1)
      ::dup2(null, STDERR_FILENO);
    fn();
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

auto exits_cleanly_in_child(const std::function<void()> &fn) -> bool {
  return child_exit_code(fn) == 0;
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

// Where the node pool is on, ASan is off: a node freed twice would be
// handed out twice without a report. Test builds stamp a freed node and
// abort on freeing or walking one, so a double free there ends the run.
TEST_CASE("alloc sweep: a key directory node freed twice ends the test build",
          "[alloc_sweep]") {
  using N = bytecask::btree_detail::Node<std::uint64_t>;
  using Traits = bytecask::btree_detail::ChainTraits<std::uint64_t>;
  if constexpr (!bytecask::btree_detail::kNodePoolEnabled) {
    SKIP("the pool is off: the sanitizer reports a double free itself");
  } else {
    CHECK(exits_cleanly_in_child([] {
      auto *n = N::allocate(bytecask::kBTreeNodeBytes, true, 1, {});
      N::destroy(n);
    }));
    // The stamp's own exit, not any other way of dying.
    constexpr auto kFreed = bytecask::btree_detail::kFreedNodeExitCode;
    CHECK(child_exit_code([] {
            auto *n = N::allocate(bytecask::kBTreeNodeBytes, true, 1, {});
            N::destroy(n);
            N::destroy(n);
          }) == kFreed);
    CHECK(child_exit_code([] {
            auto *n = N::allocate(bytecask::kBTreeNodeBytes, true, 1, {});
            N::destroy(n);
            (void)Traits::tag(n);
          }) == kFreed);
  }
}

// A write that fails partway leaves the transient's root on nodes that are
// all alive, and dropping the transient frees each of them once. Each
// allocation of a workload is failed in turn, once and from there on. A
// split, rebuild or collapse replaces a node; freeing it before the root, or
// a rebuilt node's parent, reached the replacement left the root on a freed
// node when an allocation in between failed, and the drop walked and freed
// it again (heap-use-after-free under ASan, #580).
//
// Which writes a sweep reaches is decided by its workload, and the engine's
// sweep once missed that bug because its seed never filled a leaf (#588).
// So the workload's writes are counted by the structural transition they
// make (btree_detail::Transition), and the sweep requires two things of it:
// that a clean run makes every transition in `expected`, and, where every
// node allocation is counted, that each of them was made by some write that
// an allocation then failed. Under ASan the node pool is off, so every node
// allocation is counted. Elsewhere the pool is emptied before each run, so
// the first allocations of a run reach operator new, but nodes the run frees
// are reused uncounted (#583) and the second check is skipped.
//
// The workload below starts from a published base, so its first writes copy
// foreign nodes, then: inserts that split the root leaf, inner nodes and the
// inner root and shrink inner prefixes on nodes the transient owns; erases
// that leave nodes with dead bytes, and inserts that compact them; and
// erases of everything it wrote. Those last empty leaves and inner nodes and
// collapse the root, but on a path the transient already owns nothing
// allocates after a node empties, so no failure can land there: the sweep
// over a published tree, below, requires those. `fresh()` makes the
// transient, `step(tr, i)` runs the i-th write, `check(tr)` publishes a
// completed run and validates it.
namespace {

using bytecask::btree_detail::Transition;
using TransitionCounts =
    std::array<std::uint64_t, bytecask::btree_detail::kTransitionCount>;

auto transition_counts() noexcept -> TransitionCounts {
  TransitionCounts c{};
  for (std::size_t k = 0; k < c.size(); ++k)
    c[k] = bytecask::btree_detail::transition_counts[k].load(
        std::memory_order_relaxed);
  return c;
}

// A switch, so a new Transition without a name does not compile.
auto transition_name(Transition t) -> const char * {
  switch (t) {
  case Transition::kLeafSplit: return "leaf split";
  case Transition::kInnerSplit: return "inner split";
  case Transition::kRootGrowth: return "root growth";
  case Transition::kPrefixRebuild: return "prefix rebuild";
  case Transition::kCompaction: return "compaction";
  case Transition::kLeafEmptied: return "leaf emptied";
  case Transition::kInnerEmptied: return "inner emptied";
  case Transition::kRootCollapse: return "root collapse";
  case Transition::kRunJoin: return "join of sealed runs";
  case Transition::kLevelBuild: return "inner level build";
  case Transition::kCount: break;
  }
  return "?";
}

// Set by a Writes::kOnePerStep step once its write has installed its result,
// before it publishes: a failure after that point is not in the window the
// sweep is proving, between a node's replacement and the root's move.
bool g_step_wrote = false;
void step_wrote() noexcept { g_step_wrote = true; }

// How a workload's writes reach the tree: all through one transient, which
// must refuse the next write once one has failed, or each through a transient
// of its own, derived from the version the last one published, as the
// engine's batches are, and dropped by the step itself when it fails.
enum class Writes { kOneTransient, kOnePerStep };

template <typename Fresh, typename Step, typename Check>
void sweep_partial_writes(Writes writes, int steps,
                          std::initializer_list<Transition> expected,
                          const Fresh &fresh, const Step &step,
                          const Check &check) {
  const bool one_transient = writes == Writes::kOneTransient;
  {
    auto tr = fresh();
    const auto before = transition_counts();
    for (int i = 0; i < steps; ++i)
      step(tr, i);
    const auto after = transition_counts();
    check(tr);
    for (const auto t : expected) {
      INFO("transition: " << transition_name(t));
      REQUIRE(after[static_cast<std::size_t>(t)] >
              before[static_cast<std::size_t>(t)]);
    }
  }
  std::array<bool, bytecask::btree_detail::kTransitionCount> failed_in{};
  for (const bool cascade : {false, true}) {
    for (long n = 1;; ++n) {
      REQUIRE(n <= kMaxAllocations);
      INFO("allocation " << n << (cascade ? " and every one after it" : "")
                         << " failed");
      auto tr = fresh();
      bytecask::btree_detail::node_pool_trim();
      bool threw = false;
      bool fired = false;
      TransitionCounts before{};
      {
        ScopedAllocFaults faults{n, cascade};
        try {
          for (int i = 0; i < steps; ++i) {
            before = transition_counts();
            g_step_wrote = false;
            step(tr, i);
          }
        } catch (const std::bad_alloc &) {
          threw = true;
        }
        fired = faults.report().fired;
      }
      if (!fired) {
        check(tr);
        if (one_transient)
          CHECK_THROWS_AS(step(tr, 0), std::logic_error);  // consumed
        break;
      }
      if (threw && !g_step_wrote) {
        // The transitions the failed write began: an allocation failed
        // inside each, or after it before the write installed its result.
        const auto after = transition_counts();
        for (std::size_t k = 0; k < after.size(); ++k)
          failed_in[k] = failed_in[k] || after[k] > before[k];
      }
      if (threw) {
        if (one_transient)
          CHECK_THROWS_AS(step(tr, 0), std::logic_error);
      }
    }  // tr is dropped at the end of each run, after the failure
  }
  if constexpr (!bytecask::btree_detail::kNodePoolEnabled) {
    for (const auto t : expected) {
      INFO("transition: " << transition_name(t));
      CHECK(failed_in[static_cast<std::size_t>(t)]);
    }
  }
}

struct PartialWrite {
  bool erase{false};
  std::string key;
};

// Keys of the workload above. A long ascending family fills leaves in order
// and splits the root and inner nodes into halves of that family alone, so
// their prefixes are long; keys that sort before it then reach those nodes
// with separators that share less, and shrink them. Neighbouring keys of the
// second set share 103 bytes and a node's keys far fewer, so its separators
// are long and inner nodes few to a page. Erasing every other key of a
// stretch leaves its leaves with dead bytes, and erasing a run empties
// leaves and leaves their parents with dead bytes; writing keys back into
// both compacts them. Then every key the workload wrote is erased, in key
// order.
auto partial_write_workload() -> std::vector<PartialWrite> {
  constexpr int kFamily = 1'000;
  constexpr int kMixed = 1'000;
  const std::string q(100, 'q');
  std::vector<std::string> keys;
  for (int i = 0; i < kFamily; ++i)
    keys.push_back(std::format("5{}{:04}", q, i));
  for (int i = 0; i < kMixed; ++i) {
    const auto j = i * 7 % kMixed;
    keys.push_back(std::format("{:03}{}{:03}", j % 40, q, j / 40));
  }
  std::vector<PartialWrite> w;
  std::set<std::string> live;
  auto put = [&](const std::string &k) {
    w.push_back({false, k});
    live.insert(k);
  };
  auto erase = [&](const std::string &k) {
    w.push_back({true, k});
    live.erase(k);
  };
  for (const auto &k : keys)
    put(k);
  auto sorted = keys;
  std::ranges::sort(sorted);
  // sorted[0, 1000) is the second set, sorted[1000, 2000) the family.
  for (std::size_t i = 1'001; i < 1'600; i += 2)
    erase(sorted[i]);
  for (std::size_t i = 300; i < 600; ++i)
    erase(sorted[i]);
  for (std::size_t i = 1'001; i < 1'600; i += 2)
    put(sorted[i]);
  for (std::size_t i = 300; i < 600; ++i)
    put(sorted[i] + "r");
  for (const auto &k : std::vector<std::string>{live.begin(), live.end()})
    erase(k);
  return w;
}

// What the base holds: keys that sort after the workload's, so the erases
// never empty the tree.
auto partial_write_base_key(int i) -> std::string {
  return std::format("z{:04}", i);
}
constexpr int kPartialWriteBase = 200;

auto partial_write_size(const std::vector<PartialWrite> &w) -> std::size_t {
  std::set<std::string> live;
  for (int i = 0; i < kPartialWriteBase; ++i)
    live.insert(partial_write_base_key(i));
  for (const auto &op : w) {
    if (op.erase)
      live.erase(op.key);
    else
      live.insert(op.key);
  }
  return live.size();
}

}  // namespace

TEST_CASE("alloc sweep: a transient whose write fails partway is freed once",
          "[alloc_sweep]") {
  // A node a failed write had not yet placed is leaked (#390).
  const ExpectedLeaks leaks;
  const auto work = partial_write_workload();
  KeyStore res;
  Tree base;
  {
    auto tr = base.transient();
    for (int i = 0; i < kPartialWriteBase; ++i) {
      const auto k = partial_write_base_key(i);
      tr.set(to_bytes(k), res.ref(k), res);
    }
    base = std::move(tr).persistent();
  }
  std::vector<bytecask::BlindRef> refs;
  for (const auto &op : work)
    refs.push_back(op.erase ? bytecask::BlindRef{} : res.ref(op.key));
  sweep_partial_writes(
      Writes::kOneTransient, static_cast<int>(work.size()),
      // A blind leaf is an array, never compacted, and holds no prefix: both
      // happen to inner nodes only, in the BuildSession::place() the keyed
      // sweep below reaches for compaction too.
      {Transition::kLeafSplit, Transition::kInnerSplit, Transition::kRootGrowth,
       Transition::kPrefixRebuild},
      [&] { return base.transient(); },
      [&](auto &tr, int i) {
        const auto u = static_cast<std::size_t>(i);
        if (work[u].erase)
          (void)tr.erase(to_bytes(work[u].key), res);
        else
          tr.set(to_bytes(work[u].key), refs[u], res);
      },
      [&](auto &tr) {
        const auto t = std::move(tr).persistent();
        (void)t.validate(res);
        CHECK(t.size() == partial_write_size(work));
      });
}

// The same sweep over the keyed tree (BYTECASK_KEYDIR=btree), whose
// transient installs its root through the same BuildSession::write.
TEST_CASE("alloc sweep: a keyed transient whose write fails partway is freed "
          "once",
          "[alloc_sweep]") {
  const ExpectedLeaks leaks;
  const auto work = partial_write_workload();
  using KeyedTree = bytecask::PersistentBTree<std::uint64_t>;
  KeyedTree base;
  for (int i = 0; i < kPartialWriteBase; ++i)
    base = base.set(to_bytes(partial_write_base_key(i)),
                    static_cast<std::uint64_t>(i));
  sweep_partial_writes(
      Writes::kOneTransient, static_cast<int>(work.size()),
      {Transition::kLeafSplit, Transition::kInnerSplit, Transition::kRootGrowth,
       Transition::kPrefixRebuild, Transition::kCompaction},
      [&] { return base.transient(); },
      [&](auto &tr, int i) {
        const auto u = static_cast<std::size_t>(i);
        if (work[u].erase)
          (void)tr.erase(to_bytes(work[u].key));
        else
          tr.set(to_bytes(work[u].key), static_cast<std::uint64_t>(i));
      },
      [&](auto &tr) {
        const auto t = std::move(tr).persistent();
        (void)t.validate();
        CHECK(t.size() == partial_write_size(work));
      });
}

// Erases that empty leaves and inner nodes and collapse the root, each in a
// transient of its own over the version the last one published, as the
// engine's batches are. Within one transient nothing on an erase's path
// allocates once the path is owned, so the sweeps above never fail an
// allocation in the write that empties a node; here every erase copies a
// foreign path, retires what it replaces and publishes, and each of those
// allocations is failed after the node emptied. Separators are long, so
// inner nodes are few to a page and the base has two levels above its
// leaves: for the keyed tree, neighbouring keys that share 103 bytes; for
// the blind tree, whose leaves hold far more, more keys of one family
// written in order, which split its root into two halves of their own. The
// base keeps one key in 25 of them, a few to each leaf, so the sweep's
// erases are the ones that empty nodes.
TEST_CASE("alloc sweep: an erase from a published tree that fails partway "
          "frees each node once",
          "[alloc_sweep]") {
  const ExpectedLeaks leaks;
  const std::string q(100, 'q');
  std::vector<std::string> keys;
  // The tree keeps its last key, so the last erases collapse the root onto
  // it. Keys are erased in order.
  std::vector<std::string> kept;
  auto keep = [&](const std::vector<std::string> &sorted) {
    for (std::size_t i = 0; i < sorted.size(); i += 25)
      kept.push_back(sorted[i]);
  };
  auto thinned = [&](const std::string &k) {
    return !std::ranges::binary_search(kept, k);
  };
  const std::initializer_list<Transition> emptied{Transition::kLeafEmptied,
                                                  Transition::kInnerEmptied,
                                                  Transition::kRootCollapse};
  SECTION("blind") {
    for (int i = 0; i < 1'000; ++i)
      keys.push_back(std::format("5{}{:04}", q, i));
    keep(keys);
    KeyStore res;
    Tree base;
    {
      auto tr = base.transient();
      for (const auto &k : keys)
        tr.set(to_bytes(k), res.ref(k), res);
      base = std::move(tr).persistent();
      tr = base.transient();
      for (const auto &k : keys)
        if (thinned(k))
          (void)tr.erase(to_bytes(k), res);
      base = std::move(tr).persistent();
    }
    sweep_partial_writes(
        Writes::kOnePerStep, static_cast<int>(kept.size()) - 1, emptied,
        [&] { return base; },
        [&](Tree &t, int i) {
          auto tr = t.transient();
          (void)tr.erase(to_bytes(kept[static_cast<std::size_t>(i)]), res);
          step_wrote();
          t = std::move(tr).persistent();
        },
        [&](Tree &t) {
          (void)t.validate(res);
          CHECK(t.size() == 1);
        });
  }
  SECTION("keyed") {
    for (int i = 0; i < 600; ++i) {
      const auto j = i * 7 % 600;
      keys.push_back(std::format("{:03}{}{:03}", j % 40, q, j / 40));
    }
    auto sorted = keys;
    std::ranges::sort(sorted);
    keep(sorted);
    using KeyedTree = bytecask::PersistentBTree<std::uint64_t>;
    KeyedTree base;
    {
      auto tr = base.transient();
      for (const auto &k : keys)
        tr.set(to_bytes(k), 1);
      base = std::move(tr).persistent();
      tr = base.transient();
      for (const auto &k : keys)
        if (thinned(k))
          (void)tr.erase(to_bytes(k));
      base = std::move(tr).persistent();
    }
    sweep_partial_writes(
        Writes::kOnePerStep, static_cast<int>(kept.size()) - 1, emptied,
        [&] { return base; },
        [&](KeyedTree &t, int i) {
          auto tr = t.transient();
          (void)tr.erase(to_bytes(kept[static_cast<std::size_t>(i)]));
          step_wrote();
          t = std::move(tr).persistent();
        },
        [&](KeyedTree &t) {
          (void)t.validate();
          CHECK(t.size() == 1);
        });
  }
}

// A bulk build that fails partway frees each node it made once: the leaves
// a loader sealed, the inner levels built over them, and the leaves a run
// hands to the loader that joins the runs. concat used to add a run's leaves
// to the joining loader while the run still owned them, so an allocation
// failing partway freed each leaf already moved twice, once by each
// (heap-use-after-free under ASan, #588). Parallel recovery joins its
// partitions this way. `build(stage)` makes four runs over ascending
// quarters of the keys, joins them, then builds the same keys with one
// loader's finish(), and returns both trees' sizes and the lower of their
// heights; it runs each of those three through `stage`. As in the tree
// sweeps, the sweep requires the clean run to join runs and build inner
// levels (Transition::kRunJoin, kLevelBuild), and, under ASan, each to be
// begun by a stage that a failed allocation then ended. Neighbouring keys
// share 103 bytes, a node's keys far fewer, so separators are long, inner
// nodes few to a page, and both trees three levels deep from 1,200 keys.
namespace {

struct BulkBuilt {
  std::size_t size{0};
  std::size_t height{0};
};

template <typename Build>
void sweep_bulk_build(std::size_t keys, const Build &build) {
  constexpr std::array kExpected{Transition::kRunJoin, Transition::kLevelBuild};
  TransitionCounts stage_before{};
  auto stage = [&stage_before](auto &&fn) {
    stage_before = transition_counts();
    return fn();
  };
  {
    const auto before = transition_counts();
    (void)build(stage);
    const auto after = transition_counts();
    for (const auto t : kExpected) {
      INFO("transition: " << transition_name(t));
      REQUIRE(after[static_cast<std::size_t>(t)] >
              before[static_cast<std::size_t>(t)]);
    }
  }
  std::array<bool, bytecask::btree_detail::kTransitionCount> failed_in{};
  for (const bool cascade : {false, true}) {
    for (long n = 1;; ++n) {
      REQUIRE(n <= kMaxAllocations);
      INFO("allocation " << n << (cascade ? " and every one after it" : "")
                         << " failed");
      bytecask::btree_detail::node_pool_trim();
      BulkBuilt built;
      bool fired = false;
      {
        ScopedAllocFaults faults{n, cascade};
        try {
          built = build(stage);
        } catch (const std::bad_alloc &) {
          const auto after = transition_counts();
          for (std::size_t k = 0; k < after.size(); ++k)
            failed_in[k] = failed_in[k] || after[k] > stage_before[k];
        }
        fired = faults.report().fired;
      }
      if (!fired) {
        CHECK(built.size == 2 * keys);
        CHECK(built.height >= 3);
        break;
      }
    }
  }
  if constexpr (!bytecask::btree_detail::kNodePoolEnabled) {
    for (const auto t : kExpected) {
      INFO("transition: " << transition_name(t));
      CHECK(failed_in[static_cast<std::size_t>(t)]);
    }
  }
}

// n keys in ascending order, in families of `family` that share 103 bytes.
auto bulk_keys(std::size_t n, std::size_t family) -> std::vector<std::string> {
  std::vector<std::string> keys;
  for (std::size_t i = 0; i < n; ++i)
    keys.push_back(std::format("{:03}{}{:03}", i / family,
                               std::string(100, 'q'), i % family));
  return keys;
}

}  // namespace

TEST_CASE("alloc sweep: a bulk build that fails partway frees each node once",
          "[alloc_sweep]") {
  SECTION("blind") {
    const auto keys = bulk_keys(1'200, 25);
    const auto quarter = keys.size() / 4;
    using Loader = bytecask::BlindBulkLoader<bytecask::kBlindLeafBytes>;
    KeyStore res;
    std::vector<bytecask::BlindRef> refs;
    for (const auto &k : keys)
      refs.push_back(res.ref(k));
    sweep_bulk_build(keys.size(), [&](auto &stage) {
      auto runs = stage([&] {
        std::vector<bytecask::btree_detail::LeafRun<bytecask::BlindRef>> rs;
        for (std::size_t r = 0; r < 4; ++r) {
          Loader l;
          for (std::size_t i = r * quarter; i < (r + 1) * quarter; ++i)
            l.append(to_bytes(keys[i]), refs[i]);
          rs.push_back(std::move(l).seal());
        }
        return rs;
      });
      const auto joined =
          stage([&] { return Loader::concat(std::move(runs)); });
      const auto t = stage([&] {
        Loader whole;
        for (std::size_t i = 0; i < keys.size(); ++i)
          whole.append(to_bytes(keys[i]), refs[i]);
        return std::move(whole).finish();
      });
      return BulkBuilt{joined.size() + t.size(),
                       std::min(joined.validate(res), t.validate(res))};
    });
  }
  SECTION("keyed") {
    // A keyed leaf shares its keys' prefix, so it ends where a family does;
    // a family of two leaves leaves a long separator inside it.
    const auto keys = bulk_keys(1'200, 60);
    const auto quarter = keys.size() / 4;
    using Loader = bytecask::btree_detail::BulkLoader<std::uint64_t>;
    sweep_bulk_build(keys.size(), [&](auto &stage) {
      auto runs = stage([&] {
        std::vector<bytecask::btree_detail::LeafRun<std::uint64_t>> rs;
        for (std::size_t r = 0; r < 4; ++r) {
          Loader l;
          for (std::size_t i = r * quarter; i < (r + 1) * quarter; ++i)
            l.append(to_bytes(keys[i]), i);
          rs.push_back(std::move(l).seal());
        }
        return rs;
      });
      const auto joined =
          stage([&] { return Loader::concat(std::move(runs)); });
      const auto t = stage([&] {
        Loader whole;
        for (std::size_t i = 0; i < keys.size(); ++i)
          whole.append(to_bytes(keys[i]), i);
        return std::move(whole).finish();
      });
      return BulkBuilt{joined.size() + t.size(),
                       std::min(joined.validate(), t.validate())};
    });
  }
}

// A version whose publication fails leaves its base as it was, so the base
// can still be derived from: a half-published version would make every later
// write fail with "a version that already has a successor".
//
// The base u1 is itself derived from a live t, and the version published from
// it changes a leaf far from the one u1 changed. Its retired nodes then park
// on both: the root u1 copied on u1, the path t built on t, which already
// holds u1's parcel. Parking that allocates, so every allocation of the
// publication is failed somewhere, the ones after the link included.
TEST_CASE("alloc sweep: a failed publication leaves the base derivable",
          "[alloc_sweep]") {
  KeyStore res;
  const auto t = deep_tree(res);
  auto first = t.transient();
  first.set(to_bytes("aaa"), res.ref("aaa"), res);
  const auto u1 = std::move(first).persistent();
  for (long n = 1;; ++n) {
    REQUIRE(n <= kMaxAllocations);
    auto tr = u1.transient();
    tr.set(to_bytes("zzz"), res.ref("zzz"), res);
    bool fired = false;
    {
      ScopedAllocFaults faults{n, false};
      try {
        // Released under the fault too: the version dies with its parcel
        // parked on u1 and t.
        (void)std::move(tr).persistent();
      } catch (const std::bad_alloc &) {
      }
      fired = faults.report().fired;
    }
    INFO("allocation " << n << " of the publication failed");
    auto again = u1.transient();
    again.set(to_bytes("other"), res.ref("other"), res);
    CHECK_NOTHROW((void)std::move(again).persistent());
    if (!fired) break;
  }
}

// Dropping the last handle of a version frees what only it reached and hands
// on what it was holding for older ones, in noexcept destructors, often
// because memory just ran out. Each way a version can die is run with every
// allocation failing: the release must not end the process, and the versions
// left must still be whole once memory is back (#390). In-process, so
// coverage sees the fallbacks; a regression ends the test binary.
//
// t is a live base; u1 changes one leaf of it and u2 a leaf far away, so
// u2's retired nodes park on both u1 and t.
TEST_CASE("alloc sweep: a version is released when nothing can be allocated",
          "[alloc_sweep]") {
  // A parcel hold() cannot add to a version for want of memory is leaked by
  // design; the trees are built inside this scope so LeakSanitizer accepts it.
  const ExpectedLeaks leaks;
  KeyStore res;
  auto derive = [&res](const Tree &base, const std::string &k) {
    auto tr = base.transient();
    tr.set(to_bytes(k), res.ref(k), res);
    return std::move(tr).persistent();
  };
  std::optional<Tree> t{deep_tree(res)};
  std::optional<Tree> u1{derive(*t, "aaa")};
  std::optional<Tree> u2{derive(*u1, "zzz")};
  const auto keys = t->size();

  auto released_under_fault = [](std::optional<Tree> &v) {
    ScopedAllocFaults every_allocation_fails{1, true};
    v.reset();
  };
  // validate() reads every node and throws on the first broken invariant,
  // so a node freed while still reached shows here (or under ASan).
  auto still_whole = [&] {
    if (t) {
      (void)t->validate(res);
      REQUIRE(t->size() == keys);
    }
    if (u1) {
      (void)u1->validate(res);
      REQUIRE(u1->contains(to_bytes("aaa"), res));
    }
    if (u2) {
      (void)u2->validate(res);
      REQUIRE(u2->contains(to_bytes("zzz"), res));
    }
  };

  SECTION("the head, over a live predecessor") {
    // Retraction: what u2 created is walked and freed, what it retired is
    // live again in u1 and t.
    released_under_fault(u2);
    still_whole();
  }
  SECTION("a version inside the chain") {
    // u1 dies with a successor: the nodes u2 retired from it are freed, and
    // what u1 held for t moves on.
    released_under_fault(u1);
    still_whole();
  }
  SECTION("the base, then the rest of the lineage") {
    released_under_fault(t);
    still_whole();
    released_under_fault(u1);
    still_whole();
    // The last version of the lineage: everything it retired is freed.
    released_under_fault(u2);
  }
}

// A parcel that could not be split when it was parked is held whole by its
// smallest blocker, and split when that version dies — then some of its
// nodes may be reached by nothing and others still be (#390).
//
// t, v, w, x in one lineage. v changes the leftmost leaf, w the rightmost, x
// the rightmost and a middle one, so x retires nodes created by w (the path
// w copied) and by t (the middle path), none by v. Each allocation of x's
// publication is failed in turn; one of them is the split, and the parcel is
// held whole on t. w dies holding nothing of it. When t dies the parcel is
// looked at again: t's nodes are still reached by v, w's by nothing.
TEST_CASE("alloc sweep: a parcel held whole is split when its holder dies",
          "[alloc_sweep]") {
  const ExpectedLeaks leaks;
  for (long n = 1;; ++n) {
    REQUIRE(n <= kMaxAllocations);
    KeyStore res;
    auto derive = [&res](const Tree &base,
                         std::initializer_list<std::string> ks) {
      auto tr = base.transient();
      for (const auto &k : ks)
        tr.set(to_bytes(k), res.ref(k), res);
      return std::move(tr).persistent();
    };
    std::optional<Tree> t{deep_tree(res)};
    const auto keys = t->size();
    std::optional<Tree> v{derive(*t, {"aaa"})};
    std::optional<Tree> w{derive(*v, {"zzz"})};
    std::optional<Tree> x;
    auto tr = w->transient();
    for (const auto &k : {std::string{"zzz2"}, std::string{"user::5::y"}})
      tr.set(to_bytes(k), res.ref(k), res);
    bool fired = false;
    {
      ScopedAllocFaults faults{n, false};
      try {
        x.emplace(std::move(tr).persistent());
      } catch (const std::bad_alloc &) {
      }
      fired = faults.report().fired;
    }
    INFO("allocation " << n << " of the publication failed");
    w.reset();
    t.reset();
    (void)v->validate(res);
    REQUIRE(v->size() == keys + 1);
    if (x) {
      (void)x->validate(res);
      REQUIRE(x->contains(to_bytes("user::5::y"), res));
    }
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
  CHECK(db->degraded_reason().empty());
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

// create_manifest seals the active file on disk before it installs the next
// one in the transient: a failure there must refuse writes, not leave them
// going to the sealed file.
TEST_CASE("refuse writes: create_manifest's rotation that fails in memory "
          "refuses writes",
          "[refuse_writes]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = open_db(dir, {});
  seed(*db);
  db->test_in_finish_rotation_ = [] { throw std::bad_alloc{}; };
  CHECK_THROWS_AS((void)db->create_manifest(), std::bad_alloc);
  db->test_in_finish_rotation_ = nullptr;
  expect_refused_then_resumed(db, dir, {}, "k1", "v1");
}

// ingest checks the flag on its own entry path. A slice that crosses a
// rotation and fails while installing it, with no memory to publish the
// degraded state, leaves the flag as the only refusal.
TEST_CASE("refuse writes: ingest is refused by the flag alone",
          "[refuse_writes]") {
  TempDir td;
  const Options opts{.max_file_bytes = 512, .initial_mode = bytecask::Mode::Follower};
  bytecask::testing::OwnedEntries slice;
  {
    auto leader = DB::open(td.path / "leader", {.max_file_bytes = 512});
    for (int i = 0; i < 3; ++i)
      leader.put({.sync = true}, to_bytes(std::format("r{}", i)),
                 to_bytes(std::string(300, static_cast<char>('a' + i))));
    const auto snap = leader.snapshot();
    slice = bytecask::testing::collect_changes(leader.changes_since(snap, 0));
  }
  const auto dir = td.path / "follower";
  auto db = open_db(dir, opts);
  db->test_in_finish_rotation_ = [] { throw std::bad_alloc{}; };
  db->test_before_refusal_publish_ = [] { throw std::bad_alloc{}; };
  CHECK_THROWS_AS(db->ingest(slice.header, slice.views()), std::bad_alloc);
  db->test_in_finish_rotation_ = nullptr;
  db->test_before_refusal_publish_ = nullptr;
  CHECK(db->is_degraded());
  CHECK_FALSE(db->engine_state()->degraded);
  CHECK_THROWS_AS(db->ingest(slice.header, slice.views()), DbDegraded);
  REQUIRE_NOTHROW(db->resume());
  CHECK_FALSE(db->is_degraded());
  db->ingest(slice.header, slice.views());  // what resume() replayed is skipped
  for (int i = 0; i < 3; ++i)
    CHECK(value_of(*db, std::format("r{}", i)) ==
          std::string(300, static_cast<char>('a' + i)));
  bytecask::testing::assert_consistent(*db);
}
