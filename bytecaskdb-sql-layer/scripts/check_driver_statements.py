#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Fails when the HammerDB driver sends a statement the proof suite does not
check against PostgreSQL (docs/server_design.md, *The HammerDB driver*).

The driver's statements are the `set ::SQL(name) {...}` lines of
hammerdb/bytecask_tprocc.tcl. Each must be one tests/proof/test_tprocc.py
runs against PostgreSQL 17.

Usage: bytecaskdb-sql-layer/scripts/check_driver_statements.py [layer root]
"""

from __future__ import annotations

import ast
import pathlib
import re
import sys

STATEMENT = re.compile(r"^(SELECT|INSERT|UPDATE|DELETE)\b")
DRIVER_LINE = re.compile(r"^set ::SQL\((\w+)\) \{(.*)\}\s*$")


def driver_statements(path: pathlib.Path) -> dict[str, str]:
    out = {}
    for line in path.read_text().splitlines():
        m = DRIVER_LINE.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def proved_statements(path: pathlib.Path) -> set[str]:
    """Every string constant of the proof's module that is a statement,
    adjacent literals joined as Python joins them."""
    tree = ast.parse(path.read_text())
    return {node.value for node in ast.walk(tree)
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and STATEMENT.match(node.value)}


def main() -> int:
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
    driver = driver_statements(root / "hammerdb" / "bytecask_tprocc.tcl")
    proved = proved_statements(root / "tests" / "proof" / "test_tprocc.py")
    failures = []
    if not driver:
        failures.append("no `set ::SQL(name) {...}` statements found in hammerdb/bytecask_tprocc.tcl")
    for name, sql in sorted(driver.items()):
        if sql not in proved:
            failures.append(f"{name}: not a statement tests/proof/test_tprocc.py checks against PostgreSQL: {sql}")
    for f in failures:
        print(f"check_driver_statements: {f}", file=sys.stderr)
    if not failures:
        print(f"check_driver_statements: {len(driver)} driver statements, each proved")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
