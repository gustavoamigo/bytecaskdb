// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_degraded.cpp — the plugin aborts when a write leaves the engine
// degraded (#294). The generated matrix (prove_plugin.cpp) covers a failed
// append and a failed sync; this covers the failure that comes after the
// write is durable and published: the rotation that follows it.

#include "harness.h"
#include "fault_injector.h"

#include <catch2/catch_test_macros.hpp>

using namespace bytecaskdb::testing;
using namespace bytecask::testing;

// With max_file_bytes = 0 every commit seals the active file and opens the
// next. When opening the next fails, the commit's write is already synced and
// published, the engine degrades, and apply_batch throws to the commit: the
// plugin aborts, and the write is in the data file for the restart.
TEST_CASE("P-DEGRADE-1: a rotation that fails after the commit is durable "
          "aborts the server",
          "[proof][degraded]") {
  TableSpec spec;
  spec.num_int_columns = 3;
  spec.has_pk = true;
  bytecask::Options opts;
  opts.max_file_bytes = 0;
  PluginTestHarness h(spec, opts);

  REQUIRE(h.insert_row({100, 200, 300}) == 0);
  ScopedFaultInjector guard("io_rotate_file_creation");
  REQUIRE(h.commit_aborts());
}
