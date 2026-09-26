#!/usr/bin/env bash
# HammerDB TPROC-C comparison: ByteCaskDB vs InnoDB
#
# Spawns an ephemeral local mariadbd per engine, builds the HammerDB TPROC-C
# schema (TPC-C derived: 9 tables, stored procedures) into it, then runs the
# timed driver at each virtual-user count and reports NOPM/TPM side by side.
#
# Prerequisites:
#   - mariadbd + mariadb-install-db on PATH
#   - HammerDB 5.0+ (hammerdbcli). Found via --hammerdb-home, $HAMMERDB_HOME,
#     ~/HammerDB-*, or PATH. Download the Linux tarball from
#     https://github.com/TPC-Council/HammerDB/releases and unpack it anywhere.
#   - jemalloc (libjemalloc.so.2), as for run-sysbench.sh — see lib_common.sh
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/benchmarks/run-hammerdb.sh [--warehouses=N] [--vus=LIST]
#       [--rampup=MIN] [--duration=MIN] [--engines=LIST] [--data-root=PATH]
#       [--build-vus=N] [--no-restore] [--reuse-data] [--hammerdb-home=PATH]
#
#   --warehouses: TPROC-C warehouses (default: 20, ~2 GiB on InnoDB). Keep it
#                 at or above the largest --vus: a virtual user picks a home
#                 warehouse, and with fewer warehouses than users they collide
#                 on the same district rows.
#   --vus:        comma-separated virtual-user counts (default: 16)
#   --rampup:     unmeasured minutes before the measured window (default: 2)
#   --duration:   measured minutes (default: 5)
#   --engines:    bytecaskdb, innodb, or both (default: bytecaskdb,innodb)
#   --build-vus:  virtual users loading the schema (default: min(nproc, warehouses))
#   --no-restore: run every cell against the data the previous cell left
#                 behind instead of a fresh copy of the built schema
#   --reuse-data: keep the built schema under --data-root at exit, and on the
#                 next run skip the build for any engine that already has one.
#                 --warehouses must match what was built.
#   --data-root:  where instance directories live (default: repository root).
#                 Point it at a filesystem with native fdatasync/O_DIRECT, e.g.
#                 --data-root=/mnt/bench. A reflink-capable filesystem (btrfs,
#                 xfs) makes the per-cell restore instant; elsewhere it is a
#                 full copy.
#
# Run structure:
#   1. Build    — per engine: initialise the data directory, start the server,
#                 build the schema, stop the server cleanly, and keep a copy of
#                 the data directory as the pristine image.
#   2. Measure  — per VU count x engine: restore the pristine image, start the
#                 server, run HammerDB's timed driver (ramp-up then measured
#                 window), stop the server. Only the engine under test runs.
#   3. Teardown — remove every instance directory.
#
# TPROC-C inserts orders and history on every run, so without the restore each
# cell would start from a larger, more fragmented database than the one before
# it, and results would depend on the order VU counts are listed in.
#
# Concurrency control differs between the engines, and it shows in the
# results. InnoDB locks rows, so conflicting transactions wait. ByteCaskDB is
# optimistic: a transaction that wrote a row another one changed since its
# snapshot fails at COMMIT with 1213 (ER_LOCK_DEADLOCK), and HammerDB drops it
# and moves on to the next one. NOPM counts only committed new orders, so the
# aborted work is already out of the headline figure; the `aborts` column
# says how much of it there was.
#
# CSV columns (hammerdb_results.csv):
#   nopm, tpm       — HammerDB's own result line. TPM is MariaDB's
#                     Com_commit + Com_rollback per minute, so it counts
#                     transactions the engine rolled back as well.
#   aborts          — 1213 errors HammerDB reported over the whole run
#                     (ramp-up included; it logs them without timestamps).
#   other_errors    — every other error HammerDB reported. Should be 0.
#   read_mib ..     — as in run-sysbench.sh (see lib_common.sh), sampled over
#                     the measured window only: the first sample is taken when
#                     the ramp-up is due to end, measured from the start of
#                     the run, so it lands within a second or two of it.
#   flush_mib       — device writes while the server shuts down.
#   rss_mib, peak   — resident memory at the end, and its peak over the
#                     measured window.
# Per-cell HammerDB logs are kept under <data-root>/.hammerdb_logs/.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
WAREHOUSES=20
VUS="16"
RAMPUP=2
DURATION=5
ENGINES="bytecaskdb,innodb"
BUILD_VUS=""
RESTORE="on"
REUSE_DATA="off"
DATA_ROOT=""
HAMMERDB_HOME="${HAMMERDB_HOME:-}"

