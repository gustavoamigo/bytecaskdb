#!/usr/bin/env bash
# Runs once after the devcontainer is created. Keep this in sync with any
# manual setup steps documented in README.md / CONTRIBUTING.md.
set -euo pipefail

mkdir -p .claude
cp -n .claude.defaults/settings.json .claude/settings.json

python3 -c "import nanobind; print(nanobind.include_dir())"

RESOURCE_DIR=$(clang --print-resource-dir)
xmake f --toolchain=clang --cxflags="-resource-dir=${RESOURCE_DIR}" -y

# graft — repo context graph (see .github/copilot-instructions.md).
npm install -g @nanonets/graft
graft init
