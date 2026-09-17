#!/usr/bin/env bash
# buffer_pool vs mmap vs pread on the current checkout, unpressured and
# under a memory limit with swap. The db-versus-OS question.
#
#   backend-comparison.sh [mem_limit_bytes] [pool_bytes]
#
# Environment as in lib.sh. Reproduces FINDINGS.md §4.
set -uo pipefail
EXP_NAME=backend-comparison
source "$(dirname "$0")/lib.sh"
LIMIT="${1:-$GiB}"; POOL="${2:-$((256 * MiB))}"
build_engine
for be in pool mmap pread; do
  extra=(); [[ $be == pool ]] && extra=(--pool-bytes="$POOL")
  arm bytecaskdb-$be "$be-unlimited" $NO_LIMIT "${extra[@]}" --recovery-threads=1
  arm bytecaskdb-$be "$be-limited" --mem-limit="$LIMIT" --swap-limit=$((2 * GiB)) \
      "${extra[@]}" --recovery-threads=1
done
log "results in $OUT_DIR"
