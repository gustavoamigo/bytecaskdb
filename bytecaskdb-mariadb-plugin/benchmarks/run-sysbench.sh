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
#   ./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh [--table-size=N] [--threads=LIST] [--time=S] [--engines=LIST]
#
#   --engines: comma-separated list of engines to benchmark (default: bytecaskdb,innodb,rocksdb)
#              e.g. --engines=bytecaskdb or --engines=bytecaskdb,innodb

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
TABLE_SIZE=50000
THREADS
THREADS="1,8,16"
DURATION=10
ENGINES="bytecaskdb,innodb,rocksdb"
WORKLOADS="oltp_point_select oltp_read_only oltp_write_only oltp_insert oltp_read_write"
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
    --engines=*)    ENGINES="${arg#*=}" ;;
    --help|-h)
      echo "Usage: $0 [--table-size=N] [--threads=1,4,8] [--time=30] [--engines=bytecaskdb,innodb,rocksdb]"
      exit 0
      ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

IFS=',' read -ra THREAD_LIST <<< "$THREADS"
IFS=',' read -ra ENGINE_LIST <<< "$ENGINES"

# Helper to check if an engine is enabled
engine_enabled() { for e in "${ENGINE_LIST[@]}"; do [[ "$e" == "$1" ]] && return 0; done; return 1; }

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

# ---------------------------------------------------------------------------
# Build plugin in Release mode
# ---------------------------------------------------------------------------
if engine_enabled bytecaskdb; then
  echo "=== Building ByteCaskDB plugin (Release) ==="
  PLUGIN_SRC="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin"
  cmake -S "$PLUGIN_SRC" -B "$PLUGIN_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "$PLUGIN_DIR" --parallel --target ha_bytecaskdb >/dev/null 2>&1 || {
    echo "ERROR: plugin build failed"; exit 1;
  }

  if [[ ! -f "$PLUGIN_DIR/ha_bytecaskdb.so" ]]; then
    echo "ERROR: $PLUGIN_DIR/ha_bytecaskdb.so not found after build"
    exit 1
  fi
fi

ROCKSDB_PLUGIN_DIR=""
if engine_enabled rocksdb; then
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
  local defaults_file="$6"
  shift 6
  local extra_args=("$@")

  rm -rf "$data_dir"
  mkdir -p "$data_dir" "$(dirname "$data_dir")/tmp"

  mariadb-install-db --datadir="$data_dir" --auth-root-authentication-method=normal \
    >/dev/null 2>&1

  local defaults_arg=()
  if [[ -n "$defaults_file" ]]; then
    defaults_arg=("--defaults-extra-file=$defaults_file")
  fi

  mariadbd \
    "${defaults_arg[@]}" \
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
trap cleanup INT TERM

# ---------------------------------------------------------------------------
# Start instances
# ---------------------------------------------------------------------------
if engine_enabled bytecaskdb; then
  echo "=== Starting ByteCaskDB MariaDB instance (port $BYTECASKDB_PORT) ==="
  symlink_providers "$PLUGIN_DIR"
  start_mariadbd \
    "$BYTECASKDB_DIR/data" \
    "$BYTECASKDB_DIR/mysql.sock" \
    "$BYTECASKDB_PORT" \
    "$BYTECASKDB_DIR/mariadbd.pid" \
    "$BYTECASKDB_DIR/error.log" \
    "$SCRIPT_DIR/bytecaskdb.cnf" \
    --plugin-dir="$PLUGIN_DIR" \
    --plugin-load-add=bytecaskdb=ha_bytecaskdb.so
fi

if engine_enabled innodb; then
  echo "=== Starting InnoDB MariaDB instance (port $INNODB_PORT) ==="
  start_mariadbd \
    "$INNODB_DIR/data" \
    "$INNODB_DIR/mysql.sock" \
    "$INNODB_PORT" \
    "$INNODB_DIR/mariadbd.pid" \
    "$INNODB_DIR/error.log" \
    "$SCRIPT_DIR/innodb.cnf"
fi

