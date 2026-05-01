#!/usr/bin/env bash
# Run the Python functional test suite for the ByteCaskDB MariaDB plugin.
#
# Prerequisites:
#   - Plugin .so built (run-unit-tests.sh or cmake --build)
#   - Python 3 with pip
#   - mariadbd on PATH
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/tests/run-functional-tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FUNCTIONAL_DIR="${SCRIPT_DIR}/functional"

echo "=== Installing Python dependencies ==="
pip install -q -r "${FUNCTIONAL_DIR}/requirements.txt"

echo ""
echo "=== Running functional tests ==="
pytest "${FUNCTIONAL_DIR}" -v "$@"
