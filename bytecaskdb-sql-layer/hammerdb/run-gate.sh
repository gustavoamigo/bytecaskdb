#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# The MVP gate (docs/server_design.md, *Running the gate*): HammerDB's
# TPROC-C, through bcdboltp.tcl (or, with --driver=custom, the older
# bytecask_tprocc.tcl), against sql_layer_server, at each
# user count, each cell on a fresh copy of one load, each ending with the
# consistency conditions checked on what it left. It follows
# bytecaskdb-mariadb-plugin/benchmarks/run-hammerdb.sh.
#
# Usage:
#   bytecaskdb-sql-layer/hammerdb/run-gate.sh [--warehouses=W] [--vus=1,16,48]
#       [--rampup=MIN] [--duration=MIN] [--mix="neword payment ostat delivery slev"]
#       [--driver=bcdboltp|custom] [--hammerdb-home=PATH] [--data-root=PATH]
#       [--server-flags="--sync AT_INTERVAL --sync-interval-ms 1000"]
#       [--smoke --pg="libpq conninfo"]
#
#   --warehouses: warehouses loaded (default 48). Every user count must be at
#                 most W: each user has its own home warehouse.
#   --vus:        comma-separated user counts (default 1,16,48).
#   --rampup:     unmeasured minutes before the window (default 2).
#   --duration:   measured minutes (default 5).
#   --mix:        the transaction types the driver runs (default all five).
#   --hammerdb-home: where hammerdbcli is (default $HAMMERDB_HOME, the newest
#                 ~/HammerDB-*, or PATH). HammerDB 6.0's Linux release bundles Tcl and pgtcl.
#   --driver:     bcdboltp (default): HammerDB 6.0's PostgreSQL stored-procedure
#                 driver logic, each transaction one CALL of a procedure of
#                 tprocc_procs.sql, which the load creates; or custom: the
#                 client-side bytecask_tprocc.tcl, until it is removed (#758).
#   --data-root:  where the load and each cell's copy live (default a temporary
#                 directory, removed at exit).
#   --server-flags: more flags for sql_layer_server in every cell (default
#                 none, so AT_EVERY_COMMIT, which on a SATA SSD mostly measures
#                 fdatasync). Runs compared with each other use
#                 "--sync AT_INTERVAL --sync-interval-ms 1000".
#   --smoke:      the nightly smoke: 2 warehouses, 2 users, no ramp-up, a
#                 one-minute window; and the same run on PostgreSQL, through
#                 --pg, to check the driver itself (its consistency, never its
#                 numbers: the gate reports ByteCaskDB's alone).
#                 --pg is a libpq conninfo to a server where the user may
#                 create a database; its password goes in PGPASSWORD, which
#                 the driver's libpq reads too.
#   XMAKE_FLAGS:  more `xmake f` flags for the build (CI's toolchain).
#
# Each cell appends a row to bytecaskdb-sql-layer/benchmarks/hammerdb_results.csv
# (ignored by git), with the driver, the server flags and the data directory's
# size when the server stopped. A cell whose check fails, or whose run printed
# no result, fails the script, whatever its NOPM. The build's output is
# build.log in the data root.

set -euo pipefail

LAYER="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$LAYER/.." && pwd)"
WAREHOUSES=48
DRIVER=bcdboltp
VUS=1,16,48
RAMPUP=2
DURATION=5
MIX="neword payment ostat delivery slev"
HAMMERDB_HOME="${HAMMERDB_HOME:-}"
DATA_ROOT=""
SERVER_FLAGS=()
SMOKE=0
PG=""

for arg in "$@"; do
  case "$arg" in
    --warehouses=*) WAREHOUSES="${arg#*=}" ;;
    --vus=*) VUS="${arg#*=}" ;;
    --rampup=*) RAMPUP="${arg#*=}" ;;
    --duration=*) DURATION="${arg#*=}" ;;
    --mix=*) MIX="${arg#*=}" ;;
    --driver=*) DRIVER="${arg#*=}" ;;
    --hammerdb-home=*) HAMMERDB_HOME="${arg#*=}" ;;
    --data-root=*) DATA_ROOT="${arg#*=}" ;;
    --server-flags=*) read -r -a SERVER_FLAGS <<<"${arg#*=}" ;;
    --smoke) SMOKE=1 ;;
    --pg=*) PG="${arg#*=}" ;;
    *) echo "run-gate.sh: unknown argument $arg" >&2; exit 2 ;;
  esac
