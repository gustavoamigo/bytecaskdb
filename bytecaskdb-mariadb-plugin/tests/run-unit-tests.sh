#!/usr/bin/env bash
# Run C++ unit tests (encoding, txn) and integration tests for the MariaDB plugin.
#
# Prerequisites:
#   - libbytecask.a built: cd <root> && xmake build bytecask
#   - MariaDB dev headers installed (mariadb-devel / libmariadb-dev)
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/tests/run-unit-tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PLUGIN_DIR}/build"

echo "=== Building plugin + tests ==="
cmake -S "${PLUGIN_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD_DIR}" --parallel

echo ""
echo "=== Running unit tests ==="
cd "${BUILD_DIR}"
ctest --output-on-failure
