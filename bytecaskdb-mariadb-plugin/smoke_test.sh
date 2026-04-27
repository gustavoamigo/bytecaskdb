#!/usr/bin/env bash
# bytecaskdb-mariadb-plugin/smoke_test.sh — Self-contained smoke test for the ByteCaskDB MariaDB
# storage-engine plugin.
#
# Starts a local MariaDB instance (no root required), builds the plugin,
# loads it, runs basic SQL operations, and tears everything down.
#
# Prerequisites:
#   - mariadbd and mariadb-install-db on PATH (mariadb-server package).
#   - MariaDB development headers (mariadb-devel / libmariadb-dev).
#   - libbytecask.a already built:  cd <root> && xmake build bytecask
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/smoke_test.sh          # default — starts local instance
#   ./bytecaskdb-mariadb-plugin/smoke_test.sh --keep   # leave the local instance running after tests

set -euo pipefail

BYTECASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BYTECASK_ROOT}/bytecaskdb-mariadb-plugin/build"
LOCAL_DIR="${BYTECASK_ROOT}/.mariadb_local"
SOCK="${LOCAL_DIR}/mysql.sock"
PID_FILE="${LOCAL_DIR}/mariadbd.pid"
LOG_FILE="${LOCAL_DIR}/error.log"
PORT=3307
TEST_DB="bytecask_smoke_test"
KEEP=false

for arg in "$@"; do
  case "$arg" in
    --keep) KEEP=true ;;
    *) echo "Unknown option: $arg"; exit 1 ;;
  esac
done

passed=0
failed=0

run_sql() {
  mariadb --socket="${SOCK}" --batch --skip-column-names -e "$1" 2>&1
}

check() {
  local description="$1"
  local expected="$2"
  local actual="$3"

  if [[ "$actual" == *"$expected"* ]]; then
    echo "  PASS: ${description}"
    passed=$((passed + 1))
  else
    echo "  FAIL: ${description}"
    echo "    expected: ${expected}"
    echo "    actual:   ${actual}"
    failed=$((failed + 1))
  fi
}

stop_local_mariadb() {
  if [[ -f "$PID_FILE" ]]; then
    local pid
    pid=$(cat "$PID_FILE" 2>/dev/null) || true
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      # Wait for clean shutdown (up to 10 s).
      for _ in $(seq 1 20); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
      done
    fi
    rm -f "$PID_FILE"
  fi
  rm -f "$SOCK"
}

cleanup_sql() {
  run_sql "DROP TABLE IF EXISTS ${TEST_DB}.t;" 2>/dev/null || true
  run_sql "DROP TABLE IF EXISTS ${TEST_DB}.t2;" 2>/dev/null || true
  run_sql "DROP TABLE IF EXISTS ${TEST_DB}.t_renamed;" 2>/dev/null || true
  run_sql "DROP DATABASE IF EXISTS ${TEST_DB};" 2>/dev/null || true
  run_sql "UNINSTALL PLUGIN bytecaskdb;" 2>/dev/null || true
}

on_exit() {
  echo ""
  echo "--- Cleanup ---"
  cleanup_sql 2>/dev/null || true
  if [[ "$KEEP" == false ]]; then
    stop_local_mariadb
    echo "  Local MariaDB stopped."
  else
    echo "  --keep: local MariaDB left running (socket: ${SOCK})."
  fi
}
trap on_exit EXIT

# ---------------------------------------------------------------------------
# 1. Build the plugin
# ---------------------------------------------------------------------------

echo "=== ByteCaskDB MariaDB Plugin Smoke Test ==="
echo ""
echo "--- Build plugin ---"

cmake -S "${BYTECASK_ROOT}/bytecaskdb-mariadb-plugin" -B "${BUILD_DIR}" \
      -DBYTECASK_ROOT="${BYTECASK_ROOT}" 2>&1 | tail -3
cmake --build "${BUILD_DIR}" 2>&1 | tail -3

if [[ ! -f "${BUILD_DIR}/ha_bytecaskdb.so" ]]; then
  echo "FATAL: ha_bytecaskdb.so not found after build."
  exit 1
fi
echo "  Built: ${BUILD_DIR}/ha_bytecaskdb.so"

