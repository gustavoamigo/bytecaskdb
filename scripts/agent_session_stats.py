#!/usr/bin/env python3
"""Token and exploration statistics for Claude Code session transcripts.

Reads ~/.claude/projects/<project>/*.jsonl (or the files given) and prints,
per transcript: fixed context at the first model call, exploration calls
before the first edit, tool mix, and a weighted token cost.

Cost = output + cache_creation + 0.1 * cache_read (reads are billed at a
tenth of writes). Fixed context is input + cache_creation + cache_read of the
first main-thread assistant message. Subagent (sidechain) messages are
excluded from both. Local files only; no network.
"""

import argparse
import json
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

DEFAULT_DIR = Path.home() / ".claude" / "projects"
SEARCH_TOOLS = {"Grep", "Glob", "Read"}
SEARCH_BASH = re.compile(r"(^|[|;&]\s*)(rg|grep|egrep|find|ls|cat|head|tail|sed|awk|wc|tree)\b")
EDIT_TOOLS = {"Edit", "Write", "NotebookEdit"}


def is_exploration(name, tool_input):
    if name in SEARCH_TOOLS:
        return True
    return name == "Bash" and bool(SEARCH_BASH.search(tool_input.get("command", "")))


def analyse(path):
    first_ctx = None
    cost = 0.0
    tools = Counter()
    explore_before_edit = 0
    explore_total = 0
    edited = False
    for line in path.read_text(errors="replace").splitlines():
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        msg = rec.get("message")
        if rec.get("type") != "assistant" or not isinstance(msg, dict):
            continue
        if rec.get("isSidechain"):
            continue
        usage = msg.get("usage") or {}
        created = usage.get("cache_creation_input_tokens", 0)
        read = usage.get("cache_read_input_tokens", 0)
        if first_ctx is None and usage:
            first_ctx = usage.get("input_tokens", 0) + created + read
        cost += usage.get("output_tokens", 0) + created + 0.1 * read
        content = msg.get("content")
        for block in content if isinstance(content, list) else []:
            if block.get("type") != "tool_use":
                continue
            name = block["name"]
            tools[name] += 1
            if name in EDIT_TOOLS:
                edited = True
            elif is_exploration(name, block.get("input") or {}):
                explore_total += 1
                if not edited:
                    explore_before_edit += 1
    return {
        "file": path.name[:8],
        "first_ctx": first_ctx,
        "explore_before_edit": explore_before_edit,
        "explore_total": explore_total,
        "tool_calls": sum(tools.values()),
        "cost": int(cost),
        "top_tools": tools.most_common(4),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("transcripts", nargs="*", type=Path)
    parser.add_argument("--project-dir", type=Path,
                        help="a directory of .jsonl transcripts (default: ~/.claude/projects)")
    parser.add_argument("--match", default="bytecaskdb-rc",
                        help="keep transcripts whose path contains this (default: bytecaskdb-rc)")
    args = parser.parse_args()

    paths = args.transcripts
    if not paths:
        root = args.project_dir or DEFAULT_DIR
        paths = sorted((p for p in root.rglob("*.jsonl") if args.match in str(p)),
                       key=lambda p: p.stat().st_mtime)
    rows = [r for r in (analyse(p) for p in paths if p.stat().st_size) if r["first_ctx"]]
    if not rows:
        sys.exit("no transcripts with usage data")

    print(f"{'session':9}{'first_ctx':>10}{'expl<edit':>10}{'expl_all':>9}{'calls':>7}{'cost':>13}  top tools")
    for r in rows:
        tools = ", ".join(f"{n}:{c}" for n, c in r["top_tools"])
        print(f"{r['file']:9}{r['first_ctx']:>10}{r['explore_before_edit']:>10}"
              f"{r['explore_total']:>9}{r['tool_calls']:>7}{r['cost']:>13}  {tools}")

    def med(key):
        return int(statistics.median(r[key] for r in rows))

    print(f"\nmedians over {len(rows)} sessions: first_ctx {med('first_ctx')},"
          f" expl<edit {med('explore_before_edit')}, calls {med('tool_calls')}, cost {med('cost')}")


if __name__ == "__main__":
    main()
