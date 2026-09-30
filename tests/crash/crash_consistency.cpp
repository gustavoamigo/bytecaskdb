// SPDX-License-Identifier: MIT
// Crash-consistency harness: SIGKILL a writer process at a random point and
// verify the durable prefix on reopen. See docs/correctness_validation.md,
// "Process-crash harness".
//
// One binary, two roles. The parent re-executes itself as a child that opens
// the database, runs a random single-writer workload (plus a vacuum thread),
// and streams every operation to the parent over a pipe: an Intent frame
// before the call, a Commit or Abort frame after it returns. The parent
// SIGKILLs the child after a random delay, then reopens copies of the
// directory and checks the recovered contents against its model:
//
//   - The recovered key/value set equals the model after applying some prefix
//     of the child's operations, in commit order. Batches are one operation,
//     so this also checks batch atomicity. No value from nowhere, no rollback.
//   - That prefix covers every operation at or below the durable watermark:
//     the highest of every durable_sequence() the child reported and every
//     CommitResult with durable == true.
//   - DB::open with default options (fail_recovery_on_crc_errors = true)
//     succeeds, and serial and parallel recovery agree on contents,
//     file_stats and the key count.
//
// The next child reopens the killed directory itself, so every iteration
// after the first also starts from a crash. The directory is wiped every
// --reset-every iterations to bound its size.
//
// The kernel page cache survives SIGKILL, so this checks the process-crash
// contract, not power loss.
//
// Usage:
//   crash_consistency [--iterations N] [--seed S] [--max-delay-ms MS]
//                     [--reset-every N] [--dir PATH] [--keep] [--verbose]
//                     [--no-vacuum]

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crash_model.h"

extern char **environ; // NOLINT(readability-redundant-declaration)

namespace {

using namespace crash_model;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Per-iteration configuration, derived from the iteration seed so parent and
// child agree without passing more than the seed.
// ---------------------------------------------------------------------------

struct Config {
  bytecask::IoBackend backend{bytecask::IoBackend::Pread};
  std::uint64_t max_file_bytes{0};
  int sync_percent{0};
  bool vacuum{false};
  unsigned recovery_threads{1};
};

auto config_for(std::uint64_t seed) -> Config {
  std::mt19937_64 rng{seed ^ 0x9e3779b97f4a7c15ULL};
  Config c;
  constexpr std::array backends{bytecask::IoBackend::Pread,
                                bytecask::IoBackend::Mmap,
                                bytecask::IoBackend::BufferPool};
  c.backend = backends[rng() % backends.size()];
  // Small files rotate often, so rotation, sealing and the background hint
  // worker are in flight when the kill lands.
  constexpr std::array sizes{std::uint64_t{16} << 10, std::uint64_t{64} << 10,
                             std::uint64_t{256} << 10, std::uint64_t{1} << 20};
  c.max_file_bytes = sizes[rng() % sizes.size()];
  constexpr std::array sync_percents{0, 5, 50, 100};
  c.sync_percent = sync_percents[rng() % sync_percents.size()];
  c.vacuum = rng() % 4 != 0;
  c.recovery_threads = 1 + static_cast<unsigned>(rng() % 4);
  return c;
}

auto backend_name(bytecask::IoBackend b) -> std::string_view {
  switch (b) {
  case bytecask::IoBackend::Pread:
    return "pread";
  case bytecask::IoBackend::Mmap:
    return "mmap";
  case bytecask::IoBackend::BufferPool:
    return "buffer_pool";
  }
  return "?";
}

auto db_options(const Config &c) -> bytecask::Options {
  bytecask::Options o;
  o.max_file_bytes = c.max_file_bytes;
  o.recovery_threads = c.recovery_threads;
  o.io_backend = c.backend;
  if (c.backend == bytecask::IoBackend::BufferPool)
    o.buffer_pool.capacity_bytes = 4 * c.max_file_bytes + (1 << 20);
  return o;
}

// ---------------------------------------------------------------------------
// Child
// ---------------------------------------------------------------------------
auto run_child(const fs::path &dir, std::uint64_t seed, bool no_vacuum, int fd)
    -> int {
  auto cfg = config_for(seed);
  if (no_vacuum) cfg.vacuum = false;
  std::mt19937_64 rng{seed};
  auto db = bytecask::DB::open(dir, db_options(cfg));
  {
    Writer w;
    w.u8(static_cast<std::uint8_t>(FrameType::Opened));
    w.u64(db.durable_sequence());
    w.u64(static_cast<std::uint64_t>(db.stats().at("bytecask.keydir_keys")));
    send_frame(fd, w);
  }

  std::atomic<bool> stop{false};
  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&db, &stop, seed] {
      std::mt19937_64 vrng{seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        (void)db.vacuum({.fragmentation_threshold = threshold});
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }

  std::uint64_t value_id = 0;
  // Bounded so a slow kill cannot fill the disk; the child then idles until
  // the parent kills it.
  constexpr int kMaxOps = 200'000;
  for (int i = 0; i < kMaxOps; ++i) {
    const auto op = random_op(rng, cfg.sync_percent, seed, value_id);
    {
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::Intent));
      encode_op(w, op);
      send_frame(fd, w);
    }
    const auto result = execute(db, op);
    {
      Writer w;
      if (result) {
        w.u8(static_cast<std::uint8_t>(FrameType::Commit));
        w.u64(result->sequence);
        w.u8(result->durable ? 1 : 0);
      } else {
        w.u8(static_cast<std::uint8_t>(FrameType::Abort));
      }
      send_frame(fd, w);
    }
    if (rng() % 16 == 0) {
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::Watermark));
      w.u64(db.durable_sequence());
      send_frame(fd, w);
    }
  }
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ---------------------------------------------------------------------------
// Parent
// ---------------------------------------------------------------------------

