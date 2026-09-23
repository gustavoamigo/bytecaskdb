#!/usr/bin/env bash
# Memory-pressure benchmark: ByteCaskDB (buffer pool, mmap, pread) and InnoDB,
# each run as one mariadbd inside a cgroup with memory.max (and optionally
# memory.swap.max) set, so the page cache, the key directory and the buffer
# pool all have to fit the same limit.
#
# Requirements: mariadbd, mariadb, sysbench and systemd-run on the PATH; the
# plugin built (this script builds it). No root: each server runs in a
# transient scope under your systemd user session (`systemd-run --user
# --scope -p MemoryMax=`), which needs cgroup v2 with the memory controller
# delegated to that session — the default on a systemd host, checked at
# start — and the page cache is dropped per file with posix_fadvise
# (DONTNEED) rather than /proc/sys/vm/drop_caches, which reaches exactly
# the files the run is about and nothing another engine's run has warmed.
# jemalloc (libjemalloc.so.2): every mariadbd runs under it, because memory
# glibc's arenas keep after a free counts against memory.max exactly like
# live data and would be reported as the engine's; MARIADB_MALLOC=none runs
# on the system allocator instead (see lib_common.sh).
#
# Usage:
#   ./run-memory-pressure.sh [--rows=N] [--mem-limit=BYTES] [--swap-limit=BYTES|max]
#       [--pool-bytes=BYTES] [--engines=LIST] [--workloads=LIST] [--threads=LIST]
#       [--warmup=S] [--time=S] [--start-timeout=S] [--data-root=DIR] [--out=FILE]
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
#   --start-timeout how long to wait for mariadbd to accept connections
#                (default 900 s). Recovery of the key directory takes seconds
#                with memory to spare and minutes once it is being swapped;
#                the time it took is recorded per run as startup_s.
#   --data-root  where the data directories live (default: this directory's
#                results/). The engine directories under it are wiped at the
#                start of every run: nothing from an earlier run is reused,
#                so a server an earlier run's OOM killer ended cannot hand
#                the next run its directory. Within a run the three
#                ByteCaskDB back-ends share one prepare, and only while the
#                previous cell's server shut down cleanly.
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
START_TIMEOUT=900
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_ROOT="$SCRIPT_DIR/results"
OUT=""
PORT=3322
SOCKET=/tmp/bytecaskdb-mempressure.sock   # short: mariadbd rejects paths over 107 bytes
CG=""   # the running server's cgroup path, set by start_db; empty with no limit
USER_SLICE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service"

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
    --start-timeout=*) START_TIMEOUT="${arg#*=}" ;;
    --data-root=*)  DATA_ROOT="${arg#*=}" ;;
    --out=*)        OUT="${arg#*=}" ;;
    --help|-h)      sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done
mkdir -p "$DATA_ROOT"
DATA_ROOT="$(cd "$DATA_ROOT" && pwd)"
OUT="${OUT:-$DATA_ROOT/memory_pressure_$(date +%Y%m%dT%H%M%S).csv}"
# Every run starts from nothing: only the engine directories, so earlier
# runs' CSVs beside them stay.
rm -rf "$DATA_ROOT/bytecaskdb" "$DATA_ROOT/innodb"

BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
# shellcheck source=../lib_common.sh
source "$SCRIPT_DIR/../lib_common.sh"

for tool in mariadbd mariadb sysbench systemd-run; do
  command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool not found"; exit 1; }
done
if (( MEM_LIMIT > 0 )) && ! grep -qw memory "$USER_SLICE/cgroup.subtree_control" 2>/dev/null; then
  echo "ERROR: the memory controller is not delegated to your systemd user session"
  echo "       ($USER_SLICE/cgroup.subtree_control does not list memory), so a"
  echo "       memory.max cannot be set without root. Check cgroup v2 is in use and"
  echo "       user@.service has Delegate= including memory (the systemd default)."
  exit 1
