#!/usr/bin/env python3
"""Fails when a contract names something its public interface does not declare.

A contract states what is guaranteed, not how it works. A sentence that
names a private function, field or type describes mechanism, and mechanism
changes under refactors that change no guarantee (#399). Each contract is
paired with its public interface, and every identifier in a backticked span
of the contract must be one of:

  - a name declared in the interface's files, comments excluded;
  - a name the pair allows (ALLOWED and the pair's own words below).

The pairs:

  - engine: CONTRACT.md against include/bytecask.hpp;
  - sql-layer: bytecaskdb-sql-layer/CONTRACT.md against the plan API's C
    header and the embedded API's module interface, the two surfaces a
    caller of the layer programs against, plus SQL's own words.

Spans that are not identifiers are skipped: file names and paths, test
tags like `[limits]`, numbers, and the placeholders of examples (`k`, `v1`,
the `db` of `db.vacuum()`).

Usage: scripts/check_contract_vocabulary.py [--only engine|sql-layer] [root]
"""

import pathlib
import re
import sys


IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
SPAN = re.compile(r"`([^`]*)`")
# Not an identifier: a path or file name, a test tag, a template like <stem>.
SKIP_SPAN = re.compile(r"^(?:[<.\[]|.*/)")
# One- or two-letter names, and v1, v2: the placeholders of examples.
PLACEHOLDER = re.compile(r"^(?:[A-Za-z_]{1,2}|v\d+)$")
# The receiver of a call in an example (`follower.durable_sequence()`) is a
# placeholder too; the call is what has to be public.
RECEIVER = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*(?=\.)")

ALLOWED = {
    # C++ and the standard library.
    "std", "const", "noexcept", "true", "false", "nullopt", "operator",
    "while", "optional", "span", "shared_ptr", "invalid_argument",
    "runtime_error", "logic_error", "system_error", "bad_alloc",
    "milliseconds", "max", "min",
    # The platform. A syscall may be named where a public option (io_backend)
    # or the storage assumption under `open` selects it.
    "fdatasync", "fsync", "pread", "pwritev", "writev", "mmap", "ftruncate",
    "SIGBUS", "O_EXCL", "O_DIRECT", "MAP_SHARED", "MAP_PRIVATE", "ENOENT",
    "EIO",
    # File naming, documented in docs/file_format.md.
    "stem",
}

# The words of SQL a contract about a SQL layer uses: statements, clauses and
# constraint names, which no C++ interface declares.
SQL_WORDS = {
    "SELECT", "INSERT", "UPDATE", "DELETE", "VALUES", "WHERE", "CREATE",
    "DROP", "ALTER", "TABLE", "INDEX", "UNIQUE", "ADD", "COLUMN", "NOT",
    "NULL", "DEFAULT", "PRIMARY", "KEY", "DESC", "ASC", "REBUILD", "SET",
    "IF", "EXISTS",
}

# name -> (contract, interface files, extra allowed words)
PAIRS = {
    "engine": ("CONTRACT.md", ["include/bytecask.hpp"], set()),
    "sql-layer": (
        "bytecaskdb-sql-layer/CONTRACT.md",
        ["bytecaskdb-sql-layer/algebra/bc_plan.h", "bytecaskdb-sql-layer/api/database.cppm"],
        SQL_WORDS,
    ),
}


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def public_names(root: pathlib.Path, interface: list[str]) -> set[str]:
    names: set[str] = set()
    for path in interface:
        names |= set(IDENT.findall(strip_comments((root / path).read_text())))
    return names


def contract_names(root: pathlib.Path, contract: str) -> dict[str, list[int]]:
    """Every identifier in a backticked span, with the lines it is on."""
    found: dict[str, list[int]] = {}
    in_fence = False
    for lineno, line in enumerate((root / contract).read_text().splitlines(), 1):
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for span in SPAN.findall(line):
            if SKIP_SPAN.match(span):
                continue
            for name in IDENT.findall(RECEIVER.sub("", span)):
                if PLACEHOLDER.match(name):
                    continue
                found.setdefault(name, []).append(lineno)
    return found


def check(root: pathlib.Path, contract: str, interface: list[str], words: set[str]) -> bool:
    public = public_names(root, interface) | ALLOWED | words
    used = contract_names(root, contract)

    unknown = {n: ls for n, ls in used.items() if n not in public}

    for name in sorted(unknown):
        lines = ", ".join(str(l) for l in unknown[name][:5])
        print(f"{contract}:{lines}: `{name}` is not declared in {', '.join(interface)}. "
              "The contract states guarantees in public terms: say what is "
              "guaranteed without naming the mechanism, or declare the name.")
    if not unknown:
        print(f"{contract}: {len(used)} names checked")
    return not unknown


def main() -> int:
    args = sys.argv[1:]
    only = None
    if len(args) >= 2 and args[0] == "--only":
        only = args[1]
        args = args[2:]
        if only not in PAIRS:
            print(f"--only takes one of {', '.join(PAIRS)}, not {only}")
            return 2
    root = pathlib.Path(args[0] if args else ".")
    ok = True
    for name, (contract, interface, words) in PAIRS.items():
        if only is None or only == name:
            ok = check(root, contract, interface, words) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
