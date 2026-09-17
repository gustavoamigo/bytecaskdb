#!/usr/bin/env bash
# Two engine branches, same dataset, unpressured and under a memory limit
# with swap. Built to compare the refcounted radix tree with the epoch-
# reclamation one; works for any two branches that both carry this
# benchmark (cherry-pick the memory-pressure commits onto the older branch
# if it lacks them — the script needs --recovery-threads and
# --no-secondary-index on both sides).
#
#   tree-comparison.sh <branch-A> <branch-B> [mem_limit_bytes] [pool_bytes]
#
# Environment: ROWS (default 20000000), SECONDARY (on|off, default off),
# DATA_ROOT, THREADS, WARMUP, TIME, WORKLOADS, RECOVERY_THREADS (default 1).
#
# Findings this reproduces: FINDINGS.md §2.
set -uo pipefail
EXP_NAME=tree-comparison
source "$(dirname "$0")/lib.sh"
A="${1:?branch A}"; B="${2:?branch B}"
LIMIT="${3:-$GiB}"; POOL="${4:-$((256 * MiB))}"
RT="${RECOVERY_THREADS:-1}"

for br in "$A" "$B"; do
  (cd "$ROOT" && git checkout "$br" >/dev/null 2>&1) || { echo "checkout $br failed"; exit 1; }
  build_engine
  lbl="$(echo "$br" | tr '/' '_')"
  arm bytecaskdb-pool "${lbl}-unlimited" $NO_LIMIT --pool-bytes="$POOL" --recovery-threads="$RT"
  arm bytecaskdb-pool "${lbl}-limited" --mem-limit="$LIMIT" --swap-limit=$((2 * GiB)) \
      --pool-bytes="$POOL" --recovery-threads="$RT"
done
log "results in $OUT_DIR"