fi
if [[ $SWAP_LIMIT != 0 ]] && [[ -z "$(swapon --show --noheadings 2>/dev/null)" ]]; then
  echo "ERROR: --swap-limit=$SWAP_LIMIT but the host has no active swap device; the limit would do nothing"
  echo "       and anything over memory.max is an OOM kill. Enable swap (e.g. a swapfile) or use --swap-limit=0."
  exit 1
fi

log() { echo "[$(date +%H:%M:%S)] $*"; }

DB_PID=""
DB_ENGINE=""  # storage engine of the running server
# Engines prepared in this run whose last server shut down cleanly. A cell
# whose server was killed leaves the engine off this list, and the next
# cell re-prepares rather than open the directory the kill left.
PREPARED_CLEAN=""
start_db() {  # <storage engine: bytecaskdb|innodb> <backend> <pool bytes> <limit or 0> <base dir>
  local engine="$1" backend="$2" pool="$3" limit="$4" base="$5"
  local data="$base/data" cnf="$base/bench.cnf"
  mkdir -p "$data" "$base/tmp"
  DB_ENGINE="$engine"
  PREPARED_CLEAN="${PREPARED_CLEAN// $engine / }"
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
  evict_page_cache "$data"
  # The limit is a transient scope under the user session; systemd creates
  # the cgroup, moves the server into it and removes it when the server
  # exits, so nothing here needs root and nothing is left behind.
  local scope=()
  if (( limit > 0 )); then
    local swap_max=$SWAP_LIMIT
    [[ $swap_max == max ]] && swap_max=infinity
    scope=(systemd-run --user --scope --quiet -p "MemoryMax=$limit" -p "MemorySwapMax=$swap_max" --)
  fi
  local preload=()
  local malloc_lib
  malloc_lib="$(jemalloc_library)"
  if [[ "$malloc_lib" == "none" ]]; then
    :
  elif [[ -n "$malloc_lib" ]]; then
    preload=(env "LD_PRELOAD=$malloc_lib${LD_PRELOAD:+:$LD_PRELOAD}")
  else
    echo "WARNING: libjemalloc.so.2 not found; mariadbd ($engine/$backend) runs on the" \
         "system allocator and its RSS will include memory glibc has not returned" >&2
  fi
  "${scope[@]}" "${preload[@]}" mariadbd --defaults-extra-file="$cnf" --datadir="$data" --socket="$SOCKET" --port=$PORT \
    --pid-file="$base/mariadbd.pid" --skip-grant-tables --tmpdir="$base/tmp" \
    --log-error="$base/error.log" "${extra[@]}" &
  DB_PID=$!
  CG=""
  if (( limit > 0 )); then
    # systemd-run --scope execs the server, so $! is mariadbd and its cgroup
    # is the scope's. memory.current, memory.stat and memory.events are read
    # from there for the CSV. A server already dead here is left to the
    # readiness loop below, which says why.
    sleep 1
    if kill -0 "$DB_PID" 2>/dev/null; then
      CG="/sys/fs/cgroup$(cut -d: -f3 "/proc/$DB_PID/cgroup" 2>/dev/null)"
      if [[ ! -f $CG/memory.max ]] || (( $(cat "$CG/memory.max" 2>/dev/null || echo 0) != limit )); then
        echo "ERROR: mariadbd ($engine/$backend) is not under a memory.max=$limit scope (cgroup: ${CG#/sys/fs/cgroup})"
        kill "$DB_PID" 2>/dev/null
        exit 1
      fi
    fi
  fi
  local tries=0
  STARTUP_S=0
  while ! mariadb --socket="$SOCKET" -u root -e "SELECT 1" >/dev/null 2>&1; do
    sleep 1; tries=$((tries + 1)); STARTUP_S=$tries
    if (( tries > START_TIMEOUT )) || ! kill -0 "$DB_PID" 2>/dev/null; then
      echo "ERROR: mariadbd ($engine/$backend) did not start; see $base/error.log"
      if (( $(oom_kills) > 0 )); then
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
  if mariadb --socket="$SOCKET" -u root -e "SHUTDOWN" >/dev/null 2>&1 && wait "$DB_PID" 2>/dev/null; then
    PREPARED_CLEAN="$PREPARED_CLEAN $DB_ENGINE "
  else
    kill "$DB_PID" 2>/dev/null
    wait "$DB_PID" 2>/dev/null
    log "$DB_ENGINE: server did not shut down cleanly; the next cell re-prepares its data"
  fi
  DB_PID=""
  DB_ENGINE=""
  CG=""
  rm -f "$SOCKET"
}
cleanup() { [[ -n $DB_PID ]] && stop_db; }
trap cleanup EXIT INT TERM