BYTECASKDB_PORT=3330
INNODB_PORT=3331

# HammerDB passes the password as a positional argument to its generated
# loader, so an empty one breaks the call. --skip-grant-tables accepts any.
HAMMERDB_PASS="maria"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
for arg in "$@"; do
  case "$arg" in
    --warehouses=*)    WAREHOUSES="${arg#*=}" ;;
    --vus=*)           VUS="${arg#*=}" ;;
    --rampup=*)        RAMPUP="${arg#*=}" ;;
    --duration=*)      DURATION="${arg#*=}" ;;
    --engines=*)       ENGINES="${arg#*=}" ;;
    --build-vus=*)     BUILD_VUS="${arg#*=}" ;;
    --no-restore)      RESTORE="off" ;;
    --reuse-data)      REUSE_DATA="on" ;;
    --data-root=*)     DATA_ROOT="${arg#*=}" ;;
    --hammerdb-home=*) HAMMERDB_HOME="${arg#*=}" ;;
    --help|-h)
      echo "Usage: $0 [--warehouses=N] [--vus=8,16] [--rampup=MIN] [--duration=MIN] [--engines=bytecaskdb,innodb] [--build-vus=N] [--no-restore] [--reuse-data] [--data-root=PATH] [--hammerdb-home=PATH]"
      exit 0
      ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

IFS=',' read -ra VU_LIST <<< "$VUS"
IFS=',' read -ra ENGINE_LIST <<< "$ENGINES"

ACTIVE_ENGINES=()
for e in "${ENGINE_LIST[@]}"; do
  case "$e" in
    bytecaskdb|innodb) ACTIVE_ENGINES+=("$e") ;;
    *) echo "ERROR: unsupported engine '$e' (bytecaskdb, innodb)"; exit 1 ;;
  esac
done

MAX_VUS=0
for v in "${VU_LIST[@]}"; do (( v > MAX_VUS )) && MAX_VUS=$v; done
if (( MAX_VUS > WAREHOUSES )); then
  echo "WARNING: --vus=$MAX_VUS exceeds --warehouses=$WAREHOUSES; virtual users" \
       "will share home warehouses and contend on the same district rows." >&2
fi
if [[ -z "$BUILD_VUS" ]]; then
  BUILD_VUS="$(nproc)"
  (( BUILD_VUS > WAREHOUSES )) && BUILD_VUS=$WAREHOUSES
fi

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BYTECASK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PLUGIN_DIR="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin/build"
RESULTS_CSV="$SCRIPT_DIR/hammerdb_results.csv"

DATA_ROOT="${DATA_ROOT:-$BYTECASK_ROOT}"
mkdir -p "$DATA_ROOT" || { echo "ERROR: cannot create --data-root=$DATA_ROOT"; exit 1; }
DATA_ROOT="$(cd "$DATA_ROOT" && pwd)"
LOG_DIR="$DATA_ROOT/.hammerdb_logs"
mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
command -v mariadbd >/dev/null 2>&1 || { echo "ERROR: mariadbd not found"; exit 1; }

find_hammerdb() {
  if [[ -n "$HAMMERDB_HOME" ]]; then
    [[ -x "$HAMMERDB_HOME/hammerdbcli" ]] && { echo "$HAMMERDB_HOME"; return 0; }
    return 1
  fi
  local d
  for d in $(ls -d "$HOME"/HammerDB-* 2>/dev/null | sort -V -r); do
    [[ -x "$d/hammerdbcli" ]] && { echo "$d"; return 0; }
  done
  if command -v hammerdbcli >/dev/null 2>&1; then
    dirname "$(readlink -f "$(command -v hammerdbcli)")"
    return 0
  fi
  return 1
}
HAMMERDB_HOME="$(find_hammerdb)" || {
  echo "ERROR: hammerdbcli not found. Unpack a release from"
  echo "  https://github.com/TPC-Council/HammerDB/releases"
  echo "into ~/HammerDB-<version>, or pass --hammerdb-home=PATH."
  exit 1
}

