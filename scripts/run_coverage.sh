#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COV_DIR="$PROJECT_DIR/coverage"

# Clean previous run
rm -rf "$COV_DIR"
mkdir -p "$COV_DIR/html"

# Detect and export the Clang target triple so xmake.lua can pass --target=
# to the linker. See scripts/run_sanitizer.sh for rationale.
export CLANG_TARGET_TRIPLE
CLANG_TARGET_TRIPLE="$(clang --print-target-triple)"

# Configure and build with coverage instrumentation
echo "==> Configuring with coverage..."
xmake f --toolchain=clang --coverage=true -m debug -y

echo "==> Building..."
xmake build bytecask_tests
xmake build radix_tree_memory_tests
xmake build unordered_view_tests

BYTECASK_TEST_BIN="$PROJECT_DIR/build/linux/x86_64/debug/bytecask_tests"
RADIX_TREE_TEST_BIN="$PROJECT_DIR/build/linux/x86_64/debug/radix_tree_memory_tests"
UNORDERED_VIEW_TEST_BIN="$PROJECT_DIR/build/linux/x86_64/debug/unordered_view_tests"

if [ ! -x "$BYTECASK_TEST_BIN" ]; then
    echo "ERROR: could not find bytecask_tests binary at $BYTECASK_TEST_BIN"
    exit 1
fi
if [ ! -x "$RADIX_TREE_TEST_BIN" ]; then
    echo "ERROR: could not find radix_tree_memory_tests binary at $RADIX_TREE_TEST_BIN"
    exit 1
fi
if [ ! -x "$UNORDERED_VIEW_TEST_BIN" ]; then
    echo "ERROR: could not find unordered_view_tests binary at $UNORDERED_VIEW_TEST_BIN"
    exit 1
fi

echo "==> Running tests..."
LLVM_PROFILE_FILE="$COV_DIR/bytecask_tests.profraw" "$BYTECASK_TEST_BIN"
LLVM_PROFILE_FILE="$COV_DIR/radix_tree_memory_tests.profraw" "$RADIX_TREE_TEST_BIN"
LLVM_PROFILE_FILE="$COV_DIR/unordered_view_tests.profraw" "$UNORDERED_VIEW_TEST_BIN"

echo "==> Merging profile data..."
llvm-profdata merge -sparse "$COV_DIR"/*.profraw -o "$COV_DIR/coverage.profdata"

echo "==> Generating summary..."
llvm-cov report "$BYTECASK_TEST_BIN" \
    -object="$RADIX_TREE_TEST_BIN" \
    -object="$UNORDERED_VIEW_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/'

echo ""
echo "==> Generating HTML report..."
llvm-cov show "$BYTECASK_TEST_BIN" \
    -object="$RADIX_TREE_TEST_BIN" \
    -object="$UNORDERED_VIEW_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
    -format=html \
    -output-dir="$COV_DIR/html"

echo ""
echo "==> Generating lcov report (for VS Code Coverage Gutters)..."
llvm-cov export "$BYTECASK_TEST_BIN" \
    -object="$RADIX_TREE_TEST_BIN" \
    -object="$UNORDERED_VIEW_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
    -format=lcov > "$PROJECT_DIR/lcov.info"

echo ""
echo "Coverage HTML report: $COV_DIR/html/index.html"
echo "VS Code: install Coverage Gutters extension, then Ctrl+Shift+P > Coverage Gutters: Display Coverage"

# Restore default config
echo "==> Restoring default build config..."
xmake f --coverage= -m release -y
