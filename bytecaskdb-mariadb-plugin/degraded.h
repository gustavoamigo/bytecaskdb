// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// degraded.h — what the plugin does when the engine degrades (#294).

#pragma once

#include "bytecask.hpp"

// A degraded engine refuses writes until resume() or a reopen. MariaDB's
// engines do not stay up refusing writes: MyRocks aborts on any write I/O
// error, InnoDB on a failed fsync, and recovery runs at restart. So does
// this plugin. DB::open does what resume() would — it reads back and syncs
// a file the last process may not have synced, and truncates a torn tail —
// so a restart loses nothing a resume would have kept.
//
// Called from the catch of every engine write. An error that left the engine
// healthy (a conflict, a bad argument) returns; the caller reports it. When
// the engine is degraded it logs the reason to the error log and aborts.
//
// Declared here and defined in bytecaskdb_plugin.cc, so this header pulls in
// no MariaDB header: those must come in their own order (my_global.h first).
void abort_if_degraded(bytecask::DB &db, const char *what) noexcept;
