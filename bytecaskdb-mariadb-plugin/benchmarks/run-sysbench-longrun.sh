#!/usr/bin/env bash
# Long-running sysbench comparison: ByteCaskDB vs InnoDB (optionally RocksDB).
#
# Unlike run-sysbench.sh (a matrix of short, fixed-duration runs across many
# workloads/thread counts), this runs ONE workload at a fixed thread count for
# a long duration per engine, using sysbench's --report-interval to print
# (and log) a tps/latency snapshot every N seconds. That's what you want to
# see a trend over time — e.g. InnoDB's throughput decaying as buffer pool /
# purge lag / redo log pressure build up on a dataset that outgrows RAM, vs
# ByteCaskDB's flat-latency design holding steady.
#
# Each engine gets the full --duration, one after another (not concurrently),
# so neither run's numbers are skewed by the other competing for CPU/disk.
# Every interval sample from every engine lands in one CSV (one "engine"
# column) as it happens, so a plot can overlay both series, and Ctrl-C at any
# point still leaves whatever samples were already collected.
#
# Prerequisites: same as run-sysbench.sh (plugin buildable, mariadbd +
# mariadb-install-db + sysbench on PATH).
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench-longrun.sh \
#       [--table-size=N] [--duration=4h] [--report-interval=30s] \
#       [--threads=N] [--workload=NAME] [--engines=LIST] [--out=PATH] [--data-root=PATH]
#
#   --duration / --report-interval accept a plain integer (seconds) or a
#   number with a trailing s/m/h/d suffix, e.g. --duration=6h --report-interval=30s.
#   --table-size defaults large (5,000,000 rows) so the dataset exceeds the
#   3G buffer pool / block cache configured in innodb.cnf / rocksdb.cnf —
#   below that, everything fits in RAM and no engine visibly degrades.
#   --data-root is the directory under which the ephemeral mariadbd instances
#   create their data/tmp/socket files (default: repository root). Point it at
#   a filesystem that supports native fdatasync/O_DIRECT when the repo lives on
#   a bind mount or overlay that doesn't, e.g. --data-root=/mnt/nvme.

set -uo pipefail  # not -e: one sysbench hiccup mid-run shouldn't abort everything

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
TABLE_SIZE=5000000
DURATION_RAW="1h"
REPORT_INTERVAL_RAW="30s"
THREADS=8
WORKLOAD="oltp_read_write"
ENGINES="bytecaskdb,innodb"
CREATE_SECONDARY="on"

BYTECASKDB_PORT=3330
INNODB_PORT=3331
ROCKSDB_PORT=3332

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
OUT_CSV=""
DATA_ROOT=""
for arg in "$@"; do
  case "$arg" in
    --table-size=*)      TABLE_SIZE="${arg#*=}" ;;
    --duration=*)         DURATION_RAW="${arg#*=}" ;;
    --report-interval=*)  REPORT_INTERVAL_RAW="${arg#*=}" ;;
    --threads=*)          THREADS="${arg#*=}" ;;
    --workload=*)         WORKLOAD="${arg#*=}" ;;
    --engines=*)          ENGINES="${arg#*=}" ;;
    --out=*)              OUT_CSV="${arg#*=}" ;;
    --data-root=*)        DATA_ROOT="${arg#*=}" ;;
    --no-secondary-index) CREATE_SECONDARY="off" ;;
    --help|-h)
      echo "Usage: $0 [--table-size=5000000] [--duration=4h] [--report-interval=30s] [--threads=8] [--workload=oltp_read_write] [--engines=bytecaskdb,innodb] [--out=PATH] [--data-root=PATH] [--no-secondary-index]"
      exit 0
      ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

# Accepts N, Ns, Nm, Nh, or Nd and echoes whole seconds.
to_seconds() {
  local raw="$1"
  case "$raw" in
    *d) echo "$(( ${raw%d} * 86400 ))" ;;
    *h) echo "$(( ${raw%h} * 3600 ))" ;;
    *m) echo "$(( ${raw%m} * 60 ))" ;;
    *s) echo "${raw%s}" ;;
    *)  echo "$raw" ;;
  esac
}

DURATION="$(to_seconds "$DURATION_RAW")"
REPORT_INTERVAL="$(to_seconds "$REPORT_INTERVAL_RAW")"

IFS=',' read -ra ENGINE_LIST <<< "$ENGINES"
engine_enabled() { for e in "${ENGINE_LIST[@]}"; do [[ "$e" == "$1" ]] && return 0; done; return 1; }

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
RESULTS_CSV="${OUT_CSV:-$SCRIPT_DIR/sysbench_longrun_results.csv}"

