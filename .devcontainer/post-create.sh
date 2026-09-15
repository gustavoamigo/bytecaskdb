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

# graft — repo context graph (see .github/copilot-instructions.md).
# Install under the devcontainer user's home so the post-create step does not
# require write access to the system npm prefix used by the base image.
NPM_PREFIX="${HOME}/.npm-global"
mkdir -p "${NPM_PREFIX}"
export PATH="${NPM_PREFIX}/bin:${PATH}"
# Installed from our fork (gustavoamigo/Graft), which knows the .cppm
# extension (C++20 module interface units) our core engine is written in.
if ! command -v graft >/dev/null 2>&1; then
  NPM_PREFIX="${NPM_PREFIX}" ./scripts/install_graft.sh
fi
graft init --agents copilot --agents claude -y

echo 'export NPM_PREFIX="${HOME}/.npm-global"' >> ~/.bashrc
echo 'export PATH=$NPM_PREFIX/bin:$PATH' >> ~/.bashrc 