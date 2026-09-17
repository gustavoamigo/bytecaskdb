#!/usr/bin/env bash
# Memory pressure benchmark: InnoDB vs ByteCaskDB (mmap) vs ByteCaskDB (buffer pool)
#
# Runs sysbench OLTP workloads against all three under the compose memory
# limit of 2.5 GiB per server. The default 10 M rows are ~3.2 GB of
# ByteCaskDB data files plus ~1.1 GB of key directory, so the page cache
# has about 1.2 GB for 3.2 GB of files: the pool (512 MiB) is the only
# back-end whose memory the kernel cannot reclaim. TABLE_SIZE is taken from
# the environment; below ~6 M rows everything fits and nothing is measured.
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
TABLE_SIZE="${TABLE_SIZE:-10000000}"
THREADS="1 8 16"
WARMUP=30
DURATION=60
WORKLOADS="oltp_point_select oltp_read_only oltp_write_only oltp_read_write"

INNODB_HOST=innodb
BYTECASKDB_HOST=bytecaskdb
BYTECASKDB_POOL_HOST=bytecaskdb-pool
DB_PORT=3306
DB_USER=root
DB_NAME=sbtest

RESULTS_CSV=/results/results.csv

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { echo "[$(date +%H:%M:%S)] $*"; }

wait_for_db() {
  local host="$1"
  local tries=0
  while ! mariadb -h "$host" -P "$DB_PORT" -u "$DB_USER" -e "SELECT 1" >/dev/null 2>&1; do
    sleep 2
    tries=$((tries + 1))
    if [[ $tries -ge 60 ]]; then
      echo "ERROR: $host did not become ready within 120s"
      exit 1
    fi
  done
}


sysbench_args() {
  local host="$1"
  local threads="$2"
  echo "--db-driver=mysql --mysql-host=$host --mysql-port=$DB_PORT --mysql-user=$DB_USER --mysql-password= --mysql-db=$DB_NAME --tables=1 --table-size=$TABLE_SIZE --threads=$threads --report-interval=0 --mysql-ignore-errors=1180,1213"
}

run_measurement() {
  local engine="$1"
  local workload="$2"
  local host="$3"
  local threads="$4"

  local args
  args="$(sysbench_args "$host" "$threads")"

  # Warmup (discard results)
  sysbench "$workload" $args --time=$WARMUP run >/dev/null 2>&1

  # Measurement
  local output
  output="$(sysbench "$workload" $args --time=$DURATION run 2>&1)"

  local tps avg_lat p95
  tps="$(echo "$output" | grep "transactions:" | awk -F'[( ]+' '{print $4}')"  # per-second figure, not the run total
  avg_lat="$(echo "$output" | grep "avg:" | tail -1 | awk '{print $2}')"
  p95="$(echo "$output" | grep "95th percentile:" | awk '{print $NF}')"

  if [[ -z "$tps" ]]; then
    log "  [FAILED] $engine $workload t=$threads"
    echo "$output" | tail -20 >&2
    tps="0"; avg_lat="0"; p95="0"
  fi

  echo "$engine,$workload,$threads,$tps,$avg_lat,$p95"
}

# ---------------------------------------------------------------------------
# Wait for databases
# ---------------------------------------------------------------------------
log "Waiting for InnoDB..."
wait_for_db "$INNODB_HOST"
log "Waiting for ByteCaskDB..."
wait_for_db "$BYTECASKDB_HOST"
log "Waiting for ByteCaskDB (buffer pool)..."
wait_for_db "$BYTECASKDB_POOL_HOST"
log "All databases ready."


# ---------------------------------------------------------------------------
# Prepare data (5M rows on each)
# ---------------------------------------------------------------------------
log "Preparing InnoDB (${TABLE_SIZE} rows)..."
mariadb -h "$INNODB_HOST" -P "$DB_PORT" -u "$DB_USER" "$DB_NAME" -e "DROP TABLE IF EXISTS sbtest1"
sysbench oltp_read_write $(sysbench_args "$INNODB_HOST" 4) \
  --mysql_storage_engine=innodb prepare