# Where the ephemeral mariadbd instances live. Defaults to the repo root; override
# with --data-root when that filesystem can't do native fdatasync (e.g. a bind mount).
DATA_ROOT="${DATA_ROOT:-$BYTECASK_ROOT}"
mkdir -p "$DATA_ROOT" || { echo "ERROR: cannot create --data-root=$DATA_ROOT"; exit 1; }
DATA_ROOT="$(cd "$DATA_ROOT" && pwd)"
BYTECASKDB_DIR="$DATA_ROOT/.mariadb_longrun_bytecaskdb"
INNODB_DIR="$DATA_ROOT/.mariadb_longrun_innodb"
ROCKSDB_DIR="$DATA_ROOT/.mariadb_longrun_rocksdb"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
command -v sysbench >/dev/null 2>&1 || { echo "ERROR: sysbench not found"; exit 1; }
command -v mariadbd >/dev/null 2>&1 || { echo "ERROR: mariadbd not found"; exit 1; }

# shellcheck source=lib_common.sh
source "$SCRIPT_DIR/lib_common.sh"

if engine_enabled bytecaskdb; then
  build_bytecaskdb_plugin
fi

ROCKSDB_PLUGIN_DIR=""
if engine_enabled rocksdb; then
  ROCKSDB_PLUGIN_DIR="$(find_rocksdb_plugin_dir || true)"
  if [[ -z "$ROCKSDB_PLUGIN_DIR" ]]; then
    echo "WARNING: ha_rocksdb.so not found — RocksDB benchmarks will be skipped"
  fi
fi

# ---------------------------------------------------------------------------
# Cleanup trap — a long run is exactly where Ctrl-C is most likely.
# ---------------------------------------------------------------------------
SYSBENCH_PID=""
CLEANED_UP=0
cleanup() {
  [[ "$CLEANED_UP" -eq 1 ]] && return
  CLEANED_UP=1
  echo ""
  echo "=== Cleaning up ==="
  # Bash defers a trapped signal until the current foreground pipeline
  # returns control to it, so `kill <this-script's-pid>` alone would never
  # actually stop a running sysbench (a very likely scenario for a
  # multi-hour run started under nohup/tmux). Kill it directly first.
  [[ -n "$SYSBENCH_PID" ]] && kill "$SYSBENCH_PID" 2>/dev/null
  stop_mariadbd "$BYTECASKDB_DIR/mariadbd.pid" "$BYTECASKDB_DIR"
  stop_mariadbd "$INNODB_DIR/mariadbd.pid" "$INNODB_DIR"
  stop_mariadbd "$ROCKSDB_DIR/mariadbd.pid" "$ROCKSDB_DIR"
}
# A plain `trap cleanup INT TERM` would run cleanup() then bash resumes the
# script right after the interrupted command (e.g. moving on to the next
# engine) instead of stopping — only the EXIT trap firing at actual script
# end is implicit. Exit explicitly on INT/TERM so Ctrl-C (or a `kill` on a
# nohup'd/tmux'd long run) stops the whole thing, not just the current engine.
on_signal() { cleanup; exit 130; }
trap on_signal INT TERM
trap cleanup EXIT

# ---------------------------------------------------------------------------
# One sed pass turns a sysbench periodic report line into a CSV row.
#   [ 30s ] thds: 8 tps: 812.34 qps: 4874.53 (r/w/o: ...) lat (ms,95%): 1.31 err/s: 0.00 reconn/s: 0.00
# -> 30,8,812.34,4874.53,1.31,0.00
# ---------------------------------------------------------------------------
parse_report_line() {
  sed -n 's/^\[ *\([0-9]*\)s *\] thds: *\([0-9]*\) tps: *\([0-9.]*\) qps: *\([0-9.]*\).*lat (ms,95%): *\([0-9.]*\) err\/s: *\([0-9.]*\).*/\1,\2,\3,\4,\5,\6/p'
}

