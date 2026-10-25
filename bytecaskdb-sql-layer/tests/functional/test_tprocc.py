# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""TPROC-C as a case of the suite: the five transactions on a small load, then
the nine consistency conditions on what they left (docs/testing_design.md,
*The TPROC-C harness*). The harness is in-process, so no server runs here."""

from __future__ import annotations

import dataclasses
import os
import subprocess

import pytest

TPROCC_ENV = "BYTECASK_SQL_TPROCC"


@dataclasses.dataclass(frozen=True)
class TproccCase:
    suite: str = "tprocc"
    id: str = "tprocc/five transactions, then the consistency conditions"
    features: tuple[str, ...] = ("layer:tprocc",)
    unimplemented: bool = False


def pytest_generate_tests(metafunc):
    if "case" in metafunc.fixturenames:
        metafunc.parametrize("case", [pytest.param(TproccCase(), id="tprocc/consistency")])


def _harness(binary: str, command: str, directory, *flags: str) -> None:
    done = subprocess.run([binary, command, "--dir", str(directory), *flags], capture_output=True, text=True, timeout=600)
    assert done.returncode == 0, f"sql_layer_tprocc {command} exited {done.returncode}\n{done.stdout}\n{done.stderr}"


def test_tprocc(case, tmp_path):
    binary = os.environ.get(TPROCC_ENV)
    if not binary:
        pytest.skip(f"{TPROCC_ENV} is not set")
    directory = tmp_path / "db"
    _harness(binary, "load", directory, "--warehouses", "1")
    # run checks the conditions after its last phase; check reopens what it left.
    _harness(binary, "run", directory, "--threads", "2", "--warmup", "0", "--duration", "3")
    _harness(binary, "check", directory)
