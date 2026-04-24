#!/usr/bin/env python3
"""Build and run memory_profile.cpp at multiple dataset sizes, printing a
summary table of peak RSS and per-key overhead.

Usage:
    python3 scripts/run_memory_profile.py [--skip-build] [--sizes 50000,100000,500000]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TARGET = "memory_profile"
BINARY = REPO_ROOT / "build/linux/x86_64/release/memory_profile"
DEFAULT_SIZES = [50_000, 100_000, 500_000, 1_000_000, 10_000_000]
VALUE_SIZE = 245  # must match kValueSize in memory_profile.cpp



def build(skip: bool) -> None:
    if skip:
        print(f"[skip-build] Using existing binary: {BINARY}")
        return
    print("Configuring release mode...")
    subprocess.run(
        ["xmake", "f", "-m", "release"], check=True, cwd=REPO_ROOT
    )
    print(f"Building {TARGET}...")
    subprocess.run(
        ["xmake", "build", TARGET], check=True, cwd=REPO_ROOT
    )


def parse_output(text: str) -> dict[str, int]:
    """Extract memory measurements and key/value sizes from memory_profile stdout."""
    results: dict[str, int] = {}
    phase = None
    for line in text.splitlines():
        # Header: === Memory Profile (N keys, K-byte [text|binary] keys, V-byte values) ===
        m = re.search(r"(\d+)-byte (?:text|binary) keys", line)
        if m:
            results["meta/key_size"] = int(m.group(1))
        m = re.search(r"(\d+)-byte values", line)
        if m:
            results["meta/value_size"] = int(m.group(1))
        m = re.match(r"\s*\[(.+)\]", line)
        if m:
            phase = m.group(1)
            continue
        m = re.match(r"\s+(\S.*?)\s+[\d.]+\s+MiB\s+\((\d+)\s+bytes\)", line)
        if m and phase:
            label = m.group(1).rstrip(":")
            results[f"{phase}/{label}"] = int(m.group(2))
    return results


def run_profile(n: int, key_format: str = "text") -> dict[str, int]:
    bend_dir = str(REPO_ROOT / ".tmp")
    env = {"BC_DATASET_SIZE": str(n), "BC_BENCH_DIR": bend_dir, "BC_KEY_FORMAT": key_format}
    # Inherit PATH and other essentials
    import os
    full_env = {**os.environ, **env}
    result = subprocess.run(
        [str(BINARY)],
        capture_output=True,
        text=True,
        check=True,
        cwd=REPO_ROOT,
        env=full_env,
    )
    print(result.stdout, end="")
    return parse_output(result.stdout)


def print_summary(label: str, rows: list[tuple[int, int, int, float]], key_size: int, value_size: int) -> None:
    size_label = f"key_size={key_size}, value_size={value_size}" if key_size else f"value_size={value_size}"
    print(f"\n{'=' * 72}")
    print(f"Summary — {label} (Peak RSS, {size_label})")
    print(f"{'=' * 72}")
    print(f"{'Keys':>12}  {'Peak RSS':>12}  {'DB overhead':>12}  {'B/key':>8}  {'Overhead':>10}")
    print(f"{'-' * 12}  {'-' * 12}  {'-' * 12}  {'-' * 8}  {'-' * 10}")
    for n, peak, db_bytes, per_key in rows:
        effective = per_key - key_size if key_size else per_key
        print(
            f"{n:>12,}  {peak / 1024 / 1024:>10.1f} MB  "
            f"{db_bytes / 1024 / 1024:>10.1f} MB  {per_key:>8.1f}  {effective:>10.1f}"
        )


def run_format(sizes: list[int], key_format: str) -> tuple[list[tuple[int, int, int, float]], int, int]:
    rows: list[tuple[int, int, int, float]] = []
    key_size: int = 0
    value_size: int = VALUE_SIZE

    for n in sizes:
        print(f"\n{'=' * 60}")
        measurements = run_profile(n, key_format)

        if not key_size:
            key_size = measurements.get("meta/key_size", 0)
        if measurements.get("meta/value_size"):
            value_size = measurements["meta/value_size"]

        before = measurements.get("before open/RSS", 0)
        after = measurements.get("after insert/RSS", 0)
        db_bytes = after - before
        per_key = db_bytes / n if n > 0 else 0
        rows.append((n, after, db_bytes, per_key))

    return rows, key_size, value_size


def main() -> None:
    parser = argparse.ArgumentParser(description="Run ByteCaskDB memory profile")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--sizes",
        type=str,
        default=None,
        help="Comma-separated dataset sizes (default: 50k,100k,500k,1M)",
    )
    args = parser.parse_args()

    sizes = DEFAULT_SIZES
    if args.sizes:
        sizes = [int(s.replace("_", "")) for s in args.sizes.split(",")]

    build(args.skip_build)

    # Run with text keys (string UUIDv7)
    print("\n" + "=" * 60)
    print("  TEXT KEYS (string UUIDv7)")
    print("=" * 60)
    text_rows, text_key_size, text_value_size = run_format(sizes, "text")

    # Run with binary keys (16-byte UUIDv7)
    print("\n" + "=" * 60)
    print("  BINARY KEYS (16-byte UUIDv7)")
    print("=" * 60)
    binary_rows, binary_key_size, binary_value_size = run_format(sizes, "binary")

    # Print both summaries
    # Overhead = B/key - key_size (structural overhead beyond key bytes)
    print_summary("Text keys", text_rows, text_key_size, text_value_size)
    print_summary("Binary keys", binary_rows, binary_key_size, binary_value_size)


if __name__ == "__main__":
    main()