struct History {
  bool opened{false};
  std::uint64_t opened_durable{0};
  std::uint64_t opened_keys{0};
  std::vector<OpRecord> ops;
  std::uint64_t watermark{0};
};

auto parse_history(std::string_view stream) -> History {
  History h;
  for (const auto payload : split_frames(stream)) {
    Reader r{payload};
    switch (static_cast<FrameType>(r.u8())) {
    case FrameType::Opened:
      h.opened = true;
      h.opened_durable = r.u64();
      h.opened_keys = r.u64();
      h.watermark = h.opened_durable;
      break;
    case FrameType::Intent:
      h.ops.push_back({.op = decode_op(r)});
      break;
    case FrameType::Commit:
      h.ops.back().outcome = OpRecord::Outcome::Committed;
      h.ops.back().sequence = r.u64();
      h.ops.back().durable = r.u8() != 0;
      if (h.ops.back().durable)
        h.watermark = std::max(h.watermark, h.ops.back().sequence);
      break;
    case FrameType::Abort:
      h.ops.back().outcome = OpRecord::Outcome::Aborted;
      break;
    case FrameType::Watermark:
      h.watermark = std::max(h.watermark, r.u64());
      break;
    case FrameType::Throw:
    case FrameType::Rejected:
    case FrameType::View:
    case FrameType::OpenFailed:
    case FrameType::Violation:
    case FrameType::Closed:
      // Sent only by the chaos worker.
      throw Failure{"unexpected frame from the crash child"};
    }
  }
  return h;
}


struct RunOptions {
  int iterations{200};
  std::uint64_t seed{0};
  int max_delay_ms{1500};
  int reset_every{25};
  fs::path dir;
  bool keep{false};
  bool verbose{false};
  bool no_vacuum{false};
};

struct Totals {
  std::size_t committed{0};
  std::size_t durable_needed{0};
  std::size_t lost_after_return{0};
  int killed_before_open{0};
};

auto spawn_child(const fs::path &self, const fs::path &dir, std::uint64_t seed,
                 bool no_vacuum, int write_fd) -> pid_t {
  const auto fd_arg = std::to_string(write_fd);
  const auto seed_arg = std::to_string(seed);
  const auto dir_arg = dir.string();
  const auto self_arg = self.string();
  std::vector<char *> argv{const_cast<char *>(self_arg.c_str()),
                           const_cast<char *>("--child"),
                           const_cast<char *>(dir_arg.c_str()),
                           const_cast<char *>(seed_arg.c_str()),
                           const_cast<char *>(no_vacuum ? "1" : "0"),
                           const_cast<char *>(fd_arg.c_str()), nullptr};
  pid_t pid = 0;
  if (const auto rc = posix_spawn(&pid, self_arg.c_str(), nullptr, nullptr,
                                  argv.data(), environ);
      rc != 0) {
    throw std::system_error{rc, std::generic_category(), "posix_spawn"};
  }
  return pid;
}

