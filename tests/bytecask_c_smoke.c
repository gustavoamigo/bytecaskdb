/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Gustavo Amigo
 *
 * Plain C11 compilation smoke test for include/bytecask_c.h (BC-231 Phase 3).
 * Exercises the CommitResult/write-options structs and durable_sequence
 * function from real C — not C++ — to catch header syntax or struct-layout
 * mistakes that only a C compiler would reject.
 */

/* mkdtemp is POSIX, not ISO C11 — expose it under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bytecask_c.h"

static int check(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s (%s)\n", msg, bytecask_errmsg());
    return 1;
  }
  return 0;
}

int main(void) {
  int failures = 0;
  char dir[] = "/tmp/bytecask_c_smoke_XXXXXX";
  bytecask_db_t *db;
  bytecask_write_options_t opts;
  bytecask_commit_result_t result;

  if (!mkdtemp(dir)) {
    fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }

  db = bytecask_open(dir, 0);
  failures += check(db != NULL, "bytecask_open");

  memset(&opts, 0, sizeof(opts));
  opts.sync = 1;
  memset(&result, 0xAA, sizeof(result));

  failures += check(
      bytecask_put(db, (const uint8_t *)"k", 1, (const uint8_t *)"v", 1,
                   &opts, &result) == 0,
      "bytecask_put");
  failures += check(result.sequence > 0, "put sequence assigned");
  failures += check(result.durable != 0, "put durable with sync=1");

  failures += check(
      bytecask_del(db, (const uint8_t *)"k", 1, NULL, &result) == 1,
      "bytecask_del existing key");

  failures += check(
      bytecask_del(db, (const uint8_t *)"k", 1, NULL, NULL) == 0,
      "bytecask_del absent key, null out");

  failures += check(bytecask_durable_sequence(db, 0, 0) > 0,
                    "bytecask_durable_sequence non-blocking poll");

  bytecask_close(db);

  if (failures == 0) {
    printf("bytecask_c_smoke: OK\n");
  }
  return failures;
}
