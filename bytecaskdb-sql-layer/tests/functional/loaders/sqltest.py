# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""SQL:2016 Core: sqltest's tests, read as they are (suites/sqltest/).

A test is a case with sqltest's id and feature, and each of its statements a
step that must succeed: sqltest has no expected results. The SQL is run as
sqltest wrote it.
"""

from __future__ import annotations

import pathlib

import yaml

from casemodel import Case, Step

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "suites" / "sqltest" / "standards" / "2016"
NOT_IMPLEMENTED = ROOT / "not_implemented" / "sqltest.txt"


def not_implemented(path: pathlib.Path = NOT_IMPLEMENTED) -> set[str]:
    ids = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            ids.add(line)
    return ids


def load_sqltest(features: set[str] | None = None, root: pathlib.Path = TESTS,
                 marked: set[str] | None = None) -> list[Case]:
    marked = not_implemented() if marked is None else marked
    cases: list[Case] = []
    seen: set[str] = set()
    for path in sorted(root.glob("*/*.tests.yml")):
        group = f"sqltest/{path.name.removesuffix('.tests.yml')}"
        for doc in yaml.safe_load_all(path.read_text()):
            if doc is None:
                continue
            case_id, feature, sql = doc["id"], doc["feature"], doc["sql"]
            if case_id in seen:
                raise ValueError(f"{path.name}: {case_id} is in sqltest twice")
            seen.add(case_id)
            if features is not None and feature not in features:
                raise ValueError(f"{case_id}: {feature} is not in features.yaml")
            statements = [sql] if isinstance(sql, str) else sql
            cases.append(Case(suite="sqltest", id=case_id, features=(feature,),
                              steps=tuple(Step(sql=s) for s in statements),
                              unimplemented=case_id in marked, group=group))
    stale = marked - seen
    if stale:
        raise ValueError(f"not_implemented/sqltest.txt names tests sqltest doesn't have: {sorted(stale)[:5]}")
    return cases
