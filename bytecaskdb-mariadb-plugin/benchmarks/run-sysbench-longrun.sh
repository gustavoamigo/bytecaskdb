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
# Each sample also records write_mib (block-layer bytes written during that
# interval, from the instance's cgroup io.stat) and eng_write_mib (what the
# engine itself reports writing). Plotted against elapsed_s these show write
# amplification developing: as updates spread over more pages than the buffer
# pool can keep coalescing, an in-place engine's bytes per transaction climb
# while an append-only one's stay flat. A single end-of-run total hides that.
# eng_write_mib is partial on both sides — see the note on engine_counters in
# lib_common.sh — so compare write_mib across engines, not the ratio.
#
# Prerequisites: same as run-sysbench.sh (plugin buildable, mariadbd +
# mariadb-install-db + sysbench on PATH).
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench-longrun.sh \
#       [--table-size=N] [--duration=4h] [--report-interval=30s] \
#       [--threads=N] [--workload=NAME] [--engines=LIST] [--out=PATH] [--data-root=PATH] \
#       [--profile=acid|fast] [--capture] [--capture-at=10m] [--capture-dir=PATH]
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
#   --profile is the durability profile (default: acid), as in run-sysbench.sh:
#   acid starts each engine with <engine>.cnf, fast with <engine>-fast.cnf,
#   where a commit survives a mariadbd crash but not an OS crash. Recorded in
#   every CSV row.
#   --capture profiles each engine's server for 120 s, as run-sysbench.sh
#   --capture does (see lib_capture.sh), starting --capture-at into the run
#   (default 60s, past the start-up transient; later to profile a run that has
#   degraded). The tarball goes to --capture-dir (default benchmarks/captures/)
#   as capture_<engine>_<workload>_t<threads>_<run start>.tar.gz. The capture
#   slows the server, so the samples it overlaps are not comparable with the
#   rest.
# ./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench-longrun.sh \
#      --duration=10m --report-interval=30s] \
#       --threads=8 --workload=oltp_read_write --engines=bytecaskdb --out=sysbench_longrun_results.csv --data-root=/mnt/data

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
PROFILE="acid"
CAPTURE="off"
CAPTURE_AT_RAW="60s"
CAPTURE_DIR=""

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
    --profile=*)          PROFILE="${arg#*=}" ;;
    --capture)            CAPTURE="on" ;;
    --capture-at=*)       CAPTURE_AT_RAW="${arg#*=}" ;;
    --capture-dir=*)      CAPTURE_DIR="${arg#*=}" ;;
    --no-secondary-index) CREATE_SECONDARY="off" ;;
    --help|-h)
      echo "Usage: $0 [--table-size=5000000] [--duration=4h] [--report-interval=30s] [--threads=8] [--workload=oltp_read_write] [--engines=bytecaskdb,innodb] [--out=PATH] [--data-root=PATH] [--no-secondary-index] [--profile=acid|fast] [--capture] [--capture-at=60s] [--capture-dir=PATH]"
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
CAPTURE_AT="$(to_seconds "$CAPTURE_AT_RAW")"

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

# Before the plugin build, so a bad --profile fails in a second, not minutes.
check_profile "$PROFILE" "${ENGINE_LIST[@]}"

# Checked before anything slow: tools, perf permissions, the capture directory.
if [[ "$CAPTURE" == on ]]; then
  # shellcheck source=lib_capture.sh
  source "$SCRIPT_DIR/lib_capture.sh"
  if (( DURATION < CAPTURE_AT + CAPTURE_SECONDS + 10 )); then
    echo "ERROR: --capture records ${CAPTURE_SECONDS}s from --capture-at=${CAPTURE_AT}s;" \
         "--duration=${DURATION}s is too short (use $(( CAPTURE_AT + CAPTURE_SECONDS + 10 ))s or more)." >&2
    exit 1
  fi
  CAPTURE_DIR="${CAPTURE_DIR:-$SCRIPT_DIR/captures}"
  capture_preflight $(( (DURATION + 59) / 60 )) "$CAPTURE_DIR" "${#ENGINE_LIST[@]}"
  CAPTURE_DIR="$(cd "$CAPTURE_DIR" && pwd)"
  # In every tarball's name, so a rerun never overwrites an earlier capture.
  CAPTURE_RUN="$(date +%Y%m%d_%H%M%S)"
fi