# ---------------------------------------------------------------------------
# 2. Symlink system provider plugins into our build dir
# ---------------------------------------------------------------------------
# MariaDB's default config (e.g. /etc/my.cnf.d/provider_*.cnf) forces
# compression-provider plugins.  When we override --plugin-dir to point at
# our build directory these must be reachable there too.

SYSTEM_PLUGIN_DIR="/usr/lib64/mariadb/plugin"
if [[ ! -d "$SYSTEM_PLUGIN_DIR" ]]; then
  SYSTEM_PLUGIN_DIR="/usr/lib/mariadb/plugin"
fi

if [[ -d "$SYSTEM_PLUGIN_DIR" ]]; then
  for f in "${SYSTEM_PLUGIN_DIR}"/provider_*.so; do
    [[ -e "$f" ]] || continue
    ln -sf "$f" "${BUILD_DIR}/" 2>/dev/null || true
  done
fi

# ---------------------------------------------------------------------------
# 3. Start a local MariaDB instance
# ---------------------------------------------------------------------------

echo ""
echo "--- Start local MariaDB ---"

# Wipe any leftover state so every run starts fresh.
stop_local_mariadb
rm -rf "${LOCAL_DIR}"

mkdir -p "${LOCAL_DIR}/data" "${LOCAL_DIR}/tmp"

echo "  Initialising data directory..."
mariadb-install-db \
  --datadir="${LOCAL_DIR}/data" \
  --auth-root-authentication-method=normal 2>&1 | tail -3

mariadbd \
  --datadir="${LOCAL_DIR}/data" \
  --socket="${SOCK}" \
  --port="${PORT}" \
  --pid-file="${PID_FILE}" \
  --skip-grant-tables \
  --tmpdir="${LOCAL_DIR}/tmp" \
  --plugin-dir="${BUILD_DIR}" \
  --log-error="${LOG_FILE}" &

# Wait for the server to accept connections (up to 15 s).
for i in $(seq 1 30); do
  if mariadb --socket="${SOCK}" -e "SELECT 1;" &>/dev/null; then
    break
  fi
  if [[ "$i" -eq 30 ]]; then
    echo "FATAL: MariaDB did not start within 15 s.  Error log:"
    tail -20 "${LOG_FILE}"
    exit 1
  fi
  sleep 0.5
done

version=$(run_sql "SELECT VERSION();")
echo "  MariaDB ${version} ready (socket: ${SOCK})"

# ---------------------------------------------------------------------------
# 4. Install the plugin
# ---------------------------------------------------------------------------

echo ""
echo "--- Install plugin ---"

run_sql "UNINSTALL PLUGIN bytecaskdb;" >/dev/null 2>/dev/null || true
run_sql "INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';"
echo "  Plugin installed."

status=$(run_sql "SELECT PLUGIN_STATUS FROM information_schema.PLUGINS WHERE PLUGIN_NAME='bytecaskdb';")
check "Plugin status is ACTIVE" "ACTIVE" "$status"

# ---------------------------------------------------------------------------
# 5. SQL smoke test
# ---------------------------------------------------------------------------

echo ""
echo "--- SQL smoke test ---"

# Clean up any leftovers from a previous run.
run_sql "DROP TABLE IF EXISTS ${TEST_DB}.t;" 2>/dev/null || true
run_sql "DROP DATABASE IF EXISTS ${TEST_DB};" 2>/dev/null || true
run_sql "CREATE DATABASE IF NOT EXISTS ${TEST_DB};"

# CREATE TABLE
run_sql "CREATE TABLE ${TEST_DB}.t (id INT PRIMARY KEY, name VARCHAR(100)) ENGINE=bytecaskdb;"
engine=$(run_sql "SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA='${TEST_DB}' AND TABLE_NAME='t';")
check "CREATE TABLE with ENGINE=bytecaskdb" "bytecaskdb" "$engine"

# INSERT + SELECT
run_sql "INSERT INTO ${TEST_DB}.t VALUES (1, 'hello');"
result=$(run_sql "SELECT * FROM ${TEST_DB}.t;")
check "INSERT / SELECT single row" "hello" "$result"

# SELECT *
result=$(run_sql "SELECT * FROM ${TEST_DB}.t;")
check "SELECT returns inserted row" "hello" "$result"

