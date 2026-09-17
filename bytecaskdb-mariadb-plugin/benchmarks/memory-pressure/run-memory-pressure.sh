#!/usr/bin/env bash
# Memory-pressure benchmark: ByteCaskDB (buffer pool, mmap, pread) and InnoDB,
# each run as one mariadbd inside a cgroup with memory.max (and optionally
# memory.swap.max) set, so the page cache, the key directory and the buffer
# pool all have to fit the same limit.
#
# Requirements: mariadbd, mariadb and sysbench on the PATH; the plugin built
# (this script builds it); passwordless sudo, for the cgroup and for dropping
# the page cache between runs. cgroup v2 with the memory controller enabled
# at the root (`cat /sys/fs/cgroup/cgroup.subtree_control` lists `memory`).
#
# Usage:
#   ./run-memory-pressure.sh [--rows=N] [--mem-limit=BYTES] [--swap-limit=BYTES|max]
#       [--pool-bytes=BYTES] [--engines=LIST] [--workloads=LIST] [--threads=LIST]
#       [--warmup=S] [--time=S] [--data-root=DIR] [--fresh] [--out=FILE]
#
#   --engines    default bytecaskdb-pool,bytecaskdb-mmap,bytecaskdb-pread,innodb
#   --rows       default 10 M: ~3.2 GB of ByteCaskDB data files plus ~1.1 GB of
#                key directory for 20 M keys. Below ~6 M rows everything fits the
#                default limit and nothing is under pressure.
#   --mem-limit  default 2560 MiB. The key directory is anonymous memory the
#                kernel cannot reclaim without swap, so the limit has to hold
#                it plus the pool plus ~300 MB of server; the page cache gets
#                what is left, ~1.2 GB for 3.2 GB of files at the defaults.
#   --swap-limit the cgroup's memory.swap.max: 0 (default) means anonymous
#                memory such as the key directory can never be swapped, so
#                exceeding the limit is an OOM kill; a byte count or `max`
#                lets the kernel swap it instead, which is the slower failure.
#                Only matters on a host that has swap configured.
#   --pool-bytes default 512 MiB for bytecaskdb-pool; InnoDB gets 1.5 GiB.
#   --data-root  where the data directories live (default: this directory's
#                results/). Prepared data is reused across runs and across the
#                three ByteCaskDB back-ends; --fresh wipes it first.
#
# Output: one CSV row per engine × workload × thread count with sysbench's
# transactions per second, average and p95 latency, the pool's hit ratio,
# the server's RSS, the cgroup's memory.current and its OOM-kill count.
set -uo pipefail

ROWS=10000000
MEM_LIMIT=$((2560 * 1024 * 1024))
SWAP_LIMIT=0
POOL_BYTES=$((512 * 1024 * 1024))
INNODB_POOL_BYTES=$((1536 * 1024 * 1024))
ENGINES="bytecaskdb-pool,bytecaskdb-mmap,bytecaskdb-pread,innodb"
WORKLOADS="oltp_point_select,oltp_read_only,oltp_write_only,oltp_read_write"
THREADS="1,8,16"
WARMUP=30
DURATION=60
FRESH=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_ROOT="$SCRIPT_DIR/results"
OUT=""
PORT=3322
SOCKET=/tmp/bytecaskdb-mempressure.sock   # short: mariadbd rejects paths over 107 bytes
CG=/sys/fs/cgroup/bytecaskdb-mempressure

for arg in "$@"; do
  case "$arg" in
    --rows=*)       ROWS="${arg#*=}" ;;
    --mem-limit=*)  MEM_LIMIT="${arg#*=}" ;;
    --swap-limit=*) SWAP_LIMIT="${arg#*=}" ;;
    --pool-bytes=*) POOL_BYTES="${arg#*=}" ;;
    --engines=*)    ENGINES="${arg#*=}" ;;
    --workloads=*)  WORKLOADS="${arg#*=}" ;;
    --threads=*)    THREADS="${arg#*=}" ;;
    --warmup=*)     WARMUP="${arg#*=}" ;;
    --time=*)       DURATION="${arg#*=}" ;;
    --data-root=*)  DATA_ROOT="${arg#*=}" ;;
    --out=*)        OUT="${arg#*=}" ;;
    --fresh)        FRESH=1 ;;
    --help|-h)      sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done
