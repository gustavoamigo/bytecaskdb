// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// vacuum_thread.cppm — the server's background vacuum (docs/session_design.md,
// *Background vacuum*). The engine and Database run vacuum only when called;
// when to call it is the host's policy, as in the MariaDB plugin's vacuum_loop.

module;
#include <chrono>
#include <condition_variable>
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

// Calls `pass` repeatedly on a thread of its own, from construction until
// destruction, which waits for a pass in flight and interrupts a pause.
// `pass` returns whether another pass may find more, as DB::vacuum does.
//
// A failure is logged once per run of failures and retried: vacuum leaves the
// engine as it was. DbDegraded ends the thread, since no later pass can
// succeed, and so does DbClosed.
class VacuumThread {
public:
  VacuumThread(std::function<bool()> pass, VacuumPacing pacing);
  ~VacuumThread();
  VacuumThread(const VacuumThread&) = delete;
  VacuumThread& operator=(const VacuumThread&) = delete;

private:
  void run();

  std::function<bool()> pass_;
  VacuumPacing pacing_;
  std::mutex mu_;
  std::condition_variable wake_;
  bool stop_{false};
  std::thread thread_;  // last: it starts once the rest is built
};

}  // namespace bytecask::sql

namespace bytecask::sql {

VacuumThread::VacuumThread(std::function<bool()> pass, VacuumPacing pacing)
    : pass_{std::move(pass)}, pacing_{pacing}, thread_{[this] { run(); }} {}

VacuumThread::~VacuumThread() {
  {
    std::lock_guard<std::mutex> lock{mu_};
    stop_ = true;
  }
  wake_.notify_one();
  thread_.join();
}

void VacuumThread::run() {
  auto failing = false;
  std::unique_lock<std::mutex> lock{mu_};
  while (!stop_) {
    lock.unlock();
    auto more_work = false;
    try {
      more_work = pass_();
      failing = false;
    } catch (const DbClosed&) {
      return;
    } catch (const DbDegraded& e) {
      std::fprintf(stderr, "sql_layer_server: background vacuum stops, the engine is degraded: %s\n", e.what());
      return;
    } catch (const std::exception& e) {
      if (!failing) std::fprintf(stderr, "sql_layer_server: background vacuum failed: %s\n", e.what());
      failing = true;
    }
    lock.lock();
    wake_.wait_for(lock, more_work ? pacing_.busy : pacing_.idle, [this] { return stop_; });
  }
}

}  // namespace bytecask::sql