done
case "$DRIVER" in
  bcdboltp) DRIVER_FILE="bcdboltp.tcl" ;;
  custom) DRIVER_FILE="bytecask_tprocc.tcl" ;;
  *) echo "run-gate.sh: --driver is bcdboltp or custom" >&2; exit 2 ;;
esac
if ((SMOKE)); then
  WAREHOUSES=2 VUS=2 RAMPUP=0 DURATION=1
  [[ -n "$PG" ]] || { echo "run-gate.sh: --smoke needs --pg, the PostgreSQL the driver is checked on" >&2; exit 2; }
  command -v psql >/dev/null || { echo "run-gate.sh: --smoke needs psql" >&2; exit 2; }
fi
IFS=, read -r -a VU_LIST <<<"$VUS"
for vu in "${VU_LIST[@]}"; do
  ((vu >= 1 && vu <= WAREHOUSES)) || { echo "run-gate.sh: $vu users need at least $vu warehouses (--warehouses=$WAREHOUSES)" >&2; exit 2; }
done

find_hammerdb() {
  if [[ -n "$HAMMERDB_HOME" ]]; then
    [[ -x "$HAMMERDB_HOME/hammerdbcli" ]] && { echo "$HAMMERDB_HOME"; return 0; }
    return 1
  fi
  # By version, newest first: glob order would put 10.0 before 9.0.
  local d
  while IFS= read -r d; do
    [[ -x "$d/hammerdbcli" ]] && { echo "$d"; return 0; }
  done < <(printf '%s\n' "$HOME"/HammerDB-* | sort -rV)
  command -v hammerdbcli >/dev/null 2>&1 && { dirname "$(readlink -f "$(command -v hammerdbcli)")"; return 0; }
  return 1
}
HAMMERDB_HOME="$(find_hammerdb)" || {
  echo "run-gate.sh: hammerdbcli not found: unpack HammerDB 6.0's Linux release from" >&2
  echo "https://github.com/TPC-Council/HammerDB/releases and pass --hammerdb-home" >&2
  exit 2
}

# Here, before a build and a load that take minutes: a header this script does
# not know would misalign every row it appended. A row from before the three
# new columns has them empty, which is "not recorded": a row since always names
# its driver.
CSV="$LAYER/benchmarks/hammerdb_results.csv"
CSV_HEADER_V1="date,commit,warehouses,users,rampup_min,duration_min,nopm,tpm,errors,check"
CSV_HEADER="$CSV_HEADER_V1,driver,server_flags,final_data_bytes"
mkdir -p "$(dirname "$CSV")"
if [[ ! -s "$CSV" ]]; then
  echo "$CSV_HEADER" >"$CSV"
else
  case "$(head -1 "$CSV")" in
    "$CSV_HEADER") ;;
    "$CSV_HEADER_V1")
      sed -e "1c\\$CSV_HEADER" -e '1!s/$/,,,/' "$CSV" >"$CSV.tmp"
      mv "$CSV.tmp" "$CSV"
      ;;
    *) echo "run-gate.sh: $CSV has a header this script does not know; move it aside" >&2; exit 2 ;;
  esac
fi

if [[ -z "$DATA_ROOT" ]]; then
  DATA_ROOT="$(mktemp -d -t bytecask_gate_XXXXXX)"
  trap 'rm -rf "$DATA_ROOT"' EXIT
fi
mkdir -p "$DATA_ROOT"

# releasedbg: a debug engine walks the key directory on every commit.
# XMAKE_FLAGS: more `xmake f` flags, as CI configures its toolchain.
# -P on each call: from a worktree under .claude/worktrees, plain xmake resolves
# to the checkout that holds it, and builds that tree. -v puts this tree's paths
# in the log.
echo "== building sql_layer_tprocc and sql_layer_server (releasedbg) in $ROOT"
read -r -a xmake_flags <<<"${XMAKE_FLAGS:-}"
build_log="$DATA_ROOT/build.log"
if ! (xmake f -P "$ROOT" -m releasedbg --sql_layer=y "${xmake_flags[@]}" -y &&
      xmake build -P "$ROOT" -v -y sql_layer_tprocc && xmake build -P "$ROOT" -v -y sql_layer_server) >"$build_log" 2>&1; then
  tail -80 "$build_log" >&2
  echo "run-gate.sh: the build failed" >&2
  exit 1
