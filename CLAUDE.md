> `AGENTS.md` is a symlink to this file, so every coding agent reads the same instructions. Edit `CLAUDE.md`; don't replace the symlink with a copy.

# Project Overview

ByteCaskDB: an embedded key-value store in C++23 (Bitcask-style append-only files, in-memory persistent B+ tree key directory). Read the file you need, not all of them:

- `README.md`: what the project is, quick start, build commands. Kept short on purpose.
- `docs/bytecask_design.md`: design reference and the complete feature list. Large: list its sections with `grep -n '^##' docs/bytecask_design.md` (headings say what each section covers), then read the range you need.
- `CONTRACT.md`: what the engine guarantees, with the test proving each row.
- `include/bytecask.hpp`, `docs/api_reference.md`: the public API and usage examples.
- `docs/project_organization.md`: repository layout, build systems, license boundaries.
- `docs/performance.md`: benchmark tables and methodology.

# Development Guidelines

These instructions apply to every repository work request in this workspace.

## General instruction

- When generating commit messages or pull request descriptions, never include 'Co-authored-by' trailers or metadata attributing the work to an AI.
- Never change git `user.name`, `user.email`, or signing config (`gpg.*`, `user.signingkey`, `commit.gpgsign`), and never pass `--author` or `-c user.*` to git. The session hook sets them; commits must be signed as the repo owner.

## Required workflow for every implementation request

1. Update `docs/bytecask_design.md` to reflect any behavior, architecture, constraints, or invariants changed by the work. If a guarantee changes, update the row in `CONTRACT.md` with the test that proves it, and name the change in the PR description: added, widened, narrowed or removed. Work on the SQL layer updates its own documents instead: `bytecaskdb-sql-layer/docs/` and `bytecaskdb-sql-layer/CONTRACT.md`, not the engine's design doc and contract.
2. Track any follow-up work as a GitHub issue. Don't open an issue for the PR's own task — the PR is its own record; use an issue when work is being deferred, discovered mid-implementation, or otherwise won't be covered by a PR opened now.
3. Update `README.md` only when the project's pitch changes: a headline feature, a headline number, or the quick-start path. A feature added or changed goes in the Features section of `docs/bytecask_design.md`; API changes go in `docs/api_reference.md`; performance figures go in `docs/performance.md`. The README is an entry point and stays short (`scripts/check_readme_size.py` fails CI past its limit). It targets an external audience — avoid internal implementation details; focus on user-visible characteristics and behavior.
4. Always validate design before moving to implementation
5. After design approval, move to the implementation
6. Add or update tests that cover the behavior being changed.
7. Run the relevant tests after making changes.
8. Create one commit with a concise, high-signal message that describes the change. No approval is needed to commit.

## Issue tracking rules

GitHub Issues are the repository's task tracker, for work not already captured by an open PR.

- Track follow-up work as GitHub issues rather than in a repo file. Don't open one just to mirror a PR that's already tracking its own task.
- Add follow-up work discovered during implementation as a new issue.

## Benchmarking rules

- Any change to the engine claimed to improve performance **must** include a benchmark run of `engine_bench` before and after the change. Run it with `python3 scripts/run_engine_bench.py`. Results are appended to `benchmarks/engine_bench_results.csv` (local, gitignored).
- A performance claim in `bytecaskdb-sql-layer` **must** include before/after numbers from `sql_layer_bench`, which prints them rather than writing a CSV (`xmake f -m releasedbg --sql_layer=y && xmake build sql_layer_bench && xmake run sql_layer_bench`); `engine_bench` does not apply to a change that leaves the engine untouched (owner decision, 2026-10-19).
- Run `python3 scripts/run_map_bench.py` (records to `benchmarks/map_bench_results.csv`, also gitignored) only when making changes to a key directory tree: `blind_btree` or `btree`.
- Show the before/after CSV rows to the user as evidence of improvement.
- If benchmarks cannot be run, state why clearly and record the blocker in the PR description or a GitHub issue.

## Testing rules

- Prefer the narrowest test coverage that proves the change.
- If the repository does not yet have the necessary test seam, create the smallest practical test harness first.
- If tests cannot be run, state why clearly and record the blocker in the PR description or a GitHub issue.

