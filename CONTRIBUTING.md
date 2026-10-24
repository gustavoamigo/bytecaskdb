# Contributing to ByteCaskDB

Thanks for your interest. ByteCaskDB is in early development, so contributions at every level are welcome — from fixing a typo to implementing a new feature.

## Getting started

The fastest way to start is to open the project in GitHub Codespaces — everything is pre-installed and ready to go:

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecask)

If you prefer to work locally, you need **Clang** (C++23 modules support) and [xmake](https://xmake.io). The included [Dev Container](.devcontainer) sets that up automatically in VS Code.

```bash
# Build and run the tests — this is the only check that must pass.
xmake build bytecask_tests
xmake run bytecask_tests
```

That's it. If the tests pass, you're in good shape.

Sessions in [Claude Code on the web](https://code.claude.com/docs/en/claude-code-on-the-web)
start from a bare Ubuntu container with none of this installed, so
`.claude/hooks/session-start.sh` provisions it on session start: it builds xmake
from source, installs `nanobind` and clang 20 (clang, clang-scan-deps and
clangd, linked as the unversioned names — Ubuntu's default clang is 18, older
than CI's, and clangd must match the compiler to read its module files), and
runs `xmake f` once so the dependency packages land in the cached container
state. Two constraints in that
environment shape the script — Ubuntu's packaged xmake (2.8.7) is too old to
load the current xmake-repo, and the egress policy blocks `xmake.io` and GitHub
archive downloads while allowing git, which is why xmake is cloned and built
rather than installed. The hook is a no-op everywhere else: local checkouts, the
Dev Container, and Codespaces already have the toolchain.

### Worktrees and editor setup

A git worktree is a bare checkout: no `build/`, so no C++23 module BMIs and no
`compile_commands.json`, and clangd reports every `import bytecask.*` as
unresolved until both exist. `scripts/opencode_worktree_setup.sh` builds them —
`xmake f`, `xmake build bytecask_tests`, then `scripts/gen_compile_commands.sh` —
writing only gitignored paths. Run it by hand in any new checkout, or after
switching `BYTECASK_KEYDIR` or the build mode.

OpenCode runs it automatically in a worktree it creates. That setting is per
project rather than in `opencode.jsonc`, which has no field for it: it is the
project's `commands.start`, stored in OpenCode's own database. A fresh clone
sets it once, reading the project ID from the list below:

```bash
opencode api get /api/project            # find the entry whose canonical path is this repo
opencode api patch /api/project/<id> \
  --data '{"commands":{"start":"scripts/opencode_worktree_setup.sh"}}'
```

Worktrees created before the script existed need it run by hand.

## Making a change

1. Open an issue or comment on an existing one before starting significant work, so we can discuss direction.
2. Keep changes focused — one concern per pull request.
3. Add or update tests that cover the behaviour you changed (see `tests/bytecask_test.cpp`).
4. Run the tests and make sure they pass before opening a PR.

There's no strict style checklist — just follow the patterns already in the code.

## On using AI assistants

This project is AI-friendly. Feel free to use GitHub Copilot, Claude, GPT, or any other tool to help you write code, explore the codebase, or draft documentation.

One ask: **read and understand every line before submitting it.** AI tools are fast and often right, but they also introduce subtle bugs in code that looks correct on the surface. If something ends up in this codebase and turns out to be wrong, that's on us — the humans who reviewed and merged it. We don't blame the tools; we just want every contributor to own what they submit.

When in doubt, leave a comment in the PR explaining your reasoning. It helps review and builds shared understanding.

## Questions

Open a GitHub Discussion or an issue — both are fine. There are no silly questions here.
