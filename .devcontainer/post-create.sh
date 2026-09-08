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
# Install under the devcontainer user's home so the post-create step does not
# require write access to the system npm prefix used by the base image.
NPM_PREFIX="${HOME}/.npm-global"
mkdir -p "${NPM_PREFIX}"
export PATH="${NPM_PREFIX}/bin:${PATH}"
if ! command -v graft >/dev/null 2>&1; then
  npm install -g --prefix "${NPM_PREFIX}" @nanonets/graft
fi
graft init --agents copilot --agents claude -y

echo 'export NPM_PREFIX="${HOME}/.npm-global"' >> ~/.bashrc
echo 'export PATH=$NPM_PREFIX/bin:$PATH' >> ~/.bashrc 