mkdir -p "$DATA_ROOT"
DATA_ROOT="$(cd "$DATA_ROOT" && pwd)"
OUT="${OUT:-$DATA_ROOT/memory_pressure_$(date +%Y%m%dT%H%M%S).csv}"

BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
# shellcheck source=../lib_common.sh
source "$SCRIPT_DIR/../lib_common.sh"

for tool in mariadbd mariadb sysbench sudo; do
  command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool not found"; exit 1; }
done
sudo -n true 2>/dev/null || { echo "ERROR: passwordless sudo is required (cgroup, drop_caches)"; exit 1; }
grep -qw memory /sys/fs/cgroup/cgroup.subtree_control || { echo "ERROR: cgroup v2 memory controller not enabled at the root"; exit 1; }
if [[ $SWAP_LIMIT != 0 ]] && [[ -z "$(swapon --show --noheadings 2>/dev/null)" ]]; then
  echo "ERROR: --swap-limit=$SWAP_LIMIT but the host has no active swap device; the limit would do nothing"
  echo "       and anything over memory.max is an OOM kill. Enable swap (e.g. a swapfile) or use --swap-limit=0."
  exit 1
fi

log() { echo "[$(date +%H:%M:%S)] $*"; }

DB_PID=""
start_db() {  # <storage engine: bytecaskdb|innodb> <backend> <pool bytes> <limit or 0> <base dir>
  local engine="$1" backend="$2" pool="$3" limit="$4" base="$5"
  local data="$base/data" cnf="$base/bench.cnf"
  mkdir -p "$data" "$base/tmp"
  if [[ ! -d $data/mysql ]]; then
    mariadb-install-db --datadir="$data" --auth-root-authentication-method=normal >/dev/null 2>&1
  fi
  local extra=()
  if [[ $engine == bytecaskdb ]]; then
    cat > "$cnf" <<CNF
[mariadbd]
default_storage_engine = bytecaskdb
innodb_buffer_pool_size = 8M
performance_schema = OFF
skip-log-bin
table_open_cache = 200
thread_cache_size = 64
max_connections = 128
sql_mode = STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER
bytecaskdb_io_backend = $backend
bytecaskdb_buffer_pool_size = $pool
bytecaskdb_max_file_bytes = 67108864
bytecaskdb_verify_checksums = OFF
bytecaskdb_vacuum_fragmentation_threshold = 0.9
CNF
    extra=(--plugin-dir="$PLUGIN_DIR" --plugin-load-add=bytecaskdb=ha_bytecaskdb.so)
  else
    cat > "$cnf" <<CNF
[mariadbd]
innodb_buffer_pool_size = $pool
innodb_flush_method = O_DIRECT
innodb_flush_log_at_trx_commit = 1
innodb_log_file_size = 256M
innodb_io_capacity = 2000
innodb_io_capacity_max = 4000
performance_schema = OFF
skip-log-bin
table_open_cache = 200
thread_cache_size = 64
max_connections = 128
CNF
  fi
  # Every run starts with the same cold page cache: an earlier back-end must
  # not hand the next one a warm one.
  sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
  if (( limit > 0 )); then
    sudo mkdir -p "$CG"
    echo "$limit" | sudo tee "$CG/memory.max" >/dev/null
    echo "$SWAP_LIMIT" | sudo tee "$CG/memory.swap.max" >/dev/null
  fi
  mariadbd --defaults-extra-file="$cnf" --datadir="$data" --socket="$SOCKET" --port=$PORT \
    --pid-file="$base/mariadbd.pid" --skip-grant-tables --tmpdir="$base/tmp" \
    --log-error="$base/error.log" "${extra[@]}" &
  DB_PID=$!
  if (( limit > 0 )); then
    echo $DB_PID | sudo tee "$CG/cgroup.procs" >/dev/null
  fi
  local tries=0
  while ! mariadb --socket="$SOCKET" -u root -e "SELECT 1" >/dev/null 2>&1; do
    sleep 1; tries=$((tries + 1))
    if (( tries > 120 )) || ! kill -0 "$DB_PID" 2>/dev/null; then
      echo "ERROR: mariadbd ($engine/$backend) did not start; see $base/error.log"
      if [[ -f $CG/memory.events ]] && (( $(awk '/^oom_kill /{print $2}' "$CG/memory.events") > 0 )); then
        echo "       The cgroup OOM killer ended it: memory.max=$limit is below what recovery of the"
        echo "       key directory needs (anonymous memory; ~50 bytes per key, and 2 keys per row here)."
      fi
      tail -5 "$base/error.log"
      exit 1
    fi
  done
  mariadb --socket="$SOCKET" -u root -e "CREATE DATABASE IF NOT EXISTS sbtest"
}

