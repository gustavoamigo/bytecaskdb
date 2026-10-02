#!/usr/bin/env bash
# Records a pool trace (BYTECASK_POOL_TRACE) of sysbench against the MariaDB
# plugin: one server process for the load and the run, because file ids are
# reassigned at recovery and the trace has to name the same files throughout.
# Writes <out>/trace.bin, <out>/phases.txt (CLOCK_MONOTONIC ns of each phase
# start) and the sysbench output.
#
#   record.sh --out=DIR [--rows=1000000] [--threads=8] [--time=120]
#             [--warmup=30] [--vacuum=0.9] [--file-mib=16] [--workload=oltp_read_write]
#             [--rand-type=uniform]   (sysbench's key distribution: uniform, pareto, zipfian...)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="" ROWS=1000000 THREADS=8 TIME=120 WARMUP=30 VACUUM=0.9 FILE_MIB=16
WORKLOAD=oltp_read_write PORT=3391 RAND=uniform
for a in "$@"; do case "$a" in
  --out=*) OUT="${a#*=}" ;; --rows=*) ROWS="${a#*=}" ;; --threads=*) THREADS="${a#*=}" ;;
  --time=*) TIME="${a#*=}" ;; --warmup=*) WARMUP="${a#*=}" ;; --vacuum=*) VACUUM="${a#*=}" ;;
  --file-mib=*) FILE_MIB="${a#*=}" ;; --workload=*) WORKLOAD="${a#*=}" ;;
  --rand-type=*) RAND="${a#*=}" ;;
  *) echo "unknown: $a"; exit 1 ;; esac; done
[[ -n "$OUT" ]] || { echo "--out=DIR required"; exit 1; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
PLUGIN_DIR="$ROOT/bytecaskdb-mariadb-plugin/build"
cmake -S "$ROOT/bytecaskdb-mariadb-plugin" -B "$PLUGIN_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$PLUGIN_DIR" --parallel --target ha_bytecaskdb >/dev/null
for f in /usr/lib64/mariadb/plugin/provider_*; do [[ -e "$f" ]] && ln -sf "$f" "$PLUGIN_DIR/"; done

D="$OUT/inst"; rm -rf "$D"; mkdir -p "$D/data" "$D/tmp"
mariadb-install-db --datadir="$D/data" --auth-root-authentication-method=normal >/dev/null 2>&1
cat > "$D/my.cnf" <<CNF
[mariadbd]
bytecaskdb_sync                           = AT_INTERVAL
bytecaskdb_sync_interval_ms               = 1000
bytecaskdb_io_backend                     = buffer_pool
bytecaskdb_buffer_pool_size               = 2147483648
bytecaskdb_buffer_pool_direct_io          = OFF
bytecaskdb_max_file_bytes                 = $((FILE_MIB * 1048576))
bytecaskdb_verify_checksums               = OFF
bytecaskdb_vacuum_fragmentation_threshold = $VACUUM
bytecaskdb_vacuum_busy_interval_ms        = 1000
bytecaskdb_vacuum_idle_interval_ms        = 30000
innodb_buffer_pool_size                   = 64M
max_connections                           = 64
CNF
now() { python3 -c 'import time; print(time.monotonic_ns())'; }
: > "$OUT/phases.txt"
echo "start $(now)" >> "$OUT/phases.txt"
BYTECASK_POOL_TRACE="$OUT/trace.bin" mariadbd --defaults-extra-file="$D/my.cnf" \
  --datadir="$D/data" --socket="$D/s.sock" --port=$PORT --pid-file="$D/pid" \
  --skip-grant-tables --skip-log-bin --performance-schema=OFF --tmpdir="$D/tmp" \
  --log-error="$D/error.log" --plugin-dir="$PLUGIN_DIR" \
  --plugin-load-add=bytecaskdb=ha_bytecaskdb.so &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for _ in $(seq 120); do mariadb --socket="$D/s.sock" -u root -e 'SELECT 1' >/dev/null 2>&1 && break; sleep 1; done
mariadb --socket="$D/s.sock" -u root -e 'CREATE DATABASE sbtest'
SB=(--db-driver=mysql --mysql-socket="$D/s.sock" --mysql-user=root --mysql-db=sbtest
    --tables=1 --table_size="$ROWS")
echo "load $(now)" >> "$OUT/phases.txt"
sysbench "$WORKLOAD" "${SB[@]}" --threads=1 --mysql_storage_engine=bytecaskdb prepare > "$OUT/prepare.log"
echo "warmup $(now)" >> "$OUT/phases.txt"
sysbench "$WORKLOAD" "${SB[@]}" --threads="$THREADS" --time="$WARMUP" --report-interval=0 --rand-type="$RAND" \
  --mysql-ignore-errors=1180,1213 run > "$OUT/warmup.log"
echo "run $(now)" >> "$OUT/phases.txt"
sysbench "$WORKLOAD" "${SB[@]}" --threads="$THREADS" --time="$TIME" --report-interval=10 --rand-type="$RAND" \
  --mysql-ignore-errors=1180,1213 run > "$OUT/run.log"
echo "end $(now)" >> "$OUT/phases.txt"
mariadb --socket="$D/s.sock" -u root -e 'SHOW ENGINE BYTECASKDB STATUS\G' > "$OUT/status_end.txt" || true
du -sb "$D/data/bytecaskdb" > "$OUT/data_bytes.txt" || true
mariadb-admin --socket="$D/s.sock" -u root shutdown || kill $SRV
wait $SRV 2>/dev/null || true
trap - EXIT
grep -E 'transactions:' "$OUT/run.log"
ls -la "$OUT/trace.bin"
