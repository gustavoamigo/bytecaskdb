#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

TARGET="${1:?Usage: run_fuzz.sh <target> [duration_seconds]}"
DURATION="${2:-60}"
NAME="${TARGET#fuzz_}"

export CLANG_TARGET_TRIPLE
CLANG_TARGET_TRIPLE="$(clang --print-target-triple)"

echo "==> Configuring with -fsanitize=fuzzer,address..."
xmake f --sanitizer="fuzzer,address" -m debug -y

echo "==> Building $TARGET..."
xmake build "$TARGET"

BIN="$PROJECT_DIR/build/linux/x86_64/debug/$TARGET"
if [ ! -x "$BIN" ]; then
    echo "ERROR: could not find $TARGET binary at $BIN"
    exit 1
fi

# Corpus: evolving directory where libFuzzer writes new inputs (gitignored).
# Seed:   committed directory with deterministic valid inputs from gen_fuzz_corpus.
CORPUS="$PROJECT_DIR/tests/fuzz/corpus/$NAME/"
SEED="$PROJECT_DIR/tests/fuzz/seed/$NAME/"
mkdir -p "$CORPUS"

echo "==> Running $TARGET for ${DURATION}s..."
echo "    corpus: $CORPUS"
echo "    seed:   $SEED"
echo ""
"$BIN" "$CORPUS" "$SEED" -max_total_time="$DURATION" -max_len=65536
STATUS=$?

echo ""
echo "==> Restoring default build config..."
xmake f --sanitizer= -m release -y

if [ $STATUS -ne 0 ]; then
    echo ""
    echo "$TARGET exited with status $STATUS — check crash-* / oom-* files."
    exit $STATUS
fi

echo ""
echo "$TARGET ran for ${DURATION}s with no findings."
