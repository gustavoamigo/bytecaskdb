#!/usr/bin/env bash
# InnoDB under the same memory limit, with its buffer pool swept so it is
# compared at its own best size rather than at a guess. Also runs one arm
# with a pool larger than the table and no limit, as InnoDB's ceiling.
#
#   innodb-sizing.sh [mem_limit_bytes] [pool_sizes_MiB...]
#
# Default sweep: 384 512 640 768 896. Reproduces FINDINGS.md §5.
set -uo pipefail
EXP_NAME=innodb-sizing
source "$(dirname "$0")/lib.sh"
LIMIT="${1:-$GiB}"; shift || true
SIZES=("${@:-384 512 640 768 896}"); SIZES=(${SIZES[@]})
for mb in "${SIZES[@]}"; do
  arm innodb "innodb-${mb}MiB-limited" --mem-limit="$LIMIT" --swap-limit=$((2 * GiB)) \
      --innodb-pool-bytes=$((mb * MiB))
done
arm innodb "innodb-ceiling" $NO_LIMIT --innodb-pool-bytes=$((6 * GiB))
log "results in $OUT_DIR"