# ---------------------------------------------------------------------------
# Runs one long benchmark against a running instance, streaming each
# --report-interval sample to $RESULTS_CSV (and the terminal) as it arrives.
# ---------------------------------------------------------------------------
run_longbench() {
  local engine="$1" port="$2" socket="$3" storage_engine="$4"

  echo "--- Preparing $TABLE_SIZE rows on $engine (this can take a while for large tables) ---"
  local conn_args
  conn_args="$(sysbench_conn_args "$port" "$socket" "$THREADS" "$TABLE_SIZE")"
  # shellcheck disable=SC2086
  sysbench "$WORKLOAD" $conn_args --mysql_storage_engine="$storage_engine" \
    --create_secondary="$CREATE_SECONDARY" prepare || {
    echo "ERROR: prepare failed for $engine" >&2
    return 1
  }

  echo "--- Running $engine | $WORKLOAD | threads=$THREADS | duration=${DURATION}s | reporting every ${REPORT_INTERVAL}s ---"
  # Backgrounded + `wait`, rather than a plain foreground pipe: `wait` is
  # interruptible the instant a trapped signal arrives, so cleanup()'s
  # explicit `kill "$SYSBENCH_PID"` actually reaches sysbench right away —
  # a plain `cmd | while read` pipe would leave the signal (and the trap)
  # pending until sysbench exits on its own.
  # shellcheck disable=SC2086
  stdbuf -oL sysbench "$WORKLOAD" $conn_args \
    --time="$DURATION" --report-interval="$REPORT_INTERVAL" \
    --mysql-ignore-errors=1180,1213 run > >(
    while IFS= read -r line; do
      local row
      row="$(echo "$line" | parse_report_line)"
      if [[ -n "$row" ]]; then
        local ts; ts="$(date -u +%FT%TZ)"
        echo "$ts,$engine,$WORKLOAD,$THREADS,$row" >> "$RESULTS_CSV"
        # elapsed_s is the row's first field — echo a compact live progress line.
        local elapsed="${row%%,*}"
        echo "  [$engine] ${elapsed}s / ${DURATION}s : $line"
      else
        echo "  [$engine] $line"
      fi
    done
  ) 2>&1 &
  SYSBENCH_PID=$!
  wait "$SYSBENCH_PID"
  SYSBENCH_PID=""
}

# ---------------------------------------------------------------------------
# Run all engines, sequentially, each for the full duration.
# ---------------------------------------------------------------------------
echo ""
echo "=== Long-running sysbench comparison ==="
echo "    Engines: ${ENGINES}"
echo "    Workload: $WORKLOAD | Threads: $THREADS"
echo "    Table size: $TABLE_SIZE rows | Duration: ${DURATION}s per engine | Report every ${REPORT_INTERVAL}s"
echo "    Results: $RESULTS_CSV"
echo "    Data root: $DATA_ROOT"
echo ""

# Append across runs so a long-run history accumulates; the timestamp column
# separates runs. The header is written only when the file is new.
if [[ ! -s "$RESULTS_CSV" ]]; then
  echo "timestamp,engine,workload,threads,elapsed_s,report_threads,tps,qps,lat95_ms,err_per_s" > "$RESULTS_CSV"
fi

if engine_enabled bytecaskdb; then
  echo "=== Starting ByteCaskDB MariaDB instance (port $BYTECASKDB_PORT) ==="
  symlink_providers "$PLUGIN_DIR"
  start_mariadbd \
    "$BYTECASKDB_DIR/data" "$BYTECASKDB_DIR/mysql.sock" "$BYTECASKDB_PORT" \
    "$BYTECASKDB_DIR/mariadbd.pid" "$BYTECASKDB_DIR/error.log" \
    "$SCRIPT_DIR/bytecaskdb.cnf" \
    --plugin-dir="$PLUGIN_DIR" --plugin-load-add=bytecaskdb=ha_bytecaskdb.so
  run_longbench bytecaskdb "$BYTECASKDB_PORT" "$BYTECASKDB_DIR/mysql.sock" bytecaskdb
  stop_mariadbd "$BYTECASKDB_DIR/mariadbd.pid" "$BYTECASKDB_DIR"
  echo ""
fi

if engine_enabled innodb; then
  echo "=== Starting InnoDB MariaDB instance (port $INNODB_PORT) ==="
  start_mariadbd \
    "$INNODB_DIR/data" "$INNODB_DIR/mysql.sock" "$INNODB_PORT" \
    "$INNODB_DIR/mariadbd.pid" "$INNODB_DIR/error.log" \
    "$SCRIPT_DIR/innodb.cnf"
  run_longbench innodb "$INNODB_PORT" "$INNODB_DIR/mysql.sock" innodb
  stop_mariadbd "$INNODB_DIR/mariadbd.pid" "$INNODB_DIR"
  echo ""
fi

if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=== Starting RocksDB MariaDB instance (port $ROCKSDB_PORT) ==="
  start_mariadbd \
    "$ROCKSDB_DIR/data" "$ROCKSDB_DIR/mysql.sock" "$ROCKSDB_PORT" \
    "$ROCKSDB_DIR/mariadbd.pid" "$ROCKSDB_DIR/error.log" \
    "$SCRIPT_DIR/rocksdb.cnf" \
    --plugin-load-add=rocksdb=ha_rocksdb.so --plugin-dir="$ROCKSDB_PLUGIN_DIR"
  run_longbench rocksdb "$ROCKSDB_PORT" "$ROCKSDB_DIR/mysql.sock" rocksdb
  stop_mariadbd "$ROCKSDB_DIR/mariadbd.pid" "$ROCKSDB_DIR"
  echo ""
fi

echo "Done. Results: $RESULTS_CSV"
echo "Plot tps (or lat95_ms) vs elapsed_s, grouped by engine, to see the trend over time."
