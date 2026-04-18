#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Assembles a Python wheel from the pre-built bytecaskdb extension module.
#
# Prerequisites:
#   - xmake has already built the bytecaskdb_python target
#   - The .so/.dylib is in bytecask-python/bytecaskdb/
#
# Usage:
#   ./scripts/build_wheel.sh [output_dir]
#
# Output: a .whl file in output_dir (default: dist/)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_DIR="$PROJECT_ROOT/bytecask-python/bytecaskdb"
OUTPUT_DIR="${1:-$PROJECT_ROOT/dist}"
# Resolve to absolute path before we cd elsewhere.
OUTPUT_DIR="$(cd "$PROJECT_ROOT" && mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd)"

# ---------------------------------------------------------------------------
# Detect version and platform
# ---------------------------------------------------------------------------

VERSION=$(python3 -c "
import sys; sys.path.insert(0, '$PROJECT_ROOT/bytecask-python')
from bytecaskdb import __version__; print(__version__)")

PYTHON_TAG=$(python3 -c "
import sys
print(f'cp{sys.version_info.major}{sys.version_info.minor}')")

ABI_TAG="$PYTHON_TAG"

case "$(uname -s)" in
  Linux)
    ARCH=$(uname -m)
    PLAT_TAG="linux_${ARCH}"
    ;;
  Darwin)
    ARCH=$(uname -m)
    PLAT_TAG="macosx_11_0_${ARCH}"
    ;;
  *)
    echo "Unsupported platform: $(uname -s)" >&2
    exit 1
    ;;
esac

WHEEL_NAME="bytecaskdb-${VERSION}-${PYTHON_TAG}-${ABI_TAG}-${PLAT_TAG}"
echo "Building wheel: ${WHEEL_NAME}"

# ---------------------------------------------------------------------------
# Find the extension module
# ---------------------------------------------------------------------------

SO_FILE=$(find "$PKG_DIR" -name '_bytecaskdb*.so' -o -name '_bytecaskdb*.dylib' | head -1)
if [ -z "$SO_FILE" ]; then
  echo "Error: _bytecaskdb extension not found in $PKG_DIR" >&2
  echo "Run 'xmake build bytecaskdb_python' first." >&2
  exit 1
fi
SO_NAME=$(basename "$SO_FILE")
echo "Extension module: $SO_NAME"

# ---------------------------------------------------------------------------
# Assemble wheel in a temp directory
# ---------------------------------------------------------------------------

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

WHEEL_DIR="$TMPDIR/$WHEEL_NAME"
DIST_INFO="$WHEEL_DIR/bytecaskdb-${VERSION}.dist-info"

mkdir -p "$WHEEL_DIR/bytecaskdb"
mkdir -p "$DIST_INFO"

# Copy package files
cp "$PKG_DIR/__init__.py"     "$WHEEL_DIR/bytecaskdb/"
cp "$PKG_DIR/ext.py"          "$WHEEL_DIR/bytecaskdb/"
cp "$PKG_DIR/_bytecaskdb.pyi" "$WHEEL_DIR/bytecaskdb/"
cp "$PKG_DIR/py.typed"        "$WHEEL_DIR/bytecaskdb/"
cp "$SO_FILE"                 "$WHEEL_DIR/bytecaskdb/"

# Write METADATA
cat > "$DIST_INFO/METADATA" <<METADATA
Metadata-Version: 2.1
Name: bytecaskdb
Version: ${VERSION}
Summary: Fast, predictable embedded key-value store
Author: Gustavo Amigo
License: MIT
Classifier: Development Status :: 3 - Alpha
Classifier: License :: OSI Approved :: MIT License
Classifier: Programming Language :: Python :: 3
Classifier: Programming Language :: C++
Classifier: Topic :: Database
Requires-Python: >=${PYTHON_TAG:2:1}.${PYTHON_TAG:3}
METADATA

# Write WHEEL
cat > "$DIST_INFO/WHEEL" <<WHEEL
Wheel-Version: 1.0
Generator: build_wheel.sh
Root-Is-Purelib: false
Tag: ${PYTHON_TAG}-${ABI_TAG}-${PLAT_TAG}
WHEEL

# Write top_level.txt
echo "bytecaskdb" > "$DIST_INFO/top_level.txt"

# Write RECORD (with hashes for all files except RECORD itself)
cd "$WHEEL_DIR"
RECORD_FILE="bytecaskdb-${VERSION}.dist-info/RECORD"
: > "$RECORD_FILE"

for f in $(find . -type f ! -name RECORD | sort); do
  # Strip leading ./
  rel="${f#./}"
  hash=$(python3 -c "
import hashlib, base64
with open('$f', 'rb') as fh:
    digest = hashlib.sha256(fh.read()).digest()
print('sha256=' + base64.urlsafe_b64encode(digest).rstrip(b'=').decode())")
  size=$(wc -c < "$f" | tr -d ' ')
  echo "${rel},${hash},${size}" >> "$RECORD_FILE"
done
echo "$RECORD_FILE,," >> "$RECORD_FILE"

# ---------------------------------------------------------------------------
# Create the .whl (it's just a zip)
# ---------------------------------------------------------------------------

mkdir -p "$OUTPUT_DIR"
WHEEL_PATH="$OUTPUT_DIR/${WHEEL_NAME}.whl"

cd "$WHEEL_DIR"
zip -q -r "$WHEEL_PATH" .

echo "Wheel created: $WHEEL_PATH"
echo ""
echo "Next steps:"
if [ "$(uname -s)" = "Linux" ]; then
  echo "  auditwheel repair $WHEEL_PATH -w $OUTPUT_DIR"
elif [ "$(uname -s)" = "Darwin" ]; then
  echo "  delocate-wheel -w $OUTPUT_DIR $WHEEL_PATH"
fi
