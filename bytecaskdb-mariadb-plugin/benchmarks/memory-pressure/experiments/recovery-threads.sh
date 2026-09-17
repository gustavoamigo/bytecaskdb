#!/usr/bin/env bash
# Does parallel recovery help or hurt when the key directory does not fit?
# Runs the current checkout under a memory limit with swap at several
# recovery thread counts; the number that matters is recovery_ms.
#
#   recovery-threads.sh [mem_limit_bytes] [thread_counts...]
#
# Default counts: 4 2 1. Uses a token workload since startup is what is
# measured. Reproduces FINDINGS.md §3.
set -uo pipefail
EXP_NAME=recovery-threads
WORKLOADS=oltp_point_select WARMUP=5 TIME=10
source "$(dirname "$0")/lib.sh"
LIMIT="${1:-$GiB}"; shift || true
COUNTS=("${@:-4 2 1}"); COUNTS=(${COUNTS[@]})
build_engine
for t in "${COUNTS[@]}"; do
  arm bytecaskdb-pool "recovery-${t}threads" --mem-limit="$LIMIT" --swap-limit=$((2 * GiB)) \
      --pool-bytes=$((256 * MiB)) --recovery-threads="$t"
done
log "results in $OUT_DIR"
