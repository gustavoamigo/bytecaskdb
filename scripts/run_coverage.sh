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

# -P: a git worktree nested in the checkout would otherwise build the
# enclosing project.
XMAKE_P=(-P "$PROJECT_DIR")

# MC/DC is held at 100% on the durability and recovery code, after the
# conditions marked `mcdc-exempt:` at their site (scripts/mcdc_report.py).
# MCDC_MAX_EXEMPT caps those marks: raising it is a reviewed change.
MCDC_REQUIRED=(bytecaskdb/data_file.cppm bytecaskdb/hint_file.cppm
               bytecaskdb/bytecask.cppm)
MCDC_MAX_EXEMPT=2

# Configure and build with coverage instrumentation
echo "==> Configuring with coverage..."
# -o build: xmake keeps the last configured output directory, which may be
# one of the per-tree directories below.
xmake f "${XMAKE_P[@]}" --toolchain=clang --coverage=true -m debug -o build -y

echo "==> Building..."
xmake build "${XMAKE_P[@]}" bytecask_tests

# The engine suite again on the keyed B+ tree: the default build runs the
# engine on the blind-leaf tree, and the keyed tree's engine paths (its
# recovery, its get) are compiled there but only run in this build. Its own
# build directory; BYTECASK_KEYDIR is read by xmake.lua at configure and at
# build time.
echo "==> Building the engine on the keyed B+ tree..."
BYTECASK_KEYDIR=btree xmake f "${XMAKE_P[@]}" --toolchain=clang \
    --coverage=true -m debug -o build/cov-btree -y
BYTECASK_KEYDIR=btree xmake build "${XMAKE_P[@]}" bytecask_tests

BYTECASK_TEST_BIN="$PROJECT_DIR/build/linux/x86_64/debug/bytecask_tests"
KEYED_ENGINE_TEST_BIN="$PROJECT_DIR/build/cov-btree/linux/x86_64/debug/bytecask_tests"

if [ ! -x "$BYTECASK_TEST_BIN" ]; then
    echo "ERROR: could not find bytecask_tests binary at $BYTECASK_TEST_BIN"
    exit 1
fi
if [ ! -x "$KEYED_ENGINE_TEST_BIN" ]; then
    echo "ERROR: could not find the keyed-tree bytecask_tests binary at $KEYED_ENGINE_TEST_BIN"
    exit 1
fi

echo "==> Running tests..."
LLVM_PROFILE_FILE="$COV_DIR/bytecask_tests.profraw" "$BYTECASK_TEST_BIN"
LLVM_PROFILE_FILE="$COV_DIR/bytecask_tests_btree.profraw" "$KEYED_ENGINE_TEST_BIN"

echo "==> Merging profile data..."
llvm-profdata merge -sparse "$COV_DIR"/*.profraw -o "$COV_DIR/coverage.profdata"

# MC/DC per build, merged by mcdc_report.py: llvm-profdata keeps one build's
# counters for a function whose MC/DC bitmap differs between builds (one with
# key-directory-specific branches) and drops the others'.
echo "==> Exporting MC/DC per build..."
MCDC_EXPORTS=()
export_build() {  # name binary profraw
    llvm-profdata merge -sparse "$3" -o "$COV_DIR/$1.profdata"
    llvm-cov export "$2" -instr-profile="$COV_DIR/$1.profdata" \
        -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
        -format=text > "$COV_DIR/$1.json"
    MCDC_EXPORTS+=("$COV_DIR/$1.json")
}
export_build blind "$BYTECASK_TEST_BIN" "$COV_DIR/bytecask_tests.profraw"
export_build btree "$KEYED_ENGINE_TEST_BIN" "$COV_DIR/bytecask_tests_btree.profraw"

echo "==> Generating summary..."
llvm-cov report "$BYTECASK_TEST_BIN" \
    -object="$KEYED_ENGINE_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
    -show-mcdc-summary

echo ""
echo "==> Generating HTML report..."
llvm-cov show "$BYTECASK_TEST_BIN" \
    -object="$KEYED_ENGINE_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
    -show-mcdc \
    -format=html \
    -output-dir="$COV_DIR/html"

echo ""
echo "==> Generating lcov report (for VS Code Coverage Gutters)..."
llvm-cov export "$BYTECASK_TEST_BIN" \
    -object="$KEYED_ENGINE_TEST_BIN" \
    -instr-profile="$COV_DIR/coverage.profdata" \
    -ignore-filename-regex='tests/|catch2|crc32c|/usr/' \
    -format=lcov > "$PROJECT_DIR/lcov.info"

echo ""
echo "==> MC/DC (merged across builds; see scripts/mcdc_report.py)..."
mcdc_status=0
python3 "$SCRIPT_DIR/mcdc_report.py" "${MCDC_EXPORTS[@]}" \
    --root "$PROJECT_DIR" \
    --require-full "${MCDC_REQUIRED[@]}" \
    --max-exempt "$MCDC_MAX_EXEMPT" | tee "$COV_DIR/html/mcdc.txt" \
    || mcdc_status=$?

echo ""
echo "Coverage HTML report: $COV_DIR/html/index.html"
echo "VS Code: install Coverage Gutters extension, then Ctrl+Shift+P > Coverage Gutters: Display Coverage"

# Restore default config
echo "==> Restoring default build config..."
xmake f "${XMAKE_P[@]}" --coverage= -m release -o build -y

if [ "$mcdc_status" -ne 0 ]; then
    echo "ERROR: MC/DC gate failed (see the MC/DC section above)."
    exit "$mcdc_status"
fi
