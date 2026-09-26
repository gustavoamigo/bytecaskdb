#!/usr/bin/env bash
# Sysbench OLTP comparison: ByteCaskDB vs InnoDB vs RocksDB (MyRocks)
#
# Spawns ephemeral local mariadbd instances (ByteCaskDB, InnoDB, and
# optionally RocksDB/MyRocks), runs sysbench workloads against each,
# and produces a side-by-side comparison.
#
# Prerequisites:
#   - Plugin .so built (run-unit-tests.sh or cmake)
#   - mariadbd + mariadb-install-db on PATH
#   - sysbench installed
#   - jemalloc (libjemalloc.so.2) — preloaded into every mariadbd so RSS reports
#     the engine, not memory glibc's arenas keep after a drop/re-prepare;
#     MARIADB_MALLOC=none runs on the system allocator instead
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh [--table-size=N] [--threads=LIST] [--time=S] [--warmup=S] [--engines=LIST] [--workloads=LIST] [--data-root=PATH]
#
#   --warmup: seconds of unmeasured load run before each measured run (default: 60).
#             Every cell starts from a freshly dropped and re-prepared table, and
#             InnoDB's first minute after a bulk load is spent flushing the dirty
#             pages and redo it left behind — a 60 s run that starts right away
#             measures that transient, not steady state. sysbench 1.0 has no
#             --warmup-time, so the warm-up is a separate run whose output is dropped.
#   --engines: comma-separated list of engines to benchmark (default: bytecaskdb,innodb,rocksdb)
#              e.g. --engines=bytecaskdb or --engines=bytecaskdb,innodb
#   --workloads: comma-separated list of sysbench workloads (default: common OLTP mix)
#                e.g. --workloads=oltp_insert
#   --reuse-data: skip Phase 1 and run against the instance directories a previous
#                 run left under --data-root, and keep them at exit. The table
#                 size must match what was loaded; a mutating workload leaves
#                 its changes behind for the next run.
#   --data-root: directory under which the ephemeral mariadbd instances create their
#                data/tmp/socket files (default: repository root). Point it at a
#                filesystem that supports native fdatasync/O_DIRECT when the repo
#                lives on a bind mount or overlay that doesn't, e.g. --data-root=/mnt/nvme
#
# Run structure:
#   1. Prepare  — per engine: initialise the data directory, start the server,
#                 load the sysbench table once, stop the server cleanly.
#   2. Measure  — per workload x thread count x engine: start the server, run
#                 an unmeasured warm-up, sample /proc/<pid>/io, run the
#                 measured workload, sample again, stop the server. Only the
#                 engine under test is running, so no other engine's background
#                 flushing or compaction competes for the disk.
#   3. Teardown — remove every instance directory.
#
# The table is loaded ONCE per engine, not once per cell. Workloads that mutate
# it (oltp_insert, oltp_write_only, oltp_read_write) leave it changed for
# whatever runs next, so results depend on the order workloads are listed in.
#
# The CSV gains read_mib/write_mib — block-layer bytes over the measured run,
# taken from the instance's cgroup io.stat — plus syscr/syscw, which count
# read()/write() on every descriptor, client sockets included, and so describe
# workload shape rather than disk traffic. Getting per-instance cgroup
# accounting needs mariadbd launched into its own systemd scope: either
# `systemd-run --user --scope` (a logind user session) or, on headless hosts
# without one, `sudo systemd-run --scope` (passwordless sudo required — see
# scope_available() in lib_common.sh). I/O columns are empty when neither is
# available.
# rss_mib/peak_rss_mib are the server's resident memory at the end of the
# measured run and its peak during it (the peak mark is reset after the
# warm-up).

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
TABLE_SIZE=5000000
THREADS="16"
DURATION=60
WARMUP=60
ENGINES="bytecaskdb,innodb,rocksdb"
# WORKLOADS="oltp_point_select oltp_read_only oltp_write_only oltp_insert oltp_read_write"
WORKLOADS="oltp_point_select oltp_read_write oltp_insert oltp_write_only"
CREATE_SECONDARY="on"
DATA_ROOT=""
REUSE_DATA="off"

