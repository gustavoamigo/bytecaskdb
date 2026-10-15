#!/usr/bin/env python3
"""Fails on a mutation's Guarded-by spec that selects no test.

Each mutation patch (tests/soak_mutations/, tests/chaos_mutations/,
tests/durability_mutations/) names its test with a `Guarded-by: "<spec>"`
header, a Catch2 test spec. Catch2 reads an unescaped `,` as OR, so a test
name holding a comma, written as it is, selects tests named after each
fragment: usually none. The test binary then exits non-zero, which reads as
"caught" to anyone checking the mutation by exit code (#463). A comma that
belongs to a name is written `\\,`.

Each fragment between unescaped commas must select at least one test on its
own, so an OR whose one side is a typo fails too. The binary lists the
matches (`--list-tests`), so the spec is matched by Catch2 itself.

A patch that touches bytecaskdb-mariadb-plugin/ names a test of the plugin's
binary (mariadb_proof_tests); every other patch names one of bytecask_tests.
Pass the binary and the scope it serves. A soak or chaos patch the rig
itself judges, with no Guarded-by header, is not checked here.

Usage: scripts/check_guarded_by.py --scope engine|plugin <test binary> [root]
"""

import argparse
import pathlib
import re
import subprocess
import sys

DIRS = ("tests/soak_mutations", "tests/chaos_mutations",
        "tests/durability_mutations")
HEADER = re.compile(r'^Guarded-by:\s*"(.*)"\s*$')
PLUGIN = "bytecaskdb-mariadb-plugin/"


def is_plugin_patch(text: str) -> bool:
    return any(line.startswith("+++ b/" + PLUGIN)
               for line in text.splitlines())


def specs_of(text: str) -> list[str]:
    specs = []
    for line in text.splitlines():
        if line.startswith("diff --git"):
            break
        m = HEADER.match(line)
        if m:
            specs.append(m.group(1))
    return specs


def fragments(spec: str) -> list[str]:
    """Splits at the commas Catch2 reads as OR, keeping `\\,` escaped."""
    out, cur, i = [], "", 0
    while i < len(spec):
        if spec[i] == "\\" and i + 1 < len(spec):
            cur += spec[i:i + 2]
            i += 2
            continue
        if spec[i] == ",":
            out.append(cur)
            cur = ""
        else:
            cur += spec[i]
        i += 1
    out.append(cur)
    return out


def matches(binary: str, spec: str) -> list[str]:
    run = subprocess.run([binary, "--list-tests", "--verbosity", "quiet",
                          spec], capture_output=True, text=True, check=False)
    return [line for line in run.stdout.splitlines() if line.strip()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scope", choices=("engine", "plugin"), required=True)
    ap.add_argument("binary")
    ap.add_argument("root", nargs="?", default=".")
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    failures = checked = 0
    for d in DIRS:
        for patch in sorted((root / d).glob("*.patch")):
            text = patch.read_text()
            if is_plugin_patch(text) != (args.scope == "plugin"):
                continue
            for spec in specs_of(text):
                for frag in fragments(spec):
                    checked += 1
                    if not matches(args.binary, frag):
                        where = (f'fragment "{frag}" of "{spec}"'
                                 if frag != spec else f'"{spec}"')
                        print(f"{patch}: {where} selects no test")
                        failures += 1
    print(f"check_guarded_by: {checked} spec fragment(s) checked, "
          f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
