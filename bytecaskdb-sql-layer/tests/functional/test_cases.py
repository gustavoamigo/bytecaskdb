# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Our cases (cases/<suite>/*.yaml), one test each."""

from __future__ import annotations

import pytest

from executor import run_case
from loaders.yaml_cases import load_suite
from manifest import load_manifest


def pytest_generate_tests(metafunc):
    if "case" not in metafunc.fixturenames:
        return
    features = set(load_manifest())
    cases = [c for suite in ("postgresql", "bytecaskdb") for c in load_suite(suite, features=features)]
    metafunc.parametrize("case", [
        pytest.param(c, id=f"{c.suite}/{c.id}", marks=[getattr(pytest.mark, f) for f in c.features])
        for c in cases
    ])


def test_case(request, case, servers):
    if case.unimplemented and not request.config.getoption("conformance"):
        pytest.skip("not implemented")
    handle = servers.acquire(case)
    request.node.server_handle = handle
    try:
        run_case(case, handle)
    finally:
        servers.release(case, handle)