if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=== Starting RocksDB MariaDB instance (port $ROCKSDB_PORT) ==="
  start_mariadbd \
    "$ROCKSDB_DIR/data" \
    "$ROCKSDB_DIR/mysql.sock" \
    "$ROCKSDB_PORT" \
    "$ROCKSDB_DIR/mariadbd.pid" \
    "$ROCKSDB_DIR/error.log" \
    "$SCRIPT_DIR/rocksdb.cnf" \
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
  shift 6
  local extra_args="$*"

  # Parse workload variant: "oltp_read_only:points_only" → base + extra flags
  local base_workload="$workload"
  local variant_args=""
  if [[ "$workload" == *:* ]]; then
    base_workload="${workload%%:*}"
    local variant="${workload##*:}"
    case "$variant" in
      points_only)    variant_args="--range-selects=off" ;;
      ranges_only)    variant_args="--point-selects=0" ;;
      simple_range)   variant_args="--point-selects=0 --sum-ranges=0 --order-ranges=0 --distinct-ranges=0" ;;
      sum_range)      variant_args="--point-selects=0 --simple-ranges=0 --order-ranges=0 --distinct-ranges=0" ;;
      order_range)    variant_args="--point-selects=0 --simple-ranges=0 --sum-ranges=0 --distinct-ranges=0" ;;
      distinct_range) variant_args="--point-selects=0 --simple-ranges=0 --sum-ranges=0 --order-ranges=0" ;;
    esac
  fi

  local args
  args="$(common_args "$port" "$socket" "$threads")"

  # Prepare
  sysbench "$base_workload" $args --mysql_storage_engine="$storage_engine" prepare >/dev/null 2>&1

  # Run and capture output
  local output
  output="$(sysbench "$base_workload" $args $variant_args --mysql-ignore-errors=1180,1213 run 2>&1)"

  # Cleanup (skip — keep data for EXPLAIN)
  # sysbench "$base_workload" $args cleanup >/dev/null 2>&1

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
echo "=== Sysbench OLTP Benchmark ==="
echo "    Engines: ${ENGINES}"
echo "    Table size: $TABLE_SIZE rows | Duration: ${DURATION}s per run"
echo "    Threads: ${THREADS}"
echo "    Workloads: $WORKLOADS"
if engine_enabled rocksdb && [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
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

    if engine_enabled bytecaskdb; then
      echo -n "  ByteCaskDB: "
      result_bc="$(run_bench bytecaskdb "$workload" "$BYTECASKDB_PORT" "$BYTECASKDB_DIR/mysql.sock" "$t" bytecaskdb)"
      echo "$result_bc" >> "$RESULTS_CSV"
      ALL_RESULTS+=("$result_bc")
      tps_bc="$(echo "$result_bc" | cut -d, -f4)"
      echo "${tps_bc} tps"
    fi

    if engine_enabled innodb; then
      echo -n "  InnoDB:     "
      result_in="$(run_bench innodb "$workload" "$INNODB_PORT" "$INNODB_DIR/mysql.sock" "$t" innodb)"
      echo "$result_in" >> "$RESULTS_CSV"
      ALL_RESULTS+=("$result_in")
      tps_in="$(echo "$result_in" | cut -d, -f4)"
      echo "${tps_in} tps"
    fi

    if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
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

# Build header dynamically based on enabled engines
header_fmt="%-22s %4s"
header_args=("Workload" "Thr")
if engine_enabled bytecaskdb; then
  header_fmt+=" | %10s %8s %8s"
  header_args+=("BC tps" "avg" "p95")
fi
if engine_enabled innodb; then
  header_fmt+=" | %10s %8s %8s"
  header_args+=("InnoDB tps" "avg" "p95")
fi
if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  header_fmt+=" | %10s %8s %8s"
  header_args+=("RocksDB tps" "avg" "p95")
fi

printf "$header_fmt\n" "${header_args[@]}"

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

    row_fmt="%-22s %4s"
    row_args=("$workload" "$t")
    if engine_enabled bytecaskdb; then
      row_fmt+=" | %10s %8s %8s"
      row_args+=("$(echo "$bc_line" | cut -d, -f4)" "$(echo "$bc_line" | cut -d, -f5)" "$(echo "$bc_line" | cut -d, -f6)")
    fi
    if engine_enabled innodb; then
      row_fmt+=" | %10s %8s %8s"
      row_args+=("$(echo "$in_line" | cut -d, -f4)" "$(echo "$in_line" | cut -d, -f5)" "$(echo "$in_line" | cut -d, -f6)")
    fi
    if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
      row_fmt+=" | %10s %8s %8s"
      row_args+=("$(echo "$rk_line" | cut -d, -f4)" "$(echo "$rk_line" | cut -d, -f5)" "$(echo "$rk_line" | cut -d, -f6)")
    fi
    printf "$row_fmt\n" "${row_args[@]}"
  done
done

echo ""
echo "Results saved to: $RESULTS_CSV"

cleanup
