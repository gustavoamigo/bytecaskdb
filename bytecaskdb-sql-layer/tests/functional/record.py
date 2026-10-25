#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Records expected results from PostgreSQL into a case file
(docs/functional_testing_design.md, *Recording expected results*).

    record.py --pg "host=localhost user=postgres password=... dbname=postgres" cases/postgresql/joins.yaml

It runs each case on PostgreSQL and writes what a step returned into its
`expected`, `rowcount` or `error`. It fills only the steps that have none of
the three. `--case NAME` records that case's steps again, over what is there,
except the steps after a `restart`, whose values are kept.
The file keeps its comments, key order and layout.
"""

from __future__ import annotations

import argparse
import io
import pathlib
import re
import sys
import uuid

import yaml
from ruamel.yaml import YAML
from ruamel.yaml.comments import CommentedMap, CommentedSeq
from ruamel.yaml.scalarstring import DoubleQuotedScalarString
from ruamel.yaml.tokens import CommentToken

from casemodel import Value
from executor import Outcome, connect, send, sort_rows
from loaders.yaml_cases import CaseLoader

RESULT_KEYS = ("expected", "rowcount", "error")
CANONICAL_INT = re.compile(r"-?(0|[1-9][0-9]{0,14})")
FLOW_WIDTH = 72


def scalar(value: Value):
    """A value as the file writes it: an integer as one, other text as text."""
    if value is None:
        return None
    if CANONICAL_INT.fullmatch(value):
        return int(value)
    try:
        # Text either YAML reader would take for another type is quoted.
        plain = yaml.load(value, Loader=CaseLoader) == value and YAML(typ="safe").load(value) == value
    except Exception:  # not a plain scalar at all: ':' or '#' in it
        plain = False
    return value if plain else DoubleQuotedScalarString(value)


def rows_node(rows: list[tuple[Value, ...]]) -> CommentedSeq:
    outer = CommentedSeq()
    for row in rows:
        inner = CommentedSeq(scalar(v) for v in row)
        inner.fa.set_flow_style()
        outer.append(inner)
    flat = "[" + ", ".join("[" + ", ".join("null" if v is None else str(v) for v in r) + "]" for r in rows) + "]"
    if len(flat) <= FLOW_WIDTH:
        outer.fa.set_flow_style()
    return outer


def dump(yml: YAML, data) -> str:
    """ruamel indents a root sequence's items as it does a nested one's, and the
    files write the root's at the margin: take the two columns back, from
    everything but a comment, which it keeps where the file had it."""
    buf = io.StringIO()
    yml.dump(data, buf)
    return "".join(
        line[2:] if line.startswith("  ") and not line.lstrip().startswith("#") else line
        for line in buf.getvalue().splitlines(keepends=True))


def put(mapping: CommentedMap, after: tuple[str, ...], key: str, value, carry=None) -> None:
    """Insert `key` after the last of `after` present, so a step reads `sql`, `params`, then its result.

    ruamel keeps the blank lines after a mapping's last key as that key's
    comment, so they would end up before the key put after it: they move to the
    new last key. `carry` is that comment from a key that was taken out.
    """
    keys = list(mapping)
    prev = max((k for k in after if k in mapping), key=keys.index)
    position = keys.index(prev) + 1
    mapping.insert(position, key, value)
    if position < len(keys):
        return
    if carry is not None:
        mapping.ca.items[key] = carry
        return
    token = mapping.ca.items.get(prev, [None] * 4)[2]
    if token is not None:
        eol, blanks = token.value[: token.value.index("\n") + 1], token.value[token.value.index("\n") + 1:]
        if blanks:
            moved = CommentToken("\n" + blanks, token.start_mark, None)
            if eol.strip():
                token.value = eol
            else:
                del mapping.ca.items[prev]
            mapping.ca.items[key] = [None, None, moved, None]


def record_into(node: CommentedMap, anchors: tuple[str, ...], out: Outcome, ordered: bool, overwrite: bool) -> bool:
    """Write `out` into a step or a short-form case. True if it changed."""
    if not overwrite and any(k in node for k in RESULT_KEYS):
        return False
    last = list(node)[-1]
    carry = node.ca.items.get(last) if last in RESULT_KEYS else None
    for k in RESULT_KEYS:
        node.pop(k, None)
    if out.failed:
        if out.sqlstate is None:
            raise RuntimeError(f"PostgreSQL did not answer: {out.message}")
        put(node, anchors, "error", DoubleQuotedScalarString(out.sqlstate), carry)
    elif out.rows is not None:
        put(node, anchors, "expected", rows_node(list(out.rows) if ordered else sort_rows(out.rows)), carry)
    elif out.rowcount is not None:
        put(node, anchors, "rowcount", out.rowcount, carry)
    return True


def record_case(raw: CommentedMap, dsn: str, overwrite: bool) -> int:
    schema = "f_" + uuid.uuid4().hex[:12]
    conn = connect(dsn)
    changed = 0
    try:
        for sql in (f"CREATE SCHEMA {schema}", f"SET search_path TO {schema}"):
            assert not send(conn, sql).failed
        for sql in raw.get("setup", []):
            out = send(conn, sql)
            if out.failed:
                raise RuntimeError(f"setup {sql!r} failed on PostgreSQL: {out.sqlstate}: {out.message}")
        case_ordered = bool(raw.get("ordered", False))
        if "query" in raw:
            params = tuple(None if p is None else str(p) for p in raw["params"]) if "params" in raw else None
            out = send(conn, raw["query"], params)
            changed += record_into(raw, ("query", "params"), out, case_ordered, overwrite)
        else:
            restarted = False
            for step in raw["steps"]:
                if "restart" in step:
                    restarted = True
                    continue  # PostgreSQL has no layer to restart
                params = tuple(None if p is None else str(p) for p in step["params"]) if "params" in step else None
                out = send(conn, step["sql"], params)
                # After a restart PostgreSQL's answer can differ from the layer's (a
                # transaction left open is still open there), so a value already
                # written there is a decision, and --case keeps it.
                changed += record_into(step, ("sql", "params"), out, bool(step.get("ordered", case_ordered)),
                                       overwrite and not restarted)
        for sql in raw.get("teardown", []):
            send(conn, sql)
    finally:
        send(conn, "RESET search_path")
        send(conn, f"DROP SCHEMA IF EXISTS {schema} CASCADE")
        conn.finish()
    return changed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pg", required=True, help="a libpq connection string for PostgreSQL 17")
    ap.add_argument("--case", action="append", default=[], metavar="NAME",
                    help="record this case again, over its recorded values (repeatable)")
    ap.add_argument("files", nargs="+", type=pathlib.Path)
    args = ap.parse_args()

    yml = YAML()
    yml.preserve_quotes = True
    yml.width = 4096
    yml.indent(mapping=2, sequence=4, offset=2)
    status = 0
    for path in args.files:
        data = yml.load(path.read_text())
        known = {c["name"] for c in data}
        for name in args.case:
            if name not in known and len(args.files) == 1:
                print(f"{path}: no case named {name!r}", file=sys.stderr)
                status = 1
        changed = cases = 0
        for raw in data:
            again = raw["name"] in args.case
            if args.case and not again:
                continue
            cases += 1
            changed += record_case(raw, args.pg, overwrite=again)
        if changed:
            path.write_text(dump(yml, data))
        print(f"{path}: {changed} steps recorded in {cases} cases")
    return status


if __name__ == "__main__":
    sys.exit(main())
