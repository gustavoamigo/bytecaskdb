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
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/tests/run-sysbench.sh [--table-size=N] [--threads=LIST] [--time=S]

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
TABLE_SIZE=1000000
THREADS="1,2,4,8"
DURATION=30
WORKLOADS="oltp_point_select oltp_read_only oltp_write_only oltp_insert oltp_read_write"
#WORKLOADS="oltp_point_select"

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
    --help|-h)
      echo "Usage: $0 [--table-size=N] [--threads=1,4,8] [--time=30]"
      exit 0
      ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

IFS=',' read -ra THREAD_LIST <<< "$THREADS"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
RESULTS_CSV="$SCRIPT_DIR/sysbench_results.csv"

BYTECASKDB_DIR="$BYTECASK_ROOT/.mariadb_sysbench_bytecaskdb"
INNODB_DIR="$BYTECASK_ROOT/.mariadb_sysbench_innodb"
ROCKSDB_DIR="$BYTECASK_ROOT/.mariadb_sysbench_rocksdb"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
command -v sysbench >/dev/null 2>&1 || { echo "ERROR: sysbench not found"; exit 1; }
command -v mariadbd >/dev/null 2>&1 || { echo "ERROR: mariadbd not found"; exit 1; }

if [[ ! -f "$PLUGIN_DIR/ha_bytecaskdb.so" ]]; then
  echo "ERROR: $PLUGIN_DIR/ha_bytecaskdb.so not found — build the plugin first"
  exit 1
fi

ROCKSDB_PLUGIN_DIR=""
for system_dir in /usr/lib64/mariadb/plugin /usr/lib/mariadb/plugin; do
  if [[ -f "$system_dir/ha_rocksdb.so" ]]; then
    ROCKSDB_PLUGIN_DIR="$system_dir"
    break
  fi
