#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Proves SERIALIZABLE's tests have teeth (docs/transaction_design.md, *Stages
# of serializable isolation*, stage 1; tests/CLAUDE.md, *Mutation sets*):
# applies each mutation in bytecaskdb-sql-layer/tests/serializable_mutations/,
# rebuilds sql_layer_tests, runs the Catch2 spec its `Guarded-by:` header
# names, and records whether a test failed. The mutation is reverted before
# the next one, and on exit whatever happens. Run from the repository root.
#
# Usage: bytecaskdb-sql-layer/scripts/serializable_mutation_check.sh [patch...]
#   patch  mutations to run (default: every serializable_mutations/*.patch)
#
# A patch whose header says "Expected: caught" and survives fails the script,
# and so does one that does not build or no longer applies. "Expected: NOT
# caught" records a break other code covers, with the reason in the patch.
# SQL_LAYER_TESTS overrides the test binary (default: the release build).
# Needs a working tree whose files the patches touch are clean.

set -euo pipefail

patches=("$@")
if [ ${#patches[@]} -eq 0 ]; then
  patches=(bytecaskdb-sql-layer/tests/serializable_mutations/*.patch)
fi
tests=${SQL_LAYER_TESTS:-build/linux/x86_64/release/sql_layer_tests}

applied=""
revert() {
  if [ -n "$applied" ]; then
    scripts/mutation_patch.sh revert "$applied"
    applied=""
  fi
}
trap revert EXIT

status=0
summary=""
for p in "${patches[@]}"; do
  name=$(basename "$p" .patch)
  expect=caught
  grep -q "Expected: NOT caught" "$p" && expect=survives
  spec=$(sed -n 's/^Guarded-by: *"\(.*\)" *$/\1/p' "$p" | head -1)
  if ! scripts/mutation_patch.sh apply "$p"; then
    summary+=$(printf '%-40s %-9s (regenerate the patch)' "$name" conflict)$'\n'
    status=1
    continue
  fi
  applied=$p
  started=$SECONDS
  build_log=$(mktemp)
  if ! xmake build -y sql_layer_tests >"$build_log" 2>&1; then
    result=error
    detail="does not build, log kept at $build_log"
  else
    rm -f "$build_log"
    if "$tests" "$spec" >/dev/null 2>&1; then
      result=survived
      detail=""
    else
      result=caught
      detail="$spec"
    fi
  fi
  elapsed=$(( SECONDS - started ))
  summary+=$(printf '%-40s %-9s (expected %s, %4ds)  %s' "$name" "$result" "$expect" "$elapsed" \
               "${detail:0:100}")$'\n'
  if [ "$result" = error ] || { [ "$expect" = caught ] && [ "$result" = survived ]; }; then
    status=1
    echo "serializable_mutation_check: $name $result" >&2
  fi
  revert
done
xmake build -y sql_layer_tests >/dev/null

printf '\n%s' "$summary"
exit $status
