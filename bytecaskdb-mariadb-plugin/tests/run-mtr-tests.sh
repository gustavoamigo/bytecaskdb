#!/usr/bin/env bash
# Run the MariaDB Test Runner (MTR) storage_engine suite against ByteCaskDB.
#
# Uses the MTR overlay mechanism: our tests/mtr/storage_engine/ directory
# provides define_engine.inc, disabled.def, suite.opt, and .rdiff files that
# patch the base storage_engine suite results for bytecaskdb-specific output.
#
# Prerequisites:
#   - Plugin .so built (run-unit-tests.sh or cmake --build)
#   - MariaDB test framework installed (mariadb-test package)
#   - MTR suite directory at /usr/share/mysql-test (or set MTR_DIR)
#
# Usage:
#   ./bytecaskdb-mariadb-plugin/tests/run-mtr-tests.sh
#   ./bytecaskdb-mariadb-plugin/tests/run-mtr-tests.sh --force  # continue on failure

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PLUGIN_DIR}/build"
OVERLAY_DIR="${SCRIPT_DIR}/mtr/storage_engine"
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

# Create a local MTR tree that mirrors the system one but adds our overlay.
# This avoids needing write access to /usr/share/mysql-test.
LOCAL_MTR="/tmp/mtr-bytecaskdb"
rm -rf "${LOCAL_MTR}"
mkdir -p "${LOCAL_MTR}/mysql-test/plugin/bytecaskdb"

# MTR derives basedir by walking up from cwd (mysql-test dir).
# For RPM installs, it expects: basedir/share/mariadb, basedir/bin.
# Symlink the necessary paths so resolution works from our local tree.
ln -s /usr/bin "${LOCAL_MTR}/bin"
ln -s /usr/share "${LOCAL_MTR}/share"

# Symlink everything from the system mysql-test directory
for item in "${MTR_DIR}"/*; do
  name=$(basename "$item")
  [[ "$name" == "plugin" ]] && continue
  ln -s "$item" "${LOCAL_MTR}/mysql-test/${name}"
done

# Symlink existing plugins and add ours
mkdir -p "${LOCAL_MTR}/mysql-test/plugin"
for item in "${MTR_DIR}/plugin"/*; do
  ln -s "$item" "${LOCAL_MTR}/mysql-test/plugin/$(basename "$item")"
done
ln -s "${OVERLAY_DIR}" "${LOCAL_MTR}/mysql-test/plugin/bytecaskdb/storage_engine"

echo "=== Running MTR storage_engine suite ==="
cd "${LOCAL_MTR}/mysql-test"
perl mariadb-test-run.pl \
  --suite=storage_engine-bytecaskdb \
  --vardir="${MTR_VARDIR}" \
  --mysqld=--plugin-dir="${BUILD_DIR}" \
  --force \
  --max-test-fail=0 \
  --testcase-timeout=60 \
  "$@"