stop_db() {
  mariadb --socket="$SOCKET" -u root -e "SHUTDOWN" >/dev/null 2>&1 || kill "$DB_PID" 2>/dev/null
  wait "$DB_PID" 2>/dev/null
  DB_PID=""
  [[ -d $CG ]] && sudo rmdir "$CG" 2>/dev/null
  rm -f "$SOCKET"
}
cleanup() { [[ -n $DB_PID ]] && stop_db; }
trap cleanup EXIT INT TERM

sysbench_args() {  # <threads>
  echo "--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$PORT --mysql-socket=$SOCKET --mysql-user=root --mysql-db=sbtest --tables=1 --table_size=$ROWS --threads=$1 --report-interval=0 --mysql-ignore-errors=1180,1213"
}

engine_stat() {  # <counter name without the bytecask. prefix>
  mariadb --socket="$SOCKET" -u root -N -e "SHOW ENGINE BYTECASKDB STATUS" 2>/dev/null \
    | tr '\n' ' ' | grep -o "bytecask\.$1: [0-9]*" | awk '{print $2}'
}

prepare() {  # <storage engine> <base dir>
  local engine="$1" base="$2"
  if [[ -f $base/prepared && -d $base/data/sbtest && $FRESH == 0 ]] && (( $(cat "$base/prepared") == ROWS )); then
    log "$engine: reusing prepared data in $base ($ROWS rows)"
    return
  fi
  rm -rf "$base"
  log "$engine: preparing $ROWS rows in $base"
  if [[ $engine == bytecaskdb ]]; then
    start_db bytecaskdb pread 0 0 "$base"
    # Table with the secondary index up front and batched inserts. sysbench's
    # own prepare adds the index afterwards with a copy-ALTER in one giant
    # transaction, which is not what a memory-pressure run should be measuring.
    mariadb --socket="$SOCKET" -u root sbtest -e "
      CREATE TABLE sbtest1 (
        id INT NOT NULL AUTO_INCREMENT, k INT NOT NULL DEFAULT 0,
        c CHAR(120) NOT NULL DEFAULT '', pad CHAR(60) NOT NULL DEFAULT '',
        PRIMARY KEY (id), KEY k_1 (k)) ENGINE=bytecaskdb;"
    local done_rows=0 batch=50000 n
    while (( done_rows < ROWS )); do
      n=$(( ROWS - done_rows < batch ? ROWS - done_rows : batch ))
      mariadb --socket="$SOCKET" -u root sbtest -e "
        INSERT INTO sbtest1 (k, c, pad)
        SELECT FLOOR(RAND() * $ROWS), LPAD(FLOOR(RAND() * 1e18), 120, '0'),
               LPAD(FLOOR(RAND() * 1e18), 60, '0')
        FROM seq_$((done_rows + 1))_to_$((done_rows + n));"
      done_rows=$((done_rows + n))
      (( done_rows % 1000000 == 0 )) && log "  $done_rows / $ROWS rows"
    done
  else
    start_db innodb innodb $((4 * 1024 * 1024 * 1024)) 0 "$base"
    sysbench oltp_read_write $(sysbench_args 4) --mysql_storage_engine=innodb prepare >/dev/null
  fi
  stop_db
  echo "$ROWS" > "$base/prepared"
  log "$engine: data size $(du -sh "$base/data" | cut -f1)"
}