# ByteCaskDB: custom prepare to avoid copy-ALTER OOM during CREATE INDEX.
# Creates the table WITH the secondary index upfront, then inserts in batches.
# Each batch is a separate transaction (~10MB write buffer), avoiding the giant
# single-transaction copy-ALTER that sysbench's default prepare triggers.
prepare_bytecaskdb() {
  local host="$1"
  log "Preparing $host (${TABLE_SIZE} rows)..."
  mariadb -h "$host" -P "$DB_PORT" -u "$DB_USER" "$DB_NAME" -e "
    DROP TABLE IF EXISTS sbtest1;
    CREATE TABLE sbtest1 (
      id INT NOT NULL AUTO_INCREMENT,
      k INT NOT NULL DEFAULT 0,
      c CHAR(120) NOT NULL DEFAULT '',
      pad CHAR(60) NOT NULL DEFAULT '',
      PRIMARY KEY (id),
      KEY k_1 (k)
    ) ENGINE=bytecaskdb;"

  BATCH_SIZE=50000
  inserted=0
  while [ $inserted -lt $TABLE_SIZE ]; do
    remaining=$((TABLE_SIZE - inserted))
    this_batch=$BATCH_SIZE
    if [ $this_batch -gt $remaining ]; then this_batch=$remaining; fi
    seq_from=$((inserted + 1))
    seq_to=$((inserted + this_batch))
    mariadb -h "$host" -P "$DB_PORT" -u "$DB_USER" "$DB_NAME" -e "
      INSERT INTO sbtest1 (k, c, pad)
      SELECT FLOOR(RAND() * $TABLE_SIZE),
             LPAD(FLOOR(RAND() * 1e18), 120, '0'),
             LPAD(FLOOR(RAND() * 1e18), 60, '0')
      FROM seq_${seq_from}_to_${seq_to};"
    inserted=$((inserted + this_batch))
    if [ $((inserted % 500000)) -eq 0 ]; then
      log "  $host: $inserted / $TABLE_SIZE rows"
    fi
  done

}
prepare_bytecaskdb "$BYTECASKDB_HOST"
prepare_bytecaskdb "$BYTECASKDB_POOL_HOST"

log "Data preparation complete."



# ---------------------------------------------------------------------------
# Run benchmarks
# ---------------------------------------------------------------------------
echo "engine,workload,threads,tps,avg_lat_ms,p95_ms" > "$RESULTS_CSV"

declare -a ALL_RESULTS=()

for workload in $WORKLOADS; do
  for t in $THREADS; do
    log "--- $workload | threads=$t ---"

    log "  InnoDB: running (warmup=${WARMUP}s, measure=${DURATION}s)..."
    result_in="$(run_measurement innodb "$workload" "$INNODB_HOST" "$t")"
    echo "$result_in" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result_in")
    log "  InnoDB: $(echo "$result_in" | cut -d, -f4) tps"

    log "  ByteCaskDB: running (warmup=${WARMUP}s, measure=${DURATION}s)..."
    result_bc="$(run_measurement bytecaskdb "$workload" "$BYTECASKDB_HOST" "$t")"
    echo "$result_bc" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result_bc")
    log "  ByteCaskDB: $(echo "$result_bc" | cut -d, -f4) tps"

    log "  ByteCaskDB (buffer pool): running (warmup=${WARMUP}s, measure=${DURATION}s)..."
    result_bp="$(run_measurement bytecaskdb-pool "$workload" "$BYTECASKDB_POOL_HOST" "$t")"
    echo "$result_bp" >> "$RESULTS_CSV"
    ALL_RESULTS+=("$result_bp")
    log "  ByteCaskDB (buffer pool): $(echo "$result_bp" | cut -d, -f4) tps"

    echo ""
  done
done

# ---------------------------------------------------------------------------
# Print comparison table
# ---------------------------------------------------------------------------
log "=== Results ==="
echo ""
printf "%-20s %4s | %10s %8s %8s | %10s %8s %8s | %10s %8s %8s\n" \
  "Workload" "Thr" "InnoDB tps" "avg" "p95" "BC mmap" "avg" "p95" "BC pool" "avg" "p95"
printf "%s\n" "$(printf '%.0s-' {1..110})"

for workload in $WORKLOADS; do
  for t in $THREADS; do
    in_line="" bc_line="" bp_line=""
    for r in "${ALL_RESULTS[@]}"; do
      eng="$(echo "$r" | cut -d, -f1)"
      wl="$(echo "$r" | cut -d, -f2)"
      thr="$(echo "$r" | cut -d, -f3)"
      if [[ "$wl" == "$workload" && "$thr" == "$t" ]]; then
        if [[ "$eng" == "innodb" ]]; then in_line="$r"; fi
        if [[ "$eng" == "bytecaskdb" ]]; then bc_line="$r"; fi
        if [[ "$eng" == "bytecaskdb-pool" ]]; then bp_line="$r"; fi
      fi
    done

    in_tps="$(echo "$in_line" | cut -d, -f4)"
    in_avg="$(echo "$in_line" | cut -d, -f5)"
    in_p95="$(echo "$in_line" | cut -d, -f6)"
    bc_tps="$(echo "$bc_line" | cut -d, -f4)"
    bc_avg="$(echo "$bc_line" | cut -d, -f5)"
    bc_p95="$(echo "$bc_line" | cut -d, -f6)"
    bp_tps="$(echo "$bp_line" | cut -d, -f4)"
    bp_avg="$(echo "$bp_line" | cut -d, -f5)"
    bp_p95="$(echo "$bp_line" | cut -d, -f6)"

    printf "%-20s %4s | %10s %8s %8s | %10s %8s %8s | %10s %8s %8s\n" \
      "$workload" "$t" "$in_tps" "$in_avg" "$in_p95" "$bc_tps" "$bc_avg" "$bc_p95" "$bp_tps" "$bp_avg" "$bp_p95"
  done
done

echo ""
log "Results saved to: $RESULTS_CSV"
