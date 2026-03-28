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