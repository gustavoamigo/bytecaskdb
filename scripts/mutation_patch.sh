#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Applies, reverts or checks one mutation patch (tests/soak_mutations/,
# tests/chaos_mutations/, tests/durability_mutations/) by three-way merge, so a patch keeps applying when
# lines near its target change. Used by soak_mutation_check.sh,
# chaos_mutation_check.sh and ci.yml.
#
# Usage: scripts/mutation_patch.sh apply|revert|check <patch>
#   apply   git apply --3way. On a conflict the patch's files are restored
#           from HEAD and the script fails: the targeted code itself changed,
#           and the patch must be regenerated (make the same break, save
#           `git diff`).
#   revert  git apply -R --3way, the same merge in reverse. If that fails the
#           files are restored from HEAD anyway, and the script fails.
#   check   apply, then revert; the tree is left as it was.
#
# The merge needs the blob each patch was made against (its `index` line), so
# run it in a clone with history (actions/checkout: fetch-depth: 0); in a
# shallow clone it says which blobs are missing rather than calling the patch
# stale. It stages what it applies and unstages it on revert; run it on a
# tree whose files the patch touches are clean, since a restore discards
# their changes.
# `git apply --check --3way` cannot stand in for `check`: it exits 0 on a
# conflict.

set -uo pipefail

action=${1:?usage: $0 apply|revert|check <patch>}
patch=${2:?usage: $0 apply|revert|check <patch>}

mapfile -t files < <(sed -n 's|^+++ b/||p' "$patch")
if [ ${#files[@]} -eq 0 ]; then
  echo "mutation_patch: $patch touches no files" >&2
  exit 2
fi

restore() {
  git checkout -q HEAD -- "${files[@]}"
}

# The pre-image blobs of the patch's `index` lines this clone lacks. In a
# shallow clone the merge fails for want of them, not because the code moved.
# A file the patch creates has an all-zero pre-image, which no clone holds.
missing_blobs() {
  sed -n 's/^index \([0-9a-f]*\)\.\..*/\1/p' "$patch" | while read -r blob; do
    [ -n "${blob//0/}" ] || continue
    git cat-file -e "$blob" 2>/dev/null || echo "$blob"
  done
}

apply() {
  if ! git apply --3way "$patch" >/dev/null 2>&1; then
    restore
    local missing
    missing=$(missing_blobs)
    if [ -n "$missing" ]; then
      echo "mutation_patch: $patch needs blob(s) $(echo $missing) that this clone" \
        "does not have; fetch the history (git fetch --unshallow) and retry" >&2
      return 1
    fi
    echo "mutation_patch: $patch conflicts with the current code; regenerate it" >&2
    return 1
  fi
}

revert() {
  if ! git apply -R --3way "$patch" >/dev/null 2>&1; then
    restore
    echo "mutation_patch: $patch did not revert cleanly; files restored from HEAD" >&2
    return 1
  fi
}

case $action in
  apply) apply ;;
  revert) revert ;;
  check) apply && revert ;;
  *)
    echo "usage: $0 apply|revert|check <patch>" >&2
    exit 2
    ;;
esac