### Engine tests (`tests/`)

Fault sweeps, the model-based recovery tests and the mutation sets have their own rules in `tests/CLAUDE.md`. Read them when a change touches recovery, a key directory tree, a durability site (`fdatasync`, rename, unlink, directory sync of a data or hint file), the chaos rig, or anything that restructures itself. A fault sweep must reach every structural transition of what it sweeps, and a durability fix adds the test that fails without it and a mutation that reverts the fix.

## Commit rules

- Commit and push without waiting for approval, then show the user what changed and the test result.
- **Never push to `main`, under any circumstances.** Committing, creating branches, and pushing to any other branch need no approval.
- Use commit messages that describe the actual behavior or architecture change, not generic messages like `update files`.

## Design philosophy

These four principles govern every design decision in the codebase, in priority order:

1. **Correctness** — Data integrity is paramount. A design that is fast but subtly wrong is worse than a slow correct one. Correctness failures include not only visible bugs but also undefined behaviour (UB): it may appear to work today and silently corrupt data or crash under a future compiler, sanitiser run, or load pattern. When a choice must be made between a correct design and an optimised one, always choose correct.

2. **Simplicity** — Prefer designs that are easy to understand and audit over clever ones. A future reader should be able to read a function and understand what it owns, what it borrows, and how long borrowed references live — without tracing through layers of abstraction. If a design requires a `// WARNING` comment to excuse a hazard, that is a sign to fix the design, not write the comment.

3. **Predictable latency over peak throughput** — Write-path operations must have bounded, predictable latency. Work that can be deferred without compromising correctness must be deferred. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes. This directly influences decisions like deferring hint file writes out of the rotation path, and avoiding heap allocations on hot iteration paths.

4. **Performance** — Optimizations require a real use case. Without one, correctness and simplicity take priority. Benchmark before and after every claimed improvement (`python3 scripts/run_engine_bench.py`). A performance win that introduces a latent UB is not a win.

**The data file is the only record.** Never persist engine state outside the data files. A sidecar, manifest or marker file is extra state that can drift out of sync with the data it describes, and every one needs its own crash and recovery testing. Hints are a rebuildable index of a data file, never a source of truth. If a design needs information the data files don't hold, change the design.

> **The BC-122 lesson:** The old `std::reverse_iterator<RadixTreeIterator>` satisfied none of the first three principles. It was UB (dangling span), it required an apology comment, and it allocated and freed a heap buffer on *every single dereference* — the opposite of predictable latency. Replacing it with `ReverseRadixTreeIterator`, which simply holds the cursor alive as a member (`cur_`) and returns `*cur_` from `operator*`, was simultaneously the correct, simpler, and 26× faster choice. That ordering is not a coincidence.

## C++ coding guidelines

### Naming and formatting — follow Google C++ Style Guide
- Private member variables use a trailing underscore: `value_`, `file_`, `offset_`.
- Constants use a `k` prefix: `kHeaderSize`, `kCrcSize`.
- Types and functions use `PascalCase` and `snake_case` respectively.
- Use 2-space indentation; braces on the same line.

### Design decisions, safety, and modern idioms — follow C++ Core Guidelines
- When adding new functionality, reuse existing internal functions instead of duplicating logic. If two code paths share the same validation, I/O, or transformation steps, factor the shared logic into a single function and have both paths call it.
- When a function is replaced or subsumed by a new one, remove the old function. Do not leave dead or redundant methods in the codebase.
- Follow Core Guidelines NL.1, NL.2, and NL.3 for comments: don't state in comments what can be clearly stated in code (avoid Doxygen `@param` boilerplate if the type signature is clear). Instead, use comments to state intent, constraints, thread-safety, and side effects (like I/O flushing). Keep comments crisp.
- Prefer C++20 features when they bring clearer intent over lower-level alternatives.
  - Use `std::span` instead of raw pointer + size pairs.
  - Use `std::as_bytes` / `std::as_writable_bytes` instead of `reinterpret_cast` to raw byte pointers.
  - Use range-for over index-based loops when the index is not needed.
  - Use `std::to_integer<T>` to convert `std::byte` values rather than casting.
