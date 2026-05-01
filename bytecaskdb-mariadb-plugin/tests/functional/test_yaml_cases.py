# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_yaml_cases.py — Discovers and runs all .yaml files in cases/.
#
# Each YAML file is a list of test cases in the format defined by runner.py.
# pytest parametrizes over all cases; test IDs are "<category>/<name>".
#
# Run a single category:
#   pytest -v -k joins
# Run a specific test:
#   pytest -v -k "INNER JOIN on PK"

import glob
import os

import pytest
import yaml

from runner import run_case

_CASES_DIR = os.path.join(os.path.dirname(__file__), "cases")


def _collect_cases():
    params = []
    for yaml_file in sorted(glob.glob(os.path.join(_CASES_DIR, "*.yaml"))):
        category = os.path.splitext(os.path.basename(yaml_file))[0]
        with open(yaml_file) as f:
            cases = yaml.safe_load(f) or []
        for case in cases:
            params.append(pytest.param(case, id=f"{category}/{case['name']}"))
    return params


@pytest.mark.parametrize("case", _collect_cases())
def test_yaml_case(case, make_connection):
    run_case(case, make_connection)
