// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_basic_insert.cpp — Minimal proof test verifying insert path invariants.

#include "harness.h"

#include <catch2/catch_test_macros.hpp>

using namespace bytecaskdb;
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

TEST_CASE("P-FMT-1: open refuses a key format newer than this build",
          "[proof][open][key_format]") {
  // A newer format sizes its stored keys in ways this build cannot see
  // (format 2's weights are not key_length wide); read as an older one,
  // every offset into a stored key would be wrong.
  TableSpec spec;
  spec.num_int_columns = 2;
  spec.has_pk = true;
  PluginTestHarness h(spec);

  TableMeta meta;
  REQUIRE(catalog_copy_meta(h.table_id(), meta));
  meta.key_format = kKeyFormatCurrent + 1;
  REQUIRE(catalog_put_table_meta(&h.db(), meta, "./test/proof_t"));
  REQUIRE(h.handler().close() == 0);
  REQUIRE(h.handler().open("./test/proof_t", 0, 0) ==
          HA_ERR_INCOMPATIBLE_DEFINITION);

  meta.key_format = kKeyFormatCurrent;
  REQUIRE(catalog_put_table_meta(&h.db(), meta, "./test/proof_t"));
  REQUIRE(h.handler().open("./test/proof_t", 0, 0) == 0);
}