#WORKLOADS="oltp_read_only:points_only oltp_read_only:ranges_only oltp_read_only:simple_range oltp_read_only:sum_range oltp_read_only:order_range oltp_read_only:distinct_range"

BYTECASKDB_PORT=3320
INNODB_PORT=3321
ROCKSDB_PORT=3322

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
for arg in "$@"; do
  case "$arg" in
    --table-size=*) TABLE_SIZE="${arg#*=}" ;;
    --threads=*)    THREADS="${arg#*=}" ;;
    --time=*)       DURATION="${arg#*=}" ;;
    --warmup=*)     WARMUP="${arg#*=}" ;;
    --engines=*)    ENGINES="${arg#*=}" ;;
    --workloads=*)  WORKLOADS="${arg#*=}" ;;
    --data-root=*)  DATA_ROOT="${arg#*=}" ;;
    --no-secondary-index) CREATE_SECONDARY="off" ;;
    --reuse-data)   REUSE_DATA="on" ;;
    --help|-h)
      echo "Usage: $0 [--table-size=N] [--threads=1,4,8] [--time=30] [--warmup=60] [--engines=bytecaskdb,innodb,rocksdb] [--workloads=oltp_insert] [--data-root=PATH] [--no-secondary-index] [--reuse-data]"
      exit 0
      ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

IFS=',' read -ra THREAD_LIST <<< "$THREADS"
IFS=',' read -ra ENGINE_LIST <<< "$ENGINES"
WORKLOADS="${WORKLOADS//,/ }"

# Helper to check if an engine is enabled
engine_enabled() { for e in "${ENGINE_LIST[@]}"; do [[ "$e" == "$1" ]] && return 0; done; return 1; }

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
RESULTS_CSV="$SCRIPT_DIR/sysbench_results.csv"

# Where the ephemeral mariadbd instances live. Defaults to the repo root; override
# with --data-root when that filesystem can't do native fdatasync (e.g. a bind mount).
DATA_ROOT="${DATA_ROOT:-$BYTECASK_ROOT}"
mkdir -p "$DATA_ROOT" || { echo "ERROR: cannot create --data-root=$DATA_ROOT"; exit 1; }
DATA_ROOT="$(cd "$DATA_ROOT" && pwd)"
BYTECASKDB_DIR="$DATA_ROOT/.mariadb_sysbench_bytecaskdb"
INNODB_DIR="$DATA_ROOT/.mariadb_sysbench_innodb"
ROCKSDB_DIR="$DATA_ROOT/.mariadb_sysbench_rocksdb"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
command -v sysbench >/dev/null 2>&1 || { echo "ERROR: sysbench not found"; exit 1; }
command -v mariadbd >/dev/null 2>&1 || { echo "ERROR: mariadbd not found"; exit 1; }

# shellcheck source=lib_common.sh
source "$SCRIPT_DIR/lib_common.sh"

# ---------------------------------------------------------------------------
# Build plugin in Release mode
# ---------------------------------------------------------------------------
if engine_enabled bytecaskdb; then
  build_bytecaskdb_plugin
fi

ROCKSDB_PLUGIN_DIR=""
if engine_enabled rocksdb; then
  ROCKSDB_PLUGIN_DIR="$(find_rocksdb_plugin_dir || true)"
  if [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
    echo "WARNING: ha_rocksdb.so not found — RocksDB benchmarks will be skipped"
    echo "  Install with: sudo dnf install MariaDB-rocksdb-engine (or equivalent)"
  fi
fi

# ---------------------------------------------------------------------------
# Engine metadata — one place that knows a name, so the phases below stay
# engine-agnostic instead of repeating a block per engine.
# ---------------------------------------------------------------------------
engine_dir() {
  case "$1" in
    bytecaskdb) echo "$BYTECASKDB_DIR" ;;
    innodb)     echo "$INNODB_DIR" ;;
    rocksdb)    echo "$ROCKSDB_DIR" ;;
  esac
}

engine_port() {
  case "$1" in
    bytecaskdb) echo "$BYTECASKDB_PORT" ;;
    innodb)     echo "$INNODB_PORT" ;;
    rocksdb)    echo "$ROCKSDB_PORT" ;;
  esac
}

