# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The gap list's `not PostgreSQL:` marks, and the ceiling line the report
makes from them (docs/functional_testing_design.md, *The PostgreSQL ceiling*).
Needs no server."""

from __future__ import annotations

from loaders.sqltest import gap_list, load_sqltest, postgresql_refuses
from report import ceiling


def test_gap_list_reads_ids_and_comments(tmp_path):
    path = tmp_path / "list.txt"
    path.write_text("# a header\n"
                    "a_1  # not PostgreSQL: VARING; PostgreSQL 17 gives 42601\n"
                    "a_2  # roles are not built\n"
                    "a_3\n")
    assert gap_list(path) == {"a_1": "not PostgreSQL: VARING; PostgreSQL 17 gives 42601",
                              "a_2": "roles are not built", "a_3": ""}
    assert postgresql_refuses(path) == {"a_1"}


def test_loaded_cases_carry_the_mark():
    cases = {c.id: c for c in load_sqltest()}
    refused = postgresql_refuses()
    assert refused, "the list marks the tests PostgreSQL refuses"
    for case_id in refused:
        assert cases[case_id].unimplemented and cases[case_id].postgresql_refuses
    assert sum(c.postgresql_refuses for c in cases.values()) == len(refused)


def entry(outcome, refused=False, suite="sqltest"):
    return {"suite": suite, "id": "x", "features": [], "marked": False,
            "postgresql_refuses": refused, "outcome": outcome}


def test_ceiling_counts_only_what_postgresql_accepts():
    results = [entry("pass"), entry("pass"), entry("fail"), entry("fail", refused=True),
               entry("pass", suite="postgresql")]
    assert ceiling(results) == ["", "PostgreSQL 17 refuses 1 of sqltest's 4 tests "
                                "(`not PostgreSQL:` in `not_implemented/sqltest.txt`). Of the 3 it "
                                "accepts, the layer passes 2 (67%)."]


def test_no_ceiling_line_without_a_refused_test():
    assert ceiling([entry("pass")]) == []
    assert ceiling([entry("pass", suite="postgresql")]) == []
