#!/usr/bin/env bash
# Runs once after the devcontainer is created. Keep this in sync with any
# manual setup steps documented in README.md / CONTRIBUTING.md.
set -euo pipefail

mkdir -p .claude
cp -n .claude.defaults/settings.json .claude/settings.json

python3 -c "import nanobind; print(nanobind.include_dir())"

RESOURCE_DIR=$(/usr/bin/clang --print-resource-dir)
# Pin cc/cxx to the system clang: emsdk's clang (also named "clang") is
# prepended to PATH below for the wasm build and would otherwise be picked up
# by xmake's "clang" toolchain, breaking native builds ("unknown target triple").
xmake f --toolchain=clang --cc=/usr/bin/clang --cxx=/usr/bin/clang++ --cxflags="-resource-dir=${RESOURCE_DIR}" -y
