# ByteCask Copilot Workflow

These instructions apply to every repository work request in this workspace.

## General instruction

- Ignore `docs/old_bytecask_design.md`, it's there just so we I can copy and paste ideas from an older project. 

## Required workflow for every implementation request

1. Update `docs/bytecask_design.md` to reflect any behavior, architecture, constraints, or invariants changed by the work.
2. Update `docs/bytecask_project_plan.md` to track the task, status, and any follow-up work.
3. Update `README.md` to keep it in sync with the project: if a feature is added or changed, update the Features list, Quick Start examples, API Reference, and Architecture sections as needed. The README targets an external audience — avoid internal implementation details; focus on user-visible characteristics and behavior.
4. Always validate design before moving to implementation
5. After design approval, move to the implementation
6. Add or update tests that cover the behavior being changed.
7. Run the relevant tests after making changes.
8. Do not create a git commit until the user has reviewed the change and explicitly approved committing it.
9. After approval, create one commit with a concise, high-signal message that describes the change.

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

### Model-based recovery tests

The `[model]` test cases in `tests/bytecask_test.cpp` (random workload, batch-heavy, delete-heavy) are the primary soundness checks for the DB engine. They run thousands of random operations, close the DB, then reopen under serial and parallel recovery and verify that all key/values and per-file `file_stats` (live_bytes, total_bytes) match a serial baseline.

Update or extend the model-based tests when a change could affect what a recovered DB contains or reports. Common examples:
- Alters recovery logic (parsing, merging, fan-in, file_stats computation).
- Adds a new write-path feature (new operation type, new on-disk format field) that must survive recovery.
- Changes how `file_stats` are computed or propagated.
- Modifies the parallel merge strategy (thread counts, merge order, tombstone handling).

This list is not exhaustive — use judgment. If a change touches anything that could make serial and parallel recovery diverge, add or update coverage.

When extending, follow the existing pattern: collect a serial baseline outside the SECTIONs, copy the sealed DB to an isolated directory per SECTION, reopen with the target thread count, and CHECK both key/values and `collect_stats(db) == serial_stats_vals`.

## Commit rules

- Show the user what changed and the test result before offering to commit.
- Wait for explicit approval before running any commit command.
- Use commit messages that describe the actual behavior or architecture change, not generic messages like `update files`.

## Design philosophy

These four principles govern every design decision in the codebase, in priority order:

1. **Correctness** — Data integrity is paramount. A design that is fast but subtly wrong is worse than a slow correct one. Correctness failures include not only visible bugs but also undefined behaviour (UB): it may appear to work today and silently corrupt data or crash under a future compiler, sanitiser run, or load pattern. When a choice must be made between a correct design and an optimised one, always choose correct.

2. **Simplicity** — Prefer designs that are easy to understand and audit over clever ones. A future reader should be able to read a function and understand what it owns, what it borrows, and how long borrowed references live — without tracing through layers of abstraction. If a design requires a `// WARNING` comment to excuse a hazard, that is a sign to fix the design, not write the comment.

3. **Predictable latency over peak throughput** — Write-path operations must have bounded, predictable latency. Work that can be deferred without compromising correctness must be deferred. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes. This directly influences decisions like deferring hint file writes out of the rotation path, and avoiding heap allocations on hot iteration paths.

4. **Performance** — Optimizations are pursued only when they do not compromise correctness or simplicity. Benchmark before and after every claimed improvement (`python3 scripts/run_engine_bench.py`). A performance win that introduces a latent UB is not a win.

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
- Prefer `std::filesystem` utilities over manual file-handle tricks (e.g. `std::filesystem::file_size`).

### Lifetime and view safety — spans, iterators, and non-owning views

The hardest class of bugs in this codebase involves returning non-owning views (`std::span`, `std::string_view`, raw pointers) into an object's own internal buffer. These are correct as long as the owning object stays alive, but silently become dangling references — confirmed by ASan as heap-use-after-free — if:

- The owning object is a temporary (destroyed at end of a full expression).
- The owning object is moved, reallocated, or goes out of scope while the view is still held.
- A standard library adaptor (e.g. `std::reverse_iterator`) creates a copy of your type internally.

**Rule: never compose an iterator whose `operator*` returns a span-into-`*this` with `std::reverse_iterator`.**