engine_short() {
  case "$1" in
    bytecaskdb) echo "BC" ;;
    innodb)     echo "InnoDB" ;;
    rocksdb)    echo "RocksDB" ;;
  esac
}

engine_label() {
  case "$1" in
    bytecaskdb) echo "ByteCaskDB" ;;
    innodb)     echo "InnoDB" ;;
    rocksdb)    echo "RocksDB" ;;
  esac
}

# Enabled on the command line, and for RocksDB only when its plugin was found.
engine_active() {
  engine_enabled "$1" || return 1
  if [[ "$1" == rocksdb && -z "$ROCKSDB_PLUGIN_DIR" ]]; then return 1; fi
  return 0
}

ACTIVE_ENGINES=()
for e in bytecaskdb innodb rocksdb; do
  if engine_active "$e"; then ACTIVE_ENGINES+=("$e"); fi
done
if [[ ${#ACTIVE_ENGINES[@]} -eq 0 ]]; then
  echo "ERROR: no engines to benchmark"; exit 1
fi

start_engine() {
  local engine="$1"
  local dir; dir="$(engine_dir "$engine")"
  local extra=()
  case "$engine" in
    bytecaskdb)
      symlink_providers "$PLUGIN_DIR"
      extra=(--plugin-dir="$PLUGIN_DIR" --plugin-load-add=bytecaskdb=ha_bytecaskdb.so) ;;
    rocksdb)
      extra=(--plugin-load-add=rocksdb=ha_rocksdb.so --plugin-dir="$ROCKSDB_PLUGIN_DIR") ;;
    innodb) ;;
  esac
  start_mariadbd \
    "$dir/data" "$dir/mysql.sock" "$(engine_port "$engine")" \
    "$dir/mariadbd.pid" "$dir/error.log" "$SCRIPT_DIR/$engine.cnf" \
    "${extra[@]}"
}

stop_engine() {
  local dir; dir="$(engine_dir "$1")"
  stop_mariadbd "$dir/mariadbd.pid"
}

# ---------------------------------------------------------------------------
# Cleanup trap
# ---------------------------------------------------------------------------
cleanup() {
  echo ""
  echo "=== Cleaning up ==="
  local engine dir
  for engine in bytecaskdb innodb rocksdb; do
    dir="$(engine_dir "$engine")"
    stop_mariadbd "$dir/mariadbd.pid"
    if [[ "$REUSE_DATA" == "off" ]]; then remove_instance "$dir"; fi
  done
}
trap cleanup INT TERM

# ---------------------------------------------------------------------------
# Common sysbench args
# ---------------------------------------------------------------------------
common_args() {
  local port="$1" socket="$2" threads="$3"
  echo "$(sysbench_conn_args "$port" "$socket" "$threads" "$TABLE_SIZE") --report-interval=0"
}

# Splits "oltp_read_only:points_only" into a base workload and variant flags.
variant_flags() {
  case "${1##*:}" in
    points_only)    echo "--range-selects=off" ;;
    ranges_only)    echo "--point-selects=0" ;;
    simple_range)   echo "--point-selects=0 --sum-ranges=0 --order-ranges=0 --distinct-ranges=0" ;;
    sum_range)      echo "--point-selects=0 --simple-ranges=0 --order-ranges=0 --distinct-ranges=0" ;;
    order_range)    echo "--point-selects=0 --simple-ranges=0 --sum-ranges=0 --distinct-ranges=0" ;;
    distinct_range) echo "--point-selects=0 --simple-ranges=0 --sum-ranges=0 --order-ranges=0" ;;
    *)              echo "" ;;
  esac
}

# ---------------------------------------------------------------------------
# Phase 1 — load the table once per engine, then stop the server.
# ---------------------------------------------------------------------------
PREPARE_WORKLOAD="${WORKLOADS%% *}"
PREPARE_WORKLOAD="${PREPARE_WORKLOAD%%:*}"

