#!/usr/bin/env python3
"""Tests of check_code_map.py: which backticked tokens count as paths, and
how they resolve. Run by code-map.yml.

Usage: python3 scripts/test_check_code_map.py
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import check_code_map as cm  # noqa: E402


class LooksLikePathTest(unittest.TestCase):
    def test_paths_and_source_names_count(self):
        for token in ("docs/code_map.md", "db.cpp", "tests/fuzz/", "tests/*_mutations/"):
            self.assertTrue(cm.looks_like_path(token), token)

    def test_commands_and_names_do_not(self):
        for token in ("xmake run bytecask_tests", "[model]", "BYTECASK_KEYDIR=btree",
                      "bytecask.cppm::open()", "<test name>", "DB"):
            self.assertFalse(cm.looks_like_path(token), token)


class ExistsTest(unittest.TestCase):
    def test_repo_path_exists(self):
        self.assertTrue(cm.exists("scripts/check_code_map.py"))

    def test_bare_engine_file_resolves_under_bytecaskdb(self):
        self.assertTrue(cm.exists("db_commit.cpp"))

    def test_glob_resolves(self):
        self.assertTrue(cm.exists("tests/*_mutations/"))

    def test_missing_path_is_reported(self):
        self.assertFalse(cm.exists("bytecaskdb/no_such_file.cppm"))


if __name__ == "__main__":
    unittest.main()
