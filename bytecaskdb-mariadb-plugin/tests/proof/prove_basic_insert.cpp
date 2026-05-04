// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_basic_insert.cpp — Minimal proof test verifying insert path invariants.

#include "harness.h"

#include <catch2/catch_test_macros.hpp>

using namespace bytecaskdb::testing;

TEST_CASE("P-INV-1: insert increments row counter", "[proof][insert][inv1]") {
  TableSpec spec;
  spec.num_int_columns = 2;
  spec.has_pk = true;

  PluginTestHarness h(spec);
  REQUIRE(h.row_counter() == 0);

  int rc = h.insert_row({10, 100});
  REQUIRE(rc == 0);
  REQUIRE(h.row_counter() == 1);

  rc = h.insert_row({20, 200});
  REQUIRE(rc == 0);
  REQUIRE(h.row_counter() == 2);
}

TEST_CASE("P-INV-2: insert duplicate PK returns HA_ERR_FOUND_DUPP_KEY", "[proof][insert][inv2]") {
  TableSpec spec;
  spec.num_int_columns = 2;
  spec.has_pk = true;

  PluginTestHarness h(spec);

  int rc = h.insert_row({10, 100});
  REQUIRE(rc == 0);

  rc = h.insert_row({10, 999});
  REQUIRE(rc == HA_ERR_FOUND_DUPP_KEY);
  REQUIRE(h.row_counter() == 1);
}

TEST_CASE("P-INV-3: commit persists data to underlying DB", "[proof][insert][inv3]") {
  TableSpec spec;
  spec.num_int_columns = 2;
  spec.has_pk = true;

  PluginTestHarness h(spec);

  int rc = h.insert_row({10, 100});
  REQUIRE(rc == 0);

  rc = h.commit();
  REQUIRE(rc == 0);
  REQUIRE(h.row_counter() == 1);
}

TEST_CASE("P-INV-4: rollback leaves row counter at zero", "[proof][insert][inv4]") {
  TableSpec spec;
  spec.num_int_columns = 2;
  spec.has_pk = true;

  PluginTestHarness h(spec);

  int rc = h.insert_row({10, 100});
  REQUIRE(rc == 0);

  h.rollback();
  REQUIRE(h.row_counter() == 0);
}
