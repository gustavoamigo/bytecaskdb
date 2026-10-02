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
Dev Container, and Codespaces already have the toolchain. On the web the hook
also installs [Serena](https://oraios.github.io/serena/), best-effort.

## Serena (optional)

[Serena](https://oraios.github.io/serena/) gives coding agents symbol-level
navigation and editing through clangd. The repository is set up for it, and
everything is a silent no-op when it is not installed: the `serena` entry in
`.mcp.json` then serves an empty MCP server, and the hooks and build step do
nothing.

```bash
uv tool install -p 3.13 serena-agent
```

Then trust the checkout in `~/.serena/serena_config.yml` (Serena creates the
file on first run). Without this Serena ignores the project's clangd settings
and downloads its own clangd, which cannot read the module files clang builds:

```yaml
trusted_project_path_patterns:
  - /path/to/bytecaskdb/**   # also covers worktrees created under it
```

How the pieces fit:

- `.serena/project.yml` configures clangd only, using the clangd on `PATH`.
  It has no `project_name`, so Serena names each project after its folder and
  every git worktree is a project of its own, with its own cache under
  `.serena/` (gitignored).
- clangd reads `compile_commands.json`, which is gitignored and per checkout.
  Serena generates it on activation when it is missing; building
  `bytecask_tests` regenerates it when the build files or its sources change,
  then refreshes Serena's index in the background (log in
  `.serena/logs/index.log`). Both are skipped when Serena is not installed or
  `CI` is set.
- The index is incremental: Serena caches symbols per file by content, so a
  refresh asks clangd only about changed files. A changed
  `compile_commands.json` drops the whole cache. `scripts/serena_index.sh`
  refreshes it by hand.
- The Claude Code hooks (`.claude/hooks/serena.sh`) run Serena's `activate`,
  `remind`, `auto-approve` and `cleanup`
  [hooks](https://oraios.github.io/serena/02-usage/030_clients.html).
  `remind` denies runs of `grep` and code-file reads that ignore Serena's
  tools.

A worktree nested inside the main checkout (as Claude Code creates under
`.claude/worktrees/`) needs `xmake -P .`, or xmake builds the parent checkout.

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
