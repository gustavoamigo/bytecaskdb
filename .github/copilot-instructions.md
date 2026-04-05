# ByteCask Copilot Workflow

These instructions apply to every repository work request in this workspace.

## General instruction

- Ignore `docs/old_bytecask_design.md`, it's there just so we I can copy and paste ideas from an older project. 

## Required workflow for every implementation request

1. Update `docs/bytecask_design.md` to reflect any behavior, architecture, constraints, or invariants changed by the work.
2. Update `docs/bytecask_project_plan.md` to track the task, status, and any follow-up work.
3. Always validate design before moving to implementation
4. After design approval, move to the implementation
5. Add or update tests that cover the behavior being changed.
6. Run the relevant tests after making changes.
7. Do not create a git commit until the user has reviewed the change and explicitly approved committing it.
8. After approval, create one commit with a concise, high-signal message that describes the change.

## Project plan rules

`docs/bytecask_project_plan.md` is the repository's simple task tracker.

- Keep it lightweight and current.
- Prefer short entries with an ID, status, title, and one-line note.
- Move completed work to the `Done` section instead of deleting history.
- Add follow-up work to `Backlog` when it is discovered during implementation.

## Benchmarking rules

- Any change claimed to improve performance **must** include a benchmark run of `engine_bench` before and after the change. Run it with `python3 scripts/run_engine_bench.py`. Results are recorded in `benchmarks/engine_bench_results.csv`.
- Run `python3 scripts/run_map_bench.py` (records to `benchmarks/map_bench_results.csv`) only when making changes to `radix_tree` or `persistent_ordered_map`.
- Show the before/after CSV rows to the user as evidence of improvement.
- If benchmarks cannot be run, state why clearly and leave the project plan updated with the blocker.

## Testing rules

- Prefer the narrowest test coverage that proves the change.
- If the repository does not yet have the necessary test seam, create the smallest practical test harness first.
- If tests cannot be run, state why clearly and leave the project plan updated with the blocker.

## Commit rules

- Show the user what changed and the test result before offering to commit.
- Wait for explicit approval before running any commit command.
- Use commit messages that describe the actual behavior or architecture change, not generic messages like `update files`.

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
- Prefer `std::filesystem` utilities over manual file-handle tricks (e.g. `std::filesystem::file_size`).

## Tips

* xmake can take a while to run, wait for the build to run before trying to do something else. 
* Don't run any command while the terminal is running as the integration with terminal is sending a ^C killing the previous process when you send a new command.
* After running a long command (build, benchmark, test suite), do NOT immediately send another command. Wait for the command to finish and return output before proceeding. If the output is truncated or the command appears to still be running, ask the user to confirm completion rather than sending a follow-up command. 