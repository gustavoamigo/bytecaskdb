#!/usr/bin/env python3
"""Fails when README.md grows past MAX_LINES.

CLAUDE.md names the README as the project's entry point, for new readers and
for coding agents alike. Detail belongs in docs/ (the complete feature list
in docs/bytecask_design.md, the API in docs/api_reference.md, benchmark
tables in docs/performance.md), and the README links to it.

Usage: scripts/check_readme_size.py [path]
"""

import pathlib
import sys

MAX_LINES = 150


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "README.md")
    lines = len(path.read_text(encoding="utf-8").splitlines())
    if lines > MAX_LINES:
        print(f"{path}: {lines} lines, over the {MAX_LINES}-line limit. "
              "Move detail into docs/ and link to it.")
        return 1
    print(f"{path}: {lines} lines (limit {MAX_LINES})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
