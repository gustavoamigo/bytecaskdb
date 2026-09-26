#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Proves the chaos soak (#92) has teeth: applies each one-line mutation in
# tests/soak_mutations/, rebuilds bytecask_tests under a sanitizer, runs the
# soak, and records whether it failed. The mutation is reverted before the
# next one, and on exit whatever happens.
#
# Usage: scripts/soak_mutation_check.sh <address|thread> [seconds] [patch...]
#   seconds  soak budget per mutation (default 120)
#   patch    mutations to run (default: every tests/soak_mutations/*.patch)
#
# A patch whose header says "Expected: caught" and survives fails the script.
# Configure the sanitizer yourself first, as CI does:
#   xmake f --toolchain=clang --sanitizer=<san> -m debug -y
# Needs a clean working tree for the files the patches touch.

set -euo pipefail

san=${1:?usage: $0 <address|thread> [seconds] [patch...]}
secs=${2:-120}
shift $(( $# >= 2 ? 2 : 1 ))
patches=("$@")
if [ ${#patches[@]} -eq 0 ]; then
  patches=(tests/soak_mutations/*.patch)
fi

bin=build/linux/x86_64/debug/bytecask_tests
applied=""
revert() {
  if [ -n "$applied" ]; then
    git apply -R "$applied"
    applied=""
  fi
}
trap revert EXIT

case $san in
  thread) export TSAN_OPTIONS="halt_on_error=1 history_size=4" ;;
  address) export ASAN_OPTIONS="detect_leaks=1" ;;
esac

status=0
summary=""
for p in "${patches[@]}"; do
  name=$(basename "$p" .patch)
  expect=caught
  grep -q "Expected: NOT caught" "$p" && expect=survives
  # "Sanitizer: thread" marks a mutation only that sanitizer can see.
  only=$(sed -n 's/^Sanitizer: *//p' "$p")
  if [ -n "$only" ] && [ "$only" != "$san" ]; then
    summary+=$(printf '%-32s %-9s (needs --sanitizer=%s)' "$name" skipped "$only")$'\n'
    continue
  fi
  git apply "$p"
  applied=$p
  xmake build bytecask_tests >/dev/null
  log=$(mktemp)
  # A fixed seed per mutation keeps runs comparable; the budget spans many
  # epochs, and so many configurations, either way.
  if BYTECASK_SOAK_SECONDS=$secs BYTECASK_SOAK_SEED=${BYTECASK_SOAK_SEED:-92} \
      timeout $(( secs * 3 + 120 )) "$bin" "[soak]" >"$log" 2>&1; then
    result=survived
  else
    result=caught
  fi
  # The sanitizer's headline, or the first soak failure with its config
  # prefix cut off.
  first=$(grep -m1 -oE "(Thread|Address)Sanitizer: [^(]*" "$log" || true)
  if [ -z "$first" ]; then
    first=$(sed -n '/soak failures:/,/^====/p' "$log" | tr -s ' \n' ' ' \
              | sed -E 's/.*soak failures: - \[[^]]*\] //' | cut -c1-120)
  fi
  summary+=$(printf '%-32s %-9s (expected %s)  %s' "$name" "$result" "$expect" "$first")$'\n'
  if [ "$expect" = caught ] && [ "$result" = survived ]; then
    status=1
    echo "soak_mutation_check: $name survived; log kept at $log" >&2
  else
    rm -f "$log"
  fi
  revert
done
xmake build bytecask_tests >/dev/null

printf '\n%s' "$summary"
exit $status
