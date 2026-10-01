#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Proves each durability site's guard has teeth (#268, #280): applies each
# mutation in tests/chaos_mutations/, rebuilds bytecask_tests, and runs the
# tests its "Guarded-by:" headers name. Every one of them must fail. The
# mutation is reverted before the next one, and on exit whatever happens.
#
# Usage: scripts/chaos_mutation_check.sh [--chaos MINUTES] [patch...]
#   patch    mutations to run (default: every tests/chaos_mutations/*.patch,
#            and every tests/soak_mutations/*.patch with a Guarded-by: header)
#   --chaos  after the guards, also run the chaos rig on each mutation for
#            MINUTES per seed (tests/chaos/run_chaos.py, every hazard): a
#            second, black-box confirmation. It is reported and gates
#            nothing. A patch's "Budget: N" header overrides MINUTES, and its
#            "Disable: a,b" header focuses the run (run_chaos.py --disable).
#
# A patch carries either
#   Guarded-by: "<Catch2 test spec>"   (one or more; each must fail), or
#   Not needed: <reason>               (a break nothing has to catch, kept
#                                       as the record of why; not run)
# Before any patch is applied, every guard runs on the clean tree, where it
# must pass and match at least one test: a misspelt name fails the check
# instead of passing it. A guard that hangs past GUARD_TIMEOUT seconds
# (default 600) counts as failing. The rig runs on the seeds in
# CHAOS_MUTATION_SEEDS (default "1 2"); CHAOS_WORKER overrides its worker
# (default: the release build), which needs FUSE and mfusepy. Needs a clean
# working tree for the files the patches touch.

set -euo pipefail

chaos_mins=""
if [ "${1:-}" = --chaos ]; then
  chaos_mins=${2:?--chaos needs MINUTES}
  shift 2
fi
patches=("$@")
if [ ${#patches[@]} -eq 0 ]; then
  patches=(tests/chaos_mutations/*.patch)
  for p in tests/soak_mutations/*.patch; do
    grep -q '^Guarded-by:' "$p" && patches+=("$p")
  done
fi
worker=${CHAOS_WORKER:-build/linux/x86_64/release/chaos_worker}
read -r -a seeds <<< "${CHAOS_MUTATION_SEEDS:-1 2}"
guard_timeout=${GUARD_TIMEOUT:-600}

applied=""
revert() {
  if [ -n "$applied" ]; then
    scripts/mutation_patch.sh revert "$applied"
    applied=""
  fi
}
trap revert EXIT

# The specs on a patch's Guarded-by: lines, one per line, unquoted.
guards_of() {
  sed -n 's/^Guarded-by: *"\(.*\)" *$/\1/p' "$1"
}

build() {
  local log
  log=$(mktemp)
  if xmake build -P . "$1" >"$log" 2>&1; then
    rm -f "$log"
  else
    echo "$log"
    return 1
  fi
}

# Runs one guard; prints pass, fail or nomatch. A comma in a Catch2 test
# spec separates specs, so the ones in test names are escaped; * stays a
# wildcard.
run_guard() {
  local out rc=0
  out=$(timeout "$guard_timeout" xmake run -P . bytecask_tests "${1//,/\\,}" 2>&1) || rc=$?
  if grep -q "No test cases matched" <<<"$out"; then
    echo nomatch
  elif [ $rc -eq 0 ]; then
    echo pass
  else
    echo fail
  fi
}

status=0
summary=""
note() {
  summary+=$(printf '%-32s %-9s %s' "$1" "$2" "$3")$'\n'
}

# The guards on the clean tree.
if ! log=$(build bytecask_tests); then
  echo "chaos_mutation_check: the clean tree does not build, log at $log" >&2
  exit 1
fi
for p in "${patches[@]}"; do
  name=$(basename "$p" .patch)
  if grep -q '^Not needed:' "$p"; then continue; fi
  mapfile -t guards < <(guards_of "$p")
  if [ ${#guards[@]} -eq 0 ]; then
    note "$name" unguarded "no Guarded-by: or Not needed: header"
    status=1
    continue
  fi
  for g in "${guards[@]}"; do
    r=$(run_guard "$g")
    if [ "$r" != pass ]; then
      note "$name" badguard "\"$g\" on the clean tree: $r"
      status=1
    fi
  done
done
if [ $status -ne 0 ]; then
  printf '\n%s' "$summary"
  exit $status
fi

for p in "${patches[@]}"; do
  name=$(basename "$p" .patch)
  if grep -q '^Not needed:' "$p"; then
    note "$name" "not run" "Not needed: $(sed -n 's/^Not needed: *//p' "$p" | cut -c1-80)"
    continue
  fi
  mapfile -t guards < <(guards_of "$p")
  # A three-way merge: the patch keeps applying when lines near its target
  # change; a conflict means the targeted code changed (mutation_patch.sh).
  if ! scripts/mutation_patch.sh apply "$p"; then
    note "$name" conflict "regenerate the patch"
    status=1
    continue
  fi
  applied=$p
  started=$SECONDS
  if ! log=$(build bytecask_tests); then
    note "$name" error "does not build, log kept at $log"
    status=1
    revert
    continue
  fi
  result=caught
  detail=""
  for g in "${guards[@]}"; do
    r=$(run_guard "$g")
    if [ "$r" = pass ]; then
      result=survived
      detail="\"$g\" passes"
      status=1
      echo "chaos_mutation_check: $name survived \"$g\"" >&2
      break
    fi
  done
  [ $result = caught ] && detail="by ${#guards[@]} guard(s)"

  if [ -n "$chaos_mins" ]; then
    budget=$(sed -n 's/^Budget: *\([0-9.]*\).*/\1/p' "$p")
    budget=${budget:-$chaos_mins}
    disable=$(sed -n 's/^Disable: *//p' "$p")
    rig=survived
    if ! log=$(build chaos_worker); then
      rig="error (worker does not build, log at $log)"
      seeds_to_run=()
    else
      seeds_to_run=("${seeds[@]}")
    fi
    for seed in "${seeds_to_run[@]}"; do
      work=$(mktemp -d)
      rc=0
      python3 tests/chaos/run_chaos.py --worker "$worker" --minutes "$budget" \
        --seed "$seed" --work "$work" ${disable:+--disable "$disable"} \
        >"$work/run.log" 2>&1 || rc=$?
      if [ $rc -eq 1 ]; then
        rig="caught (seed $seed)"
        rm -rf "$work"
        break
      elif [ $rc -ne 0 ]; then
        rig="error (seed $seed, log kept at $work/run.log)"
        break
      fi
      rm -rf "$work"
    done
    detail+="; rig: $rig"
  fi
  note "$name" "$result" "$(printf '%4ds  %s' $(( SECONDS - started )) "$detail")"
  revert
done
build bytecask_tests >/dev/null || true
[ -n "$chaos_mins" ] && { build chaos_worker >/dev/null || true; }

printf '\n%s' "$summary"
exit $status
