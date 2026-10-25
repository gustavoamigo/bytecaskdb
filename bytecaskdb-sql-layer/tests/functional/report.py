#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The conformance report, as Markdown, from a conformance run's results.json
(docs/functional_testing_design.md, *How a run works*):

    pytest bytecaskdb-sql-layer/tests/functional --conformance --results=results.json
    report.py results.json > report.md
"""

from __future__ import annotations

import collections
import json
import pathlib
import sys

from manifest import Feature, load_manifest

# Each suite's name in the report, what a test of it is, and what it checks.
SUITES = {
    "sqltest": ("SQL:2016 Core", "sqltest tests", "statement accepted"),
    "postgresql": ("PostgreSQL features", "cases", "results"),
    "bytecaskdb": ("ByteCaskDB features", "cases", "results"),
    "tprocc": ("TPROC-C", "cases", "consistency conditions 1-9"),
}


def percent(passed: int, total: int) -> str:
    return f"{100 * passed / total:.0f}%" if total else "-"


def suite_table(results: list[dict]) -> list[str]:
    by_suite = collections.defaultdict(list)
    for r in results:
        by_suite[r["suite"]].append(r)
    unknown = set(by_suite) - set(SUITES)
    if unknown:
        raise ValueError(f"results name suites the report doesn't know: {sorted(unknown)}")
    lines = ["| Suite | Coverage | Tests | Checks |", "|---|---|---|---|"]
    for key, (name, unit, checks) in SUITES.items():
        rs = by_suite.get(key, [])
        passed = sum(r["outcome"] == "pass" for r in rs)
        tests = f"{passed}/{len(rs)} {unit}" if rs else "not run"
        lines.append(f"| {name} | {percent(passed, len(rs))} | {tests} | {checks} |")
    return lines


def layer_state(cases: list[dict]) -> str:
    if not cases:
        return "no case"
    passed = sum(c["outcome"] == "pass" for c in cases)
    word = "all" if passed == len(cases) else "some" if passed else "none"
    return f"{word} ({passed}/{len(cases)})"


def feature_tables(results: list[dict], manifest: dict[str, Feature]) -> list[str]:
    by_feature = collections.defaultdict(list)
    for r in results:
        for f in r["features"]:
            if f not in manifest:
                raise ValueError(f"{r['id']}: {f} is not in features.yaml")
            by_feature[f].append(r)
    groups = {
        "Mandatory SQL features (Core)": [f for f in manifest.values() if not f.ours and f.mandatory],
        "Optional SQL features": [f for f in manifest.values() if not f.ours and not f.mandatory],
        "Our own features": [f for f in manifest.values() if f.ours],
    }
    lines: list[str] = []
    for title, features in groups.items():
        shown = [f for f in features if not f.out_of_scope or f.id in by_feature]
        left_out = len(features) - len(shown)
        covered = sum(f.id in by_feature for f in shown)
        lines += ["", f"### {title}", "", f"{covered} of {len(shown)} have a case"
                  + (f"; {left_out} left out of scope" if left_out else "") + ".", "",
                  "| Feature | Name | PostgreSQL | Layer |", "|---|---|---|---|"]
        for f in shown:
            pg = {True: "yes", False: "no", None: ""}[f.pg]
            lines.append(f"| {f.id} | {f.name} | {pg} | {layer_state(by_feature.get(f.id, []))} |")
    return lines


def render(data: dict, manifest: dict[str, Feature]) -> str:
    results = data["results"]
    out = ["# SQL layer conformance", ""]
    if not data["conformance"]:
        out += ["> These results are from a gate run, which skips what is marked not implemented: "
                "a skipped case counts as not passing. Run with `--conformance`.", ""]
    out += suite_table(results)
    marked = [r for r in results if r["marked"] and r["outcome"] == "pass"]
    if marked:
        out += ["", "## Marked not implemented, and passing", "",
                "The mark can come off these:", ""] + [f"- {r['suite']}: {r['id']}" for r in marked]
    out += ["", "## By SQL feature"] + feature_tables(results, manifest)
    return "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: report.py results.json", file=sys.stderr)
        return 2
    data = json.loads(pathlib.Path(sys.argv[1]).read_text())
    sys.stdout.write(render(data, load_manifest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
