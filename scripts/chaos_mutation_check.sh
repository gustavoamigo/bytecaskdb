#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Proves the chaos rig (#230, #268) has teeth: applies each mutation in
# tests/chaos_mutations/, rebuilds chaos_worker, runs tests/chaos/run_chaos.py
# with every hazard, and records whether a life failed. The mutation is
# reverted before the next one, and on exit whatever happens.
#
# Usage: scripts/chaos_mutation_check.sh [minutes] [patch...]
#   minutes  chaos budget per seed and mutation (default 5); a patch's
#            "Budget: N" header overrides it, and its "Disable: a,b" header
#            focuses the run on the hazards that reach it (run_chaos.py
#            --disable)
#   patch    mutations to run (default: every tests/chaos_mutations/*.patch)
#
# Each mutation runs on the seeds in CHAOS_MUTATION_SEEDS (default "1 2") in
# turn and counts as caught at the first failing life. A patch whose header
# says "Expected: caught" and survives fails the script, and so does a run the
# rig itself could not complete. "Expected: caught (rare)" marks a mutation
# the rig can catch but not within a fixed budget: it runs and is reported,
# and a survival is not a failure. CHAOS_WORKER overrides the worker binary
# (default: the release build). Needs FUSE, mfusepy, and a clean working tree
# for the files the patches touch.

set -euo pipefail

mins=${1:-5}
shift $(( $# >= 1 ? 1 : 0 ))
patches=("$@")
if [ ${#patches[@]} -eq 0 ]; then
  patches=(tests/chaos_mutations/*.patch)
fi
worker=${CHAOS_WORKER:-build/linux/x86_64/release/chaos_worker}
read -r -a seeds <<< "${CHAOS_MUTATION_SEEDS:-1 2}"

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
  grep -q "Expected: caught (rare)" "$p" && expect=rare
  budget=$(sed -n 's/^Budget: *\([0-9.]*\).*/\1/p' "$p")
  budget=${budget:-$mins}
  disable=$(sed -n 's/^Disable: *//p' "$p")
  # A three-way merge: the patch keeps applying when lines near its target
  # change; a conflict means the targeted code changed (mutation_patch.sh).
  if ! scripts/mutation_patch.sh apply "$p"; then
    summary+=$(printf '%-32s %-9s (regenerate the patch)' "$name" conflict)$'\n'
    status=1
    continue
  fi
  applied=$p
  result=survived
  detail=""
  started=$SECONDS
  build_log=$(mktemp)
  if ! xmake build chaos_worker >"$build_log" 2>&1; then
    result=error
    detail="does not build, log kept at $build_log"
    seeds_to_run=()
  else
    rm -f "$build_log"
    seeds_to_run=("${seeds[@]}")
  fi
  for seed in "${seeds_to_run[@]}"; do
    work=$(mktemp -d)
    log=$work/run.log
    if python3 tests/chaos/run_chaos.py --worker "$worker" --minutes "$budget" \
        --seed "$seed" --work "$work" ${disable:+--disable "$disable"} \
        >"$log" 2>&1; then
      rc=0
    else
      rc=$?
    fi
    if [ $rc -eq 1 ]; then
      result=caught
      # The failure's first line, and which life of which seed it took.
      detail="seed $seed: $(grep -m1 -A1 '^FAIL episode' "$log" | tail -1 \
                | sed -E 's/^ +(FAIL )?//' | cut -c1-110)"
      rm -rf "$work"
      break
    elif [ $rc -ne 0 ]; then
      result=error
      detail="seed $seed: rig error, log kept at $log"
      break
    fi
    rm -rf "$work"
  done
  elapsed=$(( SECONDS - started ))
  summary+=$(printf '%-32s %-9s (expected %s, %4ds)  %s' "$name" "$result" \
               "$expect" "$elapsed" "$detail")$'\n'
  if [ "$result" = error ] || { [ "$expect" = caught ] && [ "$result" = survived ]; }; then
    status=1
    echo "chaos_mutation_check: $name $result" >&2
  fi
  revert
done
xmake build chaos_worker >/dev/null

printf '\n%s' "$summary"
exit $status
