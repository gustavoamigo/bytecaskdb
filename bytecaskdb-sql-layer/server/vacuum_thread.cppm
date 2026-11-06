// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// vacuum_thread.cppm — the server's background vacuum (docs/session_design.md,
// *Background vacuum*). The engine and Database run vacuum only when called;
// when to call it is the host's policy, as in the MariaDB plugin's vacuum_loop.

module;
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

export module bytecask.sql.vacuum_thread;

import bytecask.sql.api;

export namespace bytecask::sql {

struct VacuumPacing {
  std::chrono::milliseconds busy{500};     // after a pass that reclaimed a file
  std::chrono::milliseconds idle{30'000};  // after one that found nothing, or failed
};

// What a failed pass is reported to. It takes `what` as given, so that
// reporting a failed allocation allocates nothing. `failures` counts the run of
// failures this one belongs to; `stops` says the thread ends with it.
using VacuumLog = void (*)(const char* what, std::uint64_t failures, bool stops);

// To stderr.
void log_vacuum_failure(const char* what, std::uint64_t failures, bool stops);

// A lasting failure is reported again every this many passes: every five
// minutes at the default idle pause.
inline constexpr std::uint64_t kVacuumLogEveryFailures = 10;

// Calls `pass` repeatedly on a thread of its own, from construction until
// destruction, which waits for a pass in flight and interrupts a pause.
// `pass` returns whether another pass may find more, as DB::vacuum does.
//
// A failure is reported and retried. One that left `degraded` true ends the
// thread, since no later pass can succeed, as DbDegraded does; DbClosed ends it
// silently. A failed fdatasync degrades the engine and throws the I/O error, so
// `degraded` is what tells it from a failure worth retrying.
class VacuumThread {
public:
  VacuumThread(std::function<bool()> pass, std::function<bool()> degraded, VacuumPacing pacing,
               VacuumLog log = log_vacuum_failure);
  ~VacuumThread();
  VacuumThread(const VacuumThread&) = delete;
  VacuumThread& operator=(const VacuumThread&) = delete;

private:
  void run();
  // Reports the failure; whether the thread ends with it.
  auto failed(const char* what) -> bool;

  std::function<bool()> pass_;
  std::function<bool()> degraded_;
  VacuumPacing pacing_;
  VacuumLog log_;
  std::uint64_t failures_{0};  // the thread's own
  std::mutex mu_;
  std::condition_variable wake_;
  bool stop_{false};
  std::thread thread_;  // last: it starts once the rest is built
};

}  // namespace bytecask::sql

namespace bytecask::sql {

void log_vacuum_failure(const char* what, std::uint64_t failures, bool stops) {
  if (stops) {
    std::fprintf(stderr, "sql_layer_server: background vacuum stops, the engine is degraded: %s\n", what);
  } else {
    std::fprintf(stderr, "sql_layer_server: background vacuum failed (%llu in a row): %s\n",
                 static_cast<unsigned long long>(failures), what);
  }
}

VacuumThread::VacuumThread(std::function<bool()> pass, std::function<bool()> degraded, VacuumPacing pacing,
                           VacuumLog log)
    : pass_{std::move(pass)}, degraded_{std::move(degraded)}, pacing_{pacing}, log_{log}, thread_{[this] { run(); }} {}

VacuumThread::~VacuumThread() {
  {
    std::lock_guard<std::mutex> lock{mu_};
    stop_ = true;
  }
  wake_.notify_one();
  thread_.join();
}

auto VacuumThread::failed(const char* what) -> bool {
  ++failures_;
  const auto stops = degraded_();
  if (stops || failures_ % kVacuumLogEveryFailures == 1) log_(what, failures_, stops);
  return stops;
}

void VacuumThread::run() {
  std::unique_lock<std::mutex> lock{mu_};
  while (!stop_) {
    lock.unlock();
    auto more_work = false;
    try {
      more_work = pass_();
      failures_ = 0;
    } catch (const DbClosed&) {
      return;
    } catch (const DbDegraded& e) {
      log_(e.what(), failures_ + 1, true);
      return;
    } catch (const std::exception& e) {
      if (failed(e.what())) return;
    } catch (...) {
      if (failed("an exception of unknown type")) return;
    }
    lock.lock();
    wake_.wait_for(lock, more_work ? pacing_.busy : pacing_.idle, [this] { return stop_; });
  }
}

}  // namespace bytecask::sql
