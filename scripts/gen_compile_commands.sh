#!/bin/bash
# Generate compile_commands.json for VS Code / clangd.
# Run after `xmake build` to get full IDE support.
#
# Usage:  ./scripts/gen_compile_commands.sh
set -e
cd "$(git rev-parse --show-toplevel)"

# Generate for each default target (bench targets are excluded because
# xmake's C++ module scanner crashes on targets whose BMIs weren't built).
for target in bytecask bytecask_tests; do
  xmake project -k compile_commands -t "$target" .
done

python3 scripts/fix_compile_commands.py