prepare_engine() {
  local engine="$1"
  local dir; dir="$(engine_dir "$engine")"
  echo "--- $(engine_label "$engine"): loading $TABLE_SIZE rows ---"
  init_datadir "$dir/data"
  start_engine "$engine"
  local args
  args="$(sysbench_conn_args "$(engine_port "$engine")" "$dir/mysql.sock" \
          "${THREAD_LIST[0]}" "$TABLE_SIZE")"
  # shellcheck disable=SC2086
  if ! sysbench "$PREPARE_WORKLOAD" $args --mysql_storage_engine="$engine" \
        --create_secondary="$CREATE_SECONDARY" prepare >/dev/null 2>&1; then
    echo "  [FAILED] sysbench prepare for $engine" >&2
  fi
  stop_engine "$engine"
}

# ---------------------------------------------------------------------------
# Phase 2 — one measured cell: start, warm up, measure, stop.
# ---------------------------------------------------------------------------
run_bench() {
  local engine="$1" workload="$2" threads="$3"
  local dir; dir="$(engine_dir "$engine")"

  local base_workload="$workload" variant_args=""
  if [[ "$workload" == *:* ]]; then
    base_workload="${workload%%:*}"
    variant_args="$(variant_flags "$workload")"
  fi

  local args
  args="$(common_args "$(engine_port "$engine")" "$dir/mysql.sock" "$threads")"

  start_engine "$engine"

  # Unmeasured warm-up. The server was just started, so this also refills
  # whatever cache the engine keeps — InnoDB's buffer pool is cold otherwise.
  if (( WARMUP > 0 )); then
    # shellcheck disable=SC2086
    sysbench "$base_workload" $args $variant_args --time="$WARMUP" \
      --mysql-ignore-errors=1180,1213 run >/dev/null 2>&1 || true
  fi

  # Sample either side of the measured run only, so prepare and warm-up I/O
  # stay out of the numbers.
  local io_before io_after io_cols output eng_before eng_after eng_cols
  rss_reset "$dir/mariadbd.pid"
  io_before="$(io_sample "$dir/mariadbd.pid")"
  eng_before="$(engine_counters "$engine" "$dir/mysql.sock")"
  # shellcheck disable=SC2086
  output="$(sysbench "$base_workload" $args $variant_args --time="$DURATION" \
    --mysql-ignore-errors=1180,1213 run 2>&1)" || true
  # Engine counters must be read while the server is still up.
  eng_after="$(engine_counters "$engine" "$dir/mysql.sock")"
  io_after="$(io_sample "$dir/mariadbd.pid")"
  local rss_cols
  rss_cols="$(rss_sample "$dir/mariadbd.pid")"
  io_cols="$(io_delta "$io_before" "$io_after")"
  eng_cols="$(eng_delta "$eng_before" "$eng_after")"

  # Shutdown flush: InnoDB writes dirty pages here, caused by the run above.
  local dev_before dev_after flush_mib
  dev_before="$(dev_written_bytes)"
  stop_engine "$engine"
  dev_after="$(dev_written_bytes)"
  flush_mib="$(awk -v b=$((dev_after - dev_before)) \
    'BEGIN { printf "%.1f", (b > 0 ? b : 0) / 1048576 }')"

  # Extract metrics (sysbench 1.0 outputs only one percentile: 95th by default).
  # The "transactions:" / "queries:" / "ignored errors:" lines are all
  # "<label>:  <total count>  (<rate> per sec.)" — field $3 is the raw total
  # event count over the whole run, not a rate; field $4 is the "X per sec."
  # rate we actually want.
  local tps qps avg_lat p95 err
  tps="$(echo "$output" | grep "transactions:" | awk -F'[( ]+' '{print $4}')"
  qps="$(echo "$output" | grep "    queries:" | awk -F'[( ]+' '{print $4}')"
  avg_lat="$(echo "$output" | grep "avg:" | tail -1 | awk '{print $2}')"
  p95="$(echo "$output" | grep "95th percentile:" | awk '{print $NF}')"
  err="$(echo "$output" | grep "ignored errors:" | awk -F'[( ]+' '{print $4}')"

  if [[ -z "$tps" ]]; then
    echo "  [FAILED] sysbench output:" >&2
    echo "$output" | tail -20 >&2
  fi

  echo "$engine,$workload,$threads,$tps,$qps,$avg_lat,$p95,$err,$io_cols,$eng_cols,$flush_mib,$rss_cols"
}

