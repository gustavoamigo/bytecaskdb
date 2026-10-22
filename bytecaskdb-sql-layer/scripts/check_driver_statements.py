#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Fails when the HammerDB driver sends a statement the proof suite does not
check against PostgreSQL (docs/server_design.md, *The HammerDB driver*).

The driver's statements are the `set ::SQL(name) {...}` lines of
hammerdb/bytecask_tprocc.tcl. Each must be one tests/proof/test_tpcc.py
runs against PostgreSQL 17, or one in PENDING: Stock-Level's, which the
proof gains with the front end's slice 5. A PENDING statement the proof
already runs is stale and fails too, so the list empties as slices land.

Usage: bytecaskdb-sql-layer/scripts/check_driver_statements.py [layer root]
"""

from __future__ import annotations

import ast
import pathlib
import re
import sys

# Stock-Level (docs/frontend_design.md, *The statements*, S1 and S2): until
# slice 5 proves them, the run script's --mix leaves Stock-Level out, and the
# monitor's S1, a point lookup by primary key, is the one in use.
PENDING = {
    "SELECT d_next_o_id FROM district WHERE d_w_id = $1 AND d_id = $2",
    "SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 "
    "AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4",
}

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
    proved = proved_statements(root / "tests" / "proof" / "test_tpcc.py")
    failures = []
    if not driver:
        failures.append("no `set ::SQL(name) {...}` statements found in hammerdb/bytecask_tprocc.tcl")
    for name, sql in sorted(driver.items()):
        if sql not in proved and sql not in PENDING:
            failures.append(f"{name}: not a statement tests/proof/test_tpcc.py checks against PostgreSQL: {sql}")
    for sql in sorted(PENDING & proved):
        failures.append(f"PENDING holds a statement the proof now runs; remove it: {sql}")
    for f in failures:
        print(f"check_driver_statements: {f}", file=sys.stderr)
    if not failures:
        print(f"check_driver_statements: {len(driver)} driver statements, each proved or pending")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
