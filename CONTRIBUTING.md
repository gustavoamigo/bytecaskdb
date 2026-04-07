# Contributing to ByteCask

Thanks for your interest. ByteCask is in early development, so contributions at every level are welcome — from fixing a typo to implementing a new feature.

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