# Multiple inserts
run_sql "INSERT INTO ${TEST_DB}.t VALUES (2, 'world');"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (3, 'foo');"
count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t;")
check "3 rows after multiple inserts" "3" "$count"

# DROP TABLE
run_sql "DROP TABLE ${TEST_DB}.t;"
tables=$(run_sql "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA='${TEST_DB}' AND TABLE_NAME='t';")
check "Table gone after DROP" "" "$tables"

# ---------------------------------------------------------------------------
# 5b. UPDATE, DELETE, PK lookup, RENAME, duplicate PK, restart persistence
# ---------------------------------------------------------------------------

echo ""
echo "--- Extended SQL tests ---"

# Create a fresh table for the extended tests.
run_sql "CREATE TABLE ${TEST_DB}.t (id INT PRIMARY KEY, name VARCHAR(100)) ENGINE=bytecaskdb;"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (1, 'alice');"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (2, 'bob');"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (3, 'carol');"

# UPDATE value (PK unchanged)
run_sql "UPDATE ${TEST_DB}.t SET name='ALICE' WHERE id=1;"
result=$(run_sql "SELECT name FROM ${TEST_DB}.t WHERE id=1;")
check "UPDATE value (PK unchanged)" "ALICE" "$result"

# UPDATE PK (changes PK)
run_sql "UPDATE ${TEST_DB}.t SET id=10 WHERE id=2;"
result_old=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t WHERE id=2;")
result_new=$(run_sql "SELECT name FROM ${TEST_DB}.t WHERE id=10;")
check "UPDATE PK: old key gone" "0" "$result_old"
check "UPDATE PK: new key present" "bob" "$result_new"

# DELETE
run_sql "DELETE FROM ${TEST_DB}.t WHERE id=3;"
result=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t WHERE id=3;")
check "DELETE removes row" "0" "$result"

count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t;")
check "Row count after UPDATE+DELETE" "2" "$count"

# PK point lookup (SELECT WHERE id=N uses index_read_map)
result=$(run_sql "SELECT name FROM ${TEST_DB}.t WHERE id=1;")
check "PK point lookup" "ALICE" "$result"

# Duplicate PK rejection
dup_result=$(run_sql "INSERT INTO ${TEST_DB}.t VALUES (1, 'duplicate');" 2>&1 || true)
check "Duplicate PK rejected" "duplicate key" "$dup_result"

# RENAME TABLE
run_sql "RENAME TABLE ${TEST_DB}.t TO ${TEST_DB}.t_renamed;"
result=$(run_sql "SELECT name FROM ${TEST_DB}.t_renamed WHERE id=1;")
check "RENAME TABLE: data accessible under new name" "ALICE" "$result"

# Verify old name is gone.
old_result=$(run_sql "SELECT * FROM ${TEST_DB}.t;" 2>&1 || true)
check "RENAME TABLE: old name gone" "doesn't exist" "$old_result"

# Rename back for the DROP + recreate test.
run_sql "RENAME TABLE ${TEST_DB}.t_renamed TO ${TEST_DB}.t;"

# DROP + recreate should be empty.
run_sql "DROP TABLE ${TEST_DB}.t;"
run_sql "CREATE TABLE ${TEST_DB}.t (id INT PRIMARY KEY, name VARCHAR(100)) ENGINE=bytecaskdb;"
count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t;")
check "DROP + recreate: table is empty" "0" "$count"

run_sql "DROP TABLE ${TEST_DB}.t;"

# ---------------------------------------------------------------------------
# 5c. Restart persistence test
# ---------------------------------------------------------------------------

echo ""
echo "--- Restart persistence test ---"

# Create and insert data.
run_sql "CREATE TABLE ${TEST_DB}.t (id INT PRIMARY KEY, name VARCHAR(100)) ENGINE=bytecaskdb;"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (42, 'persistent');"
run_sql "INSERT INTO ${TEST_DB}.t VALUES (43, 'survives');"

# Stop MariaDB.
stop_local_mariadb
echo "  MariaDB stopped."

