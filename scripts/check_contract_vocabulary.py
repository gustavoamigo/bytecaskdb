#!/usr/bin/env python3
"""Fails when CONTRACT.md names something the public header does not declare.

The contract states what the engine guarantees, not how it works. A sentence
that names a private function, field or type describes mechanism, and
mechanism changes under refactors that change no guarantee (#399). So every
identifier in a backticked span of CONTRACT.md must be one of:

  - a name declared in include/bytecask.hpp, comments excluded;
  - a C++ or standard-library name, or a platform name the contract may use
    because a public option or a stated storage assumption selects it
    (ALLOWED below).

Spans that are not identifiers are skipped: file names and paths, test
tags like `[limits]`, numbers, and the placeholders of examples (`k`, `v1`,
the `db` of `db.vacuum()`).

Usage: scripts/check_contract_vocabulary.py [root]
"""

import pathlib
import re
import sys

CONTRACT = "CONTRACT.md"
HEADER = "include/bytecask.hpp"

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


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def public_names(root: pathlib.Path) -> set[str]:
    return set(IDENT.findall(strip_comments((root / HEADER).read_text())))


def contract_names(root: pathlib.Path) -> dict[str, list[int]]:
    """Every identifier in a backticked span, with the lines it is on."""
    found: dict[str, list[int]] = {}
    in_fence = False
    for lineno, line in enumerate((root / CONTRACT).read_text().splitlines(), 1):
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


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    public = public_names(root) | ALLOWED
    used = contract_names(root)

    unknown = {n: ls for n, ls in used.items() if n not in public}

    for name in sorted(unknown):
        lines = ", ".join(str(l) for l in unknown[name][:5])
        print(f"{CONTRACT}:{lines}: `{name}` is not declared in {HEADER}. "
              "The contract states guarantees in public terms: say what is "
              "guaranteed without naming the mechanism, or declare the name.")
    if not unknown:
        print(f"contract vocabulary: {len(used)} names checked")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
