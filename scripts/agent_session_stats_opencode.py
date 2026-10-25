#!/usr/bin/env python3
"""Token and exploration statistics for OpenCode sessions.

The OpenCode counterpart of agent_session_stats.py (PR #631), for running the
pinned-task baseline of issue #632 from an OpenCode session. Reads the local
OpenCode session store (~/.local/share/opencode/opencode.db) and prints,
per session: fixed context at the first model call, exploration calls before
the first edit, tool mix, and a weighted token cost.

Cost = output + cache_write + 0.1 * cache_read (reads are billed at a tenth
of writes), summed over assistant messages. Fixed context is input +
cache_write + cache_read of the first assistant message. Subagent child
sessions (parent_id set) are excluded from both. Local DB only; no network.
"""

import argparse
import json
import re
import sqlite3
import statistics
import sys
from collections import Counter
from pathlib import Path

DEFAULT_DB = Path.home() / ".local/share/opencode/opencode.db"
SEARCH_TOOLS = {"grep", "glob", "read", "list"}
SEARCH_BASH = re.compile(r"(^|[|;&]\s*)(rg|grep|egrep|find|ls|cat|head|tail|sed|awk|wc|tree)\b")
EDIT_TOOLS = {"edit", "write", "patch"}


def is_exploration(name, tool_input):
    if name in SEARCH_TOOLS:
        return True
    return name in ("bash", "shell") and bool(SEARCH_BASH.search(tool_input.get("command", "")))


def analyse(conn, sess):
    sid, title, directory, model, version = sess[:5]
    first_ctx = None
    cost = 0.0
    tools = Counter()
    explore_before_edit = 0
    explore_total = 0
    edited = False

    # Tool calls appear twice: as `part` rows and inline in the message's
    # content blocks. Older sessions may have only the latter. Merge both,
    # deduplicating by call id, and keep the merged call order via timestamps.
    msgs = conn.execute(
        "select data, time_created from session_message where session_id = ? "
        "and type = 'assistant' order by time_created, id", (sid,))
    calls = {}
    for (mdata, mtime) in msgs:
        try:
            msg = json.loads(mdata)
        except json.JSONDecodeError:
            continue
        if msg.get("model") is None or msg.get("finish") == "error":
            continue
        usage = msg.get("tokens") or {}
        cache = usage.get("cache") or {}
        created, read = cache.get("write", 0), cache.get("read", 0)
        if first_ctx is None and (usage.get("input") or created or read):
            first_ctx = usage.get("input", 0) + created + read
        cost += usage.get("output", 0) + created + 0.1 * read
        for block in msg.get("content") or []:
            if isinstance(block, dict) and block.get("type") == "tool" and block.get("name"):
                calls.setdefault(block["id"], (mtime, block["name"],
                                               (block.get("state") or {}).get("input") or {}))

    parts = conn.execute(
        "select data, time_created from part where session_id = ? "
        "and json_extract(data, '$.type') = 'tool' "
        "and json_extract(data, '$.tool') is not null", (sid,))
    for (pdata, ptime) in parts:
        try:
            p = json.loads(pdata)
        except json.JSONDecodeError:
            continue
        calls.setdefault(p.get("callID") or f"part:{p.get('id')}",
                         (ptime, p.get("tool"), (p.get("state") or {}).get("input") or {}))

    for _, name, tool_input in sorted(calls.values(), key=lambda c: c[0]):
        tools[name] += 1
        if name in EDIT_TOOLS:
            edited = True
        elif is_exploration(name, tool_input):
            explore_total += 1
            if not edited:
                explore_before_edit += 1

    if model:
        try:
            m = json.loads(model)
        except json.JSONDecodeError:
            m = {}
    else:
        m = {}
    return {
        "id": sid,
        "title": title or "(untitled)",
        "directory": directory,
        "model": f"{m.get('providerID', '?')}/{m.get('id', '?')}",
        "version": version,
        "first_ctx": first_ctx,
        "explore_before_edit": explore_before_edit,
        "explore_total": explore_total,
        "tool_calls": sum(tools.values()),
        "cost": int(cost),
        "top_tools": tools.most_common(4),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--db", type=Path, default=DEFAULT_DB,
                        help="OpenCode session DB (default: ~/.local/share/opencode/opencode.db)")
    parser.add_argument("--session", help="report exactly this session id (recommended over --latest)")
    parser.add_argument("--match", default="bytecask",
                        help="keep sessions whose directory contains this (default: bytecask)")
    parser.add_argument("--latest", action="store_true",
                        help="report only the newest matching session, as a markdown block")
    args = parser.parse_args()

    if not args.db.exists():
        sys.exit(f"session DB not found: {args.db}")
    conn = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    sessions = conn.execute(
        "select id, title, directory, model, version, time_updated from session_v2 "
        "where parent_id is null and time_archived is null "
        + ("and instr(directory, ?) > 0" if args.match else ""),
        [args.match] if args.match else []).fetchall()
    if not sessions:
        sys.exit(f"no sessions matching {args.match!r} (try --match '' or --session <id>)")

    if args.session:
        chosen = [s for s in sessions if s[0] == args.session]
        if not chosen:
            sys.exit(f"session {args.session} not found or is a subagent session")
    elif args.latest:
        chosen = [max(sessions, key=lambda s: s[5])]
    else:
        chosen = sorted(sessions, key=lambda s: s[5])

    rows = [r for r in (analyse(conn, s) for s in chosen) if r["first_ctx"] is not None]
    if not rows:
        sys.exit("no sessions with usage data")

    if args.latest or args.session:
        r = rows[-1]
        print(f"- session: `{r['id']}` \"{r['title']}\" ({r['directory']})\n"
              f"- model: {r['model']}\n- OpenCode version: {r['version']}\n"
              f"- first-call context: {r['first_ctx']}\n"
              f"- exploration calls before first edit: {r['explore_before_edit']}"
              f" (total {r['explore_total']})\n- tool calls: {r['tool_calls']}\n"
              f"- weighted cost: {r['cost']}\n- top tools: "
              + ", ".join(f"{n}:{c}" for n, c in r["top_tools"]))
        if len(rows) > 1:
            print(f"... and {len(rows) - 1} more sessions in the arg list: "
                  + ", ".join(r["id"][:8] for r in rows[:-1]))
        return

    print(f"{'session':9}{'first_ctx':>10}{'expl<edit':>10}{'expl_all':>9}{'calls':>7}{'cost':>13}  top tools")
    for r in rows:
        tools = ", ".join(f"{n}:{c}" for n, c in r["top_tools"])
        print(f"{r['id'][-8:]:9}{r['first_ctx']:>10}{r['explore_before_edit']:>10}"
              f"{r['explore_total']:>9}{r['tool_calls']:>7}{r['cost']:>13}  {tools}")

    def med(key):
        return int(statistics.median(r[key] for r in rows))

    print(f"\nmedians over {len(rows)} sessions: first_ctx {med('first_ctx')},"
          f" expl<edit {med('explore_before_edit')}, calls {med('tool_calls')}, cost {med('cost')}")


if __name__ == "__main__":
    main()