# Restart MariaDB.
mariadbd \
  --datadir="${LOCAL_DIR}/data" \
  --socket="${SOCK}" \
  --port="${PORT}" \
  --pid-file="${PID_FILE}" \
  --skip-grant-tables \
  --tmpdir="${LOCAL_DIR}/tmp" \
  --plugin-dir="${BUILD_DIR}" \
  --plugin-load-add="bytecaskdb=ha_bytecaskdb.so" \
  --log-error="${LOG_FILE}" &

for i in $(seq 1 30); do
  if mariadb --socket="${SOCK}" -e "SELECT 1;" &>/dev/null; then
    break
  fi
  if [[ "$i" -eq 30 ]]; then
    echo "FATAL: MariaDB did not restart within 15 s.  Error log:"
    tail -20 "${LOG_FILE}"
    exit 1
  fi
  sleep 0.5
done
echo "  MariaDB restarted."

# Verify data survived restart.
result=$(run_sql "SELECT name FROM ${TEST_DB}.t WHERE id=42;" 2>&1 || true)
check "Restart persistence: row 42" "persistent" "$result"

result=$(run_sql "SELECT name FROM ${TEST_DB}.t WHERE id=43;" 2>&1 || true)
check "Restart persistence: row 43" "survives" "$result"

count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.t;" 2>&1 || true)
check "Restart persistence: row count" "2" "$count"

# Clean up.
run_sql "DROP TABLE ${TEST_DB}.t;"

# ---------------------------------------------------------------------------
# 6. Transaction tests
# ---------------------------------------------------------------------------

echo ""
echo "--- Transaction tests ---"

# BEGIN/COMMIT — two inserts committed together
run_sql "CREATE TABLE ${TEST_DB}.txn1 (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=bytecaskdb;"
run_sql "BEGIN; INSERT INTO ${TEST_DB}.txn1 VALUES (1,'a'); INSERT INTO ${TEST_DB}.txn1 VALUES (2,'b'); COMMIT;"
count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.txn1;" 2>&1 || true)
check "BEGIN/COMMIT: both rows visible" "2" "$count"
run_sql "DROP TABLE ${TEST_DB}.txn1;"

# BEGIN/ROLLBACK — inserts rolled back
run_sql "CREATE TABLE ${TEST_DB}.txn2 (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=bytecaskdb;"
run_sql "BEGIN; INSERT INTO ${TEST_DB}.txn2 VALUES (1,'a'); INSERT INTO ${TEST_DB}.txn2 VALUES (2,'b'); ROLLBACK;"
count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.txn2;" 2>&1 || true)
check "BEGIN/ROLLBACK: no rows" "0" "$count"
run_sql "DROP TABLE ${TEST_DB}.txn2;"

# RYOW within transaction — read your own writes
run_sql "CREATE TABLE ${TEST_DB}.txn3 (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=bytecaskdb;"
result=$(run_sql "BEGIN; INSERT INTO ${TEST_DB}.txn3 VALUES (1,'ryow'); SELECT name FROM ${TEST_DB}.txn3 WHERE id=1; ROLLBACK;" 2>&1 || true)
check "RYOW within txn" "ryow" "$result"
run_sql "DROP TABLE ${TEST_DB}.txn3;"

# Duplicate key within transaction
run_sql "CREATE TABLE ${TEST_DB}.txn4 (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=bytecaskdb;"
result=$(run_sql "BEGIN; INSERT INTO ${TEST_DB}.txn4 VALUES (1,'first'); INSERT INTO ${TEST_DB}.txn4 VALUES (1,'dup'); ROLLBACK;" 2>&1 || true)
check "Dup key within txn" "duplicate key" "$result"
run_sql "DROP TABLE ${TEST_DB}.txn4;"

# Autocommit single statement — should persist
run_sql "CREATE TABLE ${TEST_DB}.txn5 (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=bytecaskdb;"
run_sql "INSERT INTO ${TEST_DB}.txn5 VALUES (1,'auto');"
count=$(run_sql "SELECT COUNT(*) FROM ${TEST_DB}.txn5;" 2>&1 || true)
check "Autocommit persists" "1" "$count"
run_sql "DROP TABLE ${TEST_DB}.txn5;"

# ---------------------------------------------------------------------------
# 7. Summary
# ---------------------------------------------------------------------------

echo ""
total=$((passed + failed))
echo "=== Results: ${passed}/${total} passed ==="

if [[ $failed -gt 0 ]]; then
  exit 1
fi