if engine_enabled bytecaskdb; then
  build_bytecaskdb_plugin
fi
if [[ "$CAPTURE" == on ]]; then
  symbol_files=("$(command -v mariadbd)")
  engine_enabled bytecaskdb && symbol_files+=("$PLUGIN_DIR/ha_bytecaskdb.so")
  capture_check_symbols "${symbol_files[@]}"
  if engine_enabled bytecaskdb; then
    capture_check_debug_info "$PLUGIN_DIR/ha_bytecaskdb.so"
  fi
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
CAPTURER_PID=""
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
  [[ -n "$CAPTURER_PID" ]] && kill "$CAPTURER_PID" 2>/dev/null
  stop_mariadbd "${BYTECASKDB_DIR}/mariadbd.pid"
  remove_instance "${BYTECASKDB_DIR}"
  stop_mariadbd "${INNODB_DIR}/mariadbd.pid"
  remove_instance "${INNODB_DIR}"
  stop_mariadbd "${ROCKSDB_DIR}/mariadbd.pid"
  remove_instance "${ROCKSDB_DIR}"
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
  local dir pid_file
  dir="$(dirname "$socket")"
  pid_file="$dir/mariadbd.pid"

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
    # Bytes are sampled per report interval, not once at the end: the point of
    # a long run is watching write amplification build as updates spread over
    # more pages than the buffer pool can keep coalescing. A single total
    # flattens exactly that.
    local io_prev eng_prev
    io_prev="$(io_sample "$pid_file")"
    eng_prev="$(engine_counters "$engine" "$socket")"
    while IFS= read -r line; do
      local row
      row="$(echo "$line" | parse_report_line)"
      if [[ -n "$row" ]]; then
        local ts; ts="$(date -u +%FT%TZ)"
        local io_now eng_now w_mib e_mib
        io_now="$(io_sample "$pid_file")"
        eng_now="$(engine_counters "$engine" "$socket")"
        w_mib="$(io_delta "$io_prev" "$io_now" | cut -d, -f2)"
        e_mib="$(eng_delta "$eng_prev" "$eng_now" | cut -d, -f1)"
        io_prev="$io_now"
        eng_prev="$eng_now"
        echo "$ts,$engine,$WORKLOAD,$THREADS,$row,$w_mib,$e_mib,$PROFILE" >> "$RESULTS_CSV"
        # elapsed_s is the row's first field — echo a compact live progress line.
        local elapsed="${row%%,*}"
        echo "  [$engine] ${elapsed}s / ${DURATION}s : $line | wrote ${w_mib} MiB"
      else
        echo "  [$engine] $line"
      fi
    done
  ) 2>&1 &
  SYSBENCH_PID=$!
  # The capture runs beside the run, from --capture-at into it.
  local capture_dir="" capturer=""
  if [[ "$CAPTURE" == on ]]; then
    capture_dir="$(dirname "$dir")/capture_${engine}_${WORKLOAD}_t${THREADS}"
    rm -rf "$capture_dir"
    ( sleep "$CAPTURE_AT"
      capture_run "$engine" "$pid_file" "$socket" "$capture_dir" ) &
    capturer=$!
    CAPTURER_PID=$capturer
  fi
  wait "$SYSBENCH_PID"
  SYSBENCH_PID=""
  if [[ -n "$capturer" ]]; then
    wait "$capturer" 2>/dev/null || true
    CAPTURER_PID=""
    local tarball="$CAPTURE_DIR/$(basename "$capture_dir")_$CAPTURE_RUN.tar.gz"
    capture_finish "$capture_dir" "$tarball" \
      "$(engine_defaults_file "$engine" "$PROFILE")" "$dir/error.log"
    if [[ -f "$tarball" ]]; then
      echo "  [$engine] capture: $tarball ($(du -h "$tarball" | cut -f1))"
      echo "  [$engine] $(capture_fetch_hint "$(basename "$tarball" .tar.gz)" "$tarball")"
    fi
  fi
}

# ---------------------------------------------------------------------------
# Run all engines, sequentially, each for the full duration.
# ---------------------------------------------------------------------------
echo ""
echo "=== Long-running sysbench comparison ==="
echo "    Engines: ${ENGINES}"
echo "    Workload: $WORKLOAD | Threads: $THREADS"
echo "    Durability profile: $PROFILE"
echo "    Table size: $TABLE_SIZE rows | Duration: ${DURATION}s per engine | Report every ${REPORT_INTERVAL}s"
echo "    Results: $RESULTS_CSV"
echo "    Data root: $DATA_ROOT"
echo "    Capture: $CAPTURE${CAPTURE_DIR:+ (at ${CAPTURE_AT}s, tarballs to $CAPTURE_DIR)}"
echo ""

