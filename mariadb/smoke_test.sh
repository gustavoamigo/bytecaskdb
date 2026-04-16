#!/usr/bin/env bash
# mariadb/smoke_test.sh — Self-contained smoke test for the ByteCaskDB MariaDB
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
#   ./mariadb/smoke_test.sh          # default — starts local instance
#   ./mariadb/smoke_test.sh --keep   # leave the local instance running after tests

set -euo pipefail

BYTECASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BYTECASK_ROOT}/mariadb/build"
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

cmake -S "${BYTECASK_ROOT}/mariadb" -B "${BUILD_DIR}" \
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
# 6. Summary
# ---------------------------------------------------------------------------

echo ""
total=$((passed + failed))
echo "=== Results: ${passed}/${total} passed ==="

if [[ $failed -gt 0 ]]; then
  exit 1
fi
