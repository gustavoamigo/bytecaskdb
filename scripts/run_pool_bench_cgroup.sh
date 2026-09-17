#!/usr/bin/env bash
# pool_bench inside a memory cgroup: the buffer pool against the page cache
# when the values do not fit.
#
# The scenario this exists for: a key directory that fits trivially and a
# dataset that does not — say 5 000 keys of 2 MiB (10 GB) against 512 MiB
# of memory. The pool takes a fixed share of that memory for values and
# fills it O_DIRECT; pread and mmap leave the same memory to the kernel's
# page cache, which the cgroup bounds just as hard. Same budget, two
# allocators.
#
# The dataset is built once, outside the cgroup (a 10 GB build under a
# 512 MiB limit would be throttled by writeback), and reused by every run.
# Each run starts cold (--cold drops the data files' page cache) so no
# back-end inherits another's residency, and the pool rows use O_DIRECT
# fills only: a pool filled through the page cache under a limit is two
# caches, and says nothing about the pool. --buffered-too adds those rows.
#
# Requirements: passwordless sudo (cgroup), cgroup v2 with the memory
# controller at the root, and a disk with room for the dataset.
#
# Usage:
#   scripts/run_pool_bench_cgroup.sh [--keys=N] [--value-bytes=N] [--ops=N]
#       [--max-file-bytes=N] [--mem-limit=BYTES] [--pool-bytes=a,b,c]
#       [--backends=a,b,c] [--zipf=S] [--dir=PATH] [--out=FILE] [--fresh]
#       [--buffered-too]
#
# Defaults: 5000 keys x 2 MiB, 4000 ops, 64 MiB files, 512 MiB limit,
# pools 128,256,384 MiB, all back-ends, Zipf 0.99.
set -uo pipefail
KEYS=5000
VALUE_BYTES=$((2 * 1024 * 1024))
OPS=4000
MAX_FILE=$((64 * 1024 * 1024))
MEM_LIMIT=$((512 * 1024 * 1024))
POOLS="$((128 * 1024 * 1024)),$((256 * 1024 * 1024)),$((384 * 1024 * 1024))"
BACKENDS="pread,mmap,buffer_pool"
ZIPF=0.99
DIR=""
OUT=""
FRESH=0
DIRECT_ONLY=--direct-only
for arg in "$@"; do
  case "$arg" in
    --keys=*)           KEYS="${arg#*=}" ;;
    --value-bytes=*)    VALUE_BYTES="${arg#*=}" ;;
    --ops=*)            OPS="${arg#*=}" ;;
    --max-file-bytes=*) MAX_FILE="${arg#*=}" ;;
    --mem-limit=*)      MEM_LIMIT="${arg#*=}" ;;
    --pool-bytes=*)     POOLS="${arg#*=}" ;;
    --backends=*)       BACKENDS="${arg#*=}" ;;
    --zipf=*)           ZIPF="${arg#*=}" ;;
    --dir=*)            DIR="${arg#*=}" ;;
    --out=*)            OUT="${arg#*=}" ;;
    --fresh)            FRESH=1 ;;
    --buffered-too)     DIRECT_ONLY="" ;;
    --help|-h)          sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/linux/x86_64/release/pool_bench"
DIR="${DIR:-$ROOT/.tmp/pool_bench_cgroup}"
OUT="${OUT:-$ROOT/.tmp/pool_bench_cgroup_$(date +%Y%m%dT%H%M%S).csv}"
CG=/sys/fs/cgroup/bytecaskdb-pool-bench
log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

[[ -x $BIN ]] || { echo "build first: xmake build pool_bench"; exit 1; }
sudo -n true 2>/dev/null || { echo "passwordless sudo is required for the cgroup"; exit 1; }
grep -qw memory /sys/fs/cgroup/cgroup.subtree_control || { echo "cgroup v2 memory controller not enabled at the root"; exit 1; }

(( FRESH )) && rm -rf "$DIR"
mkdir -p "$(dirname "$DIR")"
COMMON=(--keys "$KEYS" --value-bytes "$VALUE_BYTES" --max-file-bytes "$MAX_FILE" --zipf "$ZIPF" --dir "$DIR")

log "dataset: $KEYS keys x $VALUE_BYTES B in $DIR"
"$BIN" "${COMMON[@]}" --reuse --build-only || exit 1
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'

cleanup() { [[ -d $CG ]] && sudo rmdir "$CG" 2>/dev/null; }
trap cleanup EXIT
sudo mkdir -p "$CG"
echo "$MEM_LIMIT" | sudo tee "$CG/memory.max" >/dev/null
echo 0 | sudo tee "$CG/memory.swap.max" >/dev/null

log "measuring inside memory.max=$MEM_LIMIT: back-ends $BACKENDS, pools $POOLS"
"$BIN" "${COMMON[@]}" --reuse --cold --ops "$OPS" --backends "$BACKENDS" \
  --pool-bytes "$POOLS" $DIRECT_ONLY > "$OUT" 2> "${OUT%.csv}.log" &
PID=$!
echo $PID | sudo tee "$CG/cgroup.procs" >/dev/null
wait $PID; STATUS=$?
OOM=$(awk '/^oom_kill /{print $2}' "$CG/memory.events" 2>/dev/null)
PEAK=$(cat "$CG/memory.peak" 2>/dev/null)
if (( STATUS != 0 )); then
  echo "pool_bench exited $STATUS (oom_kill=$OOM); see ${OUT%.csv}.log"; exit $STATUS
fi

echo "memory.max=$MEM_LIMIT  peak=$PEAK  oom_kills=${OOM:-0}  dataset=$(du -sh "$DIR" | cut -f1)"
awk -F, 'NR==1{print "backend,direct_io,pool_MiB,ops_per_sec,p50_ms,p99_ms,p999_ms,hit_ratio,cached_MiB,rss_MiB,major_faults"; next}
  {printf "%s,%s,%d,%.1f,%.2f,%.2f,%.2f,%s,%d,%d,%s\n", $1,$2,$6/1048576,$12,$13/1e6,$14/1e6,$15/1e6,($16==""?"-":$16),$18/1048576,$19/1048576,$20}' "$OUT" | column -t -s,
log "raw CSV: $OUT"