# Echoes the collected CSV row for a cell, or nothing.
find_result() {
  local r
  for r in "${ALL_RESULTS[@]}"; do
    if [[ "$(cut -d, -f1 <<< "$r")" == "$1" &&
          "$(cut -d, -f2 <<< "$r")" == "$2" &&
          "$(cut -d, -f3 <<< "$r")" == "$3" ]]; then
      echo "$r"
      return 0
    fi
  done
  echo ""
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
echo ""
echo "=== Sysbench OLTP Benchmark ==="
echo "    Engines: ${ACTIVE_ENGINES[*]}"
echo "    Table size: $TABLE_SIZE rows | Warm-up: ${WARMUP}s | Duration: ${DURATION}s per run"
echo "    Threads: ${THREADS}"
echo "    Workloads: $WORKLOADS"
echo "    Data root: $DATA_ROOT"
if engine_enabled rocksdb && [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "    RocksDB: SKIPPED (plugin not found)"
fi
echo ""

if [[ "$REUSE_DATA" == "on" ]]; then
  echo "=== Phase 1: reusing existing data ==="
  for engine in "${ACTIVE_ENGINES[@]}"; do
    if [[ ! -d "$(engine_dir "$engine")/data/sbtest" ]]; then
      echo "ERROR: --reuse-data but no loaded table in $(engine_dir "$engine")" >&2
      exit 1
    fi
    # A server orphaned by an aborted run still holds the data directory.
    stop_engine "$engine"
  done
else
  echo "=== Phase 1: preparing data ==="
  for engine in "${ACTIVE_ENGINES[@]}"; do
    prepare_engine "$engine"
  done
fi
echo ""

echo "=== Phase 2: running workloads ==="
echo "engine,workload,threads,tps,qps,avg_lat_ms,p95_ms,err_per_s,read_mib,write_mib,syscr,syscw,eng_write_mib,eng_fsyncs,flush_mib,rss_mib,peak_rss_mib" > "$RESULTS_CSV"
declare -a ALL_RESULTS=()

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    echo "--- $workload | threads=$t ---"
    for engine in "${ACTIVE_ENGINES[@]}"; do
      printf '  %-12s' "$(engine_label "$engine"):"
      result="$(run_bench "$engine" "$workload" "$t")"
      echo "$result" >> "$RESULTS_CSV"
      ALL_RESULTS+=("$result")
      printf '%8s tps | read %8s MiB | write %8s MiB | rss %8s MiB\n' \
        "$(cut -d, -f4 <<< "$result")" \
        "$(cut -d, -f9 <<< "$result")" \
        "$(cut -d, -f10 <<< "$result")" \
        "$(cut -d, -f16 <<< "$result")"
    done
    echo ""
  done
done

# ---------------------------------------------------------------------------
# Throughput / latency table
# ---------------------------------------------------------------------------
# Width of an engine's first column: wide enough for its "<engine> <metric>"
# label, so header and rows line up whichever engines are enabled.
first_col_width() {
  local label="$(engine_short "$1") $2" min="$3"
  echo $(( ${#label} > min ? ${#label} : min ))
}

echo ""
header_fmt="%-22s %4s"
header_args=("Workload" "Thr")
for engine in "${ACTIVE_ENGINES[@]}"; do
  header_fmt+=" | %$(first_col_width "$engine" tps 10)s %10s %8s %8s"
  header_args+=("$(engine_short "$engine") tps" "qps" "avg" "p95")
done
# shellcheck disable=SC2059
printf "$header_fmt\n" "${header_args[@]}"

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    row_fmt="%-22s %4s"
    row_args=("$workload" "$t")
    for engine in "${ACTIVE_ENGINES[@]}"; do
      line="$(find_result "$engine" "$workload" "$t")"
      row_fmt+=" | %$(first_col_width "$engine" tps 10)s %10s %8s %8s"
      row_args+=("$(cut -d, -f4 <<< "$line")" "$(cut -d, -f5 <<< "$line")" \
                 "$(cut -d, -f6 <<< "$line")" "$(cut -d, -f7 <<< "$line")")
    done
    # shellcheck disable=SC2059
    printf "$row_fmt\n" "${row_args[@]}"
  done
done

# ---------------------------------------------------------------------------
# Write accounting.
#   wMiB   — block layer, during the measured run (cgroup io.stat, Linux only).
#   flMiB  — block layer, during server shutdown. InnoDB defers dirty-page
#            flushing, so a short run against a dataset that fits its buffer
#            pool writes redo only and settles the B-tree here. Ignoring this
#            column makes InnoDB look like it writes far less than it does.
#   engMiB — engine-reported, and what each engine counts differs enough that
#            it is a per-engine diagnostic, never a cross-engine ratio:
#              ByteCaskDB  committed appends only — not vacuum, hint files or
#                          zero-fill-ahead (~6x below the device figure).
#              RocksDB     the WAL/memtable path only — compaction is excluded
#                          (~70-100x below the device figure).
#              InnoDB      blank. MariaDB 10.11 leaves Innodb_data_written,
#                          Innodb_pages_written and
#                          Innodb_buffer_pool_pages_flushed all at zero, so only
#                          redo is visible; a figure there would understate it
#                          by ~99% (6 MiB against 518 MiB at the device).
#            Compare wMiB across engines. Use engMiB only to see how much of an
#            engine's own accounting reaches the disk.
# ---------------------------------------------------------------------------
echo ""
io_fmt="%-22s %4s"
io_args=("Workload" "Thr")
for engine in "${ACTIVE_ENGINES[@]}"; do
  io_fmt+=" | %$(first_col_width "$engine" rMiB 8)s %8s %8s %8s"
  io_args+=("$(engine_short "$engine") rMiB" "wMiB" "flMiB" "engMiB")
done
# shellcheck disable=SC2059
printf "$io_fmt\n" "${io_args[@]}"

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    row_fmt="%-22s %4s"
    row_args=("$workload" "$t")
    for engine in "${ACTIVE_ENGINES[@]}"; do
      line="$(find_result "$engine" "$workload" "$t")"
      row_fmt+=" | %$(first_col_width "$engine" rMiB 8)s %8s %8s %8s"
      row_args+=("$(cut -d, -f9 <<< "$line")" "$(cut -d, -f10 <<< "$line")" \
                 "$(cut -d, -f15 <<< "$line")" "$(cut -d, -f13 <<< "$line")")
    done
    # shellcheck disable=SC2059
    printf "$row_fmt\n" "${row_args[@]}"
  done
done

# ---------------------------------------------------------------------------
# Memory, from /proc/<pid>/status (Linux only; blank elsewhere).
#   RSS   — resident at the end of the measured run.
#   peak  — high-water mark during the measured run only: the mark is reset
#           after the warm-up, so loading and warm-up do not set it.
# RSS excludes the kernel page cache behind read()/pread(); see rss_sample in
# lib_common.sh for which engines that understates.
# ---------------------------------------------------------------------------
echo ""
mem_fmt="%-22s %4s"
mem_args=("Workload" "Thr")
for engine in "${ACTIVE_ENGINES[@]}"; do
  mem_fmt+=" | %$(first_col_width "$engine" "RSS MiB" 8)s %8s"
  mem_args+=("$(engine_short "$engine") RSS MiB" "peak")
done
# shellcheck disable=SC2059
printf "$mem_fmt\n" "${mem_args[@]}"

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    row_fmt="%-22s %4s"
    row_args=("$workload" "$t")
    for engine in "${ACTIVE_ENGINES[@]}"; do
      line="$(find_result "$engine" "$workload" "$t")"
      row_fmt+=" | %$(first_col_width "$engine" "RSS MiB" 8)s %8s"
      row_args+=("$(cut -d, -f16 <<< "$line")" "$(cut -d, -f17 <<< "$line")")
    done
    # shellcheck disable=SC2059
    printf "$row_fmt\n" "${row_args[@]}"
  done
done

echo ""
echo "Results saved to: $RESULTS_CSV"

cleanup
