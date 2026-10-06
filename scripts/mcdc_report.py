#!/usr/bin/env python3
"""MC/DC report over an `llvm-cov export -format=text` JSON file.

Clang's MC/DC (-fcoverage-mcdc) records, for each decision of two or more
conditions, whether each condition was shown to change the outcome on its own
with the others held fixed. This script prints, per source file, how many such
conditions are covered, and lists every decision that has a condition no test
isolates.

A decision whose miss is unreachable by design is marked at the site with a
comment holding `mcdc-exempt:` and the reason, either on the decision's first
line or in the comment block directly above it:

    // mcdc-exempt: zstd checks the decompressed size against the frame
    // header, so got != size cannot hold without ZSTD_isError(got).
    if (ZSTD_isError(got) || got != size)

The marker exempts every decision that starts on that line. `mcdc-exempt(C3):`
exempts only the third condition, counted left to right as `llvm-cov show
-show-mcdc` numbers them, so the decision's other conditions still need tests.
Exemptions are listed, with their reason, in every report, so adding one shows
in review. A marker on a condition that is covered, or on no decision at all,
is an error: a marker cannot outlive the miss it excuses.

With --require-full FILE..., the script exits nonzero if any listed file has a
condition that is neither covered nor exempt, or a stale marker anywhere. With
--max-exempt N as well, it also fails once those files hold more than N exempt
decisions: the floor cannot be held by exempting what a test should cover, and
raising N is a one-line change a reviewer sees.

Several exports may be given, one per build (the engine is built on each key
directory); a file's decisions are merged across them, and across template
instantiations: a condition counts as covered if some build or instantiation
shows it independent. Exports are merged here rather than by llvm-profdata,
which drops a function whose MC/DC bitmap differs between builds.

A decision that ran but recorded no test vectors is marked `[no vectors]`.
Clang 21 records none for a decision that is the condition of a `?:` whose
result has class type; name the decision as a bool first.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path

MARKER = "mcdc-exempt"
MARKER_RE = re.compile(r"mcdc-exempt(?:\(([C0-9, ]+)\))?:")


@dataclass
class Decision:
    line: int
    col: int
    end_line: int
    covered: list[bool]
    no_vectors: bool = False
    exempt_reason: str | None = None
    # 0-based indices of the exempt conditions; None exempts all of them.
    exempt_conds: set[int] | None = None

    def exempt(self, i: int) -> bool:
        return self.exempt_reason is not None and (
            self.exempt_conds is None or i in self.exempt_conds)

    def missed(self) -> list[int]:
        return [i for i, c in enumerate(self.covered)
                if not c and not self.exempt(i)]


@dataclass
class FileReport:
    path: str
    decisions: list[Decision] = field(default_factory=list)
    stale: list[tuple[int, str]] = field(default_factory=list)

    def counts(self) -> tuple[int, int, int]:
        total = covered = exempt = 0
        for d in self.decisions:
            total += len(d.covered)
            covered += sum(d.covered)
            exempt += sum(1 for i, c in enumerate(d.covered)
                          if not c and d.exempt(i))
        return total, covered, exempt

    def misses(self) -> list[Decision]:
        return [d for d in self.decisions if d.missed()]


def comment_text(line: str) -> str | None:
    """The text of a `//` comment-only line, or None for a code line."""
    s = line.strip()
    return s[2:].strip() if s.startswith("//") else None


def find_markers(src: list[str]) -> dict[int, tuple[int, str, set[int] | None]]:
    """Maps the 1-based line a marker applies to -> (marker line, reason,
    exempt condition indices or None for all).

    A trailing marker applies to its own line. A marker in a comment block
    applies to the first code line after the block; its reason runs to the
    end of the block.
    """
    out: dict[int, tuple[int, str, set[int] | None]] = {}
    i = 0
    while i < len(src):
        line = src[i]
        m = MARKER_RE.search(line)
        if m is None:
            i += 1
            continue
        conds = None
        if m.group(1):
            conds = {int(c.strip().lstrip("C")) - 1
                     for c in m.group(1).split(",") if c.strip()}
        reason = line[m.end():].strip()
        if comment_text(line) is None:
            out[i + 1] = (i + 1, reason, conds)
            i += 1
            continue
        j = i + 1
        while j < len(src) and comment_text(src[j]) is not None:
            reason += " " + comment_text(src[j])
            j += 1
        out[j + 1] = (i + 1, reason.strip(), conds)
        i = j
    return out


def build_report(path: str, fobjs: list[dict]) -> FileReport:
    merged: OrderedDict[tuple[int, int, int, int], list[bool]] = OrderedDict()
    vectors: dict[tuple[int, int, int, int], int] = {}
    ran: set[int] = set()
    for fobj in fobjs:
        for rec in fobj.get("mcdc_records", []):
            key = tuple(rec[:4])
            conds = rec[9]
            vectors[key] = vectors.get(key, 0) + rec[4] + rec[5]
            if key in merged:
                merged[key] = [a or b for a, b in zip(merged[key], conds)]
            else:
                merged[key] = list(conds)
        for br in fobj.get("branches", []):
            if br[4] + br[5] > 0:
                ran.add(br[0])
    rep = FileReport(path)
    for key, conds in sorted(merged.items()):
        ls, cs, le, _ce = key
        rep.decisions.append(Decision(
            ls, cs, le, conds, no_vectors=vectors[key] == 0 and ls in ran))

    try:
        src = Path(path).read_text().split("\n")
    except OSError:
        return rep
    markers = find_markers(src)
    used: set[int] = set()
    for d in rep.decisions:
        m = markers.get(d.line)
        if m is None:
            continue
        used.add(d.line)
        marker_line, reason, conds = m
        if conds is None:
            if all(d.covered):
                rep.stale.append((marker_line,
                                  "marks a decision that is fully covered"))
                continue
        else:
            bad = sorted(i for i in conds
                         if i >= len(d.covered) or d.covered[i])
            if bad:
                rep.stale.append((marker_line, "exempts "
                                  + ", ".join(f"C{i + 1}" for i in bad)
                                  + ", which is covered or does not exist"))
                continue
        d.exempt_reason = reason
        d.exempt_conds = conds
    for target, (marker_line, _, _) in markers.items():
        if target not in used:
            rep.stale.append((marker_line, "marks no MC/DC decision"))
    return rep


def describe(src: list[str], d: Decision) -> str:
    text = " ".join(src[i - 1].strip() for i in range(d.line, d.end_line + 1)
                    if 0 < i <= len(src))
    flags = "".join("x" if c else ("e" if d.exempt(i) else ".")
                    for i, c in enumerate(d.covered))
    gap = " [no vectors]" if d.no_vectors else ""
    return f"{d.line}:{d.col} [{flags}]{gap} {text[:160]}"


def demangle(names: list[str]) -> list[str]:
    """llvm-cxxfilt over names, or the names as they are without it."""
    try:
        out = subprocess.run(["llvm-cxxfilt"], input="\n".join(names),
                             capture_output=True, text=True, check=True)
        return out.stdout.split("\n")[:len(names)]
    except (OSError, subprocess.CalledProcessError):
        return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("export_json", nargs="+",
                    help="llvm-cov export -format=text output, one per build")
    ap.add_argument("--require-full", nargs="*", default=[], metavar="FILE",
                    help="files (path suffixes) that must have no MC/DC miss "
                         "outside an exemption")
    ap.add_argument("--max-exempt", type=int, default=None, metavar="N",
                    help="most exempt decisions the --require-full files may "
                         "hold")
    ap.add_argument("--root", default=None,
                    help="strip this prefix from printed paths")
    args = ap.parse_args()

    by_path: OrderedDict[str, list[dict]] = OrderedDict()
    # Function name -> (file, first line, called in some build). A module's
    # internal-linkage function is emitted into every unit that imports it,
    # under a "<unit>:" prefix; the copies are one function.
    functions: dict[str, list] = {}
    unit_prefix = re.compile(r"^[^:]+\.(?:cpp|cppm|pcm):")
    for export in args.export_json:
        data = json.loads(Path(export).read_text())
        for f in data["data"][0]["files"]:
            by_path.setdefault(f["filename"], []).append(f)
        for fn in data["data"][0]["functions"]:
            if not fn["regions"]:
                continue
            entry = functions.setdefault(
                unit_prefix.sub("", fn["name"]),
                [fn["filenames"][0], fn["regions"][0][0], False])
            entry[2] = entry[2] or fn["count"] > 0
    reports = [build_report(p, fs) for p, fs in by_path.items()]
    reports = [r for r in reports if r.decisions or r.stale]
    root = args.root.rstrip("/") + "/" if args.root else ""

    def name(r: FileReport) -> str:
        return r.path[len(root):] if root and r.path.startswith(root) else r.path

    print(f"{'File':<34} {'Conditions':>10} {'Covered':>8} {'Exempt':>7} "
          f"{'Missed':>7} {'Cover':>8}")
    tt = tc = te = 0
    for r in sorted(reports, key=name):
        t, c, e = r.counts()
        tt, tc, te = tt + t, tc + c, te + e
        pct = f"{100.0 * (c + e) / t:.2f}%" if t else "-"
        print(f"{name(r):<34} {t:>10} {c:>8} {e:>7} {t - c - e:>7} {pct:>8}")
    pct = f"{100.0 * (tc + te) / tt:.2f}%" if tt else "-"
    print(f"{'TOTAL':<34} {tt:>10} {tc:>8} {te:>7} {tt - tc - te:>7} {pct:>8}")
    print("Cover counts exempt conditions as covered; Covered alone is what "
          "the tests show.")

    failed = False
    required = [r for r in reports
                if any(r.path.endswith(s) for s in args.require_full)]
    for suffix in args.require_full:
        if not any(r.path.endswith(suffix) for r in reports):
            print(f"\nERROR: --require-full {suffix}: no MC/DC data for it")
            failed = True

    print("\n==> Exempt decisions (unreachable by design)")
    for r in sorted(reports, key=name):
        src = Path(r.path).read_text().split("\n")
        for d in r.decisions:
            if d.exempt_reason is not None:
                print(f"  {name(r)}:{describe(src, d)}")
                print(f"      reason: {d.exempt_reason}")

    uncalled = sorted((f, line, n) for n, (f, line, called) in functions.items()
                      if not called
                      and any(f.endswith(s) for s in args.require_full))
    names = dict(zip((u[2] for u in uncalled),
                     demangle([u[2] for u in uncalled])))
    # A function's region starts at its body. A special member the compiler
    # declared has its class's line instead, and no declarator above it that
    # names it; one written out does.
    sources: dict[str, list[str]] = {}

    def written(f: str, line: int, demangled: str) -> bool:
        # The function's own name: the last qualified name before a "(",
        # less the "@module" a module-attached name carries.
        plain = re.sub(r"@[\w.]+", "", demangled)
        found = re.findall(r"::(operator[^(\s]+|~?\w+)\(", plain)
        if not found:
            return True
        src = sources.setdefault(f, Path(f).read_text().split("\n"))
        word = found[-1].lstrip("~") + "("
        return any(word in src[i - 1] for i in range(max(1, line - 4), line + 1)
                   if i <= len(src))

    uncalled = [u for u in uncalled if written(u[0], u[1], names[u[2]])]
    if uncalled:
        print("\n==> Functions in the required files no build calls")
        for f, line, n in uncalled:
            short = f[len(root):] if root and f.startswith(root) else f
            print(f"  {short}:{line} {names[n]}")

    for r in sorted(reports, key=name):
        for line, why in r.stale:
            print(f"ERROR: {name(r)}:{line}: stale `{MARKER}` marker: {why}")
            failed = True

    for r in sorted(required, key=name):
        misses = r.misses()
        if not misses:
            continue
        failed = True
        src = Path(r.path).read_text().split("\n")
        print(f"\nERROR: {name(r)}: MC/DC misses outside an exemption")
        for d in misses:
            print(f"  {describe(src, d)}")

    exempt_decisions = sum(1 for r in required for d in r.decisions
                           if d.exempt_reason is not None)
    if args.max_exempt is not None:
        print(f"\nExempt decisions in the required files: {exempt_decisions} "
              f"(at most {args.max_exempt})")
        if exempt_decisions > args.max_exempt:
            print("ERROR: more exempt decisions than --max-exempt allows. "
                  "Cover the new ones with a test, or raise the cap in "
                  "scripts/run_coverage.sh with the reason in the PR.")
            failed = True

    if args.require_full and not failed:
        print("\nMC/DC: every condition in "
              + ", ".join(args.require_full)
              + " is covered or exempt.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
