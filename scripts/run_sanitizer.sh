#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SANITIZER="${1:-thread}"

# Detect and export the Clang target triple so xmake.lua can pass --target=
# to the linker. Without this, Clang may default to a generic triple that
# doesn't match the OS's library layout (e.g. x86_64-unknown-linux-gnu vs
# x86_64-redhat-linux-gnu on Fedora), causing the sanitizer runtime to not
# be found.
export CLANG_TARGET_TRIPLE
CLANG_TARGET_TRIPLE="$(clang --print-target-triple)"
echo "==> Clang target triple: $CLANG_TARGET_TRIPLE"

echo "==> Configuring with -fsanitize=$SANITIZER..."
xmake f --sanitizer="$SANITIZER" -m debug -y

echo "==> Building..."
xmake build bytecask_tests

TEST_BIN="$PROJECT_DIR/build/linux/x86_64/debug/bytecask_tests"
if [ ! -x "$TEST_BIN" ]; then
    echo "ERROR: could not find bytecask_tests binary at $TEST_BIN"
    exit 1
fi

echo "==> Running tests under $SANITIZER sanitizer..."
echo ""

OPTS=""
if [ "$SANITIZER" = "thread" ]; then
    OPTS="halt_on_error=0 history_size=4"
    export TSAN_OPTIONS="$OPTS"
    echo "    TSAN_OPTIONS=$OPTS"
elif [ "$SANITIZER" = "address" ]; then
    OPTS="halt_on_error=0 detect_leaks=1"
    export ASAN_OPTIONS="$OPTS"
    echo "    ASAN_OPTIONS=$OPTS"
fi
echo ""

"$TEST_BIN"
STATUS=$?

echo ""
echo "==> Restoring default build config..."
xmake f --sanitizer= -m release -y

if [ $STATUS -ne 0 ]; then
    echo ""
    echo "Tests exited with status $STATUS under $SANITIZER sanitizer."
    exit $STATUS
fi

echo ""
echo "All tests passed under $SANITIZER sanitizer."
