#!/usr/bin/env bash
# Run the MariaDB Test Runner (MTR) storage_engine suite against ByteCaskDB.
#
# Prerequisites:
#   - Plugin .so built (run-unit-tests.sh or cmake --build)
#   - MariaDB test framework installed (mariadb-test package)
#   - MTR suite directory at /usr/share/mysql-test (or set MTR_DIR)
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/tests/run-mtr-tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PLUGIN_DIR}/build"
MTR_SUITE_DIR="${SCRIPT_DIR}/mtr"
MTR_DIR="${MTR_DIR:-/usr/share/mysql-test}"
MTR_VARDIR="${PLUGIN_DIR}/mtr-var"

if [[ ! -d "${MTR_DIR}" ]]; then
  echo "ERROR: MTR directory not found at ${MTR_DIR}"
  echo "Install mariadb-test or set MTR_DIR to the mysql-test directory."
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/ha_bytecaskdb.so" ]]; then
  echo "ERROR: ha_bytecaskdb.so not found. Build the plugin first:"
  echo "  ./bytecaskdb-mariadb-plugin/tests/run-unit-tests.sh"
  exit 1
fi

echo "=== Running MTR storage_engine suite ==="
cd "${MTR_DIR}"
perl mariadb-test-run.pl \
  --suite=storage_engine \
  --suite-dir="${MTR_SUITE_DIR}" \
  --vardir="${MTR_VARDIR}" \
  --mysqld=--plugin-dir="${BUILD_DIR}" \
  --mysqld=--plugin-load-add=bytecaskdb=ha_bytecaskdb.so \
  "$@"
