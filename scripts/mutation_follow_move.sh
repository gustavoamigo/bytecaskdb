#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Regenerates mutation patches whose target code moved verbatim to another
# file (#331). Three-way merge does not follow code across files: a patch
# with its path rewritten conflicts, because its `index` blob is the old
# file. So each patch that no longer applies is first merged into <from> as
# it was at <base>, the last commit before the move, which refreshes its
# context; that diff, with its path rewritten and its `index` line dropped,
# is then applied to <to> by context alone, and `git diff` records it with an
# `index` line against <to>, so mutation_patch.sh merges it from here on.
#
# Usage: scripts/mutation_follow_move.sh <base> <from> <to> <patch>...
#   A patch that still applies is left alone. A patch that does not apply to
#   <to> is reported and left unchanged. Needs history (not a shallow clone)
#   and a clean, committed <to>.

set -uo pipefail

usage="usage: $0 <base> <from> <to> <patch>..."
base=${1:?$usage}
from=${2:?$usage}
to=${3:?$usage}
shift 3

root=$(git rev-parse --show-toplevel)
merge="$root/scripts/mutation_patch.sh"
wt=$(mktemp -d)
tmp=$(mktemp)
cleanup() {
  git worktree remove --force "$wt" >/dev/null 2>&1
  rm -f "$tmp" "$tmp.new"
}
trap cleanup EXIT
git worktree add -q --detach "$wt" "$base" || exit 1

status=0
for patch in "$@"; do
  patch=$(realpath "$patch")
  if "$merge" check "$patch" >/dev/null 2>&1; then
    continue  # still applies where it is
  fi
  if ! (cd "$wt" && "$merge" apply "$patch" >/dev/null 2>&1); then
    echo "mutation_follow_move: $patch does not apply at $base either" >&2
    status=1
    continue
  fi
  git -C "$wt" diff HEAD -- "$from" |
    sed -e "s|a/$from|a/$to|g" -e "s|b/$from|b/$to|g" -e '/^index /d' \
      > "$tmp"
  (cd "$wt" && "$merge" revert "$patch" >/dev/null 2>&1)
  if ! git apply "$tmp" >/dev/null 2>&1; then
    echo "mutation_follow_move: $patch does not apply to $to" >&2
    status=1
    continue
  fi
  {
    sed '/^diff --git /,$d' "$patch"
    git diff -- "$to"
  } > "$tmp.new"
  git checkout -q HEAD -- "$to"
  mv "$tmp.new" "$patch"
  echo "mutation_follow_move: $patch now targets $to"
done
exit "$status"