`std::reverse_iterator<T>::operator*` is specified as:
```cpp
auto operator*() const { auto tmp = cur_; return *--tmp; }
```
It creates a temporary copy, decrements it, and dereferences it. If `T::operator*` returns a span into `this->buffer_`, that buffer is destroyed when the temporary is destroyed — at the end of the expression — leaving the caller with a dangling span. This also causes a heap alloc+free on every dereference (a performance penalty on top of the UB).

**The correct pattern — hold the cursor alive as a member (lesson from BC-122):**

```cpp
// WRONG: std::reverse_iterator creates temporaries — dangling span, heap alloc per dereference
using reverse_iterator = std::reverse_iterator<RadixTreeIterator<V>>;

// CORRECT: custom reverse iterator holds the cursor alive, operator* returns *cur_
export template <typename V> class ReverseRadixTreeIterator {
public:
  explicit ReverseRadixTreeIterator(RadixTreeIterator<V> past_pos)
      : cur_{std::move(past_pos)} { --cur_; }   // pre-decrement once in ctor

  auto operator*() const { return *cur_; }       // span into cur_'s live buffer — never dangling
  auto operator++() -> ReverseRadixTreeIterator& { --cur_; return *this; }
  auto operator==(std::default_sentinel_t) const noexcept -> bool { return cur_ == std::default_sentinel; }

private:
  RadixTreeIterator<V> cur_;   // owned — keeps the key buffer alive
};
```

**General rules for non-owning views:**

- A function returning `std::span` or `std::string_view` is lending a view. If the owning object's lifetime is not obvious from the call site, document it with a comment: `// returned span valid until next write`.
- Never return a span or view into a local variable, a value parameter, or a temporary.
- Never store a `std::span` or `std::string_view` in a struct unless you can guarantee the owner outlives the struct, and document that guarantee explicitly.
- If you see a `// WARNING:` comment excusing a lifetime hazard, treat it as a required design fix, not permanent documentation. Removing the hazard is always the goal.
- When in doubt, return an owned value (`std::vector<std::byte>`, `std::string`). The borrow optimisation is only worth it when profiling identifies it as a real bottleneck.

### Clang warnings to avoid (-Weverything is enabled)
- **CTAD on standard lock types** (`-Wctad-maybe-unsupported`): Always write the explicit template argument for `std::lock_guard` and `std::unique_lock`: `std::lock_guard<std::mutex>`, `std::unique_lock<std::mutex>`. Never rely on CTAD for these types.
- **`[[nodiscard]]` ignored** (`-Wunused-result`): Never silently discard the return value of a `[[nodiscard]]` function. In tests where the value is intentionally unused, cast to `void`: `(void)db.del(...)`. In production code, handle or assign the result.
- **Exit-time destructors** (`-Wexit-time-destructors`): `thread_local` variables with non-trivial destructors trigger this warning. Wrap the declaration in `#pragma clang diagnostic push/pop` with `-Wexit-time-destructors` ignored and a comment explaining why the destructor is intentional.
- **Switch coverage** (`-Wswitch-enum` / `-Wswitch-default` conflict): The project suppresses `-Wswitch-default` (in `xmake.lua`) because `-Wswitch-enum` already enforces exhaustive enum coverage. When writing a switch on an enum: list every case explicitly — do not add a catch-all `default:` break. If a subset of cases is genuinely irrelevant (e.g. `BulkBegin`/`BulkEnd` inside `emit_entry`), add explicit no-op cases for the remaining values rather than a `default:`.
- **Sign-conversion on array indexing** (`-Wsign-conversion`): When indexing an array/string with the result of a signed distribution or function, wrap the index in `static_cast<std::size_t>(...)`.
- **Missing `[[noreturn]]` on lambdas** (`-Wmissing-noreturn`): Add `[[noreturn]]` to lambdas that unconditionally throw: `[][[noreturn]] { throw ...; }`.
- **Unused functions/variables** (`-Wunused-function`, `-Wunused-variable`): Remove helper functions and variables that are no longer referenced after a refactor rather than leaving them in place.

## Tips

* xmake can take a while to run, wait for the build to run before trying to do something else. 
* Don't run any command while the terminal is running as the integration with terminal is sending a ^C killing the previous process when you send a new command.
* After running a long command (build, benchmark, test suite), do NOT immediately send another command. Wait for the command to finish and return output before proceeding. If the output is truncated or the command appears to still be running, ask the user to confirm completion rather than sending a follow-up command. 