# shellcheck source=lib_common.sh
source "$SCRIPT_DIR/lib_common.sh"

if [[ " ${ACTIVE_ENGINES[*]} " == *" bytecaskdb "* ]]; then
  build_bytecaskdb_plugin
fi

# ---------------------------------------------------------------------------
# Engine metadata
# ---------------------------------------------------------------------------
engine_dir() { echo "$DATA_ROOT/.mariadb_hammerdb_$1"; }

engine_port() {
  case "$1" in
    bytecaskdb) echo "$BYTECASKDB_PORT" ;;
    innodb)     echo "$INNODB_PORT" ;;
  esac
}

engine_label() {
  case "$1" in
    bytecaskdb) echo "ByteCaskDB" ;;
    innodb)     echo "InnoDB" ;;
  esac
}

start_engine() {
  local engine="$1"
  local dir; dir="$(engine_dir "$engine")"
  # One connection per VU plus HammerDB's monitor and our own status queries.
  local extra=(--max-connections=$(( (MAX_VUS > BUILD_VUS ? MAX_VUS : BUILD_VUS) + 16 )))
  if [[ "$engine" == bytecaskdb ]]; then
    symlink_providers "$PLUGIN_DIR"
    extra+=(--plugin-dir="$PLUGIN_DIR" --plugin-load-add=bytecaskdb=ha_bytecaskdb.so)
  fi
  start_mariadbd \
    "$dir/data" "$dir/mysql.sock" "$(engine_port "$engine")" \
    "$dir/mariadbd.pid" "$dir/error.log" "$SCRIPT_DIR/$engine.cnf" \
    "${extra[@]}"
}

stop_engine() {
  stop_mariadbd "$(engine_dir "$1")/mariadbd.pid"
}

# ---------------------------------------------------------------------------
# Cleanup trap
# ---------------------------------------------------------------------------
cleanup() {
  echo ""
  echo "=== Cleaning up ==="
  local engine
  for engine in "${ACTIVE_ENGINES[@]}"; do
    stop_engine "$engine"
    if [[ "$REUSE_DATA" == "off" ]]; then remove_instance "$(engine_dir "$engine")"; fi
  done
}
trap cleanup INT TERM

# ---------------------------------------------------------------------------
# HammerDB
# ---------------------------------------------------------------------------
# Connection settings shared by the build and run scripts.
hammerdb_preamble() {
  local engine="$1"
  local dir; dir="$(engine_dir "$engine")"
  cat <<TCL
dbset db maria
dbset bm TPC-C
diset connection maria_host 127.0.0.1
diset connection maria_port $(engine_port "$engine")
diset connection maria_socket $dir/mysql.sock
diset tpcc maria_user root
diset tpcc maria_pass $HAMMERDB_PASS
diset tpcc maria_dbase tpcc
TCL
}

# Runs a Tcl script through hammerdbcli, output to $2. hammerdbcli resolves
# its libraries relative to its own directory, hence the cd.
run_hammerdb() {
  local script="$1" log="$2"
  (cd "$HAMMERDB_HOME" && ./hammerdbcli auto "$script") > "$log" 2>&1 || true
}

