#!/usr/bin/env python3
"""Fails on a CONTRACT.md row that names a test or script that does not exist.

Each guarantee's "Proved by" cell names what fails if the row is broken: Catch2
tests by name (in double quotes) or by tag (in backticks, `[tag]`), or a check
script the repository runs (a backticked path). A test renamed or deleted
leaves its row pointing at nothing, and the row still reads as proved. This
check keeps the two together.

Test names and tags are read from the test binaries themselves
(`--list-tests`, `--list-tags`), so they match what Catch2 would run,
including the generated proof cells. A name must match a test exactly; a tag
must be carried by at least one test. A name or tag found in none of the given
binaries fails. A backticked path under scripts/ or tests/ must exist.

Cells that name nothing (`none`, `n/a: ...`) are not checked, and neither is a
name starting with "proof: ", which names a pytest of the SQL layer's proof
suite against PostgreSQL.

Usage: scripts/check_contract_tests.py <CONTRACT.md> <test binary>... [--root DIR]
"""

import argparse
import pathlib
import re
import subprocess
import sys

QUOTED = re.compile(r'"([^"]+)"')
TAG = re.compile(r'`(\[[^`\]]+\])`')
PATH = re.compile(r'`((?:scripts|tests)/[^`\s]+)`')
TEST_CASE = re.compile(r'TEST_CASE\(\s*"([^"]+)"')
SEPARATOR = re.compile(r'^\|[\s:|-]+\|$')


def listing(binary: str, flag: str, spec: str = "") -> set[str]:
    cmd = [binary, flag, "--verbosity", "quiet"] + ([spec] if spec else [])
    run = subprocess.run(cmd, capture_output=True, text=True, check=False)
    return {line.strip() for line in run.stdout.splitlines() if line.strip()}


def conditional_names(root: pathlib.Path) -> set[str]:
    """Names of the TEST_CASEs under an #if in the test sources."""
    out: set[str] = set()
    for path in sorted((root / "tests").rglob("*.cpp")):
        depth = 0
        for line in path.read_text(encoding="utf-8",
                                   errors="replace").splitlines():
            stripped = line.lstrip()
            if re.match(r"#\s*if", stripped):
                depth += 1
            elif re.match(r"#\s*endif", stripped):
                depth -= 1
            elif depth > 0:
                m = TEST_CASE.search(line)
                if m:
                    out.add(m.group(1))
    return out


def cells(row: str) -> list[str]:
    """Splits a table row at the pipes outside backticks."""
    out, cur, in_code = [], "", False
    for ch in row.strip().strip("|"):
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def proof_cells(text: str):
    """Yields (line number, cell) for every "Proved by" cell."""
    lines = text.splitlines()
    i = 0
    while i < len(lines) - 1:
        if lines[i].startswith("|") and SEPARATOR.match(lines[i + 1]):
            header = [c.lower() for c in cells(lines[i])]
            col = next((n for n, c in enumerate(header)
                        if c.startswith("proved by")), None)
            j = i + 2
            while j < len(lines) and lines[j].startswith("|"):
                if col is not None:
                    row = cells(lines[j])
                    if col < len(row):
                        yield j + 1, row[col]
                j += 1
            i = j
        else:
            i += 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("contract")
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--root", default=".")
    args = ap.parse_args()

    names: set[str] = set()
    tags: set[str] = set()
    for binary in args.binaries:
        # A hidden test ([.]) is listed only when the spec asks for it.
        names |= listing(binary, "--list-tests")
        names |= listing(binary, "--list-tests", "[.]")
        tags |= listing(binary, "--list-tags")
    if not names:
        print("check_contract_tests: the binaries list no tests")
        return 1

    root = pathlib.Path(args.root)
    other_config = conditional_names(root)
    text = pathlib.Path(args.contract).read_text(encoding="utf-8")
    failures = checked = 0
    for line, cell in proof_cells(text):
        for name in QUOTED.findall(cell):
            if name.startswith("proof: "):
                continue
            checked += 1
            if name not in names and name not in other_config:
                print(f'{args.contract}:{line}: no test named "{name}"')
                failures += 1
        for tag in TAG.findall(cell):
            checked += 1
            if tag not in tags:
                print(f"{args.contract}:{line}: no test carries {tag}")
                failures += 1
        for path in PATH.findall(cell):
            checked += 1
            if not (root / path).exists():
                print(f"{args.contract}:{line}: {path} does not exist")
                failures += 1
    print(f"check_contract_tests: {checked} reference(s) checked, "
          f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
