#!/bin/bash
# Runs a Serena hook (https://oraios.github.io/serena/02-usage/030_clients.html)
# for Claude Code. A silent no-op when Serena is not installed.
#
# Usage: serena.sh <activate|remind|auto-approve|cleanup>
# The hook's JSON input arrives on stdin and is passed through to serena-hooks.

# uv installs tools to ~/.local/bin, which a non-login PATH may lack.
PATH="$PATH:$HOME/.local/bin"
command -v serena-hooks >/dev/null 2>&1 || exit 0

if [ "$1" = activate ]; then
  project_dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
  "$project_dir/scripts/serena_index.sh" --background
fi

exec serena-hooks "$1" --client=claude-code
