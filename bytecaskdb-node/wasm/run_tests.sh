#!/usr/bin/env bash
# Run the Catch2 test suite under WASM/Node.js.
#
# Usage:
#   cd bytecaskdb-node/wasm && bash run_tests.sh
#   bash run_tests.sh -v            # verbose
#   bash run_tests.sh "[vacuum]"    # run only vacuum-tagged tests
#
# Excluded tags:
#   [concurrency] — requires pthreads, unavailable in single-threaded WASM
#   [lock]        — requires flock, not supported by Emscripten's NODEFS

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SCRIPT_DIR/build"

if [ ! -f "$BUILD/bytecask_tests.js" ]; then
  echo "Error: $BUILD/bytecask_tests.js not found. Run build.sh first." >&2
  exit 1
fi

exec node "$BUILD/bytecask_tests.js" "~[concurrency]" "~[lock]" "$@"