fi
BIN="$ROOT/build/linux/$(uname -m)/releasedbg"
TPROCC="$BIN/sql_layer_tprocc"
SERVER="$BIN/sql_layer_server"

PRISTINE="$DATA_ROOT/pristine_w${WAREHOUSES}_$DRIVER"
if [[ ! -d "$PRISTINE" ]]; then
  echo "== loading $WAREHOUSES warehouses"
  sql_out=()
  ((SMOKE)) && sql_out=(--sql-out "$DATA_ROOT/pristine_w$WAREHOUSES.sql")
  "$TPROCC" load --dir "$PRISTINE" --warehouses "$WAREHOUSES" "${sql_out[@]}"
  # The procedures bcdboltp.tcl calls, created once, in the load every cell copies.
  if [[ "$DRIVER" == bcdboltp ]]; then "$TPROCC" procs --dir "$PRISTINE" --file "$LAYER/hammerdb/tprocc_procs.sql"; fi
fi

free_port() { python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'; }

# The driver with this cell's options in its header, as HammerDB's editor
# writes them.
driver() {  # host port user db warehouses run engine out
  sed -e "s|^set rampup [0-9]* |set rampup $RAMPUP |" \
      -e "s|^set duration [0-9]* |set duration $DURATION |" \
      -e "s|^set host \"[^\"]*\"|set host \"$1\"|" \
      -e "s|^set port \"[^\"]*\"|set port \"$2\"|" \
      -e "s|^set user \"[^\"]*\"|set user \"$3\"|" \
      -e "s|^set db \"[^\"]*\"|set db \"$4\"|" \
      -e "s|^set warehouses [0-9]* |set warehouses $5 |" \
      -e "s|^set run [0-9]* |set run $6 |" \
      -e "s|^set mix {[^}]*}|set mix {$MIX}|" \
      -e "s|^set engine \"[^\"]*\"|set engine \"$7\"|" \
      "$LAYER/hammerdb/$DRIVER_FILE" >"$8"
}

# Runs the driver with hammerdbcli; its output to $4.
hammerdb() {  # driver-file port vus log
  local cli="$DATA_ROOT/hammerdb_$$.tcl"
  cat >"$cli" <<EOF
dbset db pg
dbset bm TPC-C
diset connection pg_host 127.0.0.1
diset connection pg_port $2
diset connection pg_sslmode disable
customscript $1
custommonitor timed
vuset logtotemp 0
vuset vu $3
vucreate
vurun
vudestroy
exit
EOF
  (cd "$HAMMERDB_HOME" && ./hammerdbcli auto "$cli") >"$4" 2>&1 || true
}

result() {  # log -> "NOPM TPM" or nothing
  sed -n 's/.*System achieved \([0-9]*\) NOPM from \([0-9]*\) .* TPM.*/\1 \2/p' "$1" | tail -1
}

# A checkout without .git (CI's, when git was not installed first) has
# GITHUB_SHA instead.
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo "${GITHUB_SHA:0:7}")"
FAILED=0
FLAGS_TEXT="${SERVER_FLAGS[*]:-}"

