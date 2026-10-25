# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Our cases: cases/<suite>/<area>.yaml (*Our cases*)."""

from __future__ import annotations

import pathlib
import re

import yaml

from casemodel import Case, Step

CASES = pathlib.Path(__file__).resolve().parent.parent / "cases"

CASE_KEYS = {"name", "features", "setup", "teardown", "query", "params", "expected", "rowcount", "error",
             "steps", "ordered", "server_flags", "unimplemented"}
STEP_KEYS = {"sql", "expected", "rowcount", "error", "params", "ordered", "restart"}
SHORT_FORM_KEYS = {"query", "params", "expected", "rowcount", "error"}


class CaseLoader(yaml.SafeLoader):
    """YAML where only what a case means is typed: null, true and false,
    integers. A number with a point, a date or `yes` is the text it is
    written as, so `1.50` and `2026-10-25` are PostgreSQL's text form as
    they stand."""


CaseLoader.yaml_implicit_resolvers = {
    first: [(tag, regexp) for tag, regexp in resolvers
            if tag in ("tag:yaml.org,2002:null", "tag:yaml.org,2002:int")]
    for first, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
CaseLoader.add_implicit_resolver("tag:yaml.org,2002:bool", re.compile(r"^(?:true|false)$"), list("tf"))


class CaseError(ValueError):
    pass


def _text(value: object, where: str) -> str | None:
    """A value as PostgreSQL prints it."""
    if value is None or isinstance(value, str):
        return value
    if isinstance(value, bool):
        return "t" if value else "f"
    if isinstance(value, int):
        return str(value)
    raise CaseError(f"{where}: {value!r} is not a value; write it as text")


def _rows(rows: object, where: str) -> tuple[tuple[str | None, ...], ...]:
    if not isinstance(rows, list) or not all(isinstance(r, list) for r in rows):
        raise CaseError(f"{where}: expected is a list of rows, each a list")
    return tuple(tuple(_text(v, where) for v in row) for row in rows)


def _step(raw: dict, ordered: bool, where: str) -> Step:
    unknown = set(raw) - STEP_KEYS
    if unknown:
        raise CaseError(f"{where}: unknown step keys {sorted(unknown)}")
    if "restart" in raw:
        if raw != {"restart": True}:
            raise CaseError(f"{where}: restart is a step on its own, `restart: true`")
        return Step(restart=True)
    if not isinstance(raw.get("sql"), str):
        raise CaseError(f"{where}: a step has sql")
    if "error" in raw and ("expected" in raw or "rowcount" in raw):
        raise CaseError(f"{where}: a step that fails has no rows or rowcount")
    if "expected" in raw and "rowcount" in raw:
        raise CaseError(f"{where}: expected and rowcount are alternatives")
    error = raw.get("error")
    if error is not None and not (isinstance(error, str) and len(error) == 5):
        raise CaseError(f"{where}: error is a SQLSTATE, a string of five characters")
    rowcount = raw.get("rowcount")
    if rowcount is not None and (not isinstance(rowcount, int) or isinstance(rowcount, bool)):
        raise CaseError(f"{where}: rowcount is a number")
    params = raw.get("params")
    if params is not None and not isinstance(params, list):
        raise CaseError(f"{where}: params is a list")
    return Step(
        sql=raw["sql"],
        expected=_rows(raw["expected"], where) if "expected" in raw else None,
        rowcount=rowcount,
        error=error,
        params=tuple(_text(p, where) for p in params) if params is not None else None,
        ordered=raw.get("ordered", ordered),
    )


def _case(raw: dict, suite: str, area: str, group: str, features: set[str] | None) -> Case:
    name = raw.get("name")
    if not isinstance(name, str) or not name:
        raise CaseError(f"{area}.yaml: a case has a name")
    where = f"{area}/{name}"
    unknown = set(raw) - CASE_KEYS
    if unknown:
        raise CaseError(f"{where}: unknown keys {sorted(unknown)}")
    listed = raw.get("features")
    if not isinstance(listed, list) or not listed or not all(isinstance(f, str) for f in listed):
        raise CaseError(f"{where}: features is a list of feature ids, at least one")
    if features is not None:
        missing = [f for f in listed if f not in features]
        if missing:
            raise CaseError(f"{where}: not in features.yaml: {missing}")
    if ("query" in raw) == ("steps" in raw):
        raise CaseError(f"{where}: a case has `query` or `steps`, one of them")
    ordered = raw.get("ordered", False)
    if "query" in raw:
        short = {"sql": raw["query"], **{k: raw[k] for k in SHORT_FORM_KEYS - {"query"} if k in raw}}
        steps = (_step(short, ordered, where),)
    else:
        if set(raw) & (SHORT_FORM_KEYS - {"query"}):
            raise CaseError(f"{where}: with steps, expected and error go on a step")
        if not isinstance(raw["steps"], list) or not raw["steps"]:
            raise CaseError(f"{where}: steps is a list, not empty")
        steps = tuple(_step(s, ordered, where) for s in raw["steps"])
    for key in ("setup", "teardown", "server_flags"):
        if not all(isinstance(s, str) for s in raw.get(key, [])):
            raise CaseError(f"{where}: {key} is a list of strings")
    return Case(
        suite=suite,
        id=where,
        features=tuple(listed),
        steps=steps,
        setup=tuple(raw.get("setup", ())),
        teardown=tuple(raw.get("teardown", ())),
        server_flags=tuple(raw.get("server_flags", ())),
        unimplemented=bool(raw.get("unimplemented", False)),
        group=group,
    )


def load_file(path: pathlib.Path, suite: str, features: set[str] | None = None) -> list[Case]:
    area = path.stem
    raw = yaml.load(path.read_text(), Loader=CaseLoader) or []
    if not isinstance(raw, list):
        raise CaseError(f"{path.name}: a file is a list of cases")
    return [_case(r, suite, area, f"{suite}/{area}", features) for r in raw]


def load_suite(suite: str, root: pathlib.Path = CASES, features: set[str] | None = None) -> list[Case]:
    cases: list[Case] = []
    seen: set[str] = set()
    for path in sorted((root / suite).glob("*.yaml")):
        for case in load_file(path, suite, features):
            if case.id in seen:
                raise CaseError(f"{suite}: {case.id} is in the suite twice")
            seen.add(case.id)
            cases.append(case)
    return cases