run_engine() {  # <engine label>
  local label="$1" engine backend pool base
  case "$label" in
    bytecaskdb-pool)  engine=bytecaskdb; backend=buffer_pool; pool=$POOL_BYTES ;;
    bytecaskdb-mmap)  engine=bytecaskdb; backend=mmap;        pool=0 ;;
    bytecaskdb-pread) engine=bytecaskdb; backend=pread;       pool=0 ;;
    innodb)           engine=innodb;     backend=innodb;      pool=$INNODB_POOL_BYTES ;;
    *) echo "Unknown engine: $label"; exit 1 ;;
  esac
  base="$DATA_ROOT/$engine"
  prepare "$engine" "$base"
  log "$label: starting under memory.max=$MEM_LIMIT"
  start_db "$engine" "$backend" "$pool" "$MEM_LIMIT" "$base"
  local wl t out tps avg p95 h0 m0 h1 m1 hit rss cur oom
  for wl in ${WORKLOADS//,/ }; do
    for t in ${THREADS//,/ }; do
      log "  $label $wl threads=$t (warm-up ${WARMUP}s, measure ${DURATION}s)"
      sysbench "$wl" $(sysbench_args "$t") --time="$WARMUP" run >/dev/null 2>&1
      h0=$(engine_stat pool_hits); m0=$(engine_stat pool_misses)
      out="$(sysbench "$wl" $(sysbench_args "$t") --time="$DURATION" run 2>&1)"
      tps="$(echo "$out" | grep "transactions:" | awk -F'[( ]+' '{print $4}')"
      avg="$(echo "$out" | grep "avg:" | tail -1 | awk '{print $2}')"
      p95="$(echo "$out" | grep "95th percentile:" | awk '{print $NF}')"
      h1=$(engine_stat pool_hits); m1=$(engine_stat pool_misses)
      hit=""
      if [[ -n $h1 && -n $m1 ]] && (( h1 - h0 + m1 - m0 > 0 )); then
        hit="$(echo "scale=4; ($h1 - $h0) / ($h1 - $h0 + $m1 - $m0)" | bc)"
      fi
      rss="$(awk '/VmRSS/{print $2 * 1024}' "/proc/$DB_PID/status" 2>/dev/null)"
      cur="$(cat "$CG/memory.current" 2>/dev/null)"
      swp="$(cat "$CG/memory.swap.current" 2>/dev/null)"
      oom="$(awk '/^oom_kill /{print $2}' "$CG/memory.events" 2>/dev/null)"
      if [[ -z $tps ]]; then
        log "  [FAILED] $label $wl threads=$t"; echo "$out" | tail -5
        tps=0; avg=0; p95=0
      fi
      echo "$label,$wl,$t,$ROWS,$MEM_LIMIT,$SWAP_LIMIT,$pool,${tps},${avg},${p95},${hit},${rss:-0},${cur:-0},${swp:-0},${oom:-0}" | tee -a "$OUT"
    done
  done
  stop_db
}

build_bytecaskdb_plugin >/dev/null
symlink_providers "$PLUGIN_DIR"

echo "engine,workload,threads,rows,mem_limit_bytes,swap_limit,pool_bytes,tps,avg_ms,p95_ms,pool_hit_ratio,rss_bytes,cgroup_memory_bytes,cgroup_swap_bytes,oom_kills" > "$OUT"
for e in ${ENGINES//,/ }; do run_engine "$e"; done

echo
log "=== Results (memory.max=$MEM_LIMIT, memory.swap.max=$SWAP_LIMIT, $ROWS rows) ==="
printf "%-17s %-18s %4s %10s %8s %8s %6s %7s %7s %5s\n" engine workload thr tps "avg ms" "p95 ms" "hit" "RSS MB" "swp MB" OOM
tail -n +2 "$OUT" | awk -F, '{printf "%-17s %-18s %4s %10.0f %8s %8s %6s %7d %7d %5s\n", $1, $2, $3, $8, $9, $10, ($11 == "" ? "-" : substr($11, 1, 5)), $12 / 1048576, $14 / 1048576, $15}'
log "Results saved to: $OUT"