// Runs one child until `delay`, SIGKILLs it, and returns everything it
// streamed. Throws Failure if the child exited on its own.
auto run_and_kill(const fs::path &self, const fs::path &dir, std::uint64_t seed,
                  bool no_vacuum, std::chrono::microseconds delay)
    -> std::string {
  int fds[2];
  if (::pipe(fds) != 0)
    throw std::system_error{errno, std::generic_category(), "pipe"};
  const auto pid = spawn_child(self, dir, seed, no_vacuum, fds[1]);
  ::close(fds[1]);

  std::string stream;
  char buf[1 << 16];
  const auto deadline = Clock::now() + delay;
  bool killed = false;
  bool eof = false;
  while (!eof) {
    int timeout_ms = -1;
    if (!killed) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
      if (left.count() <= 0) {
        ::kill(pid, SIGKILL);
        killed = true;
      } else {
        timeout_ms = static_cast<int>(left.count());
      }
    }
    pollfd pfd{.fd = fds[0], .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&pfd, 1, killed ? -1 : std::max(timeout_ms, 1));
    if (ready < 0 && errno != EINTR)
      throw std::system_error{errno, std::generic_category(), "poll"};
    if (ready <= 0) continue;
    const auto n = ::read(fds[0], buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error{errno, std::generic_category(), "read"};
    }
    if (n == 0) {
      eof = true;
    } else {
      stream.append(buf, static_cast<std::size_t>(n));
    }
  }
  ::close(fds[0]);
  // A child that closed the pipe before the deadline exited on its own.
  if (!killed) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!killed || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    throw Failure{std::format(
        "child exited before the kill (status {:#x}); see its stderr above",
        status)};
  }
  return stream;
}

auto verify(const fs::path &crashed, const fs::path &work, const Config &cfg,
            const State &base, std::uint64_t base_durable, const History &h,
            Totals &totals) -> Recovered {
  if (h.opened) {
    // The child reopened the directory the previous iteration verified.
    if (h.opened_durable != base_durable ||
        h.opened_keys != base.size()) {
      throw Failure{std::format(
          "child's open disagrees with the verified reopen: durable_sequence "
          "{} vs {}, keys {} vs {}",
          h.opened_durable, base_durable, h.opened_keys, base.size())};
    }
  } else {
    ++totals.killed_before_open;
  }

  // A directory left by a process crash must open with default options.
  const auto serial = recover_both(crashed, work, db_options(cfg));

  if (!h.opened) {
    // Killed during open: nothing was written, so recovery must reproduce the
    // previous verified state exactly.
    if (serial.contents != base)
      throw Failure{"killed during open, and the directory lost state"};
    return serial;
  }

  const auto prefix =
      match_prefix(base, h.ops, h.opened_durable, h.watermark, serial.contents);
  if (serial.durable_sequence < h.watermark)
    throw Failure{std::format("recovered durable_sequence {} below watermark {}",
                              serial.durable_sequence, h.watermark)};
  totals.committed += prefix.committed;
  std::size_t needed = 0;
  for (const auto &rec : h.ops)
    if (rec.outcome == OpRecord::Outcome::Committed &&
        rec.sequence <= h.watermark)
      ++needed;
  totals.durable_needed += needed;
  if (prefix.applied < prefix.committed)
    totals.lost_after_return += prefix.committed - prefix.applied;
  return serial;
}

auto parse_args(int argc, char **argv) -> RunOptions {
  RunOptions o;
  o.seed = std::random_device{}();
  o.seed = (o.seed << 32) | std::random_device{}();
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      if (i + 1 >= argc) throw std::invalid_argument{std::format("{} needs a value", a)};
      return argv[++i];
    };
    if (a == "--iterations") {
      o.iterations = std::stoi(std::string{next()});
    } else if (a == "--seed") {
      o.seed = std::stoull(std::string{next()});
    } else if (a == "--max-delay-ms") {
      o.max_delay_ms = std::stoi(std::string{next()});
    } else if (a == "--reset-every") {
      o.reset_every = std::max(1, std::stoi(std::string{next()}));
    } else if (a == "--dir") {
      o.dir = next();
    } else if (a == "--no-vacuum") {
      o.no_vacuum = true;
    } else if (a == "--verbose") {
      o.verbose = true;
    } else if (a == "--keep") {
      o.keep = true;
    } else {
      throw std::invalid_argument{std::format("unknown argument {}", a)};
    }
  }
  if (o.dir.empty())
    o.dir = fs::temp_directory_path() /
            std::format("bytecask_crash_{}", ::getpid());
  return o;
}

