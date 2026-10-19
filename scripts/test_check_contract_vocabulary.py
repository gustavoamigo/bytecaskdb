#!/usr/bin/env python3
"""Tests of check_contract_vocabulary.py's reading of a module interface:
which names a .cppm offers a contract. Run by contract.yml.

Usage: python3 scripts/test_check_contract_vocabulary.py
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import check_contract_vocabulary as cv  # noqa: E402


def names(src: str) -> set[str]:
    keywords = {"export", "module", "namespace", "class", "struct", "enum", "int",
                "void", "public", "private", "auto", "template", "return", "const"}
    return set(cv.IDENT.findall(cv.module_text(cv.strip_comments(src)))) - keywords


class ModuleTextTest(unittest.TestCase):
    def test_only_exported_declarations_count(self):
        src = """module;
#include <hidden_header>
export module a.b;
export namespace pub { struct Shown { int field; }; }
namespace priv { struct Hidden { int h; }; }
export void shown_fn();
void hidden_fn();
"""
        self.assertEqual(names(src), {"pub", "Shown", "field", "shown_fn"})

    def test_export_is_a_whole_word(self):
        src = "export module a.b; namespace n { exporter_t delta; export_state_ x; }"
        self.assertEqual(names(src), set())

    def test_an_export_block_is_a_scope(self):
        src = "export module a.b; export { struct Alpha { int beta; }; void gamma(); } struct Hidden { int h; };"
        self.assertEqual(names(src), {"Alpha", "beta", "gamma"})

    def test_class_members_by_access(self):
        src = """export module a.b;
export class C { int secret_; public: int shown(); private: int p_; };
export struct S { int s; private: int hid_; };
"""
        self.assertEqual(names(src), {"C", "shown", "S", "s"})

    def test_bodies_and_initializer_lists_are_dropped(self):
        src = """export module a.b;
export class E { public: E(int c) : base{c}, code_{c} {} int code() const { return code_; } };
export template <class T> auto f(T t) { return t.zz_; }
"""
        self.assertEqual(names(src), {"E", "c", "code", "T", "f", "t"})


if __name__ == "__main__":
    unittest.main()