# ---------------------------------------------------------------------------
# Phase 1 — build the schema once per engine and keep a pristine copy.
# ---------------------------------------------------------------------------
build_engine() {
  local engine="$1"
  local dir; dir="$(engine_dir "$engine")"
  local log="$LOG_DIR/build_${engine}_w${WAREHOUSES}.log"
  echo "--- $(engine_label "$engine"): building $WAREHOUSES warehouses with $BUILD_VUS VUs ---"
  init_datadir "$dir/data"
  rm -rf "$dir/pristine"
  start_engine "$engine"
  {
    hammerdb_preamble "$engine"
    cat <<TCL
diset tpcc maria_count_ware $WAREHOUSES
diset tpcc maria_num_vu $BUILD_VUS
diset tpcc maria_storage_engine $engine
diset tpcc maria_partition false
buildschema
TCL
  } > "$dir/build.tcl"
  local t0=$SECONDS
  run_hammerdb "$dir/build.tcl" "$log"
  if ! grep -q "TPCC SCHEMA COMPLETE" "$log"; then
    echo "  [FAILED] schema build; log: $log" >&2
    tail -20 "$log" >&2
    stop_engine "$engine"
    exit 1
  fi
  local engines
  engines="$(mariadb --socket="$dir/mysql.sock" -u root -sN -e \
    "SELECT GROUP_CONCAT(DISTINCT LOWER(engine)) FROM information_schema.tables WHERE table_schema='tpcc'")"
  if [[ "$engines" != "$engine" ]]; then
    echo "  [FAILED] tpcc tables use '$engines', expected '$engine'" >&2
    stop_engine "$engine"
    exit 1
  fi
  stop_engine "$engine"
  cp -a --reflink=auto "$dir/data" "$dir/pristine"
  echo "  built in $((SECONDS - t0))s ($(du -sh "$dir/pristine" | cut -f1))"
}

restore_engine() {
  local dir; dir="$(engine_dir "$1")"
  rm -rf "$dir/data"
  cp -a --reflink=auto "$dir/pristine" "$dir/data"
}

# ---------------------------------------------------------------------------
# Phase 2 — one measured cell.
# ---------------------------------------------------------------------------
run_bench() {
  local engine="$1" vus="$2"
  local dir; dir="$(engine_dir "$engine")"
  local log="$LOG_DIR/run_${engine}_w${WAREHOUSES}_vu${vus}.log"

  if [[ "$RESTORE" == "on" ]]; then restore_engine "$engine"; fi
  start_engine "$engine"

  {
    hammerdb_preamble "$engine"
    cat <<TCL
diset tpcc maria_driver timed
diset tpcc maria_rampup $RAMPUP
diset tpcc maria_duration $DURATION
diset tpcc maria_allwarehouse false
diset tpcc maria_timeprofile false
diset tpcc maria_raiseerror false
loadscript
vuset vu $vus
vucreate
vurun
vudestroy
TCL
  } > "$dir/run.tcl"

  # The timed driver runs ramp-up and the measured window as one vurun, so
  # the "before" samples are taken from the background when ramp-up ends.
  local marks="$dir/window_start"
  rm -f "$marks"
  (
    sleep $(( RAMPUP * 60 ))
    rss_reset "$dir/mariadbd.pid"
    { io_sample "$dir/mariadbd.pid"; engine_counters "$engine" "$dir/mysql.sock"; } > "$marks.tmp"
    mv "$marks.tmp" "$marks"
  ) &
  local sampler=$!

  run_hammerdb "$dir/run.tcl" "$log"

  wait "$sampler" 2>/dev/null || true
  local io_before="0 0 0 0" eng_before="0 0 0"
  if [[ -f "$marks" ]]; then
    io_before="$(sed -n 1p "$marks")"
    eng_before="$(sed -n 2p "$marks")"
  fi
  local eng_after io_after rss_cols io_cols eng_cols
  eng_after="$(engine_counters "$engine" "$dir/mysql.sock")"
  io_after="$(io_sample "$dir/mariadbd.pid")"
  rss_cols="$(rss_sample "$dir/mariadbd.pid")"
  io_cols="$(io_delta "$io_before" "$io_after")"
  eng_cols="$(eng_delta "$eng_before" "$eng_after")"

  local dev_before dev_after flush_mib
  dev_before="$(dev_written_bytes)"
  stop_engine "$engine"
  dev_after="$(dev_written_bytes)"
  flush_mib="$(awk -v b=$((dev_after - dev_before)) \
    'BEGIN { printf "%.1f", (b > 0 ? b : 0) / 1048576 }')"

  # "Vuser 1:TEST RESULT : System achieved 24510 NOPM from 61164 MariaDB TPM"
  local nopm tpm aborts errors
  read -r nopm tpm <<< "$(sed -n 's/.*System achieved \([0-9]*\) NOPM from \([0-9]*\) .*TPM.*/\1 \2/p' "$log" | tail -1)"
  aborts="$(grep -c "Deadlock found" "$log" || true)"
  errors="$(grep -E "mariaexec|Error in Virtual User|FINISHED FAILED" "$log" |
    grep -v -c "Deadlock found" || true)"

  if [[ -z "$nopm" ]]; then
    echo "  [FAILED] no TEST RESULT line; log: $log" >&2
    tail -20 "$log" >&2
  fi

  echo "$engine,$WAREHOUSES,$vus,$nopm,$tpm,$aborts,$errors,$io_cols,$eng_cols,$flush_mib,$rss_cols"
}