# Append across runs so a long-run history accumulates; the timestamp column
# separates runs. An existing file must already have this exact layout —
# appending rows with a different column count under someone else's header
# silently misaligns every field, and the file looks fine until something
# tries to read it.
CSV_HEADER="timestamp,engine,workload,threads,elapsed_s,report_threads,tps,qps,lat95_ms,err_per_s,write_mib,eng_write_mib,profile"
if [[ ! -s "$RESULTS_CSV" ]]; then
  echo "$CSV_HEADER" > "$RESULTS_CSV"
else
  existing_header="$(head -1 "$RESULTS_CSV")"
  if [[ "$existing_header" != "$CSV_HEADER" ]]; then
    echo "ERROR: $RESULTS_CSV already exists with a different column layout." >&2
    echo "  expected: $CSV_HEADER" >&2
    echo "  found:    $existing_header" >&2
    echo "  Appending would misalign every row against that header. Move the file" >&2
    echo "  aside, or pass --out=PATH to write somewhere new." >&2
    exit 1
  fi
fi

if engine_enabled bytecaskdb; then
  echo "=== Starting ByteCaskDB MariaDB instance (port $BYTECASKDB_PORT) ==="
  symlink_providers "$PLUGIN_DIR"
  init_datadir "${BYTECASKDB_DIR}/data"
  start_mariadbd \
    "${BYTECASKDB_DIR}/data" "$BYTECASKDB_DIR/mysql.sock" "$BYTECASKDB_PORT" \
    "$BYTECASKDB_DIR/mariadbd.pid" "$BYTECASKDB_DIR/error.log" \
    "$(engine_defaults_file bytecaskdb "$PROFILE")" \
    --plugin-dir="$PLUGIN_DIR" --plugin-load-add=bytecaskdb=ha_bytecaskdb.so
  run_longbench bytecaskdb "$BYTECASKDB_PORT" "$BYTECASKDB_DIR/mysql.sock" bytecaskdb
  stop_mariadbd "${BYTECASKDB_DIR}/mariadbd.pid"
  remove_instance "${BYTECASKDB_DIR}"
  echo ""
fi

if engine_enabled innodb; then
  echo "=== Starting InnoDB MariaDB instance (port $INNODB_PORT) ==="
  init_datadir "${INNODB_DIR}/data"
  start_mariadbd \
    "${INNODB_DIR}/data" "$INNODB_DIR/mysql.sock" "$INNODB_PORT" \
    "$INNODB_DIR/mariadbd.pid" "$INNODB_DIR/error.log" \
    "$(engine_defaults_file innodb "$PROFILE")"
  run_longbench innodb "$INNODB_PORT" "$INNODB_DIR/mysql.sock" innodb
  stop_mariadbd "${INNODB_DIR}/mariadbd.pid"
  remove_instance "${INNODB_DIR}"
  echo ""
fi

if engine_enabled rocksdb && [[ -n "$ROCKSDB_PLUGIN_DIR" ]]; then
  echo "=== Starting RocksDB MariaDB instance (port $ROCKSDB_PORT) ==="
  init_datadir "${ROCKSDB_DIR}/data"
  start_mariadbd \
    "${ROCKSDB_DIR}/data" "$ROCKSDB_DIR/mysql.sock" "$ROCKSDB_PORT" \
    "$ROCKSDB_DIR/mariadbd.pid" "$ROCKSDB_DIR/error.log" \
    "$(engine_defaults_file rocksdb "$PROFILE")" \
    --plugin-load-add=rocksdb=ha_rocksdb.so --plugin-dir="$ROCKSDB_PLUGIN_DIR"
  run_longbench rocksdb "$ROCKSDB_PORT" "$ROCKSDB_DIR/mysql.sock" rocksdb
  stop_mariadbd "${ROCKSDB_DIR}/mariadbd.pid"
  remove_instance "${ROCKSDB_DIR}"
  echo ""
fi

echo "Done. Results: $RESULTS_CSV"
echo "Plot tps (or lat95_ms) vs elapsed_s, grouped by engine, to see the trend over time."
