#!/usr/bin/env python3
"""Fails when a contract names something its public interface does not declare.

A contract states what is guaranteed, not how it works. A sentence that
names a private function, field or type describes mechanism, and mechanism
changes under refactors that change no guarantee (#399). Each contract is
paired with its public interface, and every identifier in a backticked span
of the contract must be one of:

  - a name declared in the interface's files, comments excluded;
  - a name the pair allows (ALLOWED and the pair's own words below).

A C++ module interface (.cppm) contributes what an importer of it sees: its
text after `export module`, without the members under `private:`, and the
same of every module it re-exports with `export import`, found by name among
the root's .cppm files.

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

import argparse
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
}

# The engine's own words. A syscall may be named where a public option
# (io_backend) or the storage assumption under `open` selects it; file naming
# is documented in docs/file_format.md.
ENGINE_WORDS = {
    "fdatasync", "fsync", "pread", "pwritev", "writev", "mmap", "ftruncate",
    "SIGBUS", "O_EXCL", "O_DIRECT", "MAP_SHARED", "MAP_PRIVATE", "ENOENT",
    "EIO",
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
    "engine": ("CONTRACT.md", ["include/bytecask.hpp"], ENGINE_WORDS),
    "sql-layer": (
        "bytecaskdb-sql-layer/CONTRACT.md",
        ["bytecaskdb-sql-layer/algebra/bc_plan.h", "bytecaskdb-sql-layer/api/database.cppm"],
        SQL_WORDS,
    ),
}


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


EXPORT_MODULE = re.compile(r"^export\s+module\s+([\w.]+)\s*;", re.M)
EXPORT_IMPORT = re.compile(r"^export\s+import\s+([\w.]+)\s*;", re.M)
# A class's private members, up to its next access specifier or its closing
# brace at column 0 (the layer's classes are not nested).
PRIVATE = re.compile(r"\bprivate:.*?(?=\bpublic:|\bprotected:|^\};)", re.S | re.M)


def module_text(src: str) -> str:
    """What an importer of a module interface can name."""
    m = EXPORT_MODULE.search(src)
    if m:
        src = src[m.end():]  # drops the global module fragment
    return PRIVATE.sub(" ", src)


def public_names(root: pathlib.Path, interface: list[str]) -> set[str]:
    modules = {}
    for path in root.rglob("*.cppm"):
        m = EXPORT_MODULE.search(strip_comments(path.read_text()))
        if m:
            modules[m.group(1)] = path
    names: set[str] = set()
    pending = [root / path for path in interface]
    seen: set[pathlib.Path] = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        src = strip_comments(path.read_text())
        if path.suffix == ".cppm":
            pending += [modules[m] for m in EXPORT_IMPORT.findall(src) if m in modules]
            src = module_text(src)
        names |= set(IDENT.findall(src))
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
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", choices=PAIRS, help="check one pair (default: all)")
    parser.add_argument("root", nargs="?", default=".", type=pathlib.Path)
    args = parser.parse_args()
    ok = True
    for name, (contract, interface, words) in PAIRS.items():
        if args.only is None or args.only == name:
            ok = check(args.root, contract, interface, words) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
