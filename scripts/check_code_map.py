#!/usr/bin/env python3
"""Fails when docs/code_map.md names a path that does not exist.

The map is read by coding agents to find code without searching, so a stale
path costs more than no path. A path is a backticked token that contains a
'/' or ends in a source extension. Bare engine file names resolve against
bytecaskdb/ (the engine table lists them without the prefix); tokens with
spaces, '<', '(' or '=' are commands or names, not paths.

Usage: scripts/check_code_map.py [map]
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXTENSIONS = (".cppm", ".cpp", ".cc", ".h", ".hpp", ".py", ".md", ".sh", ".ts")
TOKEN = re.compile(r"`([^`]+)`")


def looks_like_path(token: str) -> bool:
    if re.search(r"[\s<(=\[]", token):
        return False
    return "/" in token or token.endswith(EXTENSIONS)


def exists(token: str) -> bool:
    for base in (ROOT, ROOT / "bytecaskdb"):
        if "*" in token:
            if any(base.glob(token)):
                return True
        elif (base / token).exists():
            return True
    return False


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "docs" / "code_map.md")
    seen = set()
    missing = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        for token in TOKEN.findall(line):
            if token in seen or not looks_like_path(token):
                continue
            seen.add(token)
            if not exists(token):
                missing.append((line_no, token))
    for line_no, token in missing:
        print(f"{path.name}:{line_no}: `{token}` does not exist")
    if missing:
        return 1
    print(f"{path.name}: {len(seen)} paths exist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