for vu in "${VU_LIST[@]}"; do
  cell="$DATA_ROOT/cell"
  rm -rf "$cell"
  cp -a --reflink=auto "$PRISTINE" "$cell"
  run="$("$TPROCC" next-run --dir "$cell")"
  port="$(free_port)"
  "$SERVER" --dir "$cell" --listen "127.0.0.1:$port" --database tprocc ${SERVER_FLAGS[@]+"${SERVER_FLAGS[@]}"} >"$DATA_ROOT/server.log" 2>&1 &
  server=$!
  for _ in $(seq 100); do grep -q "ready on" "$DATA_ROOT/server.log" && break; sleep 0.1; done
  grep -q "ready on" "$DATA_ROOT/server.log" || { cat "$DATA_ROOT/server.log" >&2; exit 1; }
  echo "== $vu users, $WAREHOUSES warehouses, ${RAMPUP}+${DURATION} min"
  driver 127.0.0.1 "$port" tprocc tprocc "$WAREHOUSES" "$run" ByteCaskDB "$DATA_ROOT/driver.tcl"
  hammerdb "$DATA_ROOT/driver.tcl" "$port" "$vu" "$DATA_ROOT/hammerdb.log"
  kill -TERM "$server"
  wait "$server" || { echo "sql_layer_server exited with $?" >&2; FAILED=1; }
  read -r nopm tpm <<<"$(result "$DATA_ROOT/hammerdb.log")" || true
  errors="$(sed -n 's/.*Errors by SQLSTATE: \(.*\)/\1/p' "$DATA_ROOT/hammerdb.log" | tail -1)"
  # Apparent size, so a reflinked copy of the load counts in full.
  data_bytes="$(du -sb "$cell" | cut -f1)"
  if "$TPROCC" check --dir "$cell"; then check=ok; else check=failed; FAILED=1; fi
  if [[ -z "${nopm:-}" ]]; then
    echo "no result: the driver's output follows" >&2
    tail -40 "$DATA_ROOT/hammerdb.log" >&2
    FAILED=1
  fi
  echo "   NOPM ${nopm:-none}, TPM ${tpm:-none}, errors ${errors:-unknown}, check $check, driver $DRIVER, server flags ${FLAGS_TEXT:-none}, final data $(numfmt --to=iec-i --suffix=B --format=%.1f "$data_bytes")"
  echo "$(date -u +%FT%TZ),$COMMIT,$WAREHOUSES,$vu,$RAMPUP,$DURATION,${nopm:-},${tpm:-},\"${errors:-}\",$check,$DRIVER,\"${FLAGS_TEXT//\"/\"\"}\",$data_bytes" >>"$CSV"
  unset nopm tpm
done

if ((SMOKE)); then
  # The driver on PostgreSQL, on the same population: it must leave the
  # conditions holding there too. PostgreSQL's numbers are not reported.
  echo "== the driver on PostgreSQL"
  db="bytecask_gate_$$"
  psql "$PG" -X -q -v ON_ERROR_STOP=1 -c "CREATE DATABASE $db"
  pg_db="$PG dbname=$db"
  psql "$pg_db" -X -q -v ON_ERROR_STOP=1 -f "$DATA_ROOT/pristine_w$WAREHOUSES.sql"
  if [[ "$DRIVER" == bcdboltp ]]; then psql "$pg_db" -X -q -v ON_ERROR_STOP=1 -f "$LAYER/hammerdb/tprocc_procs.sql"; fi
  host="$(psql "$pg_db" -X -At -c "SELECT coalesce(host(inet_server_addr()), '127.0.0.1')")"
  pg_port="$(psql "$pg_db" -X -At -c "SHOW port")"
  pg_user="$(psql "$pg_db" -X -At -c "SELECT current_user")"
  driver "$host" "$pg_port" "$pg_user" "$db" "$WAREHOUSES" 1 PostgreSQL "$DATA_ROOT/driver_pg.tcl"
  hammerdb "$DATA_ROOT/driver_pg.tcl" "$pg_port" "${VU_LIST[0]}" "$DATA_ROOT/hammerdb_pg.log"
  errors="$(sed -n 's/.*Errors by SQLSTATE: \(.*\)/\1/p' "$DATA_ROOT/hammerdb_pg.log" | tail -1)"
  broken="$(psql "$pg_db" -X -q -At -v ON_ERROR_STOP=1 -f "$LAYER/hammerdb/pg_consistency.sql")"
  psql "$PG" -X -q -c "DROP DATABASE $db"
  if [[ -z "$(result "$DATA_ROOT/hammerdb_pg.log")" ]]; then
    echo "the driver printed no result on PostgreSQL:" >&2
    tail -40 "$DATA_ROOT/hammerdb_pg.log" >&2
    FAILED=1
  elif [[ -n "$broken" ]]; then
    echo "consistency on PostgreSQL: $broken" >&2
    FAILED=1
  else
    echo "   on PostgreSQL: errors ${errors:-unknown}, the nine conditions hold"
  fi
fi

exit "$FAILED"
