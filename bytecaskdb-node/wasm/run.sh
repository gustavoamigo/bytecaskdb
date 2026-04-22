#!/usr/bin/env bash
# Run an Emscripten-built binary with host environment variables propagated.
# Usage: bash run.sh build/engine_bench_nodefs.js [args...]
set -euo pipefail
SCRIPT="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift

# Patch ENV={} to copy process.env if not already patched
if grep -q 'ENV={}' "$SCRIPT"; then
  sed -i 's/ENV={}/ENV=typeof process!=="undefined"\&\&process.env?Object.assign({},process.env):{}/' "$SCRIPT"
fi

node "$SCRIPT" "$@"