- Use `auto` for local variables to avoid redundant repetition of type names (C++ Core Guidelines ES.11). Use explicit types only for loop variables where the exact type is semantically significant for bit-level correctness.
- Prefer `static_assert` over runtime checks for properties that are knowable at compile time.
- Use `[[nodiscard]]` on functions whose return value must not be silently ignored.
- Keep `reinterpret_cast` only where the C++ standard explicitly permits it (e.g. passing a buffer to a legacy C-style API that cannot be changed).
- Use `std::is_integral_v<T>` instead of `std::is_integral<T>::value` (variable templates since C++17).
- Use `std::in_range<T>` (C++20) to validate narrowing conversions at system boundaries.
- Wrap checked narrowing conversions in a `narrow<To>(From)` helper that calls `std::in_range` and throws on overflow, rather than scattering `if (!std::in_range<T>) throw` / `static_cast` pairs at every call site.
- Wrap ugly or unsafe low-level calls (e.g. `reinterpret_cast` through a legacy C-style API) in a small named function so the cast appears once and call sites stay readable (C++ Core Guidelines ES.50 / T.69).
- Use `std::ssize` (C++20) instead of casting `.size()` to a signed type.

### Safety through abstraction — compile-time first, runtime second

Prefer designs where incorrect usage is impossible over designs where incorrect usage is merely caught. In priority order:

1. **Compile-time safety**: Use the type system to make invalid states unrepresentable. If a capability or field is absent for a subset of instances, encode that in the types so that invalid access is a compiler error, not a runtime bug.
2. **Runtime safety**: When compile-time enforcement is not possible (e.g. base-pointer dispatch), use assertions, tag checks, or safe defaults so that bugs produce crashes or wrong-but-safe behavior — never silent memory corruption or UB.

Avoid designs where correctness depends on programmer discipline alone (e.g. "never touch field X on this variant"). A future contributor will forget. The abstraction should make forgetting either impossible (compile error) or immediately visible (assertion failure).

**Example — the P4 Node/InternalNode split**: Rather than allocating leaf nodes at a smaller size and relying on discipline to never access the `children_` field (heap buffer overflow if violated), we split into `Node` (base, no `children_` field) and `InternalNode : Node` (derived, has `children_`). Accessing `children_` through a `Node*` is a compile error. Child accessor methods on `Node` check a tag bit before downcasting and return safe defaults ("no children") when the tag is absent.

### Engine code (`bytecaskdb/`)

Rules specific to the engine's code (lazy iterators, non-owning views, ownership through failure, batch markers) are in `bytecaskdb/CLAUDE.md`. Read them before changing code in that folder.

### Thread-local state

A `thread_local` outlives every DB its thread touches, so one holding a DB's state or recognising a DB by address leaks across instances (BC-243, #306). Tag every `thread_local` in engine and binding code with a `TLS: scratch|process|per-DB` comment on it or in the comment block above; `scripts/check_thread_local.py` fails CI otherwise. Prefer `scratch` or `process`. A `per-DB` one must not match a later DB opened at a dead one's address — name the DB by `DB::instance_id_`, or have `close()` and `~DB` release it — and its tag names the test proving that. See `docs/bytecask_design.md`, *Per-thread state*.