done
if [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "WARNING: ha_rocksdb.so not found — RocksDB benchmarks will be skipped"
  echo "  Install with: sudo dnf install MariaDB-rocksdb-engine (or equivalent)"
fi

# ---------------------------------------------------------------------------
# Symlink compression provider plugins (same as conftest.py)
# ---------------------------------------------------------------------------
symlink_providers() {
  local plugin_dir="$1"
  for system_dir in /usr/lib64/mariadb/plugin /usr/lib/mariadb/plugin; do
    if [[ -d "$system_dir" ]]; then
      for f in "$system_dir"/provider_*; do
        [[ -e "$f" ]] || continue
        local name; name="$(basename "$f")"
        [[ -e "$plugin_dir/$name" ]] || ln -sf "$f" "$plugin_dir/$name" 2>/dev/null || true
      done
      break
    fi
  done
}

# ---------------------------------------------------------------------------
# Start / stop mariadbd
# ---------------------------------------------------------------------------
start_mariadbd() {
  local data_dir="$1"
  local socket="$2"
  local port="$3"
  local pid_file="$4"
  local log_file="$5"
  shift 5
  local extra_args=("$@")

  rm -rf "$data_dir"
  mkdir -p "$data_dir" "$(dirname "$data_dir")/tmp"

  mariadb-install-db --datadir="$data_dir" --auth-root-authentication-method=normal \
    >/dev/null 2>&1

  mariadbd \
    --datadir="$data_dir" \
    --socket="$socket" \
    --port="$port" \
    --pid-file="$pid_file" \
    --skip-grant-tables \
    --skip-log-bin \
    --performance-schema=OFF \
    --tmpdir="$(dirname "$data_dir")/tmp" \
    --log-error="$log_file" \
    "${extra_args[@]}" &

  # Wait for readiness
  local tries=0
  while ! mariadb --socket="$socket" -u root -e "SELECT 1" >/dev/null 2>&1; do
    sleep 1
    tries=$((tries + 1))
    if [[ $tries -ge 30 ]]; then
      echo "ERROR: mariadbd on port $port did not start within 30s"
      echo "Log: $log_file"
      cat "$log_file" | tail -20
      exit 1
    fi
  done

  # Create the sbtest database
  mariadb --socket="$socket" -u root -e "CREATE DATABASE IF NOT EXISTS sbtest"
}

stop_mariadbd() {
  local pid_file="$1"
  local base_dir="$2"
  if [[ -f "$pid_file" ]]; then
    local pid
    pid="$(cat "$pid_file" 2>/dev/null)" || true
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
      sleep 2
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
  rm -rf "$base_dir"
}

# ---------------------------------------------------------------------------
# Cleanup trap
# ---------------------------------------------------------------------------
cleanup() {
  echo ""
  echo "=== Cleaning up ==="
  stop_mariadbd "$BYTECASKDB_DIR/mariadbd.pid" "$BYTECASKDB_DIR"
  stop_mariadbd "$INNODB_DIR/mariadbd.pid" "$INNODB_DIR"
  stop_mariadbd "$ROCKSDB_DIR/mariadbd.pid" "$ROCKSDB_DIR"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Start instances
# ---------------------------------------------------------------------------
echo "=== Starting ByteCaskDB MariaDB instance (port $BYTECASKDB_PORT) ==="
symlink_providers "$PLUGIN_DIR"
start_mariadbd \
  "$BYTECASKDB_DIR/data" \
  "$BYTECASKDB_DIR/mysql.sock" \
  "$BYTECASKDB_PORT" \
  "$BYTECASKDB_DIR/mariadbd.pid" \
  "$BYTECASKDB_DIR/error.log" \
  --plugin-dir="$PLUGIN_DIR" \
  --plugin-load-add=bytecaskdb=ha_bytecaskdb.so

echo "=== Starting InnoDB MariaDB instance (port $INNODB_PORT) ==="
start_mariadbd \
  "$INNODB_DIR/data" \
  "$INNODB_DIR/mysql.sock" \
  "$INNODB_PORT" \
  "$INNODB_DIR/mariadbd.pid" \
  "$INNODB_DIR/error.log"

if [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=== Starting RocksDB MariaDB instance (port $ROCKSDB_PORT) ==="
  start_mariadbd \
    "$ROCKSDB_DIR/data" \
    "$ROCKSDB_DIR/mysql.sock" \
    "$ROCKSDB_PORT" \
    "$ROCKSDB_DIR/mariadbd.pid" \
    "$ROCKSDB_DIR/error.log" \
    --plugin-load-add=rocksdb=ha_rocksdb.so \
    --plugin-dir="$ROCKSDB_PLUGIN_DIR"
fi

# ---------------------------------------------------------------------------
# Common sysbench args
# ---------------------------------------------------------------------------
common_args() {
  local port="$1"
  local socket="$2"
  local threads="$3"
  echo "--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$port --mysql-socket=$socket --mysql-user=root --mysql-password= --mysql-db=sbtest --tables=1 --table_size=$TABLE_SIZE --threads=$threads --time=$DURATION --report-interval=0"
}

# ---------------------------------------------------------------------------
# Run one benchmark and extract results
# ---------------------------------------------------------------------------
run_bench() {
  local engine="$1"
  local workload="$2"
  local port="$3"
  local socket="$4"
  local threads="$5"
  local storage_engine="$6"

  local args
  args="$(common_args "$port" "$socket" "$threads")"

  # Prepare
  sysbench "$workload" $args --mysql_storage_engine="$storage_engine" prepare >/dev/null 2>&1

  # Run and capture output
  local output
  output="$(sysbench "$workload" $args --mysql-ignore-errors=1180,1213  run 2>&1)"

# FATAL: `thread_run' function failed: /usr/share/sysbench/oltp_common.lua:469: SQL error, errno = 1213, state = '40001': Deadlock found when trying to get lock; try restarting transaction (snapshot conflict)
  # Cleanup
  sysbench "$workload" $args cleanup >/dev/null 2>&1

  # Extract metrics (sysbench 1.0 outputs only one percentile: 95th by default)
  local tps avg_lat p95
  tps="$(echo "$output" | grep "transactions:" | awk -F'[( ]+' '{print $3}')"
  avg_lat="$(echo "$output" | grep "avg:" | tail -1 | awk '{print $2}')"
  p95="$(echo "$output" | grep "95th percentile:" | awk '{print $NF}')"

  if [[ -z "$tps" ]]; then
    echo "  [FAILED] sysbench output:" >&2
    echo "$output" | tail -20 >&2
  fi

  echo "$engine,$workload,$threads,$tps,$avg_lat,$p95"
}

# ---------------------------------------------------------------------------
# Run all benchmarks
# ---------------------------------------------------------------------------
echo ""
echo "=== Sysbench OLTP Benchmark: ByteCaskDB vs InnoDB vs RocksDB ==="
echo "    Table size: $TABLE_SIZE rows | Duration: ${DURATION}s per run"
echo "    Threads: ${THREADS}"
echo "    Workloads: $WORKLOADS"
if [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "    RocksDB: SKIPPED (plugin not found)"
fi
echo ""

# CSV header
echo "engine,workload,threads,tps,avg_lat_ms,p95_ms" > "$RESULTS_CSV"

# Collect all results
declare -a ALL_RESULTS=()

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    echo "--- $workload | threads=$t ---"

    echo -n "  ByteCaskDB: "
    result_bc="$(run_bench bytecaskdb "$workload" "$BYTECASKDB_PORT" "$BYTECASKDB_DIR/mysql.sock" "$t" bytecaskdb)"
    echo "$result_bc" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result_bc")
    tps_bc="$(echo "$result_bc" | cut -d, -f4)"
    echo "${tps_bc} tps"

    echo -n "  InnoDB:     "
    result_in="$(run_bench innodb "$workload" "$INNODB_PORT" "$INNODB_DIR/mysql.sock" "$t" innodb)"
    echo "$result_in" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result_in")
    tps_in="$(echo "$result_in" | cut -d, -f4)"
    echo "${tps_in} tps"

    if [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
      echo -n "  RocksDB:    "
      result_rk="$(run_bench rocksdb "$workload" "$ROCKSDB_PORT" "$ROCKSDB_DIR/mysql.sock" "$t" rocksdb)"
      echo "$result_rk" >> "$RESULTS_CSV"
      ALL_RESULTS+=("$result_rk")
      tps_rk="$(echo "$result_rk" | cut -d, -f4)"
      echo "${tps_rk} tps"
    fi

    echo ""
  done
done

# ---------------------------------------------------------------------------
# Print comparison table
# ---------------------------------------------------------------------------
echo ""
if [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=============================================================================================================="
  printf "%-22s %4s | %10s %8s %8s | %10s %8s %8s | %10s %8s %8s\n" \
    "Workload" "Thr" "BC tps" "avg" "p95" "InnoDB tps" "avg" "p95" "RocksDB tps" "avg" "p95"
  echo "--------------------------------------------------------------------------------------------------------------"
else
  echo "========================================================================================"
  printf "%-22s %4s | %10s %8s %8s | %10s %8s %8s\n" \
    "Workload" "Thr" "BC tps" "avg" "p95" "InnoDB tps" "avg" "p95"
  echo "----------------------------------------------------------------------------------------"
fi

for workload in $WORKLOADS; do
  for t in "${THREAD_LIST[@]}"; do
    bc_line="" in_line="" rk_line=""
    for r in "${ALL_RESULTS[@]}"; do
      eng="$(echo "$r" | cut -d, -f1)"
      wl="$(echo "$r" | cut -d, -f2)"
      thr="$(echo "$r" | cut -d, -f3)"
      if [[ "$wl" == "$workload" && "$thr" == "$t" ]]; then
        if [[ "$eng" == "bytecaskdb" ]]; then bc_line="$r"; fi
        if [[ "$eng" == "innodb" ]]; then in_line="$r"; fi
        if [[ "$eng" == "rocksdb" ]]; then rk_line="$r"; fi
      fi
    done

    bc_tps="$(echo "$bc_line" | cut -d, -f4)"
    bc_avg="$(echo "$bc_line" | cut -d, -f5)"
    bc_p95="$(echo "$bc_line" | cut -d, -f6)"
    in_tps="$(echo "$in_line" | cut -d, -f4)"
    in_avg="$(echo "$in_line" | cut -d, -f5)"
    in_p95="$(echo "$in_line" | cut -d, -f6)"

    if [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
      rk_tps="$(echo "$rk_line" | cut -d, -f4)"
      rk_avg="$(echo "$rk_line" | cut -d, -f5)"
      rk_p95="$(echo "$rk_line" | cut -d, -f6)"
      printf "%-22s %4s | %10s %8s %8s | %10s %8s %8s | %10s %8s %8s\n" \
        "$workload" "$t" "$bc_tps" "$bc_avg" "$bc_p95" "$in_tps" "$in_avg" "$in_p95" "$rk_tps" "$rk_avg" "$rk_p95"
    else
      printf "%-22s %4s | %10s %8s %8s | %10s %8s %8s\n" \
        "$workload" "$t" "$bc_tps" "$bc_avg" "$bc_p95" "$in_tps" "$in_avg" "$in_p95"
    fi
  done
done
if [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=============================================================================================================="
else
  echo "========================================================================================"
fi
echo ""
echo "Results saved to: $RESULTS_CSV"
