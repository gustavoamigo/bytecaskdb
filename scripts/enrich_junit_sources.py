#!/usr/bin/env python3
"""Enrich Catch2 JUnit reports with source file/line metadata.

Codecov Test Analytics shows a flat list when JUnit cases lack source metadata.
This script joins:
1) Catch2 JUnit output (from --reporter junit)
2) Catch2 list-tests XML output (from --list-tests --reporter xml)
and writes `file` / `line` attributes onto each JUnit <testcase> node.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--pair",
        action="append",
        required=True,
        help="PAIR format: <test-binary-path>:<junit-xml-path>",
    )
    return parser.parse_args()


def build_source_map(test_bin: pathlib.Path) -> dict[str, tuple[str, str]]:
    result = subprocess.run(
        [str(test_bin), "--list-tests", "--reporter", "xml"],
        check=True,
        capture_output=True,
        text=True,
    )
    listing_root = ET.fromstring(result.stdout)
    mapping: dict[str, tuple[str, str]] = {}

    for case in listing_root.findall("TestCase"):
        name = case.findtext("Name")
        src = case.find("SourceInfo")
        if name is None or src is None:
            continue
        file_path = src.findtext("File")
        line = src.findtext("Line")
        if file_path is None or line is None:
            continue
        mapping[name] = (file_path, line)

    return mapping


def enrich_junit(junit_path: pathlib.Path, source_map: dict[str, tuple[str, str]]) -> int:
    tree = ET.parse(junit_path)
    root = tree.getroot()
    updated = 0

    for case in root.iter("testcase"):
        name = case.attrib.get("name")
        if name is None:
            continue
        src = source_map.get(name)
        if src is None:
            continue
        file_path, line = src
        case.set("file", file_path)
        case.set("line", line)
        updated += 1

    tree.write(junit_path, encoding="UTF-8", xml_declaration=True)
    return updated


def main() -> int:
    args = parse_args()

    for pair in args.pair:
        if ":" not in pair:
            raise ValueError(f"Invalid --pair format: {pair}")
        bin_part, junit_part = pair.split(":", 1)
        test_bin = pathlib.Path(bin_part)
        junit_xml = pathlib.Path(junit_part)

        if not test_bin.exists():
            raise FileNotFoundError(f"Test binary not found: {test_bin}")
        if not junit_xml.exists():
            raise FileNotFoundError(f"JUnit XML not found: {junit_xml}")

        source_map = build_source_map(test_bin)
        updated = enrich_junit(junit_xml, source_map)
        print(f"enriched {junit_xml} with {updated} testcase source mappings")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover
        print(f"ERROR: {exc}", file=sys.stderr)
        raise