### Clang warnings to avoid (-Weverything is enabled)
- **CTAD on standard lock types** (`-Wctad-maybe-unsupported`): Always write the explicit template argument for `std::lock_guard` and `std::unique_lock`: `std::lock_guard<std::mutex>`, `std::unique_lock<std::mutex>`. Never rely on CTAD for these types.
- **`[[nodiscard]]` ignored** (`-Wunused-result`): Never silently discard the return value of a `[[nodiscard]]` function. In tests where the value is intentionally unused, cast to `void`: `(void)db.del(...)`. In production code, handle or assign the result.
- **Exit-time destructors** (`-Wexit-time-destructors`): `thread_local` variables with non-trivial destructors trigger this warning. Wrap the declaration in `#pragma clang diagnostic push/pop` with `-Wexit-time-destructors` ignored and a comment explaining why the destructor is intentional.
- **Switch coverage** (`-Wswitch-enum` / `-Wswitch-default` conflict): The project suppresses `-Wswitch-default` (in `xmake.lua`) because `-Wswitch-enum` already enforces exhaustive enum coverage. When writing a switch on an enum: list every case explicitly — do not add a catch-all `default:` break. If a subset of cases is genuinely irrelevant (e.g. `BulkBegin`/`BulkEnd` inside `emit_entry`), add explicit no-op cases for the remaining values rather than a `default:`.
- **Sign-conversion on array indexing** (`-Wsign-conversion`): When indexing an array/string with the result of a signed distribution or function, wrap the index in `static_cast<std::size_t>(...)`.
- **Missing `[[noreturn]]` on lambdas** (`-Wmissing-noreturn`): Add `[[noreturn]]` to lambdas that unconditionally throw: `[][[noreturn]] { throw ...; }`.
- **Unused functions/variables** (`-Wunused-function`, `-Wunused-variable`): Remove helper functions and variables that are no longer referenced after a refactor rather than leaving them in place.

## Documentation writing guidelines

- Write for the audience, not for completeness. If the reader can infer it, don't say it.
- Plain, direct, technical prose. No marketing language.
- Short sentences over long ones when making a key claim.
- Don't repeat a claim in prose that a benchmark or example already proves.
- Don't pre-empt criticisms with defensive explanations. The right data point makes the point silently.
- Be honest about weaknesses. Acknowledging a real limitation earns more trust than hiding it.
- Don't extrapolate benchmark results beyond what has actually been measured.
- `CONTRACT.md` states what the engine guarantees, not how it works. A sentence that could change while every guarantee stays the same is mechanism and belongs in `docs/bytecask_design.md`. Every name in it is declared in `include/bytecask.hpp`; `scripts/check_contract_vocabulary.py` fails CI on a private name (#399). Each row names the tests that prove it, or `none`; a change to a guarantee is named in the PR that makes it. `bytecaskdb-sql-layer/CONTRACT.md` follows the same rules for the SQL layer, against `bytecaskdb-sql-layer/algebra/bc_plan.h` and `bytecaskdb-sql-layer/api/database.cppm`, with its mechanism in `bytecaskdb-sql-layer/docs/`.
- Headings in `docs/` say what their section covers, because an agent routes by `grep -n '^##' <doc>` and reads a range. "Data File Format (.data)" and "Operational Counters" are the model; "Purpose", "API" and "Scope" are not. Name the subject, and qualify a heading that repeats under another parent (``### `BackgroundWorker` API``, not `### API`).
- Before renaming a heading, search the repo for its name and its anchor (`#slug`), in other docs, source comments and `tests/*_mutations/` patches. Leave a cited heading as it is, or fix every citation in the same PR.

## README guidelines

The README targets an experienced systems/database engineering audience. They know what block caches, compaction, and B-tree rebalancing are. They will scroll past the intro to check for transactions. Write accordingly. It is an entry point, not a reference: it is also what a new reader opens first, so detail belongs in `docs/` and the README links to it.

**Framing**
- The core argument is that ByteCaskDB is designed for current hardware reality, not a reaction against other engines. Don't frame it as LSM vs B-tree vs Bitcask. Frame it as: what would you design if you started today?
- Never use "trade-off" framing that implicitly accepts disk-based designs as the neutral default.
- Make the point and move on. This is not a court trial.

**Intro section must answer, in order:**
1. What is it?
2. What is the core design bet and why?
3. How much RAM does that actually require?
4. Does it support transactions?

**Specific rules**
- Avoid repeating the project name unnecessarily within the same paragraph.
- Don't claim scale properties in prose that the benchmark tables already prove.
- Don't extrapolate benchmark results beyond what has been measured. If a number hasn't been benchmarked, don't publish it.
- Weaknesses belong in the README. The range scan caveat stays.

## Tips

* Build and run the test suite with `xmake build bytecask_tests && xmake run bytecask_tests`. xmake can take a while; wait for it to finish before doing anything else.