# OOM kills of the running server. From the scope's memory.events while the
# scope exists; once the server is dead the scope is gone with it, and the
# kill is in the user journal instead, logged by systemd against the scope,
# whose name carries the server's pid.
oom_kills() {
  local n=""
  [[ -n $CG ]] && n="$(awk '/^oom_kill /{print $2}' "$CG/memory.events" 2>/dev/null)"
  if [[ -z $n ]] && [[ -n $DB_PID ]]; then
    n="$(journalctl --user --since=-1h -o cat 2>/dev/null \
         | grep -c "run-p${DB_PID}-.*killed by the OOM killer")"
  fi
  echo "${n:-0}"
}

# Drops every file under dir from the page cache with posix_fadvise(DONTNEED),
# which needs no privilege for clean pages — and the data files are all
# fdatasync'd and closed by the time this runs. sync first so nothing is
# dirty, then dd's iflag=nocache with count=0 issues the advice and reads
# nothing.
evict_page_cache() {  # <dir>
  sync
  find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \; 2>/dev/null
}

sysbench_args() {  # <threads>
  echo "--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$PORT --mysql-socket=$SOCKET --mysql-user=root --mysql-db=sbtest --tables=1 --table_size=$ROWS --threads=$1 --report-interval=0 --mysql-ignore-errors=1180,1213"
}

cg_stat() {  # <field in memory.stat>
  awk -v f="$1" '$1 == f {print $2}' "$CG/memory.stat" 2>/dev/null
}

engine_stat() {  # <counter name without the bytecask. prefix>
  mariadb --socket="$SOCKET" -u root -N -e "SHOW ENGINE BYTECASKDB STATUS" 2>/dev/null \
    | tr '\n' ' ' | grep -o "bytecask\.$1: [0-9]*" | awk '{print $2}'
}

