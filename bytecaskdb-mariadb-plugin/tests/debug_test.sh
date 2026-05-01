#!/bin/bash

# Quick debug test for secondary index issues
set -e

LOCAL_DIR="/home/gamigo/bytecask/.mariadb_local"
SOCK="${LOCAL_DIR}/mysql.sock"
BUILD_DIR="/home/gamigo/bytecask/bytecaskdb-mariadb-plugin/build"

# Start MariaDB if not running
if ! mariadb --socket="${SOCK}" -e "SELECT 1;" &>/dev/null; then
  echo "Starting MariaDB for debug test..."
  mkdir -p "${LOCAL_DIR}"/{data,tmp}

  mariadb-install-db \
    --datadir="${LOCAL_DIR}/data" \
    --auth-root-authentication-method=normal &>/dev/null

  mariadbd \
    --datadir="${LOCAL_DIR}/data" \
    --socket="${SOCK}" \
    --port=3307 \
    --skip-grant-tables \
    --tmpdir="${LOCAL_DIR}/tmp" \
    --plugin-dir="${BUILD_DIR}" \
    --log-error="${LOCAL_DIR}/error.log" &

  # Wait for startup
  for i in $(seq 1 20); do
    if mariadb --socket="${SOCK}" -e "SELECT 1;" &>/dev/null; then
      break
    fi
    sleep 0.5
  done
fi

run_sql() {
  mariadb --socket="${SOCK}" -e "$1" 2>&1
}

echo "Installing plugin..."
run_sql "INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';" || true

echo "Creating test database..."
run_sql "CREATE DATABASE IF NOT EXISTS debug_test;"
run_sql "USE debug_test; DROP TABLE IF EXISTS t;"

echo "Testing unique constraint..."
run_sql "
USE debug_test;
CREATE TABLE t (id INT PRIMARY KEY, score INT NOT NULL) ENGINE=bytecaskdb;
CREATE UNIQUE INDEX idx_score ON t(score);
INSERT INTO t VALUES (1, 100);
"

echo "Attempting duplicate insert (should fail)..."
result=$(run_sql "USE debug_test; INSERT INTO t VALUES (2, 100);" 2>&1 || echo "ERROR")
echo "Result: $result"

echo ""
echo "Testing DELETE with secondary index..."
run_sql "
USE debug_test;
DROP TABLE IF EXISTS t2;
CREATE TABLE t2 (id INT PRIMARY KEY, category VARCHAR(50) NOT NULL, score INT NOT NULL) ENGINE=bytecaskdb;
CREATE INDEX idx_cat ON t2(category);
INSERT INTO t2 VALUES (1, 'basic', 75), (2, 'premium', 90), (3, 'basic', 85), (4, 'premium', 95);
"

echo "Before DELETE:"
run_sql "USE debug_test; SELECT * FROM t2 ORDER BY id;"

echo ""
echo "Executing DELETE with debug output:"
run_sql "USE debug_test; DELETE FROM t2 WHERE category='basic' AND score < 80;" 2>&1

echo ""
echo "After DELETE:"
run_sql "USE debug_test; SELECT * FROM t2 ORDER BY id;"

echo ""
echo "Count after DELETE:"
run_sql "USE debug_test; SELECT COUNT(*) FROM t2;"

# Cleanup
run_sql "USE debug_test; DROP TABLE IF EXISTS t, t2;"
echo "Debug test complete."