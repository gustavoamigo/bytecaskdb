#!/usr/bin/env python3
"""Fails on any thread_local in engine or binding code without a TLS tag.

A thread_local outlives every DB the thread touches, so one that keeps a
DB's state, or recognises a DB by address, leaks across instances: a closed
DB's files stay open in another thread's cache, or a new DB allocated at a
dead one's address inherits what it left (BC-243, #306). Each declaration
says which kind it is, with a comment holding `TLS: <kind>` on its line or
in the comment block directly above it (pragma lines may sit in between):

    TLS: scratch   holds no DB data, only bytes of the current call. Say how
                   large it can grow if it can.
    TLS: process   shared by every DB on purpose (counter stripes, the node
                   pool, a zstd context).
    TLS: per-DB    holds or identifies one DB. Say how it is released when
                   the DB closes, what stops a later DB from matching it, and
                   which test checks both.

Usage: scripts/check_thread_local.py [root]
"""

import os
import pathlib
import re
import sys

KINDS = ("scratch", "process", "per-DB")
TAG = re.compile(r"TLS:\s*(\S+)")
SOURCES = {".cpp", ".cppm", ".cc", ".h", ".hpp"}
# Tests, benchmarks and vendored code own no engine state across instances.
SKIP = {"third_party", "tests", "benchmarks", "build", ".xmake", ".git",
        "node_modules"}


def code_part(line: str) -> str:
    return line.split("//", 1)[0]


def is_comment_or_pragma(line: str) -> bool:
    s = line.strip()
    return s.startswith("//") or s.startswith("#pragma")


def tag_for(lines: list[str], i: int) -> str | None:
    m = TAG.search(lines[i])
    if m:
        return m.group(1)
    j = i - 1
    while j >= 0 and is_comment_or_pragma(lines[j]):
        m = TAG.search(lines[j])
        if m:
            return m.group(1)
        j -= 1
    return None


def check(path: pathlib.Path) -> list[str]:
    errors = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if not re.search(r"\bthread_local\b", code_part(line)):
            continue
        if "#define" in line:
            continue
        kind = tag_for(lines, i)
        if kind is None:
            errors.append(f"{path}:{i + 1}: thread_local without a "
                          f"'TLS: <{'|'.join(KINDS)}>' tag")
        elif kind not in KINDS:
            errors.append(f"{path}:{i + 1}: unknown TLS kind '{kind}', "
                          f"expected one of {', '.join(KINDS)}")
    return errors


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    errors = []
    count = 0
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if path.suffix not in SOURCES or SKIP & set(rel.parts):
            continue
        errors += check(path)
        count += sum(bool(re.search(r"\bthread_local\b", code_part(l)))
                     for l in path.read_text(encoding="utf-8").splitlines())
    for e in errors:
        print(f"::error::{e}" if "GITHUB_ACTIONS" in os.environ else e)
    print(f"{count} thread_local declarations, {len(errors)} untagged")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
