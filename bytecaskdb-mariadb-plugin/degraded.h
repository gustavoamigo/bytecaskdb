// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// degraded.h — what the plugin does when the engine degrades (#294).

#pragma once

#include "bytecask.hpp"
#include "log.h"

#include <cstdlib>
#include <string>

// A degraded engine refuses writes until resume() or a reopen. MariaDB's
// engines do not stay up refusing writes: MyRocks aborts on any write I/O
// error, InnoDB on a failed fsync, and recovery runs at restart. So does
// this plugin. DB::open does what resume() would — it reads back and syncs
// a file the last process may not have synced, and truncates a torn tail —
// so a restart loses nothing a resume would have kept.
//
// Called from the catch of every engine write. An error that left the engine
// healthy (a conflict, a bad argument) returns; the caller reports it.
inline void abort_if_degraded(bytecask::DB &db, const char *what) noexcept {
  if (!db.is_degraded()) return;
  std::string reason;
  try {
    reason = db.degraded_reason();
  } catch (...) {
    // Out of memory for the copy: abort without it.
  }
  sql_print_error("ByteCaskDB: %s: the engine refused writes after a write "
                  "failure (%s). Aborting; recovery runs at restart.",
                  what, reason.c_str());
  std::abort();
}
