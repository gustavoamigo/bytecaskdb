#!/usr/bin/env python3
"""
Patch compile_commands.json to add missing -fmodule-file= mappings.

xmake's compile_commands generator only records the module-file flags that it
explicitly passed on a given compile line, but sometimes omits direct imports
(e.g. bytecask.hint_file for bytecask.cppm).  clangd then reports
"Module 'X' not found" even though the build succeeds.

Fix: for each xmake target group, collect the union of every
-fmodule-file=NAME=PATH flag that appears anywhere in that group, then
inject any that are missing into every entry of the group.
The result is idempotent — running it twice is harmless.
"""

import json
import re
import sys
from collections import defaultdict
from pathlib import Path


CDB = Path("compile_commands.json")


def target_of(entry: dict) -> str | None:
    """Return the xmake target name embedded in any output/input path."""
    for arg in entry.get("arguments", []):
        m = re.search(r"build/(?:\.gens|\.objs)/([^/]+)/", arg)
        if m:
            return m.group(1)
    return None


def module_mappings(args: list[str]) -> dict[str, str]:
    """Return {module_name: pcm_path} from -fmodule-file=NAME=PATH flags."""
    result: dict[str, str] = {}
    for arg in args:
        m = re.fullmatch(r"-fmodule-file=([^=]+)=(.*)", arg)
        if m:
            result[m.group(1)] = m.group(2)
    return result


def patch(entries: list[dict]) -> int:
    """Mutate entries in-place; return count of flags added."""
    # Group entry indices by target name.
    groups: dict[str, list[int]] = defaultdict(list)
    for i, entry in enumerate(entries):
        t = target_of(entry)
        if t:
            groups[t].append(i)

    added = 0
    for target, indices in groups.items():
        # Collect union of all module-file mappings present in this target.
        all_mappings: dict[str, str] = {}
        for i in indices:
            all_mappings.update(module_mappings(entries[i]["arguments"]))

        # Inject any missing flags into each entry.
        for i in indices:
            args = entries[i]["arguments"]
            existing = module_mappings(args)
            missing = {k: v for k, v in all_mappings.items() if k not in existing}
            if missing:
                inject = [f"-fmodule-file={k}={v}" for k, v in missing.items()]
                # Insert just before the final source-file argument.
                entries[i]["arguments"] = args[:-1] + inject + [args[-1]]
                added += len(inject)

    return added


def main() -> None:
    if not CDB.exists():
        print(f"fix_compile_commands: {CDB} not found, skipping", file=sys.stderr)
        return

    entries = json.loads(CDB.read_text())
    added = patch(entries)
    CDB.write_text(json.dumps(entries, indent=2))
    if added:
        print(f"fix_compile_commands: added {added} missing -fmodule-file= flag(s)")
    else:
        print("fix_compile_commands: compile_commands.json already complete")


if __name__ == "__main__":
    main()