find_result() {
  local r
  for r in "${ALL_RESULTS[@]}"; do
    if [[ "$(cut -d, -f1 <<< "$r")" == "$1" && "$(cut -d, -f3 <<< "$r")" == "$2" ]]; then
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
echo "=== HammerDB TPROC-C Benchmark ==="
echo "    HammerDB: $HAMMERDB_HOME"
echo "    Engines: ${ACTIVE_ENGINES[*]}"
echo "    Warehouses: $WAREHOUSES | Ramp-up: ${RAMPUP} min | Duration: ${DURATION} min"
echo "    Virtual users: ${VUS}"
echo "    Restore per cell: $RESTORE"
echo "    Data root: $DATA_ROOT"
echo ""

echo "=== Phase 1: building schema ==="
for engine in "${ACTIVE_ENGINES[@]}"; do
  # A server orphaned by an aborted run still holds the data directory.
  stop_engine "$engine"
  if [[ "$REUSE_DATA" == "on" && -d "$(engine_dir "$engine")/pristine/tpcc" ]]; then
    echo "--- $(engine_label "$engine"): reusing the schema in $(engine_dir "$engine")/pristine ---"
  else
    build_engine "$engine"
  fi
done
echo ""

echo "=== Phase 2: running TPROC-C ==="
echo "engine,warehouses,vus,nopm,tpm,aborts,other_errors,read_mib,write_mib,syscr,syscw,eng_write_mib,eng_fsyncs,flush_mib,rss_mib,peak_rss_mib" > "$RESULTS_CSV"
declare -a ALL_RESULTS=()

for v in "${VU_LIST[@]}"; do
  echo "--- vus=$v ---"
  for engine in "${ACTIVE_ENGINES[@]}"; do
    printf '  %-12s' "$(engine_label "$engine"):"
    result="$(run_bench "$engine" "$v")"
    echo "$result" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result")
    printf '%8s NOPM | %8s TPM | aborts %6s | write %8s MiB | rss %8s MiB\n' \
      "$(cut -d, -f4 <<< "$result")" "$(cut -d, -f5 <<< "$result")" \
      "$(cut -d, -f6 <<< "$result")" "$(cut -d, -f9 <<< "$result")" \
      "$(cut -d, -f15 <<< "$result")"
  done
  echo ""
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
fmt="%4s"
args=("VUs")
for engine in "${ACTIVE_ENGINES[@]}"; do
  fmt+=" | %-10s %9s %9s %7s %9s %9s %9s"
  args+=("$(engine_label "$engine")" "NOPM" "TPM" "aborts" "wMiB" "flMiB" "RSS MiB")
done
# shellcheck disable=SC2059
printf "$fmt\n" "${args[@]}"
for v in "${VU_LIST[@]}"; do
  fmt="%4s"
  args=("$v")
  for engine in "${ACTIVE_ENGINES[@]}"; do
    line="$(find_result "$engine" "$v")"
    fmt+=" | %-10s %9s %9s %7s %9s %9s %9s"
    args+=("" "$(cut -d, -f4 <<< "$line")" "$(cut -d, -f5 <<< "$line")" \
           "$(cut -d, -f6 <<< "$line")" "$(cut -d, -f9 <<< "$line")" \
           "$(cut -d, -f14 <<< "$line")" "$(cut -d, -f15 <<< "$line")")
  done
  # shellcheck disable=SC2059
  printf "$fmt\n" "${args[@]}"
done

echo ""
echo "Results saved to: $RESULTS_CSV"
echo "HammerDB logs:    $LOG_DIR"

cleanup