auto run_parent(const RunOptions &o) -> int {
  const auto self = fs::read_symlink("/proc/self/exe");
  const auto db_dir = o.dir / "db";
  const auto work = o.dir / "work";
  fs::remove_all(o.dir);
  fs::create_directories(work);

  std::printf("crash_consistency: seed=%llu iterations=%d dir=%s\n",
              static_cast<unsigned long long>(o.seed), o.iterations,
              o.dir.c_str());
  std::printf("  rerun: crash_consistency --seed %llu --iterations %d "
              "--max-delay-ms %d --reset-every %d\n",
              static_cast<unsigned long long>(o.seed), o.iterations,
              o.max_delay_ms, o.reset_every);
  std::fflush(stdout);

  std::mt19937_64 rng{o.seed};
  State base;
  std::uint64_t base_durable = 0;
  Totals totals;
  const auto started = Clock::now();

  for (int iter = 0; iter < o.iterations; ++iter) {
    if (iter % o.reset_every == 0) {
      fs::remove_all(db_dir);
      base.clear();
      base_durable = 0;
    }
    const auto child_seed = rng();
    auto cfg = config_for(child_seed);
    if (o.no_vacuum) cfg.vacuum = false;
    // Log-uniform from 1 ms to max_delay_ms: many kills land early (during
    // open, recovery and the first rotations), some after long runs.
    std::uniform_real_distribution<double> u{0.0, 1.0};
    const auto lo = std::log(1000.0);
    const auto hi = std::log(static_cast<double>(o.max_delay_ms) * 1000.0);
    const auto delay = std::chrono::microseconds{
        static_cast<std::int64_t>(std::exp(lo + (hi - lo) * u(rng)))};

    auto iteration_desc = [&] {
      return std::format("iteration {} child_seed={} backend={} "
                         "max_file_bytes={} sync%={} vacuum={} delay={}us",
                         iter, child_seed, backend_name(cfg.backend),
                         cfg.max_file_bytes, cfg.sync_percent, cfg.vacuum,
                         delay.count());
    };

    // The directory as the child will find it, kept for a failure report.
    const auto before = work / "before";
    copy_dir(db_dir, before);
    History h;
    try {
      const auto stream =
          run_and_kill(self, db_dir, child_seed, o.no_vacuum, delay);
      h = parse_history(stream);
      const auto crashed = work / "crashed";
      copy_dir(db_dir, crashed);
      const auto recovered =
          verify(crashed, work, cfg, base, base_durable, h, totals);
      base = recovered.contents;
      base_durable = recovered.durable_sequence;
    } catch (const Failure &f) {
      // The parent never opens db_dir itself, so it is still exactly what the
      // kill left behind.
      const auto keep = o.dir / "failure";
      fs::remove_all(keep);
      fs::create_directories(keep);
      copy_dir(before, keep / "before_open");
      copy_dir(db_dir, keep / "after_kill");
      {
        std::ofstream out{keep / "history.txt"};
        out << iteration_desc() << "\nwatermark " << h.watermark << "\n";
        for (const auto &rec : h.ops) out << describe(rec, nullptr) << "\n";
      }
      std::fprintf(stderr,
                   "FAIL %s\n  %s\n  history: %zu ops, watermark %llu\n"
                   "  directories and history kept in %s\n",
                   iteration_desc().c_str(), f.what.c_str(), h.ops.size(),
                   static_cast<unsigned long long>(h.watermark), keep.c_str());
      return 1;
    }
    if (o.verbose || (iter + 1) % 25 == 0 || iter + 1 == o.iterations) {
      std::printf("  [%d/%d] %s ops=%zu\n", iter + 1, o.iterations,
                  iteration_desc().c_str(), h.ops.size());
      std::fflush(stdout);
    }
  }

  const auto secs = std::chrono::duration<double>(Clock::now() - started).count();
  std::printf("PASS %d iterations in %.1fs: %zu committed ops checked, %zu at "
              "or below the durable watermark, %d kills during open, %zu "
              "returned non-durable writes lost\n",
              o.iterations, secs, totals.committed, totals.durable_needed,
              totals.killed_before_open, totals.lost_after_return);
  if (!o.keep) fs::remove_all(o.dir);
  return 0;
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    if (argc == 6 && std::string_view{argv[1]} == "--child") {
      return run_child(argv[2], std::stoull(argv[3]),
                       std::string_view{argv[4]} == "1", std::stoi(argv[5]));
    }
    return run_parent(parse_args(argc, argv));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "crash_consistency: %s\n", e.what());
    return 2;
  }
}