prepare() {  # <storage engine> <base dir>
  local engine="$1" base="$2"
  if [[ $PREPARED_CLEAN == *" $engine "* ]]; then
    log "$engine: reusing this run's prepared data in $base ($ROWS rows)"
    return
  fi
  rm -rf "$base"
  log "$engine: preparing $ROWS rows in $base"
  # Both engines load through sysbench's own prepare, as run-sysbench.sh
  # does, so their tables are identical: ids 1..ROWS with no gaps, which is
  # what sysbench's id draws assume. ByteCaskDB loads unpooled and unlimited;
  # the back-end and the limit only matter for the measured runs.
  if [[ $engine == bytecaskdb ]]; then
    start_db bytecaskdb pread 0 0 "$base"
  else
    start_db innodb innodb $((4 * 1024 * 1024 * 1024)) 0 "$base"
  fi
  sysbench oltp_read_write $(sysbench_args 4) --mysql_storage_engine="$engine" prepare >/dev/null || {
    echo "ERROR: sysbench prepare failed for $engine; see $base/error.log"
    stop_db
    exit 1
  }
  if [[ $engine == bytecaskdb ]]; then
    # sysbench adds the secondary index after the load, with a copying ALTER,
    # so ByteCaskDB's files end the prepare holding the pre-copy table as dead
    # data (~2.4 GB at 10 M rows). Reclaim it now, or the background vacuum
    # does it during the first measured cell. Vacuum reclaims one file per
    # pass and sleeps its idle interval after a pass with nothing to do; that
    # interval is shortened for this server only, so "reclaimed has not moved
    # for five seconds" means there is nothing left.
    mariadb --socket="$SOCKET" -u root -e "SET GLOBAL bytecaskdb_vacuum_idle_interval_ms = 200"
    local prev=-1 cur still=0
    while (( still < 5 )); do
      sleep 1
      cur=$(engine_stat vacuum_bytes_reclaimed)
      if [[ $cur == "$prev" ]]; then still=$((still + 1)); else still=0; fi
      prev=$cur
    done
    log "$engine: vacuum reclaimed $(( ${cur:-0} / 1048576 )) MiB after the load"
  fi
  stop_db
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
  log "$label: accepting connections after ${STARTUP_S}s"
  local wl t out tps avg p95 h0 m0 h1 m1 hit rss cur oom
  for wl in ${WORKLOADS//,/ }; do
    for t in ${THREADS//,/ }; do
      log "  $label $wl threads=$t (warm-up ${WARMUP}s, measure ${DURATION}s)"
      sysbench "$wl" $(sysbench_args "$t") --time="$WARMUP" run >/dev/null 2>&1
      h0=$(engine_stat pool_hits); m0=$(engine_stat pool_misses)
      # Major faults are what swap actually costs: each one is a page read
      # back from disk in the middle of a query. Sampled around the measured
      # run only, so the warm-up's faults are not counted.
      f0=$(cg_stat pgmajfault)
      out="$(sysbench "$wl" $(sysbench_args "$t") --time="$DURATION" run 2>&1)"
      tps="$(echo "$out" | grep "transactions:" | awk -F'[( ]+' '{print $4}')"
      avg="$(echo "$out" | grep "avg:" | tail -1 | awk '{print $2}')"
      p95="$(echo "$out" | grep "95th percentile:" | awk '{print $NF}')"
      h1=$(engine_stat pool_hits); m1=$(engine_stat pool_misses)
      majf=$(( $(cg_stat pgmajfault) - ${f0:-0} ))
      hit=""
      if [[ -n $h1 && -n $m1 ]] && (( h1 - h0 + m1 - m0 > 0 )); then
        hit="$(echo "scale=4; ($h1 - $h0) / ($h1 - $h0 + $m1 - $m0)" | bc)"
      fi
      rss="$(awk '/VmRSS/{print $2 * 1024}' "/proc/$DB_PID/status" 2>/dev/null)"
      cur="$(cat "$CG/memory.current" 2>/dev/null)"
      swp="$(cat "$CG/memory.swap.current" 2>/dev/null)"
      oom="$(oom_kills)"
      if [[ -z $tps ]]; then
        log "  [FAILED] $label $wl threads=$t"; echo "$out" | tail -5
        tps=0; avg=0; p95=0
      fi
      echo "$label,$wl,$t,$ROWS,$MEM_LIMIT,$SWAP_LIMIT,$pool,${tps},${avg},${p95},${hit},${rss:-0},${cur:-0},${swp:-0},${majf:-0},${oom:-0},$STARTUP_S" | tee -a "$OUT"
    done
  done
  stop_db
}

build_bytecaskdb_plugin >/dev/null
symlink_providers "$PLUGIN_DIR"

echo "engine,workload,threads,rows,mem_limit_bytes,swap_limit,pool_bytes,tps,avg_ms,p95_ms,pool_hit_ratio,rss_bytes,cgroup_memory_bytes,cgroup_swap_bytes,major_faults,oom_kills,startup_s" > "$OUT"
for e in ${ENGINES//,/ }; do run_engine "$e"; done

echo
log "=== Results (memory.max=$MEM_LIMIT, memory.swap.max=$SWAP_LIMIT, $ROWS rows) ==="
printf "%-17s %-18s %4s %10s %8s %8s %6s %7s %7s %9s %4s %6s\n" engine workload thr tps "avg ms" "p95 ms" "hit" "RSS MB" "swp MB" "majf/tx" OOM "start"
tail -n +2 "$OUT" | awk -F, -v dur="$DURATION" '{printf "%-17s %-18s %4s %10.0f %8s %8s %6s %7d %7d %9.2f %4s %5ss\n", $1, $2, $3, $8, $9, $10, ($11 == "" ? "-" : substr($11, 1, 5)), $12 / 1048576, $14 / 1048576, ($8 > 0 ? $15 / ($8 * dur) : 0), $16, $17}'
log "Results saved to: $OUT